/*
 * GPL HEADER START
 *
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 only,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License version 2 for more details.  A copy is
 * included in the COPYING file that accompanied this code.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 * GPL HEADER END
 */
/*
 * Copyright (c) 2011, 2012 Commissariat a l'energie atomique et aux energies
 *                          alternatives
 *
 * Copyright (c) 2013, 2014, Intel Corporation.
 * Use is subject to license terms.
 *
 * Copyright (c) 2016, Cray Inc. All rights reserved.
 */
/*
 * lustre/mdt/mdt_coordinator.c
 *
 * Lustre HSM Coordinator
 *
 * Author: Jacques-Charles Lafoucriere <jacques-charles.lafoucriere@cea.fr>
 * Author: Aurelien Degremont <aurelien.degremont@cea.fr>
 * Author: Thomas Leibovici <thomas.leibovici@cea.fr>
 */

#define DEBUG_SUBSYSTEM S_MDS

#include <obd_support.h>
#include <lustre_export.h>
#include <obd.h>
#include <lprocfs_status.h>
#include <lustre_log.h>
#include "mdt_internal.h"

static struct lprocfs_vars lprocfs_mdt_hsm_vars[];

/**
 * get obj and HSM attributes on a fid
 * \param mti [IN] context
 * \param fid [IN] object fid
 * \param hsm [OUT] HSM meta data
 * \retval obj or error (-ENOENT if not found)
 */
struct mdt_object *mdt_hsm_get_md_hsm(struct mdt_thread_info *mti,
				      const struct lu_fid *fid,
				      struct md_hsm *hsm)
{
	struct md_attr		*ma;
	struct mdt_object	*obj;
	int			 rc;
	ENTRY;

	ma = &mti->mti_attr;
	ma->ma_need = MA_HSM;
	ma->ma_valid = 0;

	/* find object by FID */
	obj = mdt_object_find(mti->mti_env, mti->mti_mdt, fid);
	if (IS_ERR(obj))
		RETURN(obj);

	if (!mdt_object_exists(obj)) {
		/* no more object */
		mdt_object_put(mti->mti_env, obj);
		RETURN(ERR_PTR(-ENOENT));
	}

	rc = mdt_attr_get_complex(mti, obj, ma);
	if (rc) {
		mdt_object_put(mti->mti_env, obj);
		RETURN(ERR_PTR(rc));
	}

	if (ma->ma_valid & MA_HSM)
		*hsm = ma->ma_hsm;
	else
		memset(hsm, 0, sizeof(*hsm));
	ma->ma_valid = 0;
	RETURN(obj);
}

void mdt_hsm_dump_hal(int level, const char *prefix,
		      struct hsm_action_list *hal)
{
	int			 i, sz;
	struct hsm_action_item	*hai;
	char			 buf[12];

	CDEBUG(level, "%s: HAL header: version %X count %d compound "LPX64
		      " archive_id %d flags "LPX64"\n",
	       prefix, hal->hal_version, hal->hal_count,
	       hal->hal_compound_id, hal->hal_archive_id, hal->hal_flags);

	hai = hai_first(hal);
	for (i = 0; i < hal->hal_count; i++) {
		sz = hai->hai_len - sizeof(*hai);
		CDEBUG(level, "%s %d: fid="DFID" dfid="DFID
		       " compound/cookie="LPX64"/"LPX64
		       " action=%s extent="LPX64"-"LPX64" gid="LPX64
		       " datalen=%d data=[%s]\n",
		       prefix, i,
		       PFID(&hai->hai_fid), PFID(&hai->hai_dfid),
		       hal->hal_compound_id, hai->hai_cookie,
		       hsm_copytool_action2name(hai->hai_action),
		       hai->hai_extent.offset,
		       hai->hai_extent.length,
		       hai->hai_gid, sz,
		       hai_dump_data_field(hai, buf, sizeof(buf)));
		hai = hai_next(hai);
	}
}

/**
 * data passed to llog_cat_process() callback
 * to scan requests and take actions
 */
struct hsm_scan_request {
	int			 hal_sz;
	int			 hal_used_sz;
	struct hsm_action_list	*hal;
};

struct hsm_scan_data {
	struct mdt_thread_info		*mti;
	char				 fs_name[MTI_NAME_MAXLEN+1];
	/* are we scanning the logs for housekeeping, or just looking
	 * for new work ? */
	bool				 housekeeping;
	/* request to be send to agents */
	int				 max_requests;	/** vector size */
	int				 request_cnt;	/** used count */
	struct hsm_scan_request		*request;
};

/**
 *  llog_cat_process() callback, used to:
 *  - find waiting request and start action
 *  - purge canceled and done requests
 * \param env [IN] environment
 * \param llh [IN] llog handle
 * \param hdr [IN] llog record
 * \param data [IN/OUT] cb data = struct hsm_scan_data
 * \retval 0 success
 * \retval -ve failure
 */
static int mdt_coordinator_cb(const struct lu_env *env,
			      struct llog_handle *llh,
			      struct llog_rec_hdr *hdr,
			      void *data)
{
	struct llog_agent_req_rec	*larr;
	struct hsm_scan_data		*hsd;
	struct hsm_action_item		*hai;
	struct mdt_device		*mdt;
	struct coordinator		*cdt;
	int				 rc;
	ENTRY;

	hsd = data;
	mdt = hsd->mti->mti_mdt;
	cdt = &mdt->mdt_coordinator;

	larr = (struct llog_agent_req_rec *)hdr;
	dump_llog_agent_req_rec("mdt_coordinator_cb(): ", larr);
	switch (larr->arr_status) {
	case ARS_WAITING: {
		int i;
		struct hsm_scan_request *request;

		/* Are agents full? */
		if (atomic_read(&cdt->cdt_request_count) >=
		    cdt->cdt_max_requests)
			break;

		/* first search whether the request is found in the
		 * list we have built. */
		request = NULL;
		for (i = 0; i < hsd->request_cnt; i++) {
			if (hsd->request[i].hal->hal_compound_id ==
			    larr->arr_compound_id) {
				request = &hsd->request[i];
				break;
			}
		}

		if (!request) {
			struct hsm_action_list *hal;

			if (hsd->request_cnt == hsd->max_requests) {
				if (!hsd->housekeeping) {
					/* The request array is full,
					 * stop here. There might be
					 * more known requests that
					 * could be merged, but this
					 * avoid analyzing too many
					 * llogs for minor gains. */
					RETURN(LLOG_PROC_BREAK);
				} else {
					/* Unknown request and no more room
					 * for a new request. Continue to scan
					 * to find other entries for already
					 * existing requests.
					 */
					RETURN(0);
				}
			}

			request = &hsd->request[hsd->request_cnt];

			/* allocates hai vector size just needs to be large
			 * enough */
			request->hal_sz =
				sizeof(*request->hal) +
				cfs_size_round(MTI_NAME_MAXLEN+1) +
				2 * cfs_size_round(larr->arr_hai.hai_len);
			OBD_ALLOC(hal, request->hal_sz);
			if (!hal) {
				CERROR("%s: Cannot allocate memory (%d o)"
				       "for compound "LPX64"\n",
				       mdt_obd_name(mdt),
				       request->hal_sz,
				       larr->arr_compound_id);
				RETURN(-ENOMEM);
			}
			hal->hal_version = HAL_VERSION;
			strlcpy(hal->hal_fsname, hsd->fs_name,
				MTI_NAME_MAXLEN + 1);
			hal->hal_compound_id = larr->arr_compound_id;
			hal->hal_archive_id = larr->arr_archive_id;
			hal->hal_flags = larr->arr_flags;
			hal->hal_count = 0;
			request->hal_used_sz = hal_size(hal);
			request->hal = hal;
			hsd->request_cnt++;
			hai = hai_first(hal);
		} else {
			/* request is known */
			/* we check if record archive num is the same as the
			 * known request, if not we will serve it in multiple
			 * time because we do not know if the agent can serve
			 * multiple backend
			 * a use case is a compound made of multiple restore
			 * where the files are not archived in the same backend
			 */
			if (larr->arr_archive_id !=
			    request->hal->hal_archive_id)
				RETURN(0);

			if (request->hal_sz <
			    request->hal_used_sz +
			    cfs_size_round(larr->arr_hai.hai_len)) {
				/* Not enough room, need an extension */
				void *hal_buffer;
				int sz;

				sz = 2 * request->hal_sz;
				OBD_ALLOC(hal_buffer, sz);
				if (!hal_buffer) {
					CERROR("%s: Cannot allocate memory "
					       "(%d o) for compound "LPX64"\n",
					       mdt_obd_name(mdt), sz,
					       larr->arr_compound_id);
					RETURN(-ENOMEM);
				}
				memcpy(hal_buffer, request->hal,
				       request->hal_used_sz);
				OBD_FREE(request->hal,
					 request->hal_sz);
				request->hal = hal_buffer;
				request->hal_sz = sz;
			}
			hai = hai_first(request->hal);
			for (i = 0; i < request->hal->hal_count; i++)
				hai = hai_next(hai);
		}
		memcpy(hai, &larr->arr_hai, larr->arr_hai.hai_len);
		hai->hai_cookie = larr->arr_hai.hai_cookie;
		hai->hai_gid = larr->arr_hai.hai_gid;

		request->hal_used_sz += cfs_size_round(hai->hai_len);
		request->hal->hal_count++;

		if (hai->hai_action != HSMA_CANCEL)
			cdt_agent_record_hash_add(cdt, hai->hai_cookie,
						  llh->lgh_hdr->llh_cat_idx,
						  hdr->lrh_index);
		break;
	}
	case ARS_STARTED: {
		struct hsm_progress_kernel pgs;
		struct cdt_agent_req *car;
		cfs_time_t now = cfs_time_current_sec();
		cfs_time_t last;

		if (!hsd->housekeeping)
			break;

		/* we search for a running request
		 * error may happen if coordinator crashes or stopped
		 * with running request
		 */
		car = mdt_cdt_find_request(cdt, larr->arr_hai.hai_cookie);
		if (car == NULL) {
			last = larr->arr_req_create;
		} else {
			last = car->car_req_update;
			mdt_cdt_put_request(car);
		}

		/* test if request too long, if yes cancel it
		 * the same way the copy tool acknowledge a cancel request */
		if (now <= last + cdt->cdt_active_req_timeout)
			RETURN(0);

		dump_llog_agent_req_rec("request timed out, start cleaning",
					larr);
		/* a too old cancel request just needs to be removed
		 * this can happen, if copy tool does not support
		 * cancel for other requests, we have to remove the
		 * running request and notify the copytool */
		pgs.hpk_fid = larr->arr_hai.hai_fid;
		pgs.hpk_cookie = larr->arr_hai.hai_cookie;
		pgs.hpk_extent = larr->arr_hai.hai_extent;
		pgs.hpk_flags = HP_FLAG_COMPLETED;
		pgs.hpk_errval = ENOSYS;
		pgs.hpk_data_version = 0;

		/* update request state, but do not record in llog, to
		 * avoid deadlock on cdt_llog_lock */
		rc = mdt_hsm_update_request_state(hsd->mti, &pgs, 0);
		if (rc)
			CERROR("%s: cannot cleanup timed out request: "
			       DFID" for cookie "LPX64" action=%s\n",
			       mdt_obd_name(mdt),
			       PFID(&pgs.hpk_fid), pgs.hpk_cookie,
			       hsm_copytool_action2name(
				       larr->arr_hai.hai_action));

		if (rc == -ENOENT) {
			/* The request no longer exists, forget
			 * about it, and do not send a cancel request
			 * to the client, for which an error will be
			 * sent back, leading to an endless cycle of
			 * cancellation. */
			cdt_agent_record_hash_del(cdt,
						  larr->arr_hai.hai_cookie);
			RETURN(LLOG_DEL_RECORD);
		}

		/* XXX A cancel request cannot be cancelled. */
		if (larr->arr_hai.hai_action == HSMA_CANCEL)
			RETURN(0);

		larr->arr_status = ARS_CANCELED;
		larr->arr_req_change = now;
		rc = llog_write(hsd->mti->mti_env, llh, hdr, hdr->lrh_index);
		if (rc < 0)
			CERROR("%s: cannot update agent log: rc = %d\n",
			       mdt_obd_name(mdt), rc);
		break;
	}
	case ARS_FAILED:
	case ARS_CANCELED:
	case ARS_SUCCEED:
		if (!hsd->housekeeping)
			break;

		if ((larr->arr_req_change + cdt->cdt_grace_delay) <
		    cfs_time_current_sec()) {
			cdt_agent_record_hash_del(cdt,
						  larr->arr_hai.hai_cookie);
			RETURN(LLOG_DEL_RECORD);
		}
		break;
	}
	RETURN(0);
}

