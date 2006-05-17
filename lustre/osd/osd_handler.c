/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  lustre/osd/osd_handler.c
 *  Top-level entry points into osd module
 *
 *  Copyright (c) 2006 Cluster File Systems, Inc.
 *   Author: Nikita Danilov <nikita@clusterfs.com>
 *
 *   This file is part of the Lustre file system, http://www.lustre.org
 *   Lustre is a trademark of Cluster File Systems, Inc.
 *
 *   You may have signed or agreed to another license before downloading
 *   this software.  If so, you are bound by the terms and conditions
 *   of that agreement, and the following does not apply to you.  See the
 *   LICENSE file included with this distribution for more information.
 *
 *   If you did not agree to a different license, then this copy of Lustre
 *   is open source software; you can redistribute it and/or modify it
 *   under the terms of version 2 of the GNU General Public License as
 *   published by the Free Software Foundation.
 *
 *   In either case, Lustre is distributed in the hope that it will be
 *   useful, but WITHOUT ANY WARRANTY; without even the implied warranty
 *   of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   license text for more details.
 */

#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif
#define DEBUG_SUBSYSTEM S_MDS

#include <linux/module.h>

/* LUSTRE_VERSION_CODE */
#include <linux/lustre_ver.h>
/*
 * XXX temporary stuff: direct access to ldiskfs/jdb. Interface between osd
 * and file system is not yet specified.
 */
/* handle_t, journal_start(), journal_stop() */
#include <linux/jbd.h>
/* LDISKFS_SB() */
#include <linux/ldiskfs_fs.h>
/* simple_mkdir() */
#include <linux/lvfs.h>

/*
 * struct OBD_{ALLOC,FREE}*()
 * OBD_FAIL_CHECK
 */
#include <linux/obd_support.h>
/* struct ptlrpc_thread */
#include <linux/lustre_net.h>
/* LUSTRE_OSD0_NAME */
#include <linux/obd.h>
/* class_register_type(), class_unregister_type(), class_get_type() */
#include <linux/obd_class.h>
#include <linux/lustre_disk.h>

/* fid_is_local() */
#include <linux/lustre_fid.h>
#include <linux/lustre_iam.h>

#include "osd_internal.h"

static int   osd_root_get      (struct lu_context *ctxt,
                                struct dt_device *dev, struct lu_fid *f);
static int   osd_statfs        (struct lu_context *ctxt,
                                struct dt_device *dev, struct kstatfs *sfs);

static int   lu_device_is_osd  (const struct lu_device *d);
static void  osd_mod_exit      (void) __exit;
static int   osd_mod_init      (void) __init;
static int   osd_type_init     (struct lu_device_type *t);
static void  osd_type_fini     (struct lu_device_type *t);
static int   osd_object_init   (struct lu_context *ctxt, struct lu_object *l);
static void  osd_object_release(struct lu_context *ctxt, struct lu_object *l);
static int   osd_object_exists (struct lu_context *ctx, struct lu_object *o);
static int   osd_object_print  (struct lu_context *ctx,
                                struct seq_file *f, const struct lu_object *o);
static void  osd_device_free   (struct lu_context *ctx, struct lu_device *m);
static void *osd_key_init      (struct lu_context *ctx);
static void  osd_key_fini      (struct lu_context *ctx, void *data);
static int   osd_has_index     (struct osd_object *obj);
static void  osd_object_init0  (struct osd_object *obj);
static int   osd_device_init   (struct lu_context *ctx,
                                struct lu_device *d, struct lu_device *);
static int   osd_fid_lookup    (struct lu_context *ctx, struct osd_object *obj,
                                const struct lu_fid *fid);
static int   osd_inode_getattr (struct lu_context *ctx,
                                struct inode *inode, struct lu_attr *attr);
static int   osd_inode_get_fid (struct osd_device *d, const struct inode *inode,
                                struct lu_fid *fid);
static int   osd_param_is_sane (const struct osd_device *dev,
                                const struct txn_param *param);
static int   osd_index_lookup  (struct lu_context *ctxt, struct dt_object *dt,
                                struct dt_rec *rec, const struct dt_key *key);
static int   osd_index_insert  (struct lu_context *ctxt, struct dt_object *dt,
                                const struct dt_rec *rec,
                                const struct dt_key *key,
                                struct thandle *handle);
