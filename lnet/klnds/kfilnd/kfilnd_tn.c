// SPDX-License-Identifier: GPL-2.0
/*
 * kfilnd transaction and state machine processing.
 * Copyright 2019 Cray Inc. All Rights Reserved.
 */

#include "kfilnd_tn.h"
#include "kfilnd_wkr.h"
#include "kfilnd_ep.h"
#include "kfilnd_dev.h"
#include "kfilnd_dom.h"
#include <asm/checksum.h>

static struct kmem_cache *tn_cache;

static __sum16 kfilnd_tn_cksum(void *ptr, int nob)
{
	return csum_fold(csum_partial(ptr, nob, 0));
}

static const char *kfilnd_tn_msgtype2str(enum kfilnd_msg_type type)
{
	switch (type) {
	case KFILND_MSG_IMMEDIATE:
		return "IMMEDIATE";

	case KFILND_MSG_BULK_PUT_REQ:
		return "BULK_PUT_REQUEST";

	case KFILND_MSG_BULK_GET_REQ:
		return "BULK_GET_REQUEST";

	case KFILND_MSG_BULK_RSP:
		return "BULK_RESPONSE";

	default:
		return "???";
	}
}

static int kfilnd_tn_msgtype2size(enum kfilnd_msg_type type)
{
	const int hdr_size = offsetof(struct kfilnd_msg, kfm_u);

	switch (type) {
	case KFILND_MSG_IMMEDIATE:
		return offsetof(struct kfilnd_msg, kfm_u.immed.payload[0]);

	case KFILND_MSG_BULK_PUT_REQ:
	case KFILND_MSG_BULK_GET_REQ:
		return hdr_size + sizeof(struct kfilnd_bulk_req);

	case KFILND_MSG_BULK_RSP:
		return hdr_size + sizeof(struct kfilnd_bulk_rsp);
	default:
		return -1;
	}
}

static void kfilnd_tn_pack_msg(struct kfilnd_transaction *tn, u8 prefer_rx)
{
	struct kfilnd_msg *msg = tn->tn_tx_msg.msg;

	/* Commented out members should be set already */
	msg->kfm_magic    = KFILND_MSG_MAGIC;
	msg->kfm_version  = KFILND_MSG_VERSION;
	/*  kfm_type */
	msg->kfm_prefer_rx = prefer_rx;
	/*  kfm_nob */
	/*  kfm_srcnid */
	msg->kfm_cksum    = kfilnd_tn_cksum(msg, msg->kfm_nob);
}

static int kfilnd_tn_unpack_msg(struct kfilnd_msg *msg, int nob)
{
	const int hdr_size = offsetof(struct kfilnd_msg, kfm_u);
	u16	version;
	int	msg_nob;
	bool	flip;

	if (nob < hdr_size) {
		CWARN("Short message: %d\n", nob);
		return -EPROTO;
	}

	if (msg->kfm_magic == KFILND_MSG_MAGIC) {
		flip = false;
	} else if (msg->kfm_magic == __swab32(KFILND_MSG_MAGIC)) {
		flip = true;
	} else {
		CWARN("Bad magic: %08x\n", msg->kfm_magic);
		return -EPROTO;
	}

	version = flip ? __swab16(msg->kfm_version) : msg->kfm_version;
	if (version != KFILND_MSG_VERSION) {
		CWARN("Bad version: %x\n", version);
		return -EPROTO;
	}

	msg_nob = flip ? __swab32(msg->kfm_nob) : msg->kfm_nob;
	if (msg_nob > nob) {
		CWARN("Short message: got %d, wanted %d\n", nob, msg_nob);
		return -EPROTO;
	}

	/* If kfilnd_tn_cksum() returns a non-zero value, checksum is bad. */
	if (msg->kfm_cksum != NO_CHECKSUM && kfilnd_tn_cksum(msg, msg_nob)) {
		CERROR("Bad checksum\n");
		return -EPROTO;
	}

	if (flip) {
		/* Leave magic unflipped as a clue to peer endianness */
		msg->kfm_version = version;
		msg->kfm_nob     = msg_nob;
		__swab64s(&msg->kfm_srcnid);
	}

	if (msg->kfm_srcnid == LNET_NID_ANY) {
		CWARN("Bad src nid: %s\n", libcfs_nid2str(msg->kfm_srcnid));
		return -EPROTO;
	}

	if (msg_nob < kfilnd_tn_msgtype2size(msg->kfm_type)) {
		CWARN("Short %s: %d(%d)\n",
		      kfilnd_tn_msgtype2str(msg->kfm_type),
		      msg_nob, kfilnd_tn_msgtype2size(msg->kfm_type));
		return -EPROTO;
	}

	switch ((enum kfilnd_msg_type)msg->kfm_type) {
	case KFILND_MSG_IMMEDIATE:
		break;

	case KFILND_MSG_BULK_PUT_REQ:
	case KFILND_MSG_BULK_GET_REQ:
		if (flip) {
			__swab32s(&msg->kfm_u.bulk_req.mr_key);
			__swab64s(&msg->kfm_u.bulk_req.cookie);
		}
		break;

	case KFILND_MSG_BULK_RSP:
		if (flip) {
			__swab32s(&msg->kfm_u.bulk_rsp.status);
			__swab64s(&msg->kfm_u.bulk_rsp.cookie);
		}
		break;

	default:
		CERROR("Unknown message type %x\n", msg->kfm_type);
		return -EPROTO;
	}
	return 0;
}

