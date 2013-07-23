/*
 * GPL HEADER START
 *
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 only,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License version 2 for more details (a copy is included
 * in the LICENSE file that accompanied this code).
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; If not, see
 * http://www.lustre.org/lustre/docs/GPLv2.pdf
 *
 * Please contact Xyratex Technology, Ltd., Langstone Road, Havant, Hampshire.
 * PO9 1SA, U.K. or visit www.xyratex.com if you need additional information or
 * have any questions.
 *
 * GPL HEADER END
 */
/*
 * Copyright (c) 2002, 2013, Xyratex Technology, Ltd . All rights reserved.
 * Use is subject to license terms.
 */
/*
 * Some portions of Lustre® software are subject to copyrights help by Intel Corp.
 * Copyright (c) 2011-2013 Intel Corporation, Inc.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre® and the Lustre logo are registered trademarks of
 * Xyratex Technology, Ltd  in the United States and/or other countries.
 */

#define DEBUG_SUBSYSTEM S_LOG

#ifndef EXPORT_SYMTAB
#define EXPORT_SYMTAB
#endif

#ifndef __KERNEL__
#include <liblustre.h>
#endif

#include <obd_class.h>
#include <lustre_log.h>
#include <libcfs/list.h>
#include "llog_internal.h"

/* helper functions for calling the llog obd methods */
static struct llog_ctxt* llog_new_ctxt(struct obd_device *obd)
{
        struct llog_ctxt *ctxt;

        OBD_ALLOC(ctxt, sizeof(*ctxt));
        if (!ctxt)
                return NULL;

        ctxt->loc_obd = obd;
        atomic_set(&ctxt->loc_refcount, 1);

        return ctxt;
}

static void llog_ctxt_destroy(struct llog_ctxt *ctxt)
{
        if (ctxt->loc_exp)
                class_export_put(ctxt->loc_exp);
        if (ctxt->loc_imp) {
                class_import_put(ctxt->loc_imp);
                ctxt->loc_imp = NULL;
        }
        LASSERT(ctxt->loc_llcd == NULL);
        OBD_FREE(ctxt, sizeof(*ctxt));
        return;
}

int __llog_ctxt_put(struct llog_ctxt *ctxt)
{
        struct obd_device *obd;
        int rc = 0;

        obd = ctxt->loc_obd;
        spin_lock(&obd->obd_dev_lock);
        if (!atomic_dec_and_test(&ctxt->loc_refcount)) {
                spin_unlock(&obd->obd_dev_lock);
                return rc;
        }
        obd->obd_llog_ctxt[ctxt->loc_idx] = NULL;
        spin_unlock(&obd->obd_dev_lock);

        LASSERTF(obd->obd_starting == 1 || 
                 obd->obd_stopping == 1 || obd->obd_set_up == 0,
                 "wrong obd state: %d/%d/%d\n", !!obd->obd_starting, 
                 !!obd->obd_stopping, !!obd->obd_set_up);

        /* cleanup the llog ctxt here */
        if (CTXTP(ctxt, cleanup))
                rc = CTXTP(ctxt, cleanup)(ctxt);
 
        llog_ctxt_destroy(ctxt);
        wake_up(&obd->obd_llog_waitq);
        return rc;
}
EXPORT_SYMBOL(__llog_ctxt_put);
 
int llog_cleanup(struct llog_ctxt *ctxt)
{
        struct l_wait_info lwi = LWI_INTR(LWI_ON_SIGNAL_NOOP, NULL);
        struct obd_device *obd;
        int rc, idx;
        ENTRY;

        if (!ctxt) {
                CERROR("No ctxt\n");
                RETURN(-ENODEV);
        }
        obd = ctxt->loc_obd;

        /*banlance the ctxt get when calling llog_cleanup */
        llog_ctxt_put(ctxt);

        /* sync with other llog ctxt user thread */
        spin_lock(&obd->obd_dev_lock);

        /* obd->obd_starting is needed for the case of cleanup
         * in error case while obd is starting up. */
        LASSERTF(obd->obd_starting == 1 || 
                 obd->obd_stopping == 1 || obd->obd_set_up == 0,
                 "wrong obd state: %d/%d/%d\n", !!obd->obd_starting, 
                 !!obd->obd_stopping, !!obd->obd_set_up);

        spin_unlock(&obd->obd_dev_lock);

        idx = ctxt->loc_idx;
        /*try to free the ctxt */
        rc = __llog_ctxt_put(ctxt);
        if (rc)
                CERROR("Error %d while cleaning up ctxt %p\n", 
                       rc, ctxt);

        l_wait_event(obd->obd_llog_waitq, llog_ctxt_null(obd, idx), &lwi);

        RETURN(rc);
}
EXPORT_SYMBOL(llog_cleanup);