static int   osd_index_probe   (struct lu_context *ctxt, struct dt_object *dt,
                                const struct dt_index_features *feat);

static struct osd_object  *osd_obj          (const struct lu_object *o);
static struct osd_device  *osd_dev          (const struct lu_device *d);
static struct osd_device  *osd_dt_dev       (const struct dt_device *d);
static struct osd_object  *osd_dt_obj       (const struct dt_object *d);
static struct osd_device  *osd_obj2dev      (struct osd_object *o);
static struct lu_device   *osd2lu_dev       (struct osd_device * osd);
static struct lu_device   *osd_device_fini  (struct lu_context *ctx,
                                             struct lu_device *d);
static struct lu_device   *osd_device_alloc (struct lu_context *ctx,
                                             struct lu_device_type *t,
                                             struct lustre_cfg *cfg);
static struct lu_object   *osd_object_alloc (struct lu_context *ctx,
                                             struct lu_device *d);
static struct inode       *osd_iget         (struct osd_thread_info *info,
                                             struct osd_device *dev,
                                             const struct osd_inode_id *id);
static struct super_block *osd_sb           (const struct osd_device *dev);
static journal_t          *osd_journal      (const struct osd_device *dev);

static struct lu_device_type_operations osd_device_type_ops;
static struct lu_device_type            osd_device_type;
static struct lu_object_operations      osd_lu_obj_ops;
static struct obd_ops                   osd_obd_device_ops;
static struct lprocfs_vars              lprocfs_osd_module_vars[];
static struct lprocfs_vars              lprocfs_osd_obd_vars[];
static struct lu_device_operations      osd_lu_ops;
static struct lu_context_key            osd_key;
static struct dt_object_operations      osd_obj_ops;
static struct dt_body_operations        osd_body_ops;
static struct dt_index_operations       osd_index_ops;

struct osd_thandle {
        struct thandle  ot_super;
        handle_t       *ot_handle;
};

/*
 * DT methods.
 */
static int osd_root_get(struct lu_context *ctx,
                        struct dt_device *dev, struct lu_fid *f)
{
        struct osd_device *d = osd_dt_dev(dev);

        return osd_inode_get_fid(d, d->od_root_dir->d_inode, f);
}


/*
 * OSD object methods.
 */

static struct lu_object *osd_object_alloc(struct lu_context *ctx,
                                          struct lu_device *d)
{
        struct osd_object *mo;

        OBD_ALLOC_PTR(mo);
        if (mo != NULL) {
                struct lu_object *l;

                l = &mo->oo_dt.do_lu;
                lu_object_init(l, NULL, d);
                mo->oo_dt.do_ops = &osd_obj_ops;
                l->lo_ops = &osd_lu_obj_ops;
                init_rwsem(&mo->oo_sem);
                return l;
        } else
                return NULL;
}

static void osd_object_init0(struct osd_object *obj)
{
        LASSERT(obj->oo_inode != NULL);

        if (osd_has_index(obj))
                obj->oo_dt.do_index_ops = &osd_index_ops;
        else
                obj->oo_dt.do_body_ops = &osd_body_ops;
}

static int osd_object_init(struct lu_context *ctxt, struct lu_object *l)
{
        struct osd_object *obj = osd_obj(l);
        int result;

        result = osd_fid_lookup(ctxt, obj, lu_object_fid(l));
        if (result == 0) {
                if (obj->oo_inode != NULL)
                        osd_object_init0(obj);
        }
        return result;
}

static void osd_object_free(struct lu_context *ctx, struct lu_object *l)
{
        struct osd_object *obj = osd_obj(l);
        OBD_FREE_PTR(obj);
}

static void osd_object_delete(struct lu_context *ctx, struct lu_object *l)
{
        struct osd_object *o = osd_obj(l);

        if (o->oo_inode != NULL)
                iput(o->oo_inode);
}

static int osd_inode_unlinked(const struct inode *inode)
{
        return inode->i_nlink == !!S_ISDIR(inode->i_mode);
}

static void osd_object_release(struct lu_context *ctxt, struct lu_object *l)
{
        struct osd_object *o = osd_obj(l);

        if (o->oo_inode != NULL && osd_inode_unlinked(o->oo_inode))
                set_bit(LU_OBJECT_HEARD_BANSHEE, &l->lo_header->loh_flags);
}