/* Get a prefer rx (CPT) number from the target NID */
static u8 kfilnd_tn_prefer_rx(struct kfilnd_transaction *tn)
{
	return tn->tn_target_nid % tn->tn_ep->end_dev->kfd_ni->ni_ncpts;
}

static void kfilnd_tn_setup_immed(struct kfilnd_transaction *tn)
{
	if (tn->tn_kiov)
		lnet_copy_kiov2flat(KFILND_IMMEDIATE_MSG_SIZE,
				    tn->tn_tx_msg.msg,
				    offsetof(struct kfilnd_msg,
					     kfm_u.immed.payload),
				    tn->tn_num_iovec, tn->tn_kiov,
				    tn->tn_offset_iovec,
				    tn->tn_nob_iovec);
	else
		lnet_copy_iov2flat(KFILND_IMMEDIATE_MSG_SIZE,
				   tn->tn_tx_msg.msg,
				   offsetof(struct kfilnd_msg,
					    kfm_u.immed.payload),
				   tn->tn_num_iovec, tn->tn_iov,
				   tn->tn_offset_iovec,
				   tn->tn_nob_iovec);
}

/**
 * kfilnd_tn_process_rx_event() - Process an immediate receive event.
 *
 * For each immediate receive, a transaction structure needs to be allocated to
 * process the receive.
 */
static void kfilnd_tn_process_rx_event(void *buf_context, void *msg_context,
				       int msg_size)
{
	struct kfilnd_transaction *tn;
	struct kfilnd_immediate_buffer *bufdesc = buf_context;
	struct kfilnd_msg *rx_msg = msg_context;
	bool alloc_msg = true;
	enum tn_events event = TN_EVENT_FAIL;
	int rc;
	bool dec_async_event_count = false;

	/* Unpack the message */
	rc = kfilnd_tn_unpack_msg(rx_msg, msg_size);
	if (rc) {
		CERROR("Failed to unpack message: rc=%d\n", rc);
		return;
	}

	switch ((enum kfilnd_msg_type)rx_msg->kfm_type) {
	case KFILND_MSG_IMMEDIATE:
		alloc_msg = false;

		/* Fall through to allocate transaction strcture. */
	case KFILND_MSG_BULK_PUT_REQ:
	case KFILND_MSG_BULK_GET_REQ:
		/* Context points to a received buffer and status is the length.
		 * Allocate a Tn structure, set its values, then launch the
		 * receive.
		 */
		tn = kfilnd_tn_alloc(bufdesc->immed_end->end_dev,
				     bufdesc->immed_end->end_cpt, alloc_msg);
		if (!tn) {
			CERROR("Can't get receive Tn: Tn descs exhausted\n");
			return;
		}
		tn->tn_rx_msg.msg = msg_context;
		tn->tn_rx_msg.length = msg_size;
		tn->tn_nob = msg_size;
		tn->tn_posted_buf = bufdesc;
		break;

	case KFILND_MSG_BULK_RSP:
		tn = (void *)rx_msg->kfm_u.bulk_rsp.cookie;
		if (rx_msg->kfm_u.bulk_rsp.status) {
			CERROR("Peer error: rc=%d\n",
			       rx_msg->kfm_u.bulk_rsp.status);
			tn->tn_status = -EREMOTEIO;
			event = TN_EVENT_FAIL;
		}
		tn->tn_posted_buf = bufdesc;
		dec_async_event_count = true;

		break;

	default:
		CERROR("Dropping receive message\n");
		return;
	};

	kfilnd_tn_event_handler(tn, TN_EVENT_RX_OK, dec_async_event_count);
}