int llog_setup(struct obd_device *obd, int index, struct obd_device *disk_obd,
               int count, struct llog_logid *logid, struct llog_operations *op)
{
        int rc = 0;
        struct llog_ctxt *ctxt, *old_ctxt;
        ENTRY;

        if (index < 0 || index >= LLOG_MAX_CTXTS)
                RETURN(-EFAULT);

        ctxt = llog_new_ctxt(obd);
        if (!ctxt)
                GOTO(out, rc = -ENOMEM);

        ctxt->loc_exp = class_export_get(disk_obd->obd_self_export);
        ctxt->loc_idx = index;
        ctxt->loc_logops = op;
        ctxt->loc_flags = LLOG_CTXT_FLAG_UNINITIALIZED;
        sema_init(&ctxt->loc_sem, 1);

        /* sync with other llog ctxt user thread */
        spin_lock(&obd->obd_dev_lock);
        old_ctxt = obd->obd_llog_ctxt[index];
        if (old_ctxt) {
                /* mds_lov_update_mds might call here multiple times. So if the
                   llog is already set up then don't to do it again. */
                CDEBUG(D_CONFIG, "obd %s ctxt %d already set up\n",
                       obd->obd_name, index);
                LASSERT(old_ctxt->loc_obd == obd);
                LASSERT(old_ctxt->loc_exp == disk_obd->obd_self_export);
                LASSERT(old_ctxt->loc_logops == op);
                spin_unlock(&obd->obd_dev_lock);

                llog_ctxt_destroy(ctxt);
                ctxt = old_ctxt;
                GOTO(out, rc = 0);
        }

        obd->obd_llog_ctxt[index] = ctxt;
        spin_unlock(&obd->obd_dev_lock);

        if (OBD_FAIL_CHECK(OBD_FAIL_OBD_LLOG_SETUP)) {
                rc = -EOPNOTSUPP;
        } else {
                if (op->lop_setup)
                        rc = op->lop_setup(obd, index, disk_obd, count, logid);
        }

        if (rc) {
                CERROR("obd %s ctxt %d lop_setup=%p failed %d\n",
                       obd->obd_name, index, op->lop_setup, rc);
                llog_ctxt_put(ctxt);
        } else {
                CDEBUG(D_CONFIG, "obd %s ctxt %d is initialized\n",
                       obd->obd_name, index);
                ctxt->loc_flags &= ~LLOG_CTXT_FLAG_UNINITIALIZED;
        }
out:
        RETURN(rc);
}
EXPORT_SYMBOL(llog_setup);

int llog_sync(struct llog_ctxt *ctxt, struct obd_export *exp)
{
        int rc = 0;
        ENTRY;

        if (!ctxt)
                RETURN(0);

        if (CTXTP(ctxt, sync))
                rc = CTXTP(ctxt, sync)(ctxt, exp);

        RETURN(rc);
}
EXPORT_SYMBOL(llog_sync);