static int osd_object_exists(struct lu_context *ctx, struct lu_object *o)
{
        return osd_obj(o)->oo_inode != NULL;
}

static int osd_object_print(struct lu_context *ctx,
                            struct seq_file *f, const struct lu_object *l)
{
        struct osd_object  *o = osd_obj(l);

        return seq_printf(f, LUSTRE_OSD0_NAME"-object@%p(i:%p:%lu/%u)",
                          o, o->oo_inode,
                          o->oo_inode ? o->oo_inode->i_ino : 0UL,
                          o->oo_inode ? o->oo_inode->i_generation : 0);
}

static int osd_config(struct lu_context *ctx,
                      struct dt_device *d, const char *name,
                      void *buf, int size, int mode)
{
        if (mode == LUSTRE_CONFIG_GET) {
                /* to be continued */
                return 0;
        } else {
                /* to be continued */
                return 0;
        }
}

static int osd_statfs(struct lu_context *ctx,
                      struct dt_device *d, struct kstatfs *sfs)
{
	struct osd_device *osd = osd_dt_dev(d);
        struct super_block *sb = osd_sb(osd);
        int result;

        ENTRY;

        memset(sfs, 0, sizeof *sfs);
        result = sb->s_op->statfs(sb, sfs);

        RETURN (result);
}

/*
 * Journal
 */

static int osd_param_is_sane(const struct osd_device *dev,
                             const struct txn_param *param)
{
        return param->tp_credits <= osd_journal(dev)->j_max_transaction_buffers;
}

static struct thandle *osd_trans_start(struct lu_context *ctx,
                                       struct dt_device *d,
                                       struct txn_param *p)
{
        struct osd_device  *dev = osd_dt_dev(d);
        handle_t           *jh;
        struct osd_thandle *oh;
        struct thandle     *th;
        int hook_res;

        ENTRY;

        hook_res = dt_txn_hook_start(ctx, d, p);
        if (hook_res != 0)
                RETURN(ERR_PTR(hook_res));

        if (osd_param_is_sane(dev, p)) {
                OBD_ALLOC_PTR(oh);
                if (oh != NULL) {
                        /*
                         * XXX temporary stuff. Some abstraction layer should
                         * be used.
                         */
                        jh = journal_start(osd_journal(dev), p->tp_credits);
                        if (!IS_ERR(jh)) {
                                oh->ot_handle = jh;
                                th = &oh->ot_super;
                                th->th_dev = d;
                                lu_device_get(&d->dd_lu_dev);
                        } else {
                                OBD_FREE_PTR(oh);
                                th = (void *)jh;
                        }
                } else
                        th = ERR_PTR(-ENOMEM);
        } else {
                CERROR("Invalid transaction parameters\n");
                th = ERR_PTR(-EINVAL);
        }

        RETURN(th);
}

static void osd_trans_stop(struct lu_context *ctx, struct thandle *th)
{
        int result;
        struct osd_thandle *oh;

        ENTRY;

        oh = container_of0(th, struct osd_thandle, ot_super);
        if (oh->ot_handle != NULL) {
                /*
                 * XXX temporary stuff. Some abstraction layer should be used.
                 */
                result = dt_txn_hook_stop(ctx, th->th_dev, th);
                if (result != 0)
                        CERROR("Failure in transaction hook: %d\n", result);
                result = journal_stop(oh->ot_handle);
                if (result != 0)
                        CERROR("Failure to stop transaction: %d\n", result);
                oh->ot_handle = NULL;
        }
        if (th->th_dev != NULL) {
                lu_device_put(&th->th_dev->dd_lu_dev);
                th->th_dev = NULL;
        }
        EXIT;
}

static struct dt_device_operations osd_dt_ops = {
        .dt_root_get    = osd_root_get,
        .dt_config      = osd_config,
        .dt_statfs      = osd_statfs,
        .dt_trans_start = osd_trans_start,
        .dt_trans_stop  = osd_trans_stop,
};

static void osd_object_lock(struct lu_context *ctx, struct dt_object *dt,
                            enum dt_lock_mode mode)
{
        struct osd_object *obj = osd_dt_obj(dt);

        LASSERT(mode == DT_WRITE_LOCK || mode == DT_READ_LOCK);
        if (mode == DT_WRITE_LOCK)
                down_write(&obj->oo_sem);
        else
                down_read(&obj->oo_sem);
}