/**
 * create /proc entries for coordinator
 * \param mdt [IN]
 * \retval 0 success
 * \retval -ve failure
 */
int hsm_cdt_procfs_init(struct mdt_device *mdt)
{
	struct coordinator	*cdt = &mdt->mdt_coordinator;
	int			 rc = 0;
	ENTRY;

	/* init /proc entries, failure is not critical */
	cdt->cdt_proc_dir = lprocfs_register("hsm",
					     mdt2obd_dev(mdt)->obd_proc_entry,
					     lprocfs_mdt_hsm_vars, mdt);
	if (IS_ERR(cdt->cdt_proc_dir)) {
		rc = PTR_ERR(cdt->cdt_proc_dir);
		CERROR("%s: Cannot create 'hsm' directory in mdt proc dir,"
		       " rc=%d\n", mdt_obd_name(mdt), rc);
		cdt->cdt_proc_dir = NULL;
		RETURN(rc);
	}

	RETURN(0);
}

/**
 * remove /proc entries for coordinator
 * \param mdt [IN]
 */
void  hsm_cdt_procfs_fini(struct mdt_device *mdt)
{
	struct coordinator	*cdt = &mdt->mdt_coordinator;

	LASSERT(cdt->cdt_state == CDT_STOPPED);
	if (cdt->cdt_proc_dir != NULL)
		lprocfs_remove(&cdt->cdt_proc_dir);
}

/**
 * get vector of hsm cdt /proc vars
 * \param none
 * \retval var vector
 */
struct lprocfs_vars *hsm_cdt_get_proc_vars(void)
{
	return lprocfs_mdt_hsm_vars;
}

/* Release the ressource used by the coordinator. Called when the
 * coordinator is stopping. */
static void mdt_hsm_cdt_cleanup(struct mdt_device *mdt)
{
	struct coordinator		*cdt = &mdt->mdt_coordinator;
	struct cdt_agent_req		*car, *tmp1;
	struct hsm_agent		*ha, *tmp2;
	struct cdt_restore_handle	*crh, *tmp3;
	struct mdt_thread_info		*cdt_mti;

	/* start cleaning */
	down_write(&cdt->cdt_request_lock);
	list_for_each_entry_safe(car, tmp1, &cdt->cdt_request_list,
				 car_request_list) {
		cfs_hash_del(cdt->cdt_request_cookie_hash,
			     &car->car_hai->hai_cookie,
			     &car->car_cookie_hash);
		list_del(&car->car_request_list);
		mdt_cdt_put_request(car);
	}
	up_write(&cdt->cdt_request_lock);

	down_write(&cdt->cdt_agent_lock);
	list_for_each_entry_safe(ha, tmp2, &cdt->cdt_agents, ha_list) {
		list_del(&ha->ha_list);
		OBD_FREE_PTR(ha);
	}
	up_write(&cdt->cdt_agent_lock);

	cdt_mti = lu_context_key_get(&cdt->cdt_env.le_ctx, &mdt_thread_key);
	mutex_lock(&cdt->cdt_restore_lock);
	list_for_each_entry_safe(crh, tmp3, &cdt->cdt_restore_hdl, crh_list) {
		struct mdt_object	*child;

		/* give back layout lock */
		child = mdt_object_find(&cdt->cdt_env, mdt, &crh->crh_fid);
		if (!IS_ERR(child))
			mdt_object_unlock_put(cdt_mti, child, &crh->crh_lh, 1);

		list_del(&crh->crh_list);

		OBD_SLAB_FREE_PTR(crh, mdt_hsm_cdt_kmem);
	}
	mutex_unlock(&cdt->cdt_restore_lock);

	mutex_lock(&cdt->cdt_deferred_hals_lock);
	mdt_hsm_free_deferred_archives(&cdt->cdt_deferred_hals);
	mutex_unlock(&cdt->cdt_deferred_hals_lock);
}

/*
 * Coordinator state transition table, indexed on enum cdt_states, taking
 * from and to states. For instance since CDT_INIT to CDT_RUNNING is a
 * valid transition, cdt_transition[CDT_INIT][CDT_RUNNING] is true.
 */
static bool cdt_transition[5][5] = {
	{ true, true, false, false, false },
	{ true, false, true, false, true },
	{ false, false, true, true, true },
	{ false, false, true, true, true },
	{ true, false, false, false, true }
};

/**
 * Change coordinator thread state
 * Some combinations are not valid, so catch them here.
 *
 * Returns the 0 on success, with old_state set if not NULL, or
 * -EINVAL if the transition was not possible.
 */

static int set_cdt_state(struct coordinator *cdt, enum cdt_states new_state,
			 enum cdt_states *old_state)
{
	int rc;
	enum cdt_states state;

	spin_lock(&cdt->cdt_state_lock);

	state = cdt->cdt_state;

	if (cdt_transition[state][new_state]) {
		cdt->cdt_state = new_state;
		spin_unlock(&cdt->cdt_state_lock);
		if (old_state)
			*old_state = state;
		rc = 0;
	} else {
		spin_unlock(&cdt->cdt_state_lock);
		CDEBUG(D_HSM,
		       "unexpected coordinator transition, from=%u, to=%u\n",
		       state, new_state);
		rc = -EINVAL;
	}

	return rc;
}

/**
 * coordinator thread
 * \param data [IN] obd device
 * \retval 0 success
 * \retval -ve failure
 */