int llog_add(struct llog_ctxt *ctxt, struct llog_rec_hdr *rec,
             struct lov_stripe_md *lsm, struct llog_cookie *logcookies,
             int numcookies)
{
        int raised, rc;
        ENTRY;

        if (!ctxt) {
                CERROR("No ctxt\n");
                RETURN(-ENODEV);
        }

        if (ctxt->loc_flags & LLOG_CTXT_FLAG_UNINITIALIZED)
                RETURN(-ENXIO);

        CTXT_CHECK_OP(ctxt, add, -EOPNOTSUPP);
        raised = cfs_cap_raised(CFS_CAP_SYS_RESOURCE);
        if (!raised)
                cfs_cap_raise(CFS_CAP_SYS_RESOURCE);
        rc = CTXTP(ctxt, add)(ctxt, rec, lsm, logcookies, numcookies);
        if (!raised)
                cfs_cap_lower(CFS_CAP_SYS_RESOURCE);
        RETURN(rc);
}
EXPORT_SYMBOL(llog_add);

int llog_cancel(struct llog_ctxt *ctxt, struct lov_stripe_md *lsm,
                int count, struct llog_cookie *cookies, int flags)
{
        int rc;
        ENTRY;

        if (!ctxt) {
                CERROR("No ctxt\n");
                RETURN(-ENODEV);
        }
        
        CTXT_CHECK_OP(ctxt, cancel, -EOPNOTSUPP);
        rc = CTXTP(ctxt, cancel)(ctxt, lsm, count, cookies, flags);
        RETURN(rc);
}
EXPORT_SYMBOL(llog_cancel);

/* callback func for llog_process in llog_obd_origin_setup */
static int cat_cancel_cb(struct llog_handle *cathandle,
                          struct llog_rec_hdr *rec, void *data)
{
        struct llog_logid_rec *lir = (struct llog_logid_rec *)rec;
        struct llog_handle *loghandle;
        struct llog_log_hdr *llh;
        int rc, index;
        ENTRY;

        if (rec->lrh_type != LLOG_LOGID_MAGIC) {
                CERROR("invalid record in catalog\n");
                RETURN(-EINVAL);
        }
        CDEBUG(D_HA, "processing log "LPX64":%x at index %u of catalog "
               LPX64"\n", lir->lid_id.lgl_oid, lir->lid_id.lgl_ogen,
               rec->lrh_index, cathandle->lgh_id.lgl_oid);

        rc = llog_cat_id2handle(cathandle, &loghandle, &lir->lid_id);
        if (rc) {
                CERROR("Cannot find handle for log "LPX64"\n",
                       lir->lid_id.lgl_oid);
                if (rc == -ENOENT) {
                        index = rec->lrh_index;
                        goto cat_cleanup;
                }
                RETURN(rc);
        }

        llh = loghandle->lgh_hdr;
        if ((llh->llh_flags & LLOG_F_ZAP_WHEN_EMPTY) &&
            (llh->llh_count == 1)) {
                rc = llog_destroy(loghandle);
                if (rc)
                        CERROR("failure destroying log in postsetup: %d\n", rc);

                index = loghandle->u.phd.phd_cookie.lgc_index;
                llog_free_handle(loghandle);

cat_cleanup:
                LASSERT(index);
                llog_cat_set_first_idx(cathandle, index);
                rc = llog_cancel_rec(cathandle, index);
                if (rc == 0)
                        CDEBUG(D_HA, "cancel log "LPX64":%x at index %u of "
                               "catalog "LPX64"\n", lir->lid_id.lgl_oid,
                               lir->lid_id.lgl_ogen, rec->lrh_index,
                               cathandle->lgh_id.lgl_oid);
        }

        RETURN(rc);
}

/* lop_setup method for filter/osc */
// XXX how to set exports
int llog_obd_origin_setup(struct obd_device *obd, int index,
                          struct obd_device *disk_obd, int count,
                          struct llog_logid *logid)
{
        struct llog_ctxt *ctxt;
        struct llog_handle *handle;
        struct lvfs_run_ctxt *saved = NULL;
        int rc;
        ENTRY;

        if (count == 0)
                RETURN(0);

        OBD_SLAB_ALLOC_PTR(saved, obd_lvfs_ctxt_cache);
        if (saved == NULL)
                RETURN(-ENOMEM);

        LASSERT(count == 1);

        ctxt = llog_get_context(obd, index);
        LASSERT(ctxt);
        llog_gen_init(ctxt);