/**
 * kfilnd_tn_process_unlink_event() - Process unlink of immediate receive
 * buffer.
 *
 * Immediate buffer unlink occurs when all the space in the multi-receive buffer
 * has been consumed or the buffer is manually unlinked (cancelled). A reference
 * needs to be returned to the immediate buffer.
 */
static void kfilnd_tn_process_unlink_event(void *buf_context, void *context,
					   int status)
{
	struct kfilnd_immediate_buffer *bufdesc = buf_context;
	int rc;

	rc = kfilnd_ep_imm_buffer_put(bufdesc->immed_end, bufdesc);
	if (rc)
		CERROR("Could not repost Rx buffer, rc = %d\n", rc);
}

/**
 * kfilnd_tn_process_rma_event() - Process a RMA transaction event.
 */
static void kfilnd_tn_process_rma_event(void *ep_context, void *tn_context,
					int status)
{
	struct kfilnd_transaction *tn = tn_context;
	enum tn_events event;

	if (status)
		CERROR("RMA failed to %s: rx_ctx=%llu errno=%d\n",
		       libcfs_nid2str(tn->tn_target_nid),
		       KFILND_RX_CONTEXT(tn->tn_target_addr), status);

	tn->tn_flags &= ~KFILND_TN_FLAG_TX_POSTED;
	tn->tn_status = status;

	if (tn->tn_status)
		event = TN_EVENT_FAIL;
	else
		event = TN_EVENT_RMA_OK;

	/*
	 * The status parameter has been sent to the transaction's state
	 * machine event.
	 */
	kfilnd_tn_event_handler(tn, event, true);
}

/**
 * kfilnd_tn_process_tx_event() - Process a transmit transaction event.
 */
static void kfilnd_tn_process_tx_event(void *ep_context, void *tn_context,
				       int status)
{
	struct kfilnd_transaction *tn = tn_context;
	enum tn_events event;

	if (status)
		CERROR("Send failed to %s: rx_ctx=%llu errno=%d\n",
		       libcfs_nid2str(tn->tn_target_nid),
		       KFILND_RX_CONTEXT(tn->tn_target_addr), status);

	tn->tn_flags &= ~KFILND_TN_FLAG_TX_POSTED;
	tn->tn_status = status;

	if (tn->tn_status)
		event = TN_EVENT_FAIL;
	else
		event = TN_EVENT_TX_OK;

	/*
	 * The status parameter has been sent to the transaction's state
	 * machine event.
	 */
	kfilnd_tn_event_handler(tn, event, true);
}

/**
 * kfilnd_tn_cq_error() - Process a completion queue error entry.
 */
void kfilnd_tn_cq_error(struct kfilnd_ep *ep, struct kfi_cq_err_entry *error)
{
	struct kfilnd_immediate_buffer *buf;

	if (error->err == ECANCELED || error->flags & KFI_MULTI_RECV) {
		buf = error->op_context;

		kfilnd_wkr_post(ep->end_cpt, kfilnd_tn_process_unlink_event,
				buf, NULL, 0);
	} else if (error->flags & (KFI_RMA | KFI_READ) ||
		   error->flags & (KFI_RMA | KFI_WRITE)) {
		kfilnd_wkr_post(ep->end_cpt, kfilnd_tn_process_rma_event, ep,
				error->op_context, -error->err);
	} else if (error->flags & (KFI_MSG | KFI_SEND)) {
		kfilnd_wkr_post(ep->end_cpt, kfilnd_tn_process_tx_event, ep,
				error->op_context, -error->err);
	} else {
		CERROR("Dropping error event: flags=%llx\n", error->flags);
	}
}