static int mdt_coordinator(void *data)
{
	struct mdt_thread_info	*mti = data;
	struct mdt_device	*mdt = mti->mti_mdt;
	struct coordinator	*cdt = &mdt->mdt_coordinator;
	struct hsm_scan_data	 hsd = { NULL };
	const unsigned long	 wait_event_time = cfs_time_seconds(1);
	unsigned long		 next_loop_time = 0;
	int			 rc = 0;
	int			 request_sz;
	ENTRY;

	CDEBUG(D_HSM, "%s: coordinator thread starting, pid=%d\n",
	       mdt_obd_name(mdt), current_pid());

	/* we use a copy of cdt_max_requests in the cb, so if cdt_max_requests
	 * increases due to a change from /proc we do not overflow the
	 * hsd.request[] vector
	 */
	hsd.max_requests = cdt->cdt_max_requests;
	request_sz = hsd.max_requests * sizeof(*hsd.request);
	OBD_ALLOC(hsd.request, request_sz);
	if (!hsd.request) {
		set_cdt_state(cdt, CDT_STOPPED, NULL);
		RETURN(-ENOMEM);
	}

	hsd.mti = mti;
	obd_uuid2fsname(hsd.fs_name, mdt_obd_name(mdt), MTI_NAME_MAXLEN);

	set_cdt_state(cdt, CDT_RUNNING, NULL);

	/* Inform mdt_hsm_cdt_start(). */
	wake_up(&cdt->cdt_waitq);

	while (1) {
		struct l_wait_info lwi;
		int i;
		int updates_sz;
		int updates_cnt;
		struct hsm_record_update *updates;
		int update_idx = 0;

		/* Limit execution of the expensive requests traversal
		 * to at most every "wait_event_time" jiffies. But we
		 * also want to start or exit the coordinator as soon
		 * as it is signaled, so use an event with timeout. */
		lwi = LWI_TIMEOUT(wait_event_time, NULL, NULL);
		l_wait_event(cdt->cdt_waitq, kthread_should_stop(), &lwi);

		CDEBUG(D_HSM, "coordinator resumes\n");

		if (cdt->cdt_state == CDT_STOPPING) {
			rc = 0;
			break;
		}

		/* if coordinator is suspended continue to wait */
		if (cdt->cdt_state == CDT_DISABLE) {
			CDEBUG(D_HSM, "disable state, coordinator sleeps\n");
			continue;
		}

		mdt_hsm_process_deferred_archives(mti);

		/* If no event, and no housekeeping to do, continue to
		 * wait. */
		if (next_loop_time <= get_seconds()) {
			next_loop_time = get_seconds() +
				cdt->cdt_loop_period;
			hsd.housekeeping = true;
		} else if (cdt->cdt_event) {
			hsd.housekeeping = false;
		} else {
			continue;
		}

		cdt->cdt_event = false;

		CDEBUG(D_HSM, "coordinator starts reading llog\n");

		if (hsd.max_requests != cdt->cdt_max_requests) {
			/* cdt_max_requests has changed,
			 * we need to allocate a new buffer
			 */
			OBD_FREE(hsd.request, request_sz);
			hsd.max_requests = cdt->cdt_max_requests;
			request_sz = hsd.max_requests * sizeof(*hsd.request);
			OBD_ALLOC(hsd.request, request_sz);
			if (!hsd.request) {
				rc = -ENOMEM;
				break;
			}
		}

		hsd.request_cnt = 0;

		rc = cdt_llog_process(mti->mti_env, mdt, mdt_coordinator_cb,
				      &hsd, 0, 0, WRITE);
		if (rc < 0)
			goto clean_cb_alloc;

		CDEBUG(D_HSM, "found %d requests to send\n", hsd.request_cnt);

		if (list_empty(&cdt->cdt_agents)) {
			CDEBUG(D_HSM, "no agent available, "
				      "coordinator sleeps\n");
			goto clean_cb_alloc;
		}

		/* Compute how many HAI we have in all the requests */
		updates_cnt = 0;
		for (i = 0; i < hsd.request_cnt; i++) {
			const struct hsm_scan_request *request =
				&hsd.request[i];

			updates_cnt += request->hal->hal_count;
		}

		/* Allocate a temporary array to store the cookies to
		 * update, and their status. */
		updates_sz = updates_cnt * sizeof(*updates);
		OBD_ALLOC(updates, updates_sz);
		if (updates == NULL) {
			CERROR("%s: Cannot allocate memory (%d o) "
			       "for %d updates\n",
			       mdt_obd_name(mdt), updates_sz, updates_cnt);
			continue;
		}

		/* here hsd contains a list of requests to be started */
		for (i = 0; i < hsd.request_cnt; i++) {
			struct hsm_scan_request *request = &hsd.request[i];
			struct hsm_action_list	*hal = request->hal;
			struct hsm_action_item	*hai;
			int			 j;
			struct hsm_record_update *update = &updates[update_idx];

			/* still room for work ? */
			if (atomic_read(&cdt->cdt_request_count) >=
			    cdt->cdt_max_requests)
				break;

			if (hal == NULL)
				continue;

			/* found a request, we start it */
			rc = mdt_hsm_agent_send(mti, hal, 0, 0);

			/* if failure, we suppose it is temporary
			 * if the copy tool failed to do the request
			 * it has to use hsm_progress
			 */

			/* set up cookie vector to set records status
			 * after copy tools start or failed
			 */
			hai = hai_first(hal);
			for (j = 0; j < hal->hal_count; j++) {
				update->cookie = hai->hai_cookie;
				update->status =
					(rc ? ARS_WAITING : ARS_STARTED);
				hai = hai_next(hai);
			}

			update_idx++;
		}

		if (update_idx) {
			rc = mdt_agent_record_update(mti->mti_env, mdt,
						     updates, update_idx);
			if (rc)
				CERROR("%s: mdt_agent_record_update() failed, "
				       "rc=%d, cannot update records "
				       "for %d cookies\n",
				       mdt_obd_name(mdt), rc, update_idx);
		}

		OBD_FREE(updates, updates_sz);

clean_cb_alloc:
		/* free hal allocated by callback */
		for (i = 0; i < hsd.request_cnt; i++) {
			struct hsm_scan_request *request = &hsd.request[i];

			OBD_FREE(request->hal, request->hal_sz);
		}
	}

	set_cdt_state(cdt, CDT_STOPPING, NULL);

	if (hsd.request)
		OBD_FREE(hsd.request, request_sz);

	mdt_hsm_cdt_cleanup(mdt);

	if (rc != 0)
		CERROR("%s: coordinator thread exiting, process=%d, rc=%d\n",
		       mdt_obd_name(mdt), current_pid(), rc);
	else
		CDEBUG(D_HSM, "%s: coordinator thread exiting, process=%d,"
			      " no error\n",
		       mdt_obd_name(mdt), current_pid());

	RETURN(rc);
}

/**
 * lookup a restore handle by FID
 * caller needs to hold cdt_restore_lock
 * \param cdt [IN] coordinator
 * \param fid [IN] FID
 * \retval cdt_restore_handle found
 * \retval NULL not found
 */
struct cdt_restore_handle *mdt_hsm_restore_hdl_find(struct coordinator *cdt,
						       const struct lu_fid *fid)
{
	struct cdt_restore_handle	*crh;
	ENTRY;

	list_for_each_entry(crh, &cdt->cdt_restore_hdl, crh_list) {
		if (lu_fid_eq(&crh->crh_fid, fid))
			RETURN(crh);
	}
	RETURN(NULL);
}

/**
 * data passed to llog_cat_process() callback
 * to scan requests and take actions
 */
struct hsm_restore_data {
	struct mdt_thread_info	*hrd_mti;
};

/**
 *  llog_cat_process() callback, used to:
 *  - find restore request and allocate the restore handle
 * \param env [IN] environment
 * \param llh [IN] llog handle
 * \param hdr [IN] llog record
 * \param data [IN/OUT] cb data = struct hsm_restore_data
 * \retval 0 success
 * \retval -ve failure
 */
static int hsm_restore_cb(const struct lu_env *env,
			  struct llog_handle *llh,
			  struct llog_rec_hdr *hdr, void *data)
{
	struct llog_agent_req_rec	*larr;
	struct hsm_restore_data		*hrd;
	struct cdt_restore_handle	*crh;
	struct hsm_action_item		*hai;
	struct mdt_thread_info		*mti;
	struct coordinator		*cdt;
	struct mdt_object		*child;
	int rc;
	ENTRY;

	hrd = data;
	mti = hrd->hrd_mti;
	cdt = &mti->mti_mdt->mdt_coordinator;

	larr = (struct llog_agent_req_rec *)hdr;
	hai = &larr->arr_hai;
	if (hai->hai_cookie > cdt->cdt_last_cookie)
		/* update the cookie to avoid collision */
		cdt->cdt_last_cookie = hai->hai_cookie + 1;

	if (hai->hai_action != HSMA_RESTORE ||
	    agent_req_in_final_state(larr->arr_status))
		RETURN(0);

	/* restore request not in a final state */

	/* force replay of restore requests left in started state from previous
	 * CDT context, to be canceled later if finally found to be incompatible
	 * when being re-started */
	if (larr->arr_status == ARS_STARTED) {
		larr->arr_status = ARS_WAITING;
		larr->arr_req_change = cfs_time_current_sec();
		rc = llog_write(env, llh, hdr, hdr->lrh_index);
		if (rc != 0)
			GOTO(out, rc);
	}

	OBD_SLAB_ALLOC_PTR(crh, mdt_hsm_cdt_kmem);
	if (crh == NULL)
		RETURN(-ENOMEM);

	crh->crh_fid = hai->hai_fid;
	/* in V1 all file is restored
	crh->extent.start = hai->hai_extent.offset;
	crh->extent.end = hai->hai_extent.offset + hai->hai_extent.length;
	*/
	crh->crh_extent.start = 0;
	crh->crh_extent.end = hai->hai_extent.length;
	/* get the layout lock */
	mdt_lock_reg_init(&crh->crh_lh, LCK_EX);
	child = mdt_object_find_lock(mti, &crh->crh_fid, &crh->crh_lh,
				     MDS_INODELOCK_LAYOUT);
	if (IS_ERR(child))
		GOTO(out, rc = PTR_ERR(child));

	rc = 0;
	/* we choose to not keep a reference
	 * on the object during the restore time which can be very long */
	mdt_object_put(mti->mti_env, child);

	mutex_lock(&cdt->cdt_restore_lock);
	list_add_tail(&crh->crh_list, &cdt->cdt_restore_hdl);
	mutex_unlock(&cdt->cdt_restore_lock);

out:
	RETURN(rc);
}

/**
 * restore coordinator state at startup
 * the goal is to take a layout lock for each registered restore request
 * \param mti [IN] context
 */
static int mdt_hsm_pending_restore(struct mdt_thread_info *mti)
{
	struct hsm_restore_data	 hrd;
	int			 rc;
	ENTRY;

	hrd.hrd_mti = mti;

	rc = cdt_llog_process(mti->mti_env, mti->mti_mdt, hsm_restore_cb, &hrd,
			      0, 0, READ);

	RETURN(rc);
}

static int hsm_init_ucred(struct lu_ucred *uc)
{
	ENTRY;

	uc->uc_valid = UCRED_OLD;
	uc->uc_o_uid = 0;
	uc->uc_o_gid = 0;
	uc->uc_o_fsuid = 0;
	uc->uc_o_fsgid = 0;
	uc->uc_uid = 0;
	uc->uc_gid = 0;
	uc->uc_fsuid = 0;
	uc->uc_fsgid = 0;
	uc->uc_suppgids[0] = -1;
	uc->uc_suppgids[1] = -1;
	uc->uc_cap = CFS_CAP_FS_MASK;
	uc->uc_umask = 0777;
	uc->uc_ginfo = NULL;
	uc->uc_identity = NULL;

	RETURN(0);
}

/**
 * initialize coordinator struct
 * \param mdt [IN] device
 * \retval 0 success
 * \retval -ve failure
 */