        if (logid->lgl_oid)
                rc = llog_create(ctxt, &handle, logid, NULL);
        else {
                rc = llog_create(ctxt, &handle, NULL, NULL);
                if (!rc)
                        *logid = handle->lgh_id;
        }
        if (rc)
                GOTO(out, rc);

        ctxt->loc_handle = handle;
        push_ctxt(saved, &disk_obd->obd_lvfs_ctxt, NULL);
        rc = llog_init_handle(handle, LLOG_F_IS_CAT, NULL);
        pop_ctxt(saved, &disk_obd->obd_lvfs_ctxt, NULL);
        if (rc)
                GOTO(out, rc);

        rc = llog_process(handle, (llog_cb_t)cat_cancel_cb, NULL, NULL);
        if (rc)
                CERROR("llog_process with cat_cancel_cb failed: %d\n", rc);
out:
        llog_ctxt_put(ctxt);
        OBD_SLAB_FREE_PTR(saved, obd_lvfs_ctxt_cache);
        RETURN(rc);
}
EXPORT_SYMBOL(llog_obd_origin_setup);

int llog_obd_origin_cleanup(struct llog_ctxt *ctxt)
{
        struct llog_handle *cathandle, *n, *loghandle;
        struct llog_log_hdr *llh;
        int rc, index;
        ENTRY;

        if (!ctxt)
                RETURN(0);

        cathandle = ctxt->loc_handle;
        if (cathandle) {
                list_for_each_entry_safe(loghandle, n,
                                         &cathandle->u.chd.chd_head,
                                         u.phd.phd_entry) {
                        llh = loghandle->lgh_hdr;
                        if ((llh->llh_flags &
                                LLOG_F_ZAP_WHEN_EMPTY) &&
                            (llh->llh_count == 1)) {
                                rc = llog_destroy(loghandle);
                                if (rc)
                                        CERROR("failure destroying log during "
                                               "cleanup: %d\n", rc);

                                index = loghandle->u.phd.phd_cookie.lgc_index;
                                llog_free_handle(loghandle);

                                LASSERT(index);
                                llog_cat_set_first_idx(cathandle, index);
                                rc = llog_cancel_rec(cathandle, index);
                                if (rc == 0)
                                        CDEBUG(D_RPCTRACE, "cancel plain log at"
                                               "index %u of catalog "LPX64"\n",
                                               index,cathandle->lgh_id.lgl_oid);
                        }
                }
                llog_cat_put(ctxt->loc_handle);
        }
        RETURN(0);
}
EXPORT_SYMBOL(llog_obd_origin_cleanup);

/* add for obdfilter/sz and mds/unlink */
int llog_obd_origin_add(struct llog_ctxt *ctxt,
                        struct llog_rec_hdr *rec, struct lov_stripe_md *lsm,
                        struct llog_cookie *logcookies, int numcookies)
{
        struct llog_handle *cathandle;
        int rc;
        ENTRY;

        cathandle = ctxt->loc_handle;
        LASSERT(cathandle != NULL);
        rc = llog_cat_add_rec(cathandle, rec, logcookies, NULL);
        if (rc != 1)
                CERROR("write one catalog record failed: %d\n", rc);
        RETURN(rc);
}
EXPORT_SYMBOL(llog_obd_origin_add);

int obd_llog_init(struct obd_device *obd, struct obd_device *disk_obd,
                  int *index)
{
        int rc;
        ENTRY;
        OBD_CHECK_OP(obd, llog_init, 0);
        OBD_COUNTER_INCREMENT(obd, llog_init);

        rc = OBP(obd, llog_init)(obd, disk_obd, index);
        RETURN(rc);
}
EXPORT_SYMBOL(obd_llog_init);

int obd_llog_finish(struct obd_device *obd, int count)
{
        int rc;
        ENTRY;
        OBD_CHECK_OP(obd, llog_finish, 0);
        OBD_COUNTER_INCREMENT(obd, llog_finish);

        rc = OBP(obd, llog_finish)(obd, count);
        RETURN(rc);
}
EXPORT_SYMBOL(obd_llog_finish);