/**
 * kfilnd_tn_cq_event() - Process a completion queue event entry.
 */
void kfilnd_tn_cq_event(struct kfilnd_ep *ep, struct kfi_cq_data_entry *event)
{
	struct kfilnd_immediate_buffer *buf;

	if (event->flags & KFI_RECV) {
		buf = event->op_context;

		/* Increment buf ref count for this work */
		atomic_inc(&buf->immed_ref);
		kfilnd_wkr_post(ep->end_cpt, kfilnd_tn_process_rx_event, buf,
				event->buf, event->len);

		/* If the KFI_MULTI_RECV flag is set, the buffer was
		 * unlinked.
		 */
		if (event->flags & KFI_MULTI_RECV)
			kfilnd_wkr_post(ep->end_cpt,
					kfilnd_tn_process_unlink_event, buf,
					NULL, 0);
	} else if (event->flags & (KFI_RMA | KFI_READ) ||
		   event->flags & (KFI_RMA | KFI_WRITE)) {
		kfilnd_wkr_post(ep->end_cpt, kfilnd_tn_process_rma_event, ep,
				event->op_context, 0);
	} else if (event->flags & (KFI_MSG | KFI_SEND)) {
		kfilnd_wkr_post(ep->end_cpt, kfilnd_tn_process_tx_event, ep,
				event->op_context, 0);
	} else {
		CERROR("Unhandled CQ event: flags=%llx\n", event->flags);
	}
}

/**
 * kfilnd_tn_finalize() - Cleanup resources and finalize LNet operation.
 *
 * All state machine functions should call kfilnd_tn_finalize() instead of
 * kfilnd_tn_free(). Once all expected asynchronous events have been received,
 * if the transaction lock has not been released, it will now be released,
 * transaction resources cleaned up, and LNet finalized will be called.
 */
static void kfilnd_tn_finalize(struct kfilnd_transaction *tn, bool *tn_released)
{
	int rc;

	/* Can only finalize if all events have occurred. */
	if (atomic_read(&tn->async_event_count))
		return;

	if (!*tn_released) {
		mutex_unlock(&tn->tn_lock);
		*tn_released = true;
	}

	/* Free memory region before finalizing LNet operation and thus
	 * releasing the LNet buffer.
	 */
	if (tn->tn_mr)
		kfilnd_ep_dereg_mr(tn->tn_ep, tn);

	/* Release the reference on the multi-receive buffer. */
	if (tn->tn_posted_buf) {
		rc = kfilnd_ep_imm_buffer_put(tn->tn_ep, tn->tn_posted_buf);
		if (rc)
			CERROR("Failed to repost receive buffer: rc=%d\n", rc);
	}

	/* Finalize LNet operation. */
	if (tn->tn_lntmsg)
		lnet_finalize(tn->tn_lntmsg, tn->tn_status);

	if (tn->tn_getreply) {
		lnet_set_reply_msg_len(tn->tn_ep->end_dev->kfd_ni,
				       tn->tn_getreply, tn->tn_nob);
		lnet_finalize(tn->tn_getreply, tn->tn_status);
	}

	kfilnd_tn_free(tn);
}