int mdt_hsm_cdt_init(struct mdt_device *mdt)
{
	struct coordinator	*cdt = &mdt->mdt_coordinator;
	struct mdt_thread_info	*cdt_mti = NULL;
	int			 rc;
	ENTRY;

	set_cdt_state(cdt, CDT_STOPPED, NULL);

	init_waitqueue_head(&cdt->cdt_waitq);
	init_rwsem(&cdt->cdt_llog_lock);
	init_rwsem(&cdt->cdt_agent_lock);
	init_rwsem(&cdt->cdt_request_lock);
	mutex_init(&cdt->cdt_restore_lock);
	spin_lock_init(&cdt->cdt_state_lock);
	mutex_init(&cdt->cdt_deferred_hals_lock);

	INIT_LIST_HEAD(&cdt->cdt_request_list);
	INIT_LIST_HEAD(&cdt->cdt_agents);
	INIT_LIST_HEAD(&cdt->cdt_restore_hdl);
	INIT_LIST_HEAD(&cdt->cdt_deferred_hals);

	cdt->cdt_request_cookie_hash = cfs_hash_create("REQUEST_COOKIE_HASH",
						       CFS_HASH_BITS_MIN,
						       CFS_HASH_BITS_MAX,
						       CFS_HASH_BKT_BITS,
						       0 /* extra bytes */,
						       CFS_HASH_MIN_THETA,
						       CFS_HASH_MAX_THETA,
						&cdt_request_cookie_hash_ops,
						       CFS_HASH_DEFAULT);
	if (cdt->cdt_request_cookie_hash == NULL)
		RETURN(-ENOMEM);

	cdt->cdt_agent_record_hash = cfs_hash_create("AGENT_RECORD_HASH",
						     CFS_HASH_BITS_MIN,
						     CFS_HASH_BITS_MAX,
						     CFS_HASH_BKT_BITS,
						     0 /* extra bytes */,
						     CFS_HASH_MIN_THETA,
						     CFS_HASH_MAX_THETA,
						     &cdt_agent_record_hash_ops,
						     CFS_HASH_DEFAULT);
	if (cdt->cdt_agent_record_hash == NULL)
		GOTO(out_request_cookie_hash, rc = -ENOMEM);

	rc = lu_env_init(&cdt->cdt_env, LCT_MD_THREAD);
	if (rc < 0)
		GOTO(out_agent_record_hash, rc);

	/* for mdt_ucred(), lu_ucred stored in lu_ucred_key */
	rc = lu_context_init(&cdt->cdt_session, LCT_SERVER_SESSION);
	if (rc < 0)
		GOTO(out_env, rc);

	lu_context_enter(&cdt->cdt_session);
	cdt->cdt_env.le_ses = &cdt->cdt_session;

	cdt_mti = lu_context_key_get(&cdt->cdt_env.le_ctx, &mdt_thread_key);
	LASSERT(cdt_mti != NULL);

	cdt_mti->mti_env = &cdt->cdt_env;
	cdt_mti->mti_mdt = mdt;

	hsm_init_ucred(mdt_ucred(cdt_mti));

	/* default values for /proc tunnables
	 * can be override by MGS conf */
	cdt->cdt_default_archive_id = 1;
	cdt->cdt_grace_delay = 60;
	cdt->cdt_loop_period = 10;
	cdt->cdt_max_requests = 3;
	cdt->cdt_policy = CDT_DEFAULT_POLICY;
	cdt->cdt_active_req_timeout = 3600;

	RETURN(0);

out_env:
	lu_env_fini(&cdt->cdt_env);
out_agent_record_hash:
	cfs_hash_putref(cdt->cdt_agent_record_hash);
	cdt->cdt_agent_record_hash = NULL;
out_request_cookie_hash:
	cfs_hash_putref(cdt->cdt_request_cookie_hash);
	cdt->cdt_request_cookie_hash = NULL;

	return rc;
}

/**
 * free a coordinator thread
 * \param mdt [IN] device
 */
int  mdt_hsm_cdt_fini(struct mdt_device *mdt)
{
	struct coordinator *cdt = &mdt->mdt_coordinator;
	ENTRY;

	lu_context_exit(cdt->cdt_env.le_ses);
	lu_context_fini(cdt->cdt_env.le_ses);

	lu_env_fini(&cdt->cdt_env);

	cfs_hash_putref(cdt->cdt_agent_record_hash);
	cdt->cdt_agent_record_hash = NULL;

	cfs_hash_putref(cdt->cdt_request_cookie_hash);
	cdt->cdt_request_cookie_hash = NULL;

	RETURN(0);
}

/**
 * start a coordinator thread
 * \param mdt [IN] device
 * \retval 0 success
 * \retval -ve failure
 */
int mdt_hsm_cdt_start(struct mdt_device *mdt)
{
	struct coordinator	*cdt = &mdt->mdt_coordinator;
	int			 rc;
	void			*ptr;
	struct mdt_thread_info	*cdt_mti;
	struct task_struct	*task;
	ENTRY;

	/* functions defined but not yet used
	 * this avoid compilation warning
	 */
	ptr = dump_requests;

	rc = set_cdt_state(cdt, CDT_INIT, NULL);
	if (rc) {
		CERROR("%s: Coordinator already started or stopping\n",
		       mdt_obd_name(mdt));
		RETURN(-EALREADY);
	}

	CLASSERT(1 << (CDT_POLICY_SHIFT_COUNT - 1) == CDT_POLICY_LAST);
	cdt->cdt_policy = CDT_DEFAULT_POLICY;

	atomic_set(&cdt->cdt_compound_id, cfs_time_current_sec());
	/* just need to be larger than previous one */
	/* cdt_last_cookie is protected by cdt_llog_lock */
	cdt->cdt_last_cookie = cfs_time_current_sec();
	atomic_set(&cdt->cdt_request_count, 0);
	cdt->cdt_user_request_mask = (1UL << HSMA_RESTORE);
	cdt->cdt_group_request_mask = (1UL << HSMA_RESTORE);
	cdt->cdt_other_request_mask = (1UL << HSMA_RESTORE);

	/* to avoid deadlock when start is made through /proc
	 * /proc entries are created by the coordinator thread */

	/* set up list of started restore requests */
	cdt_mti = lu_context_key_get(&cdt->cdt_env.le_ctx, &mdt_thread_key);
	rc = mdt_hsm_pending_restore(cdt_mti);
	if (rc)
		CERROR("%s: cannot take the layout locks needed"
		       " for registered restore: %d\n",
		       mdt_obd_name(mdt), rc);

	task = kthread_run(mdt_coordinator, cdt_mti, "hsm_cdtr");
	if (IS_ERR(task)) {
		rc = PTR_ERR(task);
		set_cdt_state(cdt, CDT_STOPPED, NULL);
		CERROR("%s: error starting coordinator thread: %d\n",
		       mdt_obd_name(mdt), rc);
	} else {
		cdt->cdt_task = task;
		wait_event(cdt->cdt_waitq,
			   cdt->cdt_state != CDT_INIT);
		if (cdt->cdt_state == CDT_STOPPING) {
			CDEBUG(D_HSM,
			       "%s: coordinator thread failed to start\n",
			       mdt_obd_name(mdt));
			kthread_stop(cdt->cdt_task);
			cdt->cdt_task = NULL;
			set_cdt_state(cdt, CDT_STOPPED, NULL);
			rc = EINVAL;
		} else {
			CDEBUG(D_HSM, "%s: coordinator thread started\n",
			       mdt_obd_name(mdt));
			rc = 0;
		}
	}

	RETURN(rc);
}

/**
 * stop a coordinator thread
 * \param mdt [IN] device
 */
int mdt_hsm_cdt_stop(struct mdt_device *mdt)
{
	struct coordinator *cdt = &mdt->mdt_coordinator;
	int rc;

	ENTRY;

	/* stop coordinator thread */
	rc = set_cdt_state(cdt, CDT_STOPPING, NULL);
	if (rc == 0) {
		kthread_stop(cdt->cdt_task);
		cdt->cdt_task = NULL;
		set_cdt_state(cdt, CDT_STOPPED, NULL);
	}

	RETURN(rc);
}

/**
 * register all requests from an hal in the memory list
 * \param mti [IN] context
 * \param hal [IN] request
 * \param uuid [OUT] in case of CANCEL, the uuid of the agent
 *  which is running the CT
 * \retval 0 success
 * \retval -ve failure
 */
int mdt_hsm_add_hal(struct mdt_thread_info *mti,
		    struct hsm_action_list *hal, struct obd_uuid *uuid)
{
	struct mdt_device	*mdt = mti->mti_mdt;
	struct coordinator	*cdt = &mdt->mdt_coordinator;
	struct hsm_action_item	*hai;
	int			 rc = 0, i;
	ENTRY;

	/* register request in memory list */
	hai = hai_first(hal);
	for (i = 0; i < hal->hal_count; i++, hai = hai_next(hai)) {
		struct cdt_agent_req *car;

		/* in case of a cancel request, we first mark the ondisk
		 * record of the request we want to stop as canceled
		 * this does not change the cancel record
		 * it will be done when updating the request status
		 */
		if (hai->hai_action == HSMA_CANCEL) {
			struct hsm_record_update update = {
				.cookie = hai->hai_cookie,
				.status = ARS_CANCELED,
			};

			rc = mdt_agent_record_update(mti->mti_env, mti->mti_mdt,
						     &update, 1);
			if (rc) {
				CERROR("%s: mdt_agent_record_update() failed, "
				       "rc=%d, cannot update status to %s "
				       "for cookie "LPX64"\n",
				       mdt_obd_name(mdt), rc,
				       agent_req_status2name(ARS_CANCELED),
				       hai->hai_cookie);
				GOTO(out, rc);
			}

			/* find the running request to set it canceled */
			car = mdt_cdt_find_request(cdt, hai->hai_cookie);
			if (car != NULL) {
				car->car_canceled = 1;
				/* uuid has to be changed to the one running the
				* request to cancel */
				*uuid = car->car_uuid;
				mdt_cdt_put_request(car);
			}
			/* no need to memorize cancel request
			 * this also avoid a deadlock when we receive
			 * a purge all requests command
			 */
			continue;
		}

		if (hai->hai_action == HSMA_ARCHIVE) {
			struct mdt_object *obj;
			struct md_hsm hsm;

			obj = mdt_hsm_get_md_hsm(mti, &hai->hai_fid, &hsm);
			if (IS_ERR(obj))
				GOTO(out, rc = PTR_ERR(obj));

			hsm.mh_flags |= HS_EXISTS;
			hsm.mh_arch_id = hal->hal_archive_id;
			rc = mdt_hsm_attr_set(mti, obj, &hsm);
			mdt_object_put(mti->mti_env, obj);
			if (rc)
				GOTO(out, rc);
		}

		car = mdt_cdt_alloc_request(hal->hal_compound_id,
					    hal->hal_archive_id, hal->hal_flags,
					    uuid, hai);
		if (IS_ERR(car))
			GOTO(out, rc = PTR_ERR(car));

		rc = mdt_cdt_add_request(cdt, car);
		if (rc < 0) {
			mdt_cdt_free_request(car);
			GOTO(out, rc);
		}
	}
out:
	RETURN(rc);
}

/**
 * swap layouts between 2 fids
 * \param mti [IN] context
 * \param obj [IN]
 * \param dfid [IN]
 * \param mh_common [IN] MD HSM
 */
static int hsm_swap_layouts(struct mdt_thread_info *mti,
			    struct mdt_object *obj, const struct lu_fid *dfid,
			    struct md_hsm *mh_common)
{
	struct mdt_object	*dobj;
	struct mdt_lock_handle	*dlh;
	int			 rc;
	ENTRY;

	if (!mdt_object_exists(obj))
		GOTO(out, rc = -ENOENT);