static void osd_object_unlock(struct lu_context *ctx, struct dt_object *dt,
                              enum dt_lock_mode mode)
{
        struct osd_object *obj = osd_dt_obj(dt);

        LASSERT(mode == DT_WRITE_LOCK || mode == DT_READ_LOCK);
        if (mode == DT_WRITE_LOCK)
                up_write(&obj->oo_sem);
        else
                up_read(&obj->oo_sem);
}

static int osd_attr_get(struct lu_context *ctxt, struct dt_object *dt,
                        struct lu_attr *attr)
{
        LASSERT(lu_object_exists(ctxt, &dt->do_lu));
        return osd_inode_getattr(ctxt, osd_dt_obj(dt)->oo_inode, attr);
}

/*
 * Object creation.
 *
 * XXX temporary solution.
 */

static int osd_create_pre(struct osd_thread_info *info, struct osd_object *obj,
                          struct lu_attr *attr, struct thandle *th)
{
        return 0;
}

static int osd_create_post(struct osd_thread_info *info, struct osd_object *obj,
                           struct lu_attr *attr, struct thandle *th)
{
        LASSERT(obj->oo_inode != NULL);

        osd_object_init0(obj);
        return 0;
}

static void osd_fid_build_name(const struct lu_fid *fid, char *name)
{
        static const char *qfmt = LPX64":%lx:%lx";

        sprintf(name, qfmt, fid_seq(fid), fid_oid(fid), fid_ver(fid));
}

static int osd_mkdir(struct osd_thread_info *info, struct osd_object *obj,
                     struct lu_attr *attr, struct thandle *th)
{
        int result;
        struct osd_device *osd = osd_obj2dev(obj);
        struct inode      *dir;

        /*
         * XXX temporary solution.
         */
        struct dentry     *dentry;
        char               name[32];
        struct qstr        str = {
                .name = name
        };

        LASSERT(obj->oo_inode == NULL);
        LASSERT(S_ISDIR(attr->la_mode));
        LASSERT(osd->od_obj_area != NULL);

        dir = osd->od_obj_area->d_inode;
        LASSERT(dir->i_op != NULL && dir->i_op->mkdir != NULL);

        osd_fid_build_name(lu_object_fid(&obj->oo_dt.do_lu), name);
        str.len = strlen(name);

        dentry = d_alloc(osd->od_obj_area, &str);
        if (dentry != NULL) {
                result = dir->i_op->mkdir(dir, dentry,
                                          attr->la_mode & (S_IRWXUGO|S_ISVTX));
                if (result == 0) {
                        LASSERT(dentry->d_inode != NULL);
                        obj->oo_inode = dentry->d_inode;
                        igrab(obj->oo_inode);
                }
                dput(dentry);
        } else
                result = -ENOMEM;
        return result;
}

typedef int (*osd_obj_type_f)(struct osd_thread_info *, struct osd_object *,
                              struct lu_attr *, struct thandle *);

osd_obj_type_f osd_mkreg = NULL;
osd_obj_type_f osd_mksym = NULL;
osd_obj_type_f osd_mknod = NULL;

static osd_obj_type_f osd_create_type_f(__u32 mode)
{
        osd_obj_type_f result;

        switch (mode) {
        case S_IFDIR:
                result = osd_mkdir;
                break;
        case S_IFREG:
                result = osd_mkreg;
                break;
        case S_IFLNK:
                result = osd_mksym;
                break;
        case S_IFCHR:
        case S_IFBLK:
        case S_IFIFO:
        case S_IFSOCK:
                result = osd_mknod;
                break;
        default:
                LBUG();
                break;
        }
        return result;
 }

static int osd_object_create(struct lu_context *ctx, struct dt_object *dt,
                             struct lu_attr *attr, struct thandle *th)
{
        const struct lu_fid    *fid  = lu_object_fid(&dt->do_lu);
        struct osd_object      *obj  = osd_dt_obj(dt);
        struct osd_device      *osd  = osd_obj2dev(obj);
        struct osd_thread_info *info = lu_context_key_get(ctx, &osd_key);
        int result;

        ENTRY;

        LASSERT(!lu_object_exists(ctx, &dt->do_lu));

        /*
         * XXX missing: permission checks.
         */

        /*
         * XXX missing: sanity checks (valid ->la_mode, etc.)
         */

        /*
         * XXX missing: Quote handling.
         */