/*  The following are the state machine routines for the transactions. */
static int kfilnd_tn_idle(struct kfilnd_transaction *tn, enum tn_events event,
			  bool *tn_released)
{
	struct kfilnd_msg *msg;
	int rc;

	switch (event) {
	case TN_EVENT_TX_OK:
		msg = tn->tn_tx_msg.msg;

		/* Lookup the peer's KFI address. */
		rc = kfilnd_dev_lookup_peer_address(tn->tn_ep->end_dev,
						    tn->tn_target_nid,
						    &tn->tn_target_addr);
		if (rc) {
			CERROR("Failed to lookup KFI address: rc=%d\n", rc);
			break;
		}

		/* If the transaction is an immediate message only, pack the
		 * message and post the buffer. If not an immediate message
		 * only, register a sink/source memory region. If memory
		 * registration is successfull, an async KFI event queue event
		 * will occur and the transaction will progress.
		 */
		if (tn->tn_flags & KFILND_TN_FLAG_IMMEDIATE) {
			kfilnd_tn_setup_immed(tn);
			kfilnd_tn_pack_msg(tn, kfilnd_tn_prefer_rx(tn));

			/* Post an immediate message with KFI LND header and
			 * entire LNet payload.
			 */
			tn->tn_state = TN_STATE_IMM_SEND;
			rc = kfilnd_ep_post_send(tn->tn_ep, tn);
			if (rc)
				CERROR("Failed to send to %s: rc=%d\n",
				       libcfs_nid2str(tn->tn_target_nid), rc);
		} else {
			tn->tn_state = TN_STATE_REG_MEM;
			rc = kfilnd_ep_reg_mr(tn->tn_ep, tn);
			if (rc)
				CERROR("Failed to register MR for %s: rc=%d\n",
				       libcfs_nid2str(tn->tn_target_nid), rc);

			/* If synchronous memory registration is used, post an
			 * immediate message with MR information so peer can
			 * perform an RMA operation.
			 */
			if (sync_mr_reg && !rc) {
				kfilnd_tn_pack_msg(tn, kfilnd_tn_prefer_rx(tn));
				tn->tn_state = TN_STATE_WAIT_COMP;

				/* Issue an extra increment for the receive
				 * event.
				 */
				atomic_inc(&tn->async_event_count);

				rc = kfilnd_ep_post_send(tn->tn_ep, tn);
				if (rc) {
					CERROR("Failed to send to %s: rc=%d\n",
					       libcfs_nid2str(tn->tn_target_nid),
					       rc);
					atomic_dec(&tn->async_event_count);
				}
			}
		}
		break;

	case TN_EVENT_RX_OK:
		msg = tn->tn_rx_msg.msg;

		/* Update the NID address with the new preferred RX context.
		 * Don't drop the message if this fails.
		 */
		rc = kfilnd_dev_update_peer_address(tn->tn_ep->end_dev,
						    msg->kfm_srcnid,
						    msg->kfm_prefer_rx);
		if (rc) {
			CWARN("Failed to update peer address %s: rc=%d\n",
			      libcfs_nid2str(msg->kfm_srcnid), rc);
			rc = 0;
		}

		/*
		 * Pass message up to LNet
		 * The TN will be reused in this call chain so we need to
		 * release the lock on the TN before proceeding.
		 */
		tn->tn_state = TN_STATE_IMM_RECV;
		mutex_unlock(&tn->tn_lock);
		*tn_released = true;
		if (msg->kfm_type == KFILND_MSG_IMMEDIATE)
			rc = lnet_parse(tn->tn_ep->end_dev->kfd_ni,
					&msg->kfm_u.immed.hdr, msg->kfm_srcnid,
					tn, 0);
		else
			rc = lnet_parse(tn->tn_ep->end_dev->kfd_ni,
					&msg->kfm_u.bulk_req.hdr,
					msg->kfm_srcnid, tn, 1);
		if (rc)
			CERROR("Failed to parse LNet message from %s: rc=%d\n",
			       libcfs_nid2str(msg->kfm_srcnid), rc);
		break;

	default:
		CERROR("Invalid event for idle state: event=%d\n", event);
		rc = -EINVAL;
	}

	if (rc) {
		tn->tn_status = rc;

		kfilnd_tn_finalize(tn, tn_released);
	}

	return rc;
}

static int kfilnd_tn_imm_send(struct kfilnd_transaction *tn,
			      enum tn_events event, bool *tn_released)
{
	int rc = 0;

	switch (event) {
	case TN_EVENT_FAIL:
		/* Remove LNet NID from hash if transaction fails. This would
		 * only get called if the KFI_SEND event has an error.
		 */
		kfilnd_dev_remove_peer_address(tn->tn_ep->end_dev,
					       tn->tn_target_nid);

		/* Fall through. */
	case TN_EVENT_TX_OK:
		break;

	default:
		CERROR("Invalid event for immediate send state: event=%d\n",
		       event);
		rc = -EINVAL;
	}