	/* we already have layout lock on obj so take only
	 * on dfid */
	dlh = &mti->mti_lh[MDT_LH_OLD];
	mdt_lock_reg_init(dlh, LCK_EX);
	dobj = mdt_object_find_lock(mti, dfid, dlh, MDS_INODELOCK_LAYOUT);
	if (IS_ERR(dobj))
		GOTO(out, rc = PTR_ERR(dobj));

	/* if copy tool closes the volatile before sending the final
	 * progress through llapi_hsm_copy_end(), all the objects
	 * are removed and mdd_swap_layout LBUG */
	if (!mdt_object_exists(dobj)) {
		CERROR("%s: Copytool has closed volatile file "DFID"\n",
		       mdt_obd_name(mti->mti_mdt), PFID(dfid));
		GOTO(out_dobj, rc = -ENOENT);
	}
	/* Since we only handle restores here, unconditionally use
	 * SWAP_LAYOUTS_MDS_HSM flag to ensure original layout will
	 * be preserved in case of failure during swap_layout and not
	 * leave a file in an intermediate but incoherent state.
	 * But need to setup HSM xattr of data FID before, reuse
	 * mti and mh presets for FID in hsm_cdt_request_completed(),
	 * only need to clear RELEASED and DIRTY.
	 */
	mh_common->mh_flags &= ~(HS_RELEASED | HS_DIRTY);
	rc = mdt_hsm_attr_set(mti, dobj, mh_common);
	if (rc == 0)
		rc = mo_swap_layouts(mti->mti_env,
				     mdt_object_child(obj),
				     mdt_object_child(dobj),
				     SWAP_LAYOUTS_MDS_HSM);

out_dobj:
	mdt_object_unlock_put(mti, dobj, dlh, 1);
out:
	RETURN(rc);
}

/**
 * update status of a completed request
 * \param mti [IN] context
 * \param pgs [IN] progress of the copy tool
 * \param update_record [IN] update llog record
 * \retval 0 success
 * \retval -ve failure
 */
static int hsm_cdt_request_completed(struct mdt_thread_info *mti,
				struct hsm_progress_kernel *pgs,
				struct cdt_agent_req *car,
				enum agent_req_status *status)
{
	const struct lu_env	*env = mti->mti_env;
	struct mdt_device	*mdt = mti->mti_mdt;
	struct coordinator	*cdt = &mdt->mdt_coordinator;
	struct mdt_object	*obj = NULL;
	int			 cl_flags = 0, rc = 0;
	struct md_hsm		 mh;
	bool			 is_mh_changed;
	ENTRY;

	/* default is to retry */
	*status = ARS_WAITING;

	/* find object by FID */
	obj = mdt_hsm_get_md_hsm(mti, &car->car_hai->hai_fid, &mh);
	/* we will update MD HSM only if needed */
	is_mh_changed = false;
	if (IS_ERR(obj)) {
		/* object removed */
		*status = ARS_SUCCEED;
		goto unlock;
	}

	/* no need to change mh->mh_arch_id
	 * mdt_hsm_get_md_hsm() got it from disk and it is still valid
	 */
	if (pgs->hpk_errval != 0) {
		switch (pgs->hpk_errval) {
		case ENOSYS:
			/* the copy tool does not support cancel
			 * so the cancel request is failed
			 * As we cannot distinguish a cancel progress
			 * from another action progress (they have the
			 * same cookie), we suppose here the CT returns
			 * ENOSYS only if does not support cancel
			 */
			/* this can also happen when cdt calls it to
			 * for a timed out request */
			*status = ARS_FAILED;
			/* to have a cancel event in changelog */
			pgs->hpk_errval = ECANCELED;
			break;
		case ECANCELED:
			/* the request record has already been set to
			 * ARS_CANCELED, this set the cancel request
			 * to ARS_SUCCEED */
			*status = ARS_SUCCEED;
			break;
		default:
			*status = (cdt->cdt_policy & CDT_NORETRY_ACTION ||
				   !(pgs->hpk_flags & HP_FLAG_RETRY) ?
				   ARS_FAILED : ARS_WAITING);
			break;
		}

		if (pgs->hpk_errval > CLF_HSM_MAXERROR) {
			CERROR("%s: Request "LPX64" on "DFID
			       " failed, error code %d too large\n",
			       mdt_obd_name(mdt),
			       pgs->hpk_cookie, PFID(&pgs->hpk_fid),
			       pgs->hpk_errval);
			hsm_set_cl_error(&cl_flags,
					 CLF_HSM_ERROVERFLOW);
			rc = -EINVAL;
		} else {
			hsm_set_cl_error(&cl_flags, pgs->hpk_errval);
		}

		switch (car->car_hai->hai_action) {
		case HSMA_ARCHIVE:
			hsm_set_cl_event(&cl_flags, HE_ARCHIVE);
			break;
		case HSMA_RESTORE:
			hsm_set_cl_event(&cl_flags, HE_RESTORE);
			break;
		case HSMA_REMOVE:
			hsm_set_cl_event(&cl_flags, HE_REMOVE);
			break;
		case HSMA_CANCEL:
			hsm_set_cl_event(&cl_flags, HE_CANCEL);
			CERROR("%s: Failed request "LPX64" on "DFID
			       " cannot be a CANCEL\n",
			       mdt_obd_name(mdt),
			       pgs->hpk_cookie,
			       PFID(&pgs->hpk_fid));
			break;
		default:
			CERROR("%s: Failed request "LPX64" on "DFID
			       " %d is an unknown action\n",
			       mdt_obd_name(mdt),
			       pgs->hpk_cookie, PFID(&pgs->hpk_fid),
			       car->car_hai->hai_action);
			rc = -EINVAL;
			break;
		}
	} else {
		*status = ARS_SUCCEED;
		switch (car->car_hai->hai_action) {
		case HSMA_ARCHIVE:
			hsm_set_cl_event(&cl_flags, HE_ARCHIVE);
			/* set ARCHIVE keep EXIST and clear LOST and
			 * DIRTY */
			mh.mh_arch_ver = pgs->hpk_data_version;
			mh.mh_flags |= HS_ARCHIVED;
			mh.mh_flags &= ~(HS_LOST|HS_DIRTY);
			is_mh_changed = true;
			break;
		case HSMA_RESTORE:
			hsm_set_cl_event(&cl_flags, HE_RESTORE);

			/* do not clear RELEASED and DIRTY here
			 * this will occur in hsm_swap_layouts()
			 */

			/* Restoring has changed the file version on
			 * disk. */
			mh.mh_arch_ver = pgs->hpk_data_version;
			is_mh_changed = true;
			break;
		case HSMA_REMOVE:
			hsm_set_cl_event(&cl_flags, HE_REMOVE);
			/* clear ARCHIVED EXISTS and LOST */
			mh.mh_flags &= ~(HS_ARCHIVED | HS_EXISTS | HS_LOST);
			is_mh_changed = true;
			break;
		case HSMA_CANCEL:
			hsm_set_cl_event(&cl_flags, HE_CANCEL);
			CERROR("%s: Successful request "LPX64
			       " on "DFID
			       " cannot be a CANCEL\n",
			       mdt_obd_name(mdt),
			       pgs->hpk_cookie,
			       PFID(&pgs->hpk_fid));
			break;
		default:
			CERROR("%s: Successful request "LPX64
			       " on "DFID
			       " %d is an unknown action\n",
			       mdt_obd_name(mdt),
			       pgs->hpk_cookie, PFID(&pgs->hpk_fid),
			       car->car_hai->hai_action);
			rc = -EINVAL;
			break;
		}
	}

	/* rc != 0 means error when analysing action, it may come from
	 * a crasy CT no need to manage DIRTY
	 */
	if (rc == 0)
		hsm_set_cl_flags(&cl_flags,
				 mh.mh_flags & HS_DIRTY ? CLF_HSM_DIRTY : 0);

	/* unlock is done later, after layout lock management */
	if (is_mh_changed)
		rc = mdt_hsm_attr_set(mti, obj, &mh);

unlock:
	/* we give back layout lock only if restore was successful or
	 * if restore was canceled or if policy is to not retry
	 * in other cases we just unlock the object */
	if (car->car_hai->hai_action == HSMA_RESTORE &&
	    (pgs->hpk_errval == 0 || pgs->hpk_errval == ECANCELED ||
	     cdt->cdt_policy & CDT_NORETRY_ACTION)) {
		struct cdt_restore_handle	*crh;

		/* restore in data FID done, we swap the layouts
		 * only if restore is successfull */
		if (pgs->hpk_errval == 0 && !IS_ERR_OR_NULL(obj)) {
			rc = hsm_swap_layouts(mti, obj, &car->car_hai->hai_dfid,
					      &mh);
			if (rc) {
				if (cdt->cdt_policy & CDT_NORETRY_ACTION)
					*status = ARS_FAILED;
				pgs->hpk_errval = -rc;
			}
		}
		/* we have to retry, so keep layout lock */
		if (*status == ARS_WAITING)
			GOTO(out, rc);

		/* give back layout lock */
		mutex_lock(&cdt->cdt_restore_lock);
		crh = mdt_hsm_restore_hdl_find(cdt, &car->car_hai->hai_fid);
		if (crh != NULL)
			list_del(&crh->crh_list);
		mutex_unlock(&cdt->cdt_restore_lock);
		/* Just give back layout lock, we keep the reference
		 * which is given back later with the lock for HSM
		 * flags.
		 * XXX obj may be invalid so we do not pass it. */
		if (crh != NULL)
			mdt_object_unlock(mti, NULL, &crh->crh_lh, 1);

		if (crh != NULL)
			OBD_SLAB_FREE_PTR(crh, mdt_hsm_cdt_kmem);
	}

	GOTO(out, rc);

out:
	/* Unregister copytool(CT) process is waiting on hai_waitq.
	 * We should complete the hsm action running on a CT and
	 * then unregister the CT if there is no other CT running
	 * with same archive ID. This will make sure the process
	 * eg: md5sum on archived and release file will not be
	 * be stuck till time out*/
	car->car_progress.crp_status = 0;
	wake_up(&car->car_waitq);

	if (obj != NULL && !IS_ERR(obj)) {
		mo_changelog(env, CL_HSM, cl_flags,
			     mdt_object_child(obj));
		mdt_object_put(mti->mti_env, obj);
	}

	RETURN(rc);
}

/**
 * update status of a request
 * \param mti [IN] context
 * \param pgs [IN] progress of the copy tool
 * \param update_record [IN] update llog record
 * \retval 0 success
 * \retval -ve failure
 */