        result = osd_create_pre(info, obj, attr, th);
        if (result == 0) {
                osd_create_type_f(attr->la_mode & S_IFMT)(info, obj, attr, th);
                if (result == 0)
                        result = osd_create_post(info, obj, attr, th);
        }
        if (result == 0) {
                struct osd_inode_id *id = &info->oti_id;

                LASSERT(obj->oo_inode != NULL);

                id->oii_ino = obj->oo_inode->i_ino;
                id->oii_gen = obj->oo_inode->i_generation;

                osd_oi_write_lock(&osd->od_oi);
                result = osd_oi_insert(info, &osd->od_oi, fid, id, th);
                osd_oi_write_unlock(&osd->od_oi);
        }

        LASSERT(ergo(result == 0, lu_object_exists(ctx, &dt->do_lu)));
        return result;
}

static struct dt_object_operations osd_obj_ops = {
        .do_object_lock   = osd_object_lock,
        .do_object_unlock = osd_object_unlock,
        .do_attr_get      = osd_attr_get,
        .do_object_create = osd_object_create,
};

static struct dt_body_operations osd_body_ops = {
};

/*
 * Index operations.
 */

/*
 * XXX This is temporary solution: inode operations are used until iam is
 * ready.
 */
static int osd_index_lookup(struct lu_context *ctxt, struct dt_object *dt,
                            struct dt_rec *rec, const struct dt_key *key)
{
        struct osd_object *obj = osd_dt_obj(dt);
        struct osd_device *osd = osd_obj2dev(obj);
        struct inode      *dir;

        int result;

        /*
         * XXX temporary solution.
         */
        struct dentry     *dentry;
        struct qstr        str = {
                .name = (const char *)key,
                .len  = strlen((const char *)key)
        };

        LASSERT(osd_has_index(obj));
        LASSERT(osd->od_obj_area != NULL);

        dir = obj->oo_inode;
        LASSERT(dir->i_op != NULL && dir->i_op->lookup != NULL);

        dentry = d_alloc(NULL, &str);
        if (dentry != NULL) {
                struct dentry *d;

                /*
                 * XXX passing NULL for nameidata should work for
                 * ext3/ldiskfs.
                 */
                d = dir->i_op->lookup(dir, dentry, NULL);
                if (d == NULL) {
                        /*
                         * normal case, result is in @dentry.
                         */
                        if (dentry->d_inode != NULL) {
                                struct lu_fid *fid = (struct lu_fid *)rec;
                                /*
                                 * Build fid from inode.
                                 */
                                fid->f_seq = 0; /* XXX hard-coded */
                                fid->f_oid = dentry->d_inode->i_ino;
                                fid->f_ver = dentry->d_inode->i_generation;
                                result = 0;
                        } else
                                result = -ENOENT;
                } else {
                        /* What? Disconnected alias? Ppheeeww... */
                        CERROR("Aliasing where not expected\n");
                        result = -EIO;
                        dput(d);
                }
                dput(dentry);
        } else
                result = -ENOMEM;
        return result;
}

static int osd_index_insert(struct lu_context *ctxt, struct dt_object *dt,
                            const struct dt_rec *rec, const struct dt_key *key,
                            struct thandle *handle)
{
        return 0;
}

const struct dt_index_features dt_directory_features;

static int osd_index_probe(struct lu_context *ctxt, struct dt_object *dt,
                           const struct dt_index_features *feat)
{
        struct osd_object *obj = osd_dt_obj(dt);

        if (feat == &dt_directory_features)
                return 1;
        else
                return 0; /* nothing yet is supported */
}

static struct dt_index_operations osd_index_ops = {
        .dio_lookup = osd_index_lookup,
        .dio_insert = osd_index_insert,
        .dio_probe  = osd_index_probe
};

/*
 * OSD device type methods
 */
static int osd_type_init(struct lu_device_type *t)
{
        return lu_context_key_register(&osd_key);
}

static void osd_type_fini(struct lu_device_type *t)
{
        lu_context_key_degister(&osd_key);
}

static struct lu_context_key osd_key = {
        .lct_init = osd_key_init,
        .lct_fini = osd_key_fini
};

static void *osd_key_init(struct lu_context *ctx)
{
        struct osd_thread_info *info;

        OBD_ALLOC_PTR(info);
        if (info == NULL)
                info = ERR_PTR(-ENOMEM);
        return info;
}