	/* Transaction is always finalized when an event occurs in the immediate
	 * send state.
	 */
	if (rc)
		tn->tn_status = rc;

	kfilnd_tn_finalize(tn, tn_released);

	return rc;
}

static int kfilnd_tn_imm_recv(struct kfilnd_transaction *tn,
			      enum tn_events event, bool *tn_released)
{
	int rc = 0;
	bool finalize_tn = false;

	switch (event) {
	case TN_EVENT_RMA_PREP:
		/* Release the buffer we received the request on. All relevant
		 * information to perform the RMA operation is stored in the
		 * transaction structure. This should be done before the RMA
		 * operation to prevent two contexts from potentially processing
		 * the same transaction.
		 */
		rc = kfilnd_ep_imm_buffer_put(tn->tn_ep, tn->tn_posted_buf);
		if (rc)
			CERROR("Failed to repost receive buffer: rc=%d\n", rc);
		else
			tn->tn_posted_buf = NULL;

		/* Lookup the peer's KFI address. */
		rc = kfilnd_dev_lookup_peer_address(tn->tn_ep->end_dev,
						    tn->tn_target_nid,
						    &tn->tn_target_addr);
		if (rc) {
			CERROR("Failed to lookup KFI address: rc=%d\n", rc);
		} else {
			/* Update the KFI address to use the response RX
			 * context.
			 */
			tn->tn_target_addr =
				kfi_rx_addr(KFILND_BASE_ADDR(tn->tn_target_addr),
					    tn->tn_response_rx,
					    KFILND_FAB_RX_CTX_BITS);

			/* Initiate the RMA operation to push/pull the LNet
			 * payload.
			 */
			tn->tn_state = TN_STATE_BULK_RMA;
			if (tn->tn_flags & KFILND_TN_FLAG_SINK)
				rc = kfilnd_ep_post_read(tn->tn_ep, tn);
			else
				rc = kfilnd_ep_post_write(tn->tn_ep, tn);
		}

		if (!rc)
			break;

		/* On any failure, fallthrough */
	case TN_EVENT_FAIL:
	case TN_EVENT_RX_OK:
		finalize_tn = true;
		break;

	default:
		CERROR("Invalid event for immediate receive state: event=%d\n",
		       event);
		rc = -EINVAL;
		finalize_tn = true;
	}

	if (finalize_tn) {
		if (rc)
			tn->tn_status = rc;

		kfilnd_tn_finalize(tn, tn_released);
	}

	return rc;
}

static int kfilnd_tn_reg_mem(struct kfilnd_transaction *tn,
			     enum tn_events event, bool *tn_released)
{
	int rc = 0;
	bool finalize_tn = false;

	switch (event) {
	case TN_EVENT_MR_OK:
		kfilnd_tn_pack_msg(tn, kfilnd_tn_prefer_rx(tn));

		/* Post an immediate message only with the KFI LND header. The
		 * peer will perform an RMA operation to push/pull the LNet
		 * payload.
		 */
		tn->tn_state = TN_STATE_WAIT_COMP;

		/* Issue an extra increment for the receive event. */
		atomic_inc(&tn->async_event_count);

		rc = kfilnd_ep_post_send(tn->tn_ep, tn);
		if (!rc) {
			atomic_dec(&tn->async_event_count);
			break;
		}

		/* Fall through on bad transaction status. */
	case TN_EVENT_FAIL:
		finalize_tn = true;
		break;

	default:
		CERROR("Invalid event for reg mem state: event=%d\n", event);
		rc = -EINVAL;
		finalize_tn = true;
	}

	if (finalize_tn) {
		if (rc)
			tn->tn_status = rc;

		kfilnd_tn_finalize(tn, tn_released);
	}

	return rc;
}

static int kfilnd_tn_wait_comp(struct kfilnd_transaction *tn,
			       enum tn_events event, bool *tn_released)
{
	switch (event) {
	case TN_EVENT_FAIL:
	case TN_EVENT_TX_OK:
	case TN_EVENT_RX_OK:
		break;

	default:
		CERROR("Invalid event for wait complete state: event=%d\n",
		       event);
	}