int mdt_hsm_update_request_state(struct mdt_thread_info *mti,
				 struct hsm_progress_kernel *pgs,
				 const int update_record)
{
	struct mdt_device	*mdt = mti->mti_mdt;
	struct coordinator	*cdt = &mdt->mdt_coordinator;
	struct cdt_agent_req	*car;
	int			 rc = 0;
	ENTRY;

	/* no coordinator started, so we cannot serve requests */
	if (cdt->cdt_state == CDT_STOPPED)
		RETURN(-EAGAIN);

	/* first do sanity checks */
	car = mdt_cdt_update_request(cdt, pgs);
	if (IS_ERR(car)) {
		CERROR("%s: Cannot find running request for cookie "LPX64
		       " on fid="DFID"\n",
		       mdt_obd_name(mdt),
		       pgs->hpk_cookie, PFID(&pgs->hpk_fid));

		RETURN(PTR_ERR(car));
	}

	CDEBUG(D_HSM, "Progress received for fid="DFID" cookie="LPX64
		      " action=%s flags=%d err=%d fid="DFID" dfid="DFID"\n",
		      PFID(&pgs->hpk_fid), pgs->hpk_cookie,
		      hsm_copytool_action2name(car->car_hai->hai_action),
		      pgs->hpk_flags, pgs->hpk_errval,
		      PFID(&car->car_hai->hai_fid),
		      PFID(&car->car_hai->hai_dfid));

	/* progress is done on FID or data FID depending of the action and
	 * of the copy progress */
	/* for restore progress is used to send back the data FID to cdt */
	if (car->car_hai->hai_action == HSMA_RESTORE &&
	    lu_fid_eq(&car->car_hai->hai_fid, &car->car_hai->hai_dfid))
		car->car_hai->hai_dfid = pgs->hpk_fid;

	if ((car->car_hai->hai_action == HSMA_RESTORE ||
	     car->car_hai->hai_action == HSMA_ARCHIVE) &&
	    (!lu_fid_eq(&pgs->hpk_fid, &car->car_hai->hai_dfid) &&
	     !lu_fid_eq(&pgs->hpk_fid, &car->car_hai->hai_fid))) {
		CERROR("%s: Progress on "DFID" for cookie "LPX64
		       " does not match request FID "DFID" nor data FID "
		       DFID"\n",
		       mdt_obd_name(mdt),
		       PFID(&pgs->hpk_fid), pgs->hpk_cookie,
		       PFID(&car->car_hai->hai_fid),
		       PFID(&car->car_hai->hai_dfid));
		GOTO(out, rc = -EINVAL);
	}

	if (pgs->hpk_errval != 0 && !(pgs->hpk_flags & HP_FLAG_COMPLETED)) {
		CERROR("%s: Progress on "DFID" for cookie "LPX64" action=%s"
		       " is not coherent (err=%d and not completed"
		       " (flags=%d))\n",
		       mdt_obd_name(mdt),
		       PFID(&pgs->hpk_fid), pgs->hpk_cookie,
		       hsm_copytool_action2name(car->car_hai->hai_action),
		       pgs->hpk_errval, pgs->hpk_flags);
		GOTO(out, rc = -EINVAL);
	}

	/* now progress is valid */

	/* we use a root like ucred */
	hsm_init_ucred(mdt_ucred(mti));

	if (pgs->hpk_flags & HP_FLAG_COMPLETED) {
		enum agent_req_status	 status;

		rc = hsm_cdt_request_completed(mti, pgs, car, &status);

		/* remove request from memory list */
		mdt_cdt_remove_request(cdt, pgs->hpk_cookie);

		CDEBUG(D_HSM, "Updating record: fid="DFID" cookie="LPX64
			      " action=%s status=%s\n", PFID(&pgs->hpk_fid),
		       pgs->hpk_cookie,
		       hsm_copytool_action2name(car->car_hai->hai_action),
		       agent_req_status2name(status));

		if (update_record) {
			int rc1;
			struct hsm_record_update update = {
				.cookie = pgs->hpk_cookie,
				.status = status,
			};

			rc1 = mdt_agent_record_update(mti->mti_env, mdt,
						      &update, 1);
			if (rc1)
				CERROR("%s: mdt_agent_record_update() failed,"
				       " rc=%d, cannot update status to %s"
				       " for cookie "LPX64"\n",
				       mdt_obd_name(mdt), rc1,
				       agent_req_status2name(status),
				       pgs->hpk_cookie);
			rc = (rc != 0 ? rc : rc1);
		}
		/* ct has completed a request, so a slot is available,
		 * signal the coordinator to find new work */
		mdt_hsm_cdt_event(cdt);
	} else {
		/* if copytool send a progress on a canceled request
		 * we inform copytool it should stop
		 */
		if (car->car_canceled == 1)
			rc = -ECANCELED;
	}
	GOTO(out, rc);

out:
	/* remove ref got from mdt_cdt_update_request() */
	mdt_cdt_put_request(car);

	return rc;
}


/**
 * data passed to llog_cat_process() callback
 * to cancel requests
 */
struct hsm_cancel_all_data {
	struct mdt_device	*mdt;
};

/**
 *  llog_cat_process() callback, used to:
 *  - purge all requests
 * \param env [IN] environment
 * \param llh [IN] llog handle
 * \param hdr [IN] llog record
 * \param data [IN] cb data = struct hsm_cancel_all_data
 * \retval 0 success
 * \retval -ve failure
 */
static int mdt_cancel_all_cb(const struct lu_env *env,
			     struct llog_handle *llh,
			     struct llog_rec_hdr *hdr, void *data)
{
	struct llog_agent_req_rec	*larr;
	struct hsm_cancel_all_data	*hcad;
	int				 rc = 0;
	ENTRY;

	larr = (struct llog_agent_req_rec *)hdr;
	hcad = data;
	if (larr->arr_status == ARS_WAITING ||
	    larr->arr_status == ARS_STARTED) {
		larr->arr_status = ARS_CANCELED;
		larr->arr_req_change = cfs_time_current_sec();
		rc = llog_write(env, llh, hdr, hdr->lrh_index);
	}

	RETURN(rc);
}

/**
 * cancel all actions
 * \param obd [IN] MDT device
 * \param cl_evicted = 1, client evicted, cancel the requests
 * \param agent_unregistered = 1, already un registered
 */
int hsm_cancel_all_actions(struct mdt_device *mdt,
	const struct obd_uuid *uuid, int cl_evicted, int agent_unregistered)
{
	struct lu_env			 env;
	struct lu_context		 session;
	struct mdt_thread_info		*mti;
	struct coordinator		*cdt = &mdt->mdt_coordinator;
	struct cdt_agent_req		*car;
	struct hsm_action_list		*hal = NULL;
	struct hsm_action_item		*hai;
	struct hsm_cancel_all_data	 hcad;
	int				 hal_sz = 0, hal_len, rc = 0;
	struct mdt_object		*obj = NULL;
	struct md_hsm			 mh;
	enum cdt_states			 old_state;

	ENTRY;

	rc = lu_env_init(&env, LCT_MD_THREAD);
	if (rc < 0)
		RETURN(rc);

	/* for mdt_ucred(), lu_ucred stored in lu_ucred_key */
	rc = lu_context_init(&session, LCT_SERVER_SESSION);
	if (rc < 0)
		GOTO(out_env, rc);

	lu_context_enter(&session);
	env.le_ses = &session;

	mti = lu_context_key_get(&env.le_ctx, &mdt_thread_key);
	LASSERT(mti != NULL);

	mti->mti_env = &env;
	mti->mti_mdt = mdt;

	hsm_init_ucred(mdt_ucred(mti));

	/* disable coordinator */
	rc = set_cdt_state(cdt, CDT_DISABLE, &old_state);
	if (rc)
		RETURN(rc);

	/* send cancel to all running requests */
	down_read(&cdt->cdt_request_lock);
	list_for_each_entry(car, &cdt->cdt_request_list, car_request_list) {
		mdt_cdt_get_request(car);
		/* request is not yet removed from list, it will be done
		 * when copytool will return progress
		 */

		if (uuid != NULL &&
			!obd_uuid_equals(&car->car_uuid, uuid))
				continue;

		if (car->car_hai->hai_action == HSMA_CANCEL) {
			mdt_cdt_put_request(car);
			continue;
		}

		/* needed size */
		hal_len = sizeof(*hal) + cfs_size_round(MTI_NAME_MAXLEN + 1) +
			  cfs_size_round(car->car_hai->hai_len);

		if (hal_len > hal_sz && hal_sz > 0) {
			/* not enough room, free old buffer */
			OBD_FREE(hal, hal_sz);
			hal = NULL;
		}

		/* empty buffer, allocate one */
		if (hal == NULL) {
			hal_sz = hal_len;
			OBD_ALLOC(hal, hal_sz);
			if (hal == NULL) {
				mdt_cdt_put_request(car);
				up_read(&cdt->cdt_request_lock);
				GOTO(out_cdt_state, rc = -ENOMEM);
			}
		}

		hal->hal_version = HAL_VERSION;
		obd_uuid2fsname(hal->hal_fsname, mdt_obd_name(mdt),
				MTI_NAME_MAXLEN);
		hal->hal_fsname[MTI_NAME_MAXLEN] = '\0';
		hal->hal_compound_id = car->car_compound_id;
		hal->hal_archive_id = car->car_archive_id;
		hal->hal_flags = car->car_flags;
		hal->hal_count = 0;

		hai = hai_first(hal);
		memcpy(hai, car->car_hai, car->car_hai->hai_len);
		hai->hai_action = HSMA_CANCEL;
		hal->hal_count = 1;

		/* Give back the lay out lock in case of below condition */
		if (cl_evicted &&
			car->car_hai->hai_action == HSMA_RESTORE) {

			struct cdt_restore_handle	*crh;

			/* find object by FID */
			obj = mdt_hsm_get_md_hsm(mti, &car->car_hai->hai_fid, &mh);
			if (IS_ERR(obj))
				/* object removed */
				goto out_cdt_state;

			mutex_lock(&cdt->cdt_restore_lock);
			crh = mdt_hsm_restore_hdl_find(cdt, &car->car_hai->hai_fid);
			if (crh != NULL)
				list_del(&crh->crh_list);
			mutex_unlock(&cdt->cdt_restore_lock);

			/* just give back layout lock, and put down
			 * put down the obj ref count at goto out;
			 */
			if (!IS_ERR(obj) && crh != NULL)
				mdt_object_unlock(mti, obj, &crh->crh_lh, 1);

			if (crh != NULL)
				OBD_SLAB_FREE_PTR(crh, mdt_hsm_cdt_kmem);
		}
		/* 1. it is possible to safely call mdt_hsm_agent_send()
		 * (ie without a deadlock on cdt_request_lock), because the
		 * write lock is taken only if we are not in purge mode
		 * (mdt_hsm_agent_send() does not call mdt_cdt_add_request()
		 *  nor mdt_cdt_remove_request())
		 *
		 * 2. no conflict with cdt thread because cdt is disable and we
		 * have the request lock
		 *
		 * 3. If it is called from unregister path then do not
		 * unregister again. This happens in case of a specific
		 * agent.
		 */
		rc = mdt_hsm_agent_send(mti, hal, 1, agent_unregistered);
		/* 1. Wait for the hsm operation to complete. Otherwise
		 * process waiting on it will get hung.
		 * ex: Operation "md5sum" on released file requires
		 * file to be restored.
		 *
		 * 2. Dont wait if client is evicted as there is no
		 * copytool exists to complete the hsm operation
		 *
		 * 3. rc < 0 is not considered in the below comparison
		 * as "HSMA_CANCEL" is not yet designed/coded. So
		 * even in case of rc == 0, cancel request would not
		 * be processed. Please do the reqd when HSMA_CANCEL
		 * is implemented
		 */
		if (uuid != NULL && !cl_evicted)
			l_wait_condition(car->car_waitq,
				car->car_progress.crp_status == 0);

		mdt_cdt_put_request(car);
	}
	up_read(&cdt->cdt_request_lock);

	if (hal != NULL)
		OBD_FREE(hal, hal_sz);

	/* cancel all on-disk records */
	hcad.mdt = mdt;

	rc = cdt_llog_process(mti->mti_env, mti->mti_mdt, mdt_cancel_all_cb,
			      &hcad, 0, 0, WRITE);
out_cdt_state:
	/* Put down the obj ref count, normally incase
	 * of evicted client and for restore operation */
	if (obj != NULL && !IS_ERR(obj))
		mdt_object_put(mti->mti_env, obj);

	/* Enable coordinator, unless the coordinator was stopping. */
	set_cdt_state(cdt, old_state, NULL);
	lu_context_exit(&session);
	lu_context_fini(&session);
out_env:
	lu_env_fini(&env);

	RETURN(rc);
}