static void osd_key_fini(struct lu_context *ctx, void *data)
{
        struct osd_thread_info *info = data;
        OBD_FREE_PTR(info);
}

static int osd_device_init(struct lu_context *ctx,
                           struct lu_device *d, struct lu_device *next)
{
        return 0;
}

extern void osd_oi_init0(struct osd_oi *oi, __u64 root_ino, __u32 root_gen);
extern int osd_oi_find_fid(struct osd_oi *oi,
                           __u64 ino, __u32 gen, struct lu_fid *fid);

static int osd_mount(struct lu_context *ctx,
                     struct osd_device *o, struct lustre_cfg *cfg)
{
        struct lustre_mount_info *lmi;
        const char *dev = lustre_cfg_string(cfg, 0);
        int result;

        ENTRY;

        if (o->od_mount != NULL) {
                CERROR("Already mounted (%s)\n", dev);
                RETURN(-EEXIST);
        }

        /* get mount */
        lmi = server_get_mount(dev);
        if (lmi == NULL) {
                CERROR("Cannot get mount info for %s!\n", dev);
                RETURN(-EFAULT);
        }

        LASSERT(lmi != NULL);
        /* save lustre_mount_info in dt_device */
        o->od_mount = lmi;
        result = osd_oi_init(&o->od_oi, osd_sb(o)->s_root,
                             osd2lu_dev(o)->ld_site);
        if (result == 0) {
                struct dentry *d;

                d = simple_mkdir(osd_sb(o)->s_root, "*OBJ-TEMP*", 0777, 1);
                if (!IS_ERR(d)) {
                        o->od_obj_area = d;

                        d = simple_mkdir(osd_sb(o)->s_root, "ROOT", 0777, 1);
                        if (!IS_ERR(d)) {
                                osd_oi_init0(&o->od_oi, d->d_inode->i_ino,
                                             d->d_inode->i_generation);
                                o->od_root_dir = d;
                        } else
                                result = PTR_ERR(d);
                } else
                        result = PTR_ERR(d);
        }
        if (result != 0)
                osd_device_fini(ctx, osd2lu_dev(o));
        RETURN(result);
}

static struct lu_device *osd_device_fini(struct lu_context *ctx,
                                         struct lu_device *d)
{
        struct osd_device *o = osd_dev(d);

        ENTRY;
        if (o->od_obj_area != NULL) {
                dput(o->od_obj_area);
                o->od_obj_area = NULL;
        }
        if (o->od_root_dir != NULL) {
                dput(o->od_root_dir);
                o->od_root_dir = NULL;
        }
        osd_oi_fini(&o->od_oi);

        if (o->od_mount)
                server_put_mount(o->od_mount->lmi_name, o->od_mount->lmi_mnt);

        o->od_mount = NULL;
	RETURN(NULL);
}

static struct lu_device *osd_device_alloc(struct lu_context *ctx,
                                          struct lu_device_type *t,
                                          struct lustre_cfg *cfg)
{
        struct lu_device  *l;
        struct osd_device *o;

        OBD_ALLOC_PTR(o);
        if (o != NULL) {
                int result;

                result = dt_device_init(&o->od_dt_dev, t);
                if (result == 0) {
                        l = osd2lu_dev(o);
                        l->ld_ops = &osd_lu_ops;
                        o->od_dt_dev.dd_ops = &osd_dt_ops;
                } else
                        l = ERR_PTR(result);
        } else
                l = ERR_PTR(-ENOMEM);
        return l;
}

static void osd_device_free(struct lu_context *ctx, struct lu_device *d)
{
        struct osd_device *o = osd_dev(d);

        dt_device_fini(&o->od_dt_dev);
        OBD_FREE_PTR(o);
}

static int osd_process_config(struct lu_context *ctx,
                              struct lu_device *d, struct lustre_cfg *cfg)
{
        struct osd_device *o = osd_dev(d);
        int err;

        switch(cfg->lcfg_command) {
        case LCFG_SETUP:
                err = osd_mount(ctx, o, cfg);
                break;
        default:
                err = -ENOTTY;
        }

        RETURN(err);
}

/*
 * fid<->inode<->object functions.
 */

static int osd_inode_get_fid(struct osd_device *d, const struct inode *inode,
                             struct lu_fid *fid)
{
        int result;