	kfilnd_tn_finalize(tn, tn_released);

	return 0;
}

static int kfilnd_tn_bulk_rma(struct kfilnd_transaction *tn,
			      enum tn_events event, bool *tn_released)
{
	int rc = 0;
	bool finalize_tn = false;
	struct kfilnd_msg *tx_msg = tn->tn_tx_msg.msg;

	switch (event) {
	case TN_EVENT_RMA_OK:
		/* Build the completion message to finalize the LNet operation.
		 */
		tx_msg->kfm_u.bulk_rsp.cookie = tn->tn_response_cookie;
		tx_msg->kfm_u.bulk_rsp.status = tn->tn_status;

		kfilnd_tn_pack_msg(tn, kfilnd_tn_prefer_rx(tn));

		/* Lookup the peer's KFI address. */
		rc = kfilnd_dev_lookup_peer_address(tn->tn_ep->end_dev,
						    tn->tn_target_nid,
						    &tn->tn_target_addr);
		if (rc) {
			CERROR("Failed to lookup KFI address: rc=%d\n", rc);
		} else {
			/* Update the KFI address to use the response RX
			 * context.
			 */
			tn->tn_target_addr =
				kfi_rx_addr(KFILND_BASE_ADDR(tn->tn_target_addr),
					    tn->tn_response_rx,
					    KFILND_FAB_RX_CTX_BITS);

			tn->tn_state = TN_STATE_WAIT_COMP;
			rc = kfilnd_ep_post_send(tn->tn_ep, tn);
		}

		if (rc)
			finalize_tn = true;
		break;

	case TN_EVENT_FAIL:
		/* Remove LNet NID from hash only if transaction fails. This
		 * would only get called if the KFI_RMA or KFI_REMOTE_RMA event
		 * has an error.
		 */
		kfilnd_dev_remove_peer_address(tn->tn_ep->end_dev,
					       tn->tn_target_nid);

		/* Fall through. */
		finalize_tn = true;
		break;

	default:
		CERROR("Invalid event for wait RMA state: event=%d\n", event);
		rc = -EINVAL;
		finalize_tn = true;
	}

	if (finalize_tn) {
		if (rc)
			tn->tn_status = rc;

		kfilnd_tn_finalize(tn, tn_released);
	}

	return 0;
}

/**
 * kfilnd_tn_event_handler() - Update transaction state machine with an event.
 * @tn: Transaction to be updated.
 * @event: Transaction event.
 * @dec_async_event_count: Decrement the transaction asynchronous event counter.
 * This should occur when network events happen for a transaction.
 *
 * When the transaction event handler is first called on a new transaction, the
 * transaction is now own by the transaction system. This means that will be
 * freed by the system as the transaction is progressed through the state
 * machine.
 */
void kfilnd_tn_event_handler(struct kfilnd_transaction *tn,
			     enum tn_events event, bool dec_async_event_count)
{
	int rc;
	bool tn_released = false;

	if (!tn)
		return;

	mutex_lock(&tn->tn_lock);

	if (dec_async_event_count)
		atomic_dec(&tn->async_event_count);

	switch (tn->tn_state) {
	case TN_STATE_IDLE:
		rc = kfilnd_tn_idle(tn, event, &tn_released);
		break;
	case TN_STATE_IMM_SEND:
		rc = kfilnd_tn_imm_send(tn, event, &tn_released);
		break;
	case TN_STATE_IMM_RECV:
		rc = kfilnd_tn_imm_recv(tn, event, &tn_released);
		break;
	case TN_STATE_REG_MEM:
		rc = kfilnd_tn_reg_mem(tn, event, &tn_released);
		break;
	case TN_STATE_WAIT_COMP:
		rc = kfilnd_tn_wait_comp(tn, event, &tn_released);
		break;
	case TN_STATE_BULK_RMA:
		rc = kfilnd_tn_bulk_rma(tn, event, &tn_released);
		break;
	default:
		CERROR("Transaction in bad state: %d\n", tn->tn_state);
	}

	if (!tn_released)
		mutex_unlock(&tn->tn_lock);
}