/**
 * check if a request is comptaible with file status
 * \param hai [IN] request description
 * \param hal_an [IN] request archive number (not used)
 * \param rq_flags [IN] request flags
 * \param hsm [IN] file HSM metadata
 * \retval boolean
 */
bool mdt_hsm_is_action_compat(const struct hsm_action_item *hai,
			      const int hal_an, const __u64 rq_flags,
			      const struct md_hsm *hsm)
{
	int	 is_compat = false;
	int	 hsm_flags;
	ENTRY;

	hsm_flags = hsm->mh_flags;
	switch (hai->hai_action) {
	case HSMA_ARCHIVE:
		if (!(hsm_flags & HS_NOARCHIVE) &&
		    (hsm_flags & HS_DIRTY || !(hsm_flags & HS_ARCHIVED)))
			is_compat = true;
		break;
	case HSMA_RESTORE:
		if (!(hsm_flags & HS_DIRTY) && (hsm_flags & HS_RELEASED) &&
		    hsm_flags & HS_ARCHIVED && !(hsm_flags & HS_LOST))
			is_compat = true;
		break;
	case HSMA_REMOVE:
		if (!(hsm_flags & HS_RELEASED) &&
		    (hsm_flags & (HS_ARCHIVED | HS_EXISTS)))
			is_compat = true;
		break;
	case HSMA_CANCEL:
		is_compat = true;
		break;
	}
	CDEBUG(D_HSM, "fid="DFID" action=%s flags="LPX64
		      " extent="LPX64"-"LPX64" hsm_flags=%.8X %s\n",
		      PFID(&hai->hai_fid),
		      hsm_copytool_action2name(hai->hai_action), rq_flags,
		      hai->hai_extent.offset, hai->hai_extent.length,
		      hsm->mh_flags,
		      (is_compat ? "compatible" : "uncompatible"));

	RETURN(is_compat);
}

/*
 * /proc interface used to get/set HSM behaviour (cdt->cdt_policy)
 */
static const struct {
	__u64		 bit;
	char		*name;
	char		*nickname;
} hsm_policy_names[] = {
	{ CDT_NONBLOCKING_RESTORE,	"NonBlockingRestore",	"NBR"},
	{ CDT_NORETRY_ACTION,		"NoRetryAction",	"NRA"},
	{ 0 },
};

/**
 * convert a policy name to a bit
 * \param name [IN] policy name
 * \retval 0 unknown
 * \retval   policy bit
 */
static __u64 hsm_policy_str2bit(const char *name)
{
	int	 i;

	for (i = 0; hsm_policy_names[i].bit != 0; i++)
		if (strcmp(hsm_policy_names[i].nickname, name) == 0 ||
		    strcmp(hsm_policy_names[i].name, name) == 0)
			return hsm_policy_names[i].bit;
	return 0;
}

/**
 * convert a policy bit field to a string
 * \param mask [IN] policy bit field
 * \param hexa [IN] print mask before bit names
 * \param buffer [OUT] string
 * \param count [IN] size of buffer
 */
static void hsm_policy_bit2str(struct seq_file *m, const __u64 mask,
				const bool hexa)
{
	int	 i, j;
	__u64	 bit;
	ENTRY;

	if (hexa)
		seq_printf(m, "("LPX64") ", mask);

	for (i = 0; i < CDT_POLICY_SHIFT_COUNT; i++) {
		bit = (1ULL << i);

		for (j = 0; hsm_policy_names[j].bit != 0; j++) {
			if (hsm_policy_names[j].bit == bit)
				break;
		}
		if (bit & mask)
			seq_printf(m, "[%s] ", hsm_policy_names[j].name);
		else
			seq_printf(m, "%s ", hsm_policy_names[j].name);
	}
	/* remove last ' ' */
	m->count--;
	seq_putc(m, '\0');
}

/* methods to read/write HSM policy flags */
static int mdt_hsm_policy_seq_show(struct seq_file *m, void *data)
{
	struct mdt_device	*mdt = m->private;
	struct coordinator	*cdt = &mdt->mdt_coordinator;
	ENTRY;

	hsm_policy_bit2str(m, cdt->cdt_policy, false);
	RETURN(0);
}

static ssize_t
mdt_hsm_policy_seq_write(struct file *file, const char __user *buffer,
			 size_t count, loff_t *off)
{
	struct seq_file		*m = file->private_data;
	struct mdt_device	*mdt = m->private;
	struct coordinator	*cdt = &mdt->mdt_coordinator;
	char			*start, *token, sign;
	char			*buf;
	__u64			 policy;
	__u64			 add_mask, remove_mask, set_mask;
	int			 rc;
	ENTRY;

	if (count + 1 > PAGE_SIZE)
		RETURN(-EINVAL);

	OBD_ALLOC(buf, count + 1);
	if (buf == NULL)
		RETURN(-ENOMEM);

	if (copy_from_user(buf, buffer, count))
		GOTO(out, rc = -EFAULT);

	buf[count] = '\0';

	start = buf;
	CDEBUG(D_HSM, "%s: receive new policy: '%s'\n", mdt_obd_name(mdt),
	       start);

	add_mask = remove_mask = set_mask = 0;
	do {
		token = strsep(&start, "\n ");
		sign = *token;

		if (sign == '\0')
			continue;

		if (sign == '-' || sign == '+')
			token++;

		policy = hsm_policy_str2bit(token);
		if (policy == 0) {
			CWARN("%s: '%s' is unknown, "
			      "supported policies are:\n", mdt_obd_name(mdt),
			      token);
			hsm_policy_bit2str(m, 0, false);
			GOTO(out, rc = -EINVAL);
		}
		switch (sign) {
		case '-':
			remove_mask |= policy;
			break;
		case '+':
			add_mask |= policy;
			break;
		default:
			set_mask |= policy;
			break;
		}

	} while (start != NULL);

	CDEBUG(D_HSM, "%s: new policy: rm="LPX64" add="LPX64" set="LPX64"\n",
	       mdt_obd_name(mdt), remove_mask, add_mask, set_mask);

	/* if no sign in all string, it is a clear and set
	 * if some sign found, all unsigned are converted
	 * to add
	 * P1 P2 = set to P1 and P2
	 * P1 -P2 = add P1 clear P2 same as +P1 -P2
	 */
	if (remove_mask == 0 && add_mask == 0) {
		cdt->cdt_policy = set_mask;
	} else {
		cdt->cdt_policy |= set_mask | add_mask;
		cdt->cdt_policy &= ~remove_mask;
	}

	GOTO(out, rc = count);

out:
	OBD_FREE(buf, count + 1);
	RETURN(rc);
}
LPROC_SEQ_FOPS(mdt_hsm_policy);

#define GENERATE_PROC_METHOD(VAR)					\
static int mdt_hsm_##VAR##_seq_show(struct seq_file *m, void *data)	\
{									\
	struct mdt_device	*mdt = m->private;			\
	struct coordinator	*cdt = &mdt->mdt_coordinator;		\
	ENTRY;								\
									\
	seq_printf(m, LPU64"\n", (__u64)cdt->VAR);			\
	RETURN(0);							\
}									\
static ssize_t								\
mdt_hsm_##VAR##_seq_write(struct file *file, const char __user *buffer,	\
			  size_t count, loff_t *off)			\
									\
{									\
	struct seq_file		*m = file->private_data;		\
	struct mdt_device	*mdt = m->private;			\
	struct coordinator	*cdt = &mdt->mdt_coordinator;		\
	int			 val;					\
	int			 rc;					\
	ENTRY;								\
									\
	rc = lprocfs_write_helper(buffer, count, &val);			\
	if (rc)								\
		RETURN(rc);						\
	if (val > 0) {							\
		cdt->VAR = val;						\
		RETURN(count);						\
	}								\
	RETURN(-EINVAL);						\
}									\

GENERATE_PROC_METHOD(cdt_loop_period)
GENERATE_PROC_METHOD(cdt_grace_delay)
GENERATE_PROC_METHOD(cdt_active_req_timeout)
GENERATE_PROC_METHOD(cdt_max_requests)
GENERATE_PROC_METHOD(cdt_default_archive_id)

/*
 * procfs write method for MDT/hsm_control
 * proc entry is in mdt directory so data is mdt obd_device pointer
 */