        /*
         * XXX: Should return fid stored together with inode in memory.
         */
        if (OI_IN_MEMORY) {
                result = osd_oi_find_fid(&d->od_oi, inode->i_ino,
                                         inode->i_generation, fid);
        } else {
                fid->f_seq = inode->i_ino;
                fid->f_oid = inode->i_generation;
                result = 0;
        }
        return result;
}

struct dentry *osd_open(struct dentry *parent, const char *name, mode_t mode)
{
        struct dentry *dentry;
        struct dentry *result;

        result = dentry = osd_lookup(parent, name);
        if (IS_ERR(dentry)) {
                CERROR("Error opening %s: %ld\n", name, PTR_ERR(dentry));
                dentry = NULL; /* dput(NULL) below is OK */
        } else if (dentry->d_inode == NULL) {
                CERROR("Not found: %s\n", name);
                result = ERR_PTR(-ENOENT);
        } else if ((dentry->d_inode->i_mode & S_IFMT) != mode) {
                CERROR("Wrong mode: %s: %o != %o\n", name,
                       dentry->d_inode->i_mode, mode);
                result = ERR_PTR(mode == S_IFDIR ? -ENOTDIR : -EISDIR);
        }

        if (IS_ERR(result))
                dput(dentry);
        return result;
}

struct dentry *osd_lookup(struct dentry *parent, const char *name)
{
        struct dentry *dentry;

        CDEBUG(D_INODE, "looking up object %s\n", name);
        down(&parent->d_inode->i_sem);
        dentry = lookup_one_len(name, parent, strlen(name));
        up(&parent->d_inode->i_sem);

        if (IS_ERR(dentry)) {
                CERROR("error getting %s: %ld\n", name, PTR_ERR(dentry));
        } else if (dentry->d_inode != NULL && is_bad_inode(dentry->d_inode)) {
                CERROR("got bad object %s inode %lu\n",
                       name, dentry->d_inode->i_ino);
                dput(dentry);
                dentry = ERR_PTR(-ENOENT);
        }
        return dentry;
}

static struct inode *osd_iget(struct osd_thread_info *info,
                              struct osd_device *dev,
                              const struct osd_inode_id *id)
{
        struct inode *inode;

	inode = iget(osd_sb(dev), id->oii_ino);
	if (inode == NULL) {
                CERROR("no inode\n");
		inode = ERR_PTR(-EACCES);
	} else if (is_bad_inode(inode)) {
                CERROR("bad inode\n");
		iput(inode);
		inode = ERR_PTR(-ENOENT);
	} else if (inode->i_generation != id->oii_gen &&
                   id->oii_gen != OSD_GEN_IGNORE) {
                CERROR("stale inode\n");
		iput(inode);
		inode = ERR_PTR(-ESTALE);
        }
        return inode;

}

static int osd_fid_lookup(struct lu_context *ctx,
                          struct osd_object *obj, const struct lu_fid *fid)
{
        struct osd_thread_info *info;
        struct lu_device       *ldev = obj->oo_dt.do_lu.lo_dev;
        struct osd_device      *dev;
        struct osd_inode_id     id;
        struct inode           *inode;
        int                     result;

        LASSERT(obj->oo_inode == NULL);
        LASSERT(fid_is_sane(fid));
        LASSERT(fid_is_local(ldev->ld_site, fid));

        ENTRY;

        info = lu_context_key_get(ctx, &osd_key);
        dev  = osd_dev(ldev);

        if (OBD_FAIL_CHECK(OBD_FAIL_OST_ENOENT))
                RETURN(-ENOENT);

        osd_oi_read_lock(&dev->od_oi);
        result = osd_oi_lookup(info, &dev->od_oi, fid, &id);
        if (result == 0) {
                inode = osd_iget(info, dev, &id);
                if (!IS_ERR(inode)) {
                        obj->oo_inode = inode;
                        result = 0;
                } else
                        result = PTR_ERR(inode);
        } else if (result == -ENOENT)
                result = 0;
        osd_oi_read_unlock(&dev->od_oi);
        RETURN(result);
}

static int osd_inode_getattr(struct lu_context *ctx,
                             struct inode *inode, struct lu_attr *attr)
{
        //attr->la_atime      = inode->i_atime;
        //attr->la_mtime      = inode->i_mtime;
        //attr->la_ctime      = inode->i_ctime;
        attr->la_mode       = inode->i_mode;
        attr->la_size       = inode->i_size;
        attr->la_blocks     = inode->i_blocks;
        attr->la_uid        = inode->i_uid;
        attr->la_gid        = inode->i_gid;
        attr->la_flags      = inode->i_flags;
        attr->la_nlink      = inode->i_nlink;
        return 0;
}