/**
 * kfilnd_tn_free() - Free a transaction.
 */
void kfilnd_tn_free(struct kfilnd_transaction *tn)
{
	spin_lock(&tn->tn_ep->tn_list_lock);
	list_del(&tn->tn_entry);
	spin_unlock(&tn->tn_ep->tn_list_lock);

	kfilnd_dom_put_mr_key(tn->tn_ep->end_dev->dom, tn->tn_mr_key);

	/* Free send message buffer if needed. */
	if (tn->tn_tx_msg.msg)
		LIBCFS_FREE(tn->tn_tx_msg.msg, KFILND_IMMEDIATE_MSG_SIZE);

	kmem_cache_free(tn_cache, tn);
}

/**
 * kfilnd_tn_alloc() - Allocate a new KFI LND transaction.
 * @dev: KFI LND device used to look the KFI LND endpoint to associate with the
 * transaction.
 * @cpt: CPT of the transaction.
 * @alloc_msg: Allocate an immediate message for the transaction.
 *
 * During transaction allocation, each transaction is associated with a KFI LND
 * endpoint use to post data transfer operations. The CPT argument is used to
 * lookup the KFI LND endpoint within the KFI LND device.
 *
 * Return: On success, valid pointer. Else, negative errno pointer.
 */
struct kfilnd_transaction *kfilnd_tn_alloc(struct kfilnd_dev *dev, int cpt,
					   bool alloc_msg)
{
	struct kfilnd_transaction *tn;
	struct kfilnd_ep *ep;
	int rc;

	if (!dev)
		goto err;

	/* If the CPT does not fall into the LNet NI CPT range, force the CPT
	 * into the LNet NI CPT range. This should never happen.
	 */
	ep = dev->cpt_to_endpoint[cpt];
	if (!ep) {
		CWARN("%s used invalid cpt=%d\n",
		      libcfs_nid2str(dev->kfd_ni->ni_nid), cpt);
		ep = dev->kfd_endpoints[0];
	}
	tn = kmem_cache_alloc(tn_cache, GFP_KERNEL);
	if (!tn)
		goto err;

	memset(tn, 0, sizeof(*tn));
	if (alloc_msg) {
		LIBCFS_CPT_ALLOC(tn->tn_tx_msg.msg, lnet_cpt_table(), cpt,
				 KFILND_IMMEDIATE_MSG_SIZE);
		if (!tn->tn_tx_msg.msg)
			goto err_free_tn;
	}

	mutex_init(&tn->tn_lock);

	rc = kfilnd_dom_get_mr_key(dev->dom);
	if (rc < 0)
		goto err_free_tn;

	tn->tn_mr_key = rc;
	tn->tn_ep = ep;
	tn->tn_response_rx = ep->end_context_id;

	/* Add the transaction to an endpoint.  This is like
	 * incrementing a ref counter.
	 */
	spin_lock(&ep->tn_list_lock);
	list_add_tail(&tn->tn_entry, &ep->tn_list);
	spin_unlock(&ep->tn_list_lock);
	return tn;

err_free_tn:
	if (tn->tn_tx_msg.msg)
		LIBCFS_FREE(tn->tn_tx_msg.msg, KFILND_IMMEDIATE_MSG_SIZE);
	kmem_cache_free(tn_cache, tn);
err:
	return NULL;
}

/**
 * kfilnd_tn_cleanup() - Cleanup KFI LND transaction system.
 *
 * This function should only be called when there are no outstanding
 * transactions.
 */
void kfilnd_tn_cleanup(void)
{
	kmem_cache_destroy(tn_cache);
	tn_cache = NULL;
}

/**
 * kfilnd_tn_init() - Initialize KFI LND transaction system.
 *
 * Return: On success, zero. Else, negative errno.
 */
int kfilnd_tn_init(void)
{
	if (WARN_ON_ONCE(tn_cache))
		return -EINVAL;
	tn_cache = kmem_cache_create("kfilnd_tn",
				     sizeof(struct kfilnd_transaction), 0,
				     SLAB_HWCACHE_ALIGN, NULL);
	if (!tn_cache)
		return -ENOMEM;
	return 0;
}