#define CDT_ENABLE_CMD   "enabled"
#define CDT_STOP_CMD     "shutdown"
#define CDT_DISABLE_CMD  "disabled"
#define CDT_PURGE_CMD    "purge"
#define CDT_HELP_CMD     "help"
#define CDT_MAX_CMD_LEN  10

ssize_t
mdt_hsm_cdt_control_seq_write(struct file *file, const char __user *buffer,
			      size_t count, loff_t *off)
{
	struct seq_file		*m = file->private_data;
	struct obd_device	*obd = m->private;
	struct mdt_device	*mdt = mdt_dev(obd->obd_lu_dev);
	struct coordinator	*cdt = &(mdt->mdt_coordinator);
	int			 rc, usage = 0;
	char			 kernbuf[CDT_MAX_CMD_LEN];
	ENTRY;

	if (count == 0 || count >= sizeof(kernbuf))
		RETURN(-EINVAL);

	if (copy_from_user(kernbuf, buffer, count))
		RETURN(-EFAULT);
	kernbuf[count] = 0;

	if (kernbuf[count - 1] == '\n')
		kernbuf[count - 1] = 0;

	rc = 0;
	if (strcmp(kernbuf, CDT_ENABLE_CMD) == 0) {
		if (cdt->cdt_state == CDT_DISABLE) {
			rc = set_cdt_state(cdt, CDT_RUNNING, NULL);
			mdt_hsm_cdt_event(cdt);
			wake_up(&cdt->cdt_waitq);
		} else {
			if (mdt->mdt_bottom->dd_rdonly)
				rc = -EROFS;
			else
				rc = mdt_hsm_cdt_start(mdt);
		}
	} else if (strcmp(kernbuf, CDT_STOP_CMD) == 0) {
		if ((cdt->cdt_state == CDT_STOPPING) ||
		    (cdt->cdt_state == CDT_STOPPED)) {
			CERROR("%s: Coordinator already stopped\n",
			       mdt_obd_name(mdt));
			rc = -EALREADY;
		} else {
			rc = mdt_hsm_cdt_stop(mdt);
		}
	} else if (strcmp(kernbuf, CDT_DISABLE_CMD) == 0) {
		if ((cdt->cdt_state == CDT_STOPPING) ||
		    (cdt->cdt_state == CDT_STOPPED)) {
			CERROR("%s: Coordinator is stopped\n",
			       mdt_obd_name(mdt));
			rc = -EINVAL;
		} else {
			rc = set_cdt_state(cdt, CDT_DISABLE, NULL);
		}
	} else if (strcmp(kernbuf, CDT_PURGE_CMD) == 0) {
	/* 3rd arg = 0 indicates client is not evicted
	 * 4th arg = 0 indicates CT agent yet to be unregistered
	 */
		rc = hsm_cancel_all_actions(mdt, NULL, 0, 0);
	} else if (strcmp(kernbuf, CDT_HELP_CMD) == 0) {
		usage = 1;
	} else {
		usage = 1;
		rc = -EINVAL;
	}

	if (usage == 1)
		CERROR("%s: Valid coordinator control commands are: "
		       "%s %s %s %s %s\n", mdt_obd_name(mdt),
		       CDT_ENABLE_CMD, CDT_STOP_CMD, CDT_DISABLE_CMD,
		       CDT_PURGE_CMD, CDT_HELP_CMD);

	if (rc)
		RETURN(rc);

	RETURN(count);
}

int mdt_hsm_cdt_control_seq_show(struct seq_file *m, void *data)
{
	struct obd_device	*obd = m->private;
	struct coordinator	*cdt;
	ENTRY;

	cdt = &(mdt_dev(obd->obd_lu_dev)->mdt_coordinator);

	if (cdt->cdt_state == CDT_INIT)
		seq_printf(m, "init\n");
	else if (cdt->cdt_state == CDT_RUNNING)
		seq_printf(m, "enabled\n");
	else if (cdt->cdt_state == CDT_STOPPING)
		seq_printf(m, "stopping\n");
	else if (cdt->cdt_state == CDT_STOPPED)
		seq_printf(m, "stopped\n");
	else if (cdt->cdt_state == CDT_DISABLE)
		seq_printf(m, "disabled\n");
	else
		seq_printf(m, "unknown\n");

	RETURN(0);
}

static int
mdt_hsm_request_mask_show(struct seq_file *m, __u64 mask)
{
	bool first = true;
	int i;
	ENTRY;

	for (i = 0; i < 8 * sizeof(mask); i++) {
		if (mask & (1UL << i)) {
			seq_printf(m, "%s%s", first ? "" : " ",
				   hsm_copytool_action2name(i));
			first = false;
		}
	}
	seq_putc(m, '\n');

	RETURN(0);
}

static int
mdt_hsm_user_request_mask_seq_show(struct seq_file *m, void *data)
{
	struct mdt_device *mdt = m->private;
	struct coordinator *cdt = &mdt->mdt_coordinator;

	return mdt_hsm_request_mask_show(m, cdt->cdt_user_request_mask);
}

static int
mdt_hsm_group_request_mask_seq_show(struct seq_file *m, void *data)
{
	struct mdt_device *mdt = m->private;
	struct coordinator *cdt = &mdt->mdt_coordinator;

	return mdt_hsm_request_mask_show(m, cdt->cdt_group_request_mask);
}

static int
mdt_hsm_other_request_mask_seq_show(struct seq_file *m, void *data)
{
	struct mdt_device *mdt = m->private;
	struct coordinator *cdt = &mdt->mdt_coordinator;

	return mdt_hsm_request_mask_show(m, cdt->cdt_other_request_mask);
}

static inline enum hsm_copytool_action
hsm_copytool_name2action(const char *name)
{
	if (strcasecmp(name, "NOOP") == 0)
		return HSMA_NONE;
	else if (strcasecmp(name, "ARCHIVE") == 0)
		return HSMA_ARCHIVE;
	else if (strcasecmp(name, "RESTORE") == 0)
		return HSMA_RESTORE;
	else if (strcasecmp(name, "REMOVE") == 0)
		return HSMA_REMOVE;
	else if (strcasecmp(name, "CANCEL") == 0)
		return HSMA_CANCEL;
	else
		return -1;
}

static ssize_t
mdt_write_hsm_request_mask(struct file *file, const char __user *user_buf,
			    size_t user_count, __u64 *mask)
{
	char *buf, *pos, *name;
	size_t buf_size;
	__u64 new_mask = 0;
	int rc;
	ENTRY;

	if (!(user_count < 4096))
		RETURN(-ENOMEM);

	buf_size = user_count + 1;

	OBD_ALLOC(buf, buf_size);
	if (buf == NULL)
		RETURN(-ENOMEM);

	if (copy_from_user(buf, user_buf, buf_size - 1))
		GOTO(out, rc = -EFAULT);

	buf[buf_size - 1] = '\0';

	pos = buf;
	while ((name = strsep(&pos, " \t\v\n")) != NULL) {
		int action;

		if (*name == '\0')
			continue;

		action = hsm_copytool_name2action(name);
		if (action < 0)
			GOTO(out, rc = -EINVAL);

		new_mask |= (1UL << action);
	}

	*mask = new_mask;
	rc = user_count;
out:
	OBD_FREE(buf, buf_size);

	RETURN(rc);
}

static ssize_t
mdt_hsm_user_request_mask_seq_write(struct file *file, const char __user *buf,
					size_t count, loff_t *off)
{
	struct seq_file		*m = file->private_data;
	struct mdt_device	*mdt = m->private;
	struct coordinator *cdt = &mdt->mdt_coordinator;

	return mdt_write_hsm_request_mask(file, buf, count,
					   &cdt->cdt_user_request_mask);
}

static ssize_t
mdt_hsm_group_request_mask_seq_write(struct file *file, const char __user *buf,
					size_t count, loff_t *off)
{
	struct seq_file		*m = file->private_data;
	struct mdt_device	*mdt = m->private;
	struct coordinator	*cdt = &mdt->mdt_coordinator;

	return mdt_write_hsm_request_mask(file, buf, count,
					   &cdt->cdt_group_request_mask);
}

static ssize_t
mdt_hsm_other_request_mask_seq_write(struct file *file, const char __user *buf,
					size_t count, loff_t *off)
{
	struct seq_file		*m = file->private_data;
	struct mdt_device	*mdt = m->private;
	struct coordinator	*cdt = &mdt->mdt_coordinator;

	return mdt_write_hsm_request_mask(file, buf, count,
					   &cdt->cdt_other_request_mask);
}

LPROC_SEQ_FOPS(mdt_hsm_cdt_loop_period);
LPROC_SEQ_FOPS(mdt_hsm_cdt_grace_delay);
LPROC_SEQ_FOPS(mdt_hsm_cdt_active_req_timeout);
LPROC_SEQ_FOPS(mdt_hsm_cdt_max_requests);
LPROC_SEQ_FOPS(mdt_hsm_cdt_default_archive_id);
LPROC_SEQ_FOPS(mdt_hsm_user_request_mask);
LPROC_SEQ_FOPS(mdt_hsm_group_request_mask);
LPROC_SEQ_FOPS(mdt_hsm_other_request_mask);

static struct lprocfs_vars lprocfs_mdt_hsm_vars[] = {
	{ .name	=	"agents",
	  .fops	=	&mdt_hsm_agent_fops			},
	{ .name	=	"actions",
	  .fops	=	&mdt_hsm_actions_fops,
	  .proc_mode =	0444					},
	{ .name	=	"default_archive_id",
	  .fops	=	&mdt_hsm_cdt_default_archive_id_fops	},
	{ .name	=	"grace_delay",
	  .fops	=	&mdt_hsm_cdt_grace_delay_fops		},
	{ .name	=	"loop_period",
	  .fops	=	&mdt_hsm_cdt_loop_period_fops		},
	{ .name	=	"max_requests",
	  .fops	=	&mdt_hsm_cdt_max_requests_fops		},
	{ .name	=	"policy",
	  .fops	=	&mdt_hsm_policy_fops			},
	{ .name	=	"active_request_timeout",
	  .fops	=	&mdt_hsm_cdt_active_req_timeout_fops	},
	{ .name	=	"active_requests",
	  .fops	=	&mdt_hsm_active_requests_fops		},
	{ .name	=	"user_request_mask",
	  .fops	=	&mdt_hsm_user_request_mask_fops,	},
	{ .name	=	"group_request_mask",
	  .fops	=	&mdt_hsm_group_request_mask_fops,	},
	{ .name	=	"other_request_mask",
	  .fops	=	&mdt_hsm_other_request_mask_fops,	},
	{ 0 }
};