/*
 * Helpers.
 */

static int lu_device_is_osd(const struct lu_device *d)
{
        /*
         * XXX for now. Tags in lu_device_type->ldt_something are needed.
         */
        return ergo(d != NULL && d->ld_ops != NULL, d->ld_ops == &osd_lu_ops);
}

static struct osd_object *osd_obj(const struct lu_object *o)
{
        LASSERT(lu_device_is_osd(o->lo_dev));
        return container_of0(o, struct osd_object, oo_dt.do_lu);
}

static struct osd_device *osd_dt_dev(const struct dt_device *d)
{
        LASSERT(lu_device_is_osd(&d->dd_lu_dev));
        return container_of0(d, struct osd_device, od_dt_dev);
}

static struct osd_device *osd_dev(const struct lu_device *d)
{
        LASSERT(lu_device_is_osd(d));
        return osd_dt_dev(container_of0(d, struct dt_device, dd_lu_dev));
}

static struct osd_object *osd_dt_obj(const struct dt_object *d)
{
        return osd_obj(&d->do_lu);
}

static struct osd_device *osd_obj2dev(struct osd_object *o)
{
        return osd_dev(o->oo_dt.do_lu.lo_dev);
}

static struct lu_device *osd2lu_dev(struct osd_device * osd)
{
        return &osd->od_dt_dev.dd_lu_dev;
}

static struct super_block *osd_sb(const struct osd_device *dev)
{
        return dev->od_mount->lmi_mnt->mnt_sb;
}

static journal_t *osd_journal(const struct osd_device *dev)
{
	return LDISKFS_SB(osd_sb(dev))->s_journal;
}

static int osd_has_index(struct osd_object *obj)
{
        return S_ISDIR(obj->oo_inode->i_mode);
}

static struct lu_object_operations osd_lu_obj_ops = {
        .loo_object_init    = osd_object_init,
        .loo_object_delete  = osd_object_delete,
        .loo_object_release = osd_object_release,
        .loo_object_print   = osd_object_print,
        .loo_object_exists  = osd_object_exists
};

static struct lu_device_operations osd_lu_ops = {
        .ldo_object_alloc   = osd_object_alloc,
        .ldo_object_free    = osd_object_free,
        .ldo_process_config = osd_process_config
};

static struct lu_device_type_operations osd_device_type_ops = {
        .ldto_init = osd_type_init,
        .ldto_fini = osd_type_fini,

        .ldto_device_alloc = osd_device_alloc,
        .ldto_device_free  = osd_device_free,

        .ldto_device_init    = osd_device_init,
        .ldto_device_fini    = osd_device_fini
};

static struct lu_device_type osd_device_type = {
        .ldt_tags = LU_DEVICE_DT,
        .ldt_name = LUSTRE_OSD0_NAME,
        .ldt_ops  = &osd_device_type_ops
};

/*
 * lprocfs legacy support.
 */
static struct lprocfs_vars lprocfs_osd_obd_vars[] = {
        { 0 }
};

static struct lprocfs_vars lprocfs_osd_module_vars[] = {
        { 0 }
};

static struct obd_ops osd_obd_device_ops = {
        .o_owner = THIS_MODULE
};

LPROCFS_INIT_VARS(osd, lprocfs_osd_module_vars, lprocfs_osd_obd_vars);

static int __init osd_mod_init(void)
{
        struct lprocfs_static_vars lvars;

        lprocfs_init_vars(osd, &lvars);
        return class_register_type(&osd_obd_device_ops, NULL, lvars.module_vars,
                                   LUSTRE_OSD0_NAME, &osd_device_type);
}

static void __exit osd_mod_exit(void)
{
        class_unregister_type(LUSTRE_OSD0_NAME);
}

MODULE_AUTHOR("Cluster File Systems, Inc. <info@clusterfs.com>");
MODULE_DESCRIPTION("Lustre Object Storage Device ("LUSTRE_OSD0_NAME")");
MODULE_LICENSE("GPL");

cfs_module(osd, "0.0.2", osd_mod_init, osd_mod_exit);
