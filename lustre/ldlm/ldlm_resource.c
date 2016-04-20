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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License version 2 for more details (a copy is included
 * in the LICENSE file that accompanied this code).
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; If not, see
 * http://www.sun.com/software/products/lustre/docs/GPLv2.pdf
 *
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa Clara,
 * CA 95054 USA or visit www.sun.com if you need additional information or
 * have any questions.
 *
 * GPL HEADER END
 */
/*
 * Copyright (c) 2002, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2010, 2014, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/ldlm/ldlm_resource.c
 *
 * Author: Phil Schwan <phil@clusterfs.com>
 * Author: Peter Braam <braam@clusterfs.com>
 */

#define DEBUG_SUBSYSTEM S_LDLM
#include <lustre_dlm.h>
#include <lustre_fid.h>
#include <obd_class.h>
#include "ldlm_internal.h"

struct kmem_cache *ldlm_resource_slab, *ldlm_lock_slab;

int ldlm_srv_namespace_nr = 0;
int ldlm_cli_namespace_nr = 0;

struct mutex ldlm_srv_namespace_lock;
struct list_head ldlm_srv_namespace_list;

struct mutex ldlm_cli_namespace_lock;
/* Client Namespaces that have active resources in them.
 * Once all resources go away, ldlm_poold moves such namespaces to the
 * inactive list */
struct list_head ldlm_cli_active_namespace_list;
/* Client namespaces that don't have any locks in them */
struct list_head ldlm_cli_inactive_namespace_list;

static struct proc_dir_entry *ldlm_type_proc_dir;
static struct proc_dir_entry *ldlm_ns_proc_dir;
struct proc_dir_entry *ldlm_svc_proc_dir;

/* during debug dump certain amount of granted locks for one resource to avoid
 * DDOS. */
static unsigned int ldlm_dump_granted_max = 256;

#ifdef CONFIG_PROC_FS
static ssize_t
lprocfs_dump_ns_seq_write(struct file *file, const char __user *buffer,
			  size_t count, loff_t *off)
{
	ldlm_dump_all_namespaces(LDLM_NAMESPACE_SERVER, D_DLMTRACE);
	ldlm_dump_all_namespaces(LDLM_NAMESPACE_CLIENT, D_DLMTRACE);
	RETURN(count);
}

static ssize_t
lprocfs_drop_caches_seq_write(struct file *file, const char __user *buffer,
		  	      size_t count, loff_t *off)
{
	int rc = 0; 
	rc = ldlm_drop_caches(LDLM_NAMESPACE_CLIENT);
	if (rc < 0) 
		RETURN(rc);
	rc = ldlm_drop_caches(LDLM_NAMESPACE_SERVER);
	if (rc < 0) 
		RETURN(rc);
	RETURN(count);
}

LPROC_SEQ_FOPS_WO_TYPE(ldlm, dump_ns);
LPROC_SEQ_FOPS_WO_TYPE(ldlm, drop_caches);

LPROC_SEQ_FOPS_RW_TYPE(ldlm_rw, uint);
LPROC_SEQ_FOPS_RO_TYPE(ldlm, uint);

int ldlm_proc_setup(void)
{
	int rc;
	struct lprocfs_vars list[] = {
		{ .name	=	"dump_namespaces",
		  .fops	=	&ldlm_dump_ns_fops,
		  .proc_mode =	0222 },
		{ .name	=	"dump_granted_max",
		  .fops	=	&ldlm_rw_uint_fops,
		  .data	=	&ldlm_dump_granted_max },
		{ .name	=	"cancel_unused_locks_before_replay",
		  .fops	=	&ldlm_rw_uint_fops,
		  .data	=	&ldlm_cancel_unused_locks_before_replay },
		{ .name = 	"drop_caches",
		  .fops = 	&ldlm_drop_caches_fops,
		  .proc_mode =	0222 },
		{ NULL }};
	ENTRY;
	LASSERT(ldlm_ns_proc_dir == NULL);

	ldlm_type_proc_dir = lprocfs_register(OBD_LDLM_DEVICENAME,
					      proc_lustre_root,
					      NULL, NULL);
	if (IS_ERR(ldlm_type_proc_dir)) {
		CERROR("LProcFS failed in ldlm-init\n");
		rc = PTR_ERR(ldlm_type_proc_dir);
		GOTO(err, rc);
	}

	ldlm_ns_proc_dir = lprocfs_register("namespaces",
					    ldlm_type_proc_dir,
					    NULL, NULL);
	if (IS_ERR(ldlm_ns_proc_dir)) {
		CERROR("LProcFS failed in ldlm-init\n");
		rc = PTR_ERR(ldlm_ns_proc_dir);
		GOTO(err_type, rc);
	}

	ldlm_svc_proc_dir = lprocfs_register("services",
					     ldlm_type_proc_dir,
					     NULL, NULL);
	if (IS_ERR(ldlm_svc_proc_dir)) {
		CERROR("LProcFS failed in ldlm-init\n");
		rc = PTR_ERR(ldlm_svc_proc_dir);
		GOTO(err_ns, rc);
	}

	rc = lprocfs_add_vars(ldlm_type_proc_dir, list, NULL);
	if (rc != 0) {
		CERROR("LProcFS failed in ldlm-init\n");
		GOTO(err_svc, rc);
	}

	RETURN(0);

err_svc:
	lprocfs_remove(&ldlm_svc_proc_dir);
err_ns:
        lprocfs_remove(&ldlm_ns_proc_dir);
err_type:
        lprocfs_remove(&ldlm_type_proc_dir);
err:
        ldlm_svc_proc_dir = NULL;
        RETURN(rc);
}

void ldlm_proc_cleanup(void)
{
        if (ldlm_svc_proc_dir)
                lprocfs_remove(&ldlm_svc_proc_dir);

        if (ldlm_ns_proc_dir)
                lprocfs_remove(&ldlm_ns_proc_dir);

        if (ldlm_type_proc_dir)
                lprocfs_remove(&ldlm_type_proc_dir);
}

static int lprocfs_ns_resources_seq_show(struct seq_file *m, void *v)
{
	struct ldlm_namespace	*ns  = m->private;
	__u64			res = 0;
	cfs_hash_bd_t		bd;
	int			i;

	/* result is not strictly consistant */
	cfs_hash_for_each_bucket(ns->ns_rs_hash, &bd, i)
		res += cfs_hash_bd_count_get(&bd);
	return lprocfs_u64_seq_show(m, &res);
}
LPROC_SEQ_FOPS_RO(lprocfs_ns_resources);

static int lprocfs_ns_locks_seq_show(struct seq_file *m, void *v)
{
	struct ldlm_namespace	*ns = m->private;
	__u64			locks;

	locks = lprocfs_stats_collector(ns->ns_stats, LDLM_NSS_LOCKS,
					LPROCFS_FIELDS_FLAGS_SUM);
	return lprocfs_u64_seq_show(m, &locks);
}
LPROC_SEQ_FOPS_RO(lprocfs_ns_locks);

static int lprocfs_lru_size_seq_show(struct seq_file *m, void *v)
{
	struct ldlm_namespace *ns = m->private;
	__u32 *nr = &ns->ns_max_unused;

	if (ns_connect_lru_resize(ns))
		nr = &ns->ns_nr_unused;
	return lprocfs_uint_seq_show(m, nr);
}

static ssize_t lprocfs_lru_size_seq_write(struct file *file,
					  const char __user *buffer,
					  size_t count, loff_t *off)
{
	struct ldlm_namespace *ns = ((struct seq_file *)file->private_data)->private;
        char dummy[MAX_STRING_SIZE + 1], *end;
        unsigned long tmp;
        int lru_resize;

        dummy[MAX_STRING_SIZE] = '\0';
	if (copy_from_user(dummy, buffer, MAX_STRING_SIZE))
                return -EFAULT;

        if (strncmp(dummy, "clear", 5) == 0) {
		int rc = 0;
		rc = ldlm_ns_drop_cache(ns);
		if (rc != 0)
			return rc;
		else
			return count;
        }

        tmp = simple_strtoul(dummy, &end, 0);
        if (dummy == end) {
                CERROR("invalid value written\n");
                return -EINVAL;
        }
        lru_resize = (tmp == 0);

        if (ns_connect_lru_resize(ns)) {
                if (!lru_resize)
                        ns->ns_max_unused = (unsigned int)tmp;

                if (tmp > ns->ns_nr_unused)
                        tmp = ns->ns_nr_unused;
                tmp = ns->ns_nr_unused - tmp;

                CDEBUG(D_DLMTRACE,
                       "changing namespace %s unused locks from %u to %u\n",
                       ldlm_ns_name(ns), ns->ns_nr_unused,
                       (unsigned int)tmp);
		ldlm_cancel_lru(ns, tmp, LCF_ASYNC, LDLM_CANCEL_PASSED);

                if (!lru_resize) {
                        CDEBUG(D_DLMTRACE,
                               "disable lru_resize for namespace %s\n",
                               ldlm_ns_name(ns));
                        ns->ns_connect_flags &= ~OBD_CONNECT_LRU_RESIZE;
                }
        } else {
                CDEBUG(D_DLMTRACE,
                       "changing namespace %s max_unused from %u to %u\n",
                       ldlm_ns_name(ns), ns->ns_max_unused,
                       (unsigned int)tmp);
                ns->ns_max_unused = (unsigned int)tmp;
		ldlm_cancel_lru(ns, 0, LCF_ASYNC, LDLM_CANCEL_PASSED);

		/* Make sure that LRU resize was originally supported before
		 * turning it on here. */
                if (lru_resize &&
                    (ns->ns_orig_connect_flags & OBD_CONNECT_LRU_RESIZE)) {
                        CDEBUG(D_DLMTRACE,
                               "enable lru_resize for namespace %s\n",
                               ldlm_ns_name(ns));
                        ns->ns_connect_flags |= OBD_CONNECT_LRU_RESIZE;
                }
        }

        return count;
}
LPROC_SEQ_FOPS(lprocfs_lru_size);

static int lprocfs_elc_seq_show(struct seq_file *m, void *v)
{
	struct ldlm_namespace *ns = m->private;
	unsigned int supp = ns_connect_cancelset(ns);

	return lprocfs_uint_seq_show(m, &supp);
}

static ssize_t lprocfs_elc_seq_write(struct file *file,
				     const char __user *buffer,
				     size_t count, loff_t *off)
{
	struct ldlm_namespace *ns = ((struct seq_file *)file->private_data)->private;
	unsigned int supp = -1;
	int rc;

	rc = lprocfs_wr_uint(file, buffer, count, &supp);
	if (rc < 0)
		return rc;

	if (supp == 0)
		ns->ns_connect_flags &= ~OBD_CONNECT_CANCELSET;
	else if (ns->ns_orig_connect_flags & OBD_CONNECT_CANCELSET)
		ns->ns_connect_flags |= OBD_CONNECT_CANCELSET;
	return count;
}
LPROC_SEQ_FOPS(lprocfs_elc);

static void ldlm_namespace_proc_unregister(struct ldlm_namespace *ns)
{
	if (ns->ns_proc_dir_entry == NULL)
                CERROR("dlm namespace %s has no procfs dir?\n",
                       ldlm_ns_name(ns));
	else
		lprocfs_remove(&ns->ns_proc_dir_entry);

	if (ns->ns_stats != NULL)
		lprocfs_free_stats(&ns->ns_stats);
}

static int ldlm_namespace_proc_register(struct ldlm_namespace *ns)
{
	struct lprocfs_vars lock_vars[2];
        char lock_name[MAX_STRING_SIZE + 1];
	struct proc_dir_entry *ns_pde;

        LASSERT(ns != NULL);
        LASSERT(ns->ns_rs_hash != NULL);

	if (ns->ns_proc_dir_entry != NULL) {
		ns_pde = ns->ns_proc_dir_entry;
	} else {
		ns_pde = proc_mkdir(ldlm_ns_name(ns), ldlm_ns_proc_dir);
		if (ns_pde == NULL)
			return -ENOMEM;
		ns->ns_proc_dir_entry = ns_pde;
	}

        ns->ns_stats = lprocfs_alloc_stats(LDLM_NSS_LAST, 0);
        if (ns->ns_stats == NULL)
                return -ENOMEM;

        lprocfs_counter_init(ns->ns_stats, LDLM_NSS_LOCKS,
                             LPROCFS_CNTR_AVGMINMAX, "locks", "locks");

        lock_name[MAX_STRING_SIZE] = '\0';

        memset(lock_vars, 0, sizeof(lock_vars));
        lock_vars[0].name = lock_name;

	ldlm_add_var(&lock_vars[0], ns_pde, "resource_count", ns,
		     &lprocfs_ns_resources_fops);
	ldlm_add_var(&lock_vars[0], ns_pde, "lock_count", ns,
		     &lprocfs_ns_locks_fops);

	if (ns_is_client(ns)) {
		ldlm_add_var(&lock_vars[0], ns_pde, "lock_unused_count",
			     &ns->ns_nr_unused, &ldlm_uint_fops);
		ldlm_add_var(&lock_vars[0], ns_pde, "lru_size", ns,
			     &lprocfs_lru_size_fops);
		ldlm_add_var(&lock_vars[0], ns_pde, "lru_max_age",
			     &ns->ns_max_age, &ldlm_rw_uint_fops);
		ldlm_add_var(&lock_vars[0], ns_pde, "early_lock_cancel",
			     ns, &lprocfs_elc_fops);
	} else {
		ldlm_add_var(&lock_vars[0], ns_pde, "ctime_age_limit",
			     &ns->ns_ctime_age_limit, &ldlm_rw_uint_fops);
		ldlm_add_var(&lock_vars[0], ns_pde, "lock_timeouts",
			     &ns->ns_timeouts, &ldlm_uint_fops);
		ldlm_add_var(&lock_vars[0], ns_pde, "max_nolock_bytes",
			     &ns->ns_max_nolock_size, &ldlm_rw_uint_fops);
		ldlm_add_var(&lock_vars[0], ns_pde, "contention_seconds",
			     &ns->ns_contention_time, &ldlm_rw_uint_fops);
		ldlm_add_var(&lock_vars[0], ns_pde, "contended_locks",
			     &ns->ns_contended_locks, &ldlm_rw_uint_fops);
		ldlm_add_var(&lock_vars[0], ns_pde, "max_parallel_ast",
			     &ns->ns_max_parallel_ast, &ldlm_rw_uint_fops);
	}
	return 0;
}
#undef MAX_STRING_SIZE
#else /* CONFIG_PROC_FS */

#define ldlm_namespace_proc_unregister(ns)      ({;})
#define ldlm_namespace_proc_register(ns)        ({0;})

#endif /* CONFIG_PROC_FS */

static unsigned ldlm_res_hop_hash(cfs_hash_t *hs,
                                  const void *key, unsigned mask)
{
        const struct ldlm_res_id     *id  = key;
        unsigned                val = 0;
        unsigned                i;

        for (i = 0; i < RES_NAME_SIZE; i++)
                val += id->name[i];
        return val & mask;
}

static unsigned ldlm_res_hop_fid_hash(cfs_hash_t *hs,
                                      const void *key, unsigned mask)
{
        const struct ldlm_res_id *id = key;
        struct lu_fid       fid;
        __u32               hash;
        __u32               val;

        fid.f_seq = id->name[LUSTRE_RES_ID_SEQ_OFF];
        fid.f_oid = (__u32)id->name[LUSTRE_RES_ID_VER_OID_OFF];
        fid.f_ver = (__u32)(id->name[LUSTRE_RES_ID_VER_OID_OFF] >> 32);

	hash = fid_flatten32(&fid);
	hash += (hash >> 4) + (hash << 12); /* mixing oid and seq */
	if (id->name[LUSTRE_RES_ID_HSH_OFF] != 0) {
		val = id->name[LUSTRE_RES_ID_HSH_OFF];
		hash += (val >> 5) + (val << 11);
	} else {
		val = fid_oid(&fid);
	}
	hash = hash_long(hash, hs->hs_bkt_bits);
	/* give me another random factor */
	hash -= hash_long((unsigned long)hs, val % 11 + 3);

	hash <<= hs->hs_cur_bits - hs->hs_bkt_bits;
	hash |= ldlm_res_hop_hash(hs, key, CFS_HASH_NBKT(hs) - 1);

	return hash & mask;
}

static void *ldlm_res_hop_key(struct hlist_node *hnode)
{
        struct ldlm_resource   *res;

	res = hlist_entry(hnode, struct ldlm_resource, lr_hash);
        return &res->lr_name;
}

static int ldlm_res_hop_keycmp(const void *key, struct hlist_node *hnode)
{
        struct ldlm_resource   *res;

	res = hlist_entry(hnode, struct ldlm_resource, lr_hash);
        return ldlm_res_eq((const struct ldlm_res_id *)key,
                           (const struct ldlm_res_id *)&res->lr_name);
}

static void *ldlm_res_hop_object(struct hlist_node *hnode)
{
	return hlist_entry(hnode, struct ldlm_resource, lr_hash);
}

static void ldlm_res_hop_get_locked(cfs_hash_t *hs, struct hlist_node *hnode)
{
        struct ldlm_resource *res;

	res = hlist_entry(hnode, struct ldlm_resource, lr_hash);
        ldlm_resource_getref(res);
}

static void ldlm_res_hop_put_locked(cfs_hash_t *hs, struct hlist_node *hnode)
{
        struct ldlm_resource *res;

	res = hlist_entry(hnode, struct ldlm_resource, lr_hash);
        /* cfs_hash_for_each_nolock is the only chance we call it */
        ldlm_resource_putref_locked(res);
}

static void ldlm_res_hop_put(cfs_hash_t *hs, struct hlist_node *hnode)
{
        struct ldlm_resource *res;

	res = hlist_entry(hnode, struct ldlm_resource, lr_hash);
        ldlm_resource_putref(res);
}

static cfs_hash_ops_t ldlm_ns_hash_ops = {
        .hs_hash        = ldlm_res_hop_hash,
        .hs_key         = ldlm_res_hop_key,
        .hs_keycmp      = ldlm_res_hop_keycmp,
        .hs_keycpy      = NULL,
        .hs_object      = ldlm_res_hop_object,
        .hs_get         = ldlm_res_hop_get_locked,
        .hs_put_locked  = ldlm_res_hop_put_locked,
        .hs_put         = ldlm_res_hop_put
};

static cfs_hash_ops_t ldlm_ns_fid_hash_ops = {
        .hs_hash        = ldlm_res_hop_fid_hash,
        .hs_key         = ldlm_res_hop_key,
        .hs_keycmp      = ldlm_res_hop_keycmp,
        .hs_keycpy      = NULL,
        .hs_object      = ldlm_res_hop_object,
        .hs_get         = ldlm_res_hop_get_locked,
        .hs_put_locked  = ldlm_res_hop_put_locked,
        .hs_put         = ldlm_res_hop_put
};

typedef struct {
        ldlm_ns_type_t  nsd_type;
        /** hash bucket bits */
        unsigned        nsd_bkt_bits;
        /** hash bits */
        unsigned        nsd_all_bits;
        /** hash operations */
        cfs_hash_ops_t *nsd_hops;
} ldlm_ns_hash_def_t;

static ldlm_ns_hash_def_t ldlm_ns_hash_defs[] =
{
        {
                .nsd_type       = LDLM_NS_TYPE_MDC,
                .nsd_bkt_bits   = 11,
                .nsd_all_bits   = 16,
                .nsd_hops       = &ldlm_ns_fid_hash_ops,
        },
        {
                .nsd_type       = LDLM_NS_TYPE_MDT,
                .nsd_bkt_bits   = 14,
                .nsd_all_bits   = 21,
                .nsd_hops       = &ldlm_ns_fid_hash_ops,
        },
        {
                .nsd_type       = LDLM_NS_TYPE_OSC,
                .nsd_bkt_bits   = 8,
                .nsd_all_bits   = 12,
                .nsd_hops       = &ldlm_ns_hash_ops,
        },
        {
                .nsd_type       = LDLM_NS_TYPE_OST,
                .nsd_bkt_bits   = 11,
                .nsd_all_bits   = 17,
                .nsd_hops       = &ldlm_ns_hash_ops,
        },
        {
                .nsd_type       = LDLM_NS_TYPE_MGC,
                .nsd_bkt_bits   = 4,
                .nsd_all_bits   = 4,
                .nsd_hops       = &ldlm_ns_hash_ops,
        },
        {
                .nsd_type       = LDLM_NS_TYPE_MGT,
                .nsd_bkt_bits   = 4,
                .nsd_all_bits   = 4,
                .nsd_hops       = &ldlm_ns_hash_ops,
        },
        {
                .nsd_type       = LDLM_NS_TYPE_UNKNOWN,
        },
};

/**
 * Create and initialize new empty namespace.
 */
struct ldlm_namespace *ldlm_namespace_new(struct obd_device *obd, char *name,
                                          ldlm_side_t client,
                                          ldlm_appetite_t apt,
                                          ldlm_ns_type_t ns_type)
{
        struct ldlm_namespace *ns = NULL;
        struct ldlm_ns_bucket *nsb;
        ldlm_ns_hash_def_t    *nsd;
        cfs_hash_bd_t          bd;
        int                    idx;
        int                    rc;
        ENTRY;

        LASSERT(obd != NULL);

        rc = ldlm_get_ref();
        if (rc) {
                CERROR("ldlm_get_ref failed: %d\n", rc);
                RETURN(NULL);
        }

        for (idx = 0;;idx++) {
                nsd = &ldlm_ns_hash_defs[idx];
                if (nsd->nsd_type == LDLM_NS_TYPE_UNKNOWN) {
                        CERROR("Unknown type %d for ns %s\n", ns_type, name);
                        GOTO(out_ref, NULL);
                }

                if (nsd->nsd_type == ns_type)
                        break;
        }

        OBD_ALLOC_PTR(ns);
        if (!ns)
                GOTO(out_ref, NULL);

        ns->ns_rs_hash = cfs_hash_create(name,
                                         nsd->nsd_all_bits, nsd->nsd_all_bits,
                                         nsd->nsd_bkt_bits, sizeof(*nsb),
                                         CFS_HASH_MIN_THETA,
                                         CFS_HASH_MAX_THETA,
                                         nsd->nsd_hops,
                                         CFS_HASH_DEPTH |
                                         CFS_HASH_BIGNAME |
                                         CFS_HASH_SPIN_BKTLOCK |
                                         CFS_HASH_NO_ITEMREF);
        if (ns->ns_rs_hash == NULL)
                GOTO(out_ns, NULL);

        cfs_hash_for_each_bucket(ns->ns_rs_hash, &bd, idx) {
                nsb = cfs_hash_bd_extra_get(ns->ns_rs_hash, &bd);
                at_init(&nsb->nsb_at_estimate, ldlm_enqueue_min, 0);
                nsb->nsb_namespace = ns;
        }

        ns->ns_obd      = obd;
        ns->ns_appetite = apt;
        ns->ns_client   = client;

	INIT_LIST_HEAD(&ns->ns_list_chain);
	INIT_LIST_HEAD(&ns->ns_unused_list);
	spin_lock_init(&ns->ns_lock);
	atomic_set(&ns->ns_bref, 0);
	init_waitqueue_head(&ns->ns_waitq);

	ns->ns_max_nolock_size    = NS_DEFAULT_MAX_NOLOCK_BYTES;
	ns->ns_contention_time    = NS_DEFAULT_CONTENTION_SECONDS;
	ns->ns_contended_locks    = NS_DEFAULT_CONTENDED_LOCKS;

        ns->ns_max_parallel_ast   = LDLM_DEFAULT_PARALLEL_AST_LIMIT;
        ns->ns_nr_unused          = 0;
        ns->ns_max_unused         = LDLM_DEFAULT_LRU_SIZE;
        ns->ns_max_age            = LDLM_DEFAULT_MAX_ALIVE;
        ns->ns_ctime_age_limit    = LDLM_CTIME_AGE_LIMIT;
        ns->ns_timeouts           = 0;
        ns->ns_orig_connect_flags = 0;
        ns->ns_connect_flags      = 0;
        ns->ns_stopping           = 0;
        rc = ldlm_namespace_proc_register(ns);
        if (rc != 0) {
                CERROR("Can't initialize ns proc, rc %d\n", rc);
                GOTO(out_hash, rc);
        }

        idx = ldlm_namespace_nr_read(client);
        rc = ldlm_pool_init(&ns->ns_pool, ns, idx, client);
        if (rc) {
                CERROR("Can't initialize lock pool, rc %d\n", rc);
                GOTO(out_proc, rc);
        }

        ldlm_namespace_register(ns, client);
        RETURN(ns);
out_proc:
        ldlm_namespace_proc_unregister(ns);
        ldlm_namespace_cleanup(ns, 0);
out_hash:
        cfs_hash_putref(ns->ns_rs_hash);
out_ns:
        OBD_FREE_PTR(ns);
out_ref:
        ldlm_put_ref();
        RETURN(NULL);
}
EXPORT_SYMBOL(ldlm_namespace_new);

extern struct ldlm_lock *ldlm_lock_get(struct ldlm_lock *lock);

/**
 * Cancel and destroy all locks on a resource.
 *
 * If flags contains FL_LOCAL_ONLY, don't try to tell the server, just
 * clean up.  This is currently only used for recovery, and we make
 * certain assumptions as a result--notably, that we shouldn't cancel
 * locks with refs.
 */
static void cleanup_resource(struct ldlm_resource *res, struct list_head *q,
			     __u64 flags)
{
	struct list_head *tmp;
	int rc = 0, client = ns_is_client(ldlm_res_to_ns(res));
	bool local_only = !!(flags & LDLM_FL_LOCAL_ONLY);

        do {
                struct ldlm_lock *lock = NULL;

		/* First, we look for non-cleaned-yet lock
		 * all cleaned locks are marked by CLEANED flag. */
		lock_res(res);
		list_for_each(tmp, q) {
			lock = list_entry(tmp, struct ldlm_lock,
					  l_res_link);
			if (ldlm_is_cleaned(lock)) {
				lock = NULL;
				continue;
			}
			LDLM_LOCK_GET(lock);
			ldlm_set_cleaned(lock);
			break;
		}

                if (lock == NULL) {
                        unlock_res(res);
                        break;
                }

                /* Set CBPENDING so nothing in the cancellation path
		 * can match this lock. */
		ldlm_set_cbpending(lock);
		ldlm_set_failed(lock);
                lock->l_flags |= flags;

                /* ... without sending a CANCEL message for local_only. */
                if (local_only)
			ldlm_set_local_only(lock);

                if (local_only && (lock->l_readers || lock->l_writers)) {
                        /* This is a little bit gross, but much better than the
                         * alternative: pretend that we got a blocking AST from
                         * the server, so that when the lock is decref'd, it
                         * will go away ... */
                        unlock_res(res);
                        LDLM_DEBUG(lock, "setting FL_LOCAL_ONLY");
			if (lock->l_flags & LDLM_FL_FAIL_LOC) {
				schedule_timeout_and_set_state(
					TASK_UNINTERRUPTIBLE,
					cfs_time_seconds(4));
				set_current_state(TASK_RUNNING);
			}
                        if (lock->l_completion_ast)
				lock->l_completion_ast(lock,
						       LDLM_FL_FAILED, NULL);
                        LDLM_LOCK_RELEASE(lock);
                        continue;
                }

                if (client) {
                        struct lustre_handle lockh;

                        unlock_res(res);
                        ldlm_lock2handle(lock, &lockh);
			rc = ldlm_cli_cancel(&lockh, LCF_ASYNC);
                        if (rc)
                                CERROR("ldlm_cli_cancel: %d\n", rc);
                } else {
                        ldlm_resource_unlink_lock(lock);
                        unlock_res(res);
                        LDLM_DEBUG(lock, "Freeing a lock still held by a "
                                   "client node");
                        ldlm_lock_destroy(lock);
                }
                LDLM_LOCK_RELEASE(lock);
        } while (1);
}

static int ldlm_resource_clean(cfs_hash_t *hs, cfs_hash_bd_t *bd,
			       struct hlist_node *hnode, void *arg)
{
        struct ldlm_resource *res = cfs_hash_object(hs, hnode);
	__u64 flags = *(__u64 *)arg;

        cleanup_resource(res, &res->lr_granted, flags);
        cleanup_resource(res, &res->lr_converting, flags);
        cleanup_resource(res, &res->lr_waiting, flags);

        return 0;
}

static int ldlm_resource_complain(cfs_hash_t *hs, cfs_hash_bd_t *bd,
				  struct hlist_node *hnode, void *arg)
{
	struct ldlm_resource  *res = cfs_hash_object(hs, hnode);

	lock_res(res);
	CERROR("%s: namespace resource "DLDLMRES" (%p) refcount nonzero "
	       "(%d) after lock cleanup; forcing cleanup.\n",
	       ldlm_ns_name(ldlm_res_to_ns(res)), PLDLMRES(res), res,
	       atomic_read(&res->lr_refcount) - 1);

	ldlm_resource_dump(D_ERROR, res);
	unlock_res(res);
	return 0;
}

/**
 * Cancel and destroy all locks in the namespace.
 *
 * Typically used during evictions when server notified client that it was
 * evicted and all of its state needs to be destroyed.
 * Also used during shutdown.
 */
int ldlm_namespace_cleanup(struct ldlm_namespace *ns, __u64 flags)
{
        if (ns == NULL) {
                CDEBUG(D_INFO, "NULL ns, skipping cleanup\n");
                return ELDLM_OK;
        }

	cfs_hash_for_each_nolock(ns->ns_rs_hash, ldlm_resource_clean, &flags);
        cfs_hash_for_each_nolock(ns->ns_rs_hash, ldlm_resource_complain, NULL);
        return ELDLM_OK;
}
EXPORT_SYMBOL(ldlm_namespace_cleanup);

/**
 * Attempts to free namespace.
 *
 * Only used when namespace goes away, like during an unmount.
 */
static int __ldlm_namespace_free(struct ldlm_namespace *ns, int force)
{
	ENTRY;

	/* At shutdown time, don't call the cancellation callback */
	ldlm_namespace_cleanup(ns, force ? LDLM_FL_LOCAL_ONLY : 0);

	if (atomic_read(&ns->ns_bref) > 0) {
		struct l_wait_info lwi = LWI_INTR(LWI_ON_SIGNAL_NOOP, NULL);
		int rc;
		CDEBUG(D_DLMTRACE,
		       "dlm namespace %s free waiting on refcount %d\n",
		       ldlm_ns_name(ns), atomic_read(&ns->ns_bref));
force_wait:
		if (force)
			lwi = LWI_TIMEOUT(msecs_to_jiffies(obd_timeout *
					  MSEC_PER_SEC) / 4, NULL, NULL);

		rc = l_wait_event(ns->ns_waitq,
				  atomic_read(&ns->ns_bref) == 0, &lwi);

		/* Forced cleanups should be able to reclaim all references,
		 * so it's safe to wait forever... we can't leak locks... */
		if (force && rc == -ETIMEDOUT) {
			LCONSOLE_ERROR("Forced cleanup waiting for %s "
				       "namespace with %d resources in use, "
				       "(rc=%d)\n", ldlm_ns_name(ns),
				       atomic_read(&ns->ns_bref), rc);
			GOTO(force_wait, rc);
		}

		if (atomic_read(&ns->ns_bref)) {
			LCONSOLE_ERROR("Cleanup waiting for %s namespace "
				       "with %d resources in use, (rc=%d)\n",
				       ldlm_ns_name(ns),
				       atomic_read(&ns->ns_bref), rc);
			RETURN(ELDLM_NAMESPACE_EXISTS);
		}
		CDEBUG(D_DLMTRACE, "dlm namespace %s free done waiting\n",
		       ldlm_ns_name(ns));
	}

	RETURN(ELDLM_OK);
}

/**
 * Performs various cleanups for passed \a ns to make it drop refc and be
 * ready for freeing. Waits for refc == 0.
 *
 * The following is done:
 * (0) Unregister \a ns from its list to make inaccessible for potential
 * users like pools thread and others;
 * (1) Clear all locks in \a ns.
 */
void ldlm_namespace_free_prior(struct ldlm_namespace *ns,
                               struct obd_import *imp,
                               int force)
{
        int rc;
        ENTRY;
        if (!ns) {
                EXIT;
                return;
        }

	spin_lock(&ns->ns_lock);
	ns->ns_stopping = 1;
	spin_unlock(&ns->ns_lock);

        /*
         * Can fail with -EINTR when force == 0 in which case try harder.
         */
        rc = __ldlm_namespace_free(ns, force);
        if (rc != ELDLM_OK) {
                if (imp) {
                        ptlrpc_disconnect_import(imp, 0);
                        ptlrpc_invalidate_import(imp);
                }

                /*
                 * With all requests dropped and the import inactive
                 * we are gaurenteed all reference will be dropped.
                 */
                rc = __ldlm_namespace_free(ns, 1);
                LASSERT(rc == 0);
        }
        EXIT;
}

/**
 * Performs freeing memory structures related to \a ns. This is only done
 * when ldlm_namespce_free_prior() successfully removed all resources
 * referencing \a ns and its refc == 0.
 */
void ldlm_namespace_free_post(struct ldlm_namespace *ns)
{
        ENTRY;
        if (!ns) {
                EXIT;
                return;
        }

	/* Make sure that nobody can find this ns in its list. */
	ldlm_namespace_unregister(ns, ns->ns_client);
	/* Fini pool _before_ parent proc dir is removed. This is important as
	 * ldlm_pool_fini() removes own proc dir which is child to @dir.
	 * Removing it after @dir may cause oops. */
	ldlm_pool_fini(&ns->ns_pool);

	ldlm_namespace_proc_unregister(ns);
	cfs_hash_putref(ns->ns_rs_hash);
	/* Namespace \a ns should be not on list at this time, otherwise
	 * this will cause issues related to using freed \a ns in poold
	 * thread. */
	LASSERT(list_empty(&ns->ns_list_chain));
	OBD_FREE_PTR(ns);
	ldlm_put_ref();
	EXIT;
}

/**
 * Cleanup the resource, and free namespace.
 * bug 12864:
 * Deadlock issue:
 * proc1: destroy import
 *        class_disconnect_export(grab cl_sem) ->
 *              -> ldlm_namespace_free ->
 *              -> lprocfs_remove(grab _lprocfs_lock).
 * proc2: read proc info
 *        lprocfs_fops_read(grab _lprocfs_lock) ->
 *              -> osc_rd_active, etc(grab cl_sem).
 *
 * So that I have to split the ldlm_namespace_free into two parts - the first
 * part ldlm_namespace_free_prior is used to cleanup the resource which is
 * being used; the 2nd part ldlm_namespace_free_post is used to unregister the
 * lprocfs entries, and then free memory. It will be called w/o cli->cl_sem
 * held.
 */
void ldlm_namespace_free(struct ldlm_namespace *ns,
                         struct obd_import *imp,
                         int force)
{
        ldlm_namespace_free_prior(ns, imp, force);
        ldlm_namespace_free_post(ns);
}
EXPORT_SYMBOL(ldlm_namespace_free);

void ldlm_namespace_get(struct ldlm_namespace *ns)
{
	atomic_inc(&ns->ns_bref);
}
EXPORT_SYMBOL(ldlm_namespace_get);

/* This is only for callers that care about refcount */
static int ldlm_namespace_get_return(struct ldlm_namespace *ns)
{
	return atomic_inc_return(&ns->ns_bref);
}

void ldlm_namespace_put(struct ldlm_namespace *ns)
{
	if (atomic_dec_and_lock(&ns->ns_bref, &ns->ns_lock)) {
		wake_up(&ns->ns_waitq);
		spin_unlock(&ns->ns_lock);
	}
}
EXPORT_SYMBOL(ldlm_namespace_put);

/** Register \a ns in the list of namespaces */
void ldlm_namespace_register(struct ldlm_namespace *ns, ldlm_side_t client)
{
	mutex_lock(ldlm_namespace_lock(client));
	LASSERT(list_empty(&ns->ns_list_chain));
	list_add(&ns->ns_list_chain, ldlm_namespace_inactive_list(client));
	ldlm_namespace_nr_inc(client);
	mutex_unlock(ldlm_namespace_lock(client));
}

/** Unregister \a ns from the list of namespaces. */
void ldlm_namespace_unregister(struct ldlm_namespace *ns, ldlm_side_t client)
{
	mutex_lock(ldlm_namespace_lock(client));
	LASSERT(!list_empty(&ns->ns_list_chain));
	/* Some asserts and possibly other parts of the code are still
	 * using list_empty(&ns->ns_list_chain). This is why it is
	 * important to use list_del_init() here. */
	list_del_init(&ns->ns_list_chain);
	ldlm_namespace_nr_dec(client);
	mutex_unlock(ldlm_namespace_lock(client));
}

/** Should be called with ldlm_namespace_lock(client) taken. */
void ldlm_namespace_move_to_active_locked(struct ldlm_namespace *ns,
				       ldlm_side_t client)
{
	LASSERT(!list_empty(&ns->ns_list_chain));
	LASSERT(mutex_is_locked(ldlm_namespace_lock(client)));
	list_move_tail(&ns->ns_list_chain, ldlm_namespace_list(client));
}

/** Should be called with ldlm_namespace_lock(client) taken. */
void ldlm_namespace_move_to_inactive_locked(struct ldlm_namespace *ns,
					 ldlm_side_t client)
{
	LASSERT(!list_empty(&ns->ns_list_chain));
	LASSERT(mutex_is_locked(ldlm_namespace_lock(client)));
	list_move_tail(&ns->ns_list_chain,
		       ldlm_namespace_inactive_list(client));
}

/** Should be called with ldlm_namespace_lock(client) taken. */
struct ldlm_namespace *ldlm_namespace_first_locked(ldlm_side_t client)
{
	LASSERT(mutex_is_locked(ldlm_namespace_lock(client)));
	LASSERT(!list_empty(ldlm_namespace_list(client)));
	return container_of(ldlm_namespace_list(client)->next,
			    struct ldlm_namespace, ns_list_chain);
}

/** Create and initialize new resource. */
static struct ldlm_resource *ldlm_resource_new(void)
{
	struct ldlm_resource *res;
	int idx;

	OBD_SLAB_ALLOC_PTR_GFP(res, ldlm_resource_slab, GFP_NOFS);
	if (res == NULL)
		return NULL;

	INIT_LIST_HEAD(&res->lr_granted);
	INIT_LIST_HEAD(&res->lr_converting);
	INIT_LIST_HEAD(&res->lr_waiting);

	/* Initialize interval trees for each lock mode. */
	for (idx = 0; idx < LCK_MODE_NUM; idx++) {
		res->lr_itree[idx].lit_size = 0;
		res->lr_itree[idx].lit_mode = 1 << idx;
		res->lr_itree[idx].lit_root = NULL;
	}

	atomic_set(&res->lr_refcount, 1);
	spin_lock_init(&res->lr_lock);
	lu_ref_init(&res->lr_reference);

	/* Since LVB init can be delayed now, there is no longer need to
	 * immediatelly acquire mutex here. */
	mutex_init(&res->lr_lvb_mutex);
	res->lr_lvb_initialized = false;

	return res;
}

/**
 * Return a reference to resource with given name, creating it if necessary.
 * Args: namespace with ns_lock unlocked
 * Locks: takes and releases NS hash-lock and res->lr_lock
 * Returns: referenced, unlocked ldlm_resource or NULL
 */
struct ldlm_resource *
ldlm_resource_get(struct ldlm_namespace *ns, struct ldlm_resource *parent,
                  const struct ldlm_res_id *name, ldlm_type_t type, int create)
{
	struct hlist_node	*hnode;
	struct ldlm_resource	*res = NULL;
	cfs_hash_bd_t		bd;
	__u64			version;
	int			ns_refcount = 0;

        LASSERT(ns != NULL);
        LASSERT(parent == NULL);
        LASSERT(ns->ns_rs_hash != NULL);
        LASSERT(name->name[0] != 0);

        cfs_hash_bd_get_and_lock(ns->ns_rs_hash, (void *)name, &bd, 0);
        hnode = cfs_hash_bd_lookup_locked(ns->ns_rs_hash, &bd, (void *)name);
        if (hnode != NULL) {
                cfs_hash_bd_unlock(ns->ns_rs_hash, &bd, 0);
		GOTO(found, res);
	}

	version = cfs_hash_bd_version_get(&bd);
	cfs_hash_bd_unlock(ns->ns_rs_hash, &bd, 0);

	if (create == 0)
		return ERR_PTR(-ENOENT);

	LASSERTF(type >= LDLM_MIN_TYPE && type < LDLM_MAX_TYPE,
		 "type: %d\n", type);
	res = ldlm_resource_new();
	if (res == NULL)
		return ERR_PTR(-ENOMEM);

	res->lr_ns_bucket  = cfs_hash_bd_extra_get(ns->ns_rs_hash, &bd);
	res->lr_name       = *name;
	res->lr_type       = type;
	res->lr_most_restr = LCK_NL;

	cfs_hash_bd_lock(ns->ns_rs_hash, &bd, 1);
	hnode = (version == cfs_hash_bd_version_get(&bd)) ? NULL :
		cfs_hash_bd_lookup_locked(ns->ns_rs_hash, &bd, (void *)name);

	if (hnode != NULL) {
		/* Someone won the race and already added the resource. */
		cfs_hash_bd_unlock(ns->ns_rs_hash, &bd, 1);
		/* Clean lu_ref for failed resource. */
		lu_ref_fini(&res->lr_reference);
		OBD_SLAB_FREE(res, ldlm_resource_slab, sizeof *res);
found:
		res = hlist_entry(hnode, struct ldlm_resource, lr_hash);
		return res;
	}
	/* We won! Let's add the resource. */
        cfs_hash_bd_add_locked(ns->ns_rs_hash, &bd, &res->lr_hash);
	if (cfs_hash_bd_count_get(&bd) == 1)
		ns_refcount = ldlm_namespace_get_return(ns);

        cfs_hash_bd_unlock(ns->ns_rs_hash, &bd, 1);

	OBD_FAIL_TIMEOUT(OBD_FAIL_LDLM_CREATE_RESOURCE, 2);

	/* Let's see if we happened to be the very first resource in this
	 * namespace. If so, and this is a client namespace, we need to move
	 * the namespace into the active namespaces list to be patrolled by
	 * the ldlm_poold. */
	if (ns_is_client(ns) && ns_refcount == 1) {
		mutex_lock(ldlm_namespace_lock(LDLM_NAMESPACE_CLIENT));
		ldlm_namespace_move_to_active_locked(ns, LDLM_NAMESPACE_CLIENT);
		mutex_unlock(ldlm_namespace_lock(LDLM_NAMESPACE_CLIENT));
	}

	return res;
}
EXPORT_SYMBOL(ldlm_resource_get);

struct ldlm_resource *ldlm_resource_getref(struct ldlm_resource *res)
{
	LASSERT(res != NULL);
	LASSERT(res != LP_POISON);
	atomic_inc(&res->lr_refcount);
	CDEBUG(D_INFO, "getref res: %p count: %d\n", res,
	       atomic_read(&res->lr_refcount));
	return res;
}

static void __ldlm_resource_putref_final(cfs_hash_bd_t *bd,
                                         struct ldlm_resource *res)
{
        struct ldlm_ns_bucket *nsb = res->lr_ns_bucket;

	if (!list_empty(&res->lr_granted)) {
                ldlm_resource_dump(D_ERROR, res);
                LBUG();
        }

	if (!list_empty(&res->lr_converting)) {
                ldlm_resource_dump(D_ERROR, res);
                LBUG();
        }

	if (!list_empty(&res->lr_waiting)) {
                ldlm_resource_dump(D_ERROR, res);
                LBUG();
        }

        cfs_hash_bd_del_locked(nsb->nsb_namespace->ns_rs_hash,
                               bd, &res->lr_hash);
        lu_ref_fini(&res->lr_reference);
        if (cfs_hash_bd_count_get(bd) == 0)
                ldlm_namespace_put(nsb->nsb_namespace);
}

/* Returns 1 if the resource was freed, 0 if it remains. */
int ldlm_resource_putref(struct ldlm_resource *res)
{
	struct ldlm_namespace *ns = ldlm_res_to_ns(res);
	cfs_hash_bd_t   bd;

	LASSERT_ATOMIC_GT_LT(&res->lr_refcount, 0, LI_POISON);
	CDEBUG(D_INFO, "putref res: %p count: %d\n",
	       res, atomic_read(&res->lr_refcount) - 1);

	cfs_hash_bd_get(ns->ns_rs_hash, &res->lr_name, &bd);
	if (cfs_hash_bd_dec_and_lock(ns->ns_rs_hash, &bd, &res->lr_refcount)) {
		__ldlm_resource_putref_final(&bd, res);
		cfs_hash_bd_unlock(ns->ns_rs_hash, &bd, 1);
		if (ns->ns_lvbo && ns->ns_lvbo->lvbo_free)
			ns->ns_lvbo->lvbo_free(res);
		OBD_SLAB_FREE(res, ldlm_resource_slab, sizeof *res);
		return 1;
	}
	return 0;
}
EXPORT_SYMBOL(ldlm_resource_putref);

/* Returns 1 if the resource was freed, 0 if it remains. */
int ldlm_resource_putref_locked(struct ldlm_resource *res)
{
	struct ldlm_namespace *ns = ldlm_res_to_ns(res);

	LASSERT_ATOMIC_GT_LT(&res->lr_refcount, 0, LI_POISON);
	CDEBUG(D_INFO, "putref res: %p count: %d\n",
	       res, atomic_read(&res->lr_refcount) - 1);

	if (atomic_dec_and_test(&res->lr_refcount)) {
		cfs_hash_bd_t bd;

		cfs_hash_bd_get(ldlm_res_to_ns(res)->ns_rs_hash,
				&res->lr_name, &bd);
		__ldlm_resource_putref_final(&bd, res);
		cfs_hash_bd_unlock(ns->ns_rs_hash, &bd, 1);
		/* NB: ns_rs_hash is created with CFS_HASH_NO_ITEMREF,
		 * so we should never be here while calling cfs_hash_del,
		 * cfs_hash_for_each_nolock is the only case we can get
		 * here, which is safe to release cfs_hash_bd_lock.
		 */
		if (ns->ns_lvbo && ns->ns_lvbo->lvbo_free)
			ns->ns_lvbo->lvbo_free(res);
		OBD_SLAB_FREE(res, ldlm_resource_slab, sizeof *res);

		cfs_hash_bd_lock(ns->ns_rs_hash, &bd, 1);
		return 1;
	}
	return 0;
}

/**
 * Add a lock into a given resource into specified lock list.
 */
void ldlm_resource_add_lock(struct ldlm_resource *res, struct list_head *head,
                            struct ldlm_lock *lock)
{
	check_res_locked(res);

	LDLM_DEBUG(lock, "About to add this lock:\n");

	if (ldlm_is_destroyed(lock)) {
		CDEBUG(D_OTHER, "Lock destroyed, not adding to resource\n");
		return;
	}

	LASSERT(list_empty(&lock->l_res_link));

	list_add_tail(&lock->l_res_link, head);
}

/**
 * Insert a lock into resource after specified lock.
 *
 * Obtain resource description from the lock we are inserting after.
 */
void ldlm_resource_insert_lock_after(struct ldlm_lock *original,
                                     struct ldlm_lock *new)
{
        struct ldlm_resource *res = original->l_resource;

        check_res_locked(res);

        ldlm_resource_dump(D_INFO, res);
        LDLM_DEBUG(new, "About to insert this lock after %p:\n", original);

	if (ldlm_is_destroyed(new)) {
		CDEBUG(D_OTHER, "Lock destroyed, not adding to resource\n");
		goto out;
	}

	LASSERT(list_empty(&new->l_res_link));

	list_add(&new->l_res_link, &original->l_res_link);
 out:;
}

void ldlm_resource_unlink_lock(struct ldlm_lock *lock)
{
        int type = lock->l_resource->lr_type;

        check_res_locked(lock->l_resource);
        if (type == LDLM_IBITS || type == LDLM_PLAIN)
                ldlm_unlink_lock_skiplist(lock);
        else if (type == LDLM_EXTENT)
                ldlm_extent_unlink_lock(lock);
	list_del_init(&lock->l_res_link);
}
EXPORT_SYMBOL(ldlm_resource_unlink_lock);

void ldlm_res2desc(struct ldlm_resource *res, struct ldlm_resource_desc *desc)
{
        desc->lr_type = res->lr_type;
        desc->lr_name = res->lr_name;
}

/**
 * Print information about all locks in all namespaces on this node to debug
 * log.
 */
void ldlm_dump_all_namespaces(ldlm_side_t client, int level)
{
	struct list_head *tmp;

        if (!((libcfs_debug | D_ERROR) & level))
                return;

	mutex_lock(ldlm_namespace_lock(client));

	list_for_each(tmp, ldlm_namespace_list(client)) {
                struct ldlm_namespace *ns;
		ns = list_entry(tmp, struct ldlm_namespace, ns_list_chain);
                ldlm_namespace_dump(level, ns);
        }

	mutex_unlock(ldlm_namespace_lock(client));
}
EXPORT_SYMBOL(ldlm_dump_all_namespaces);

static int ldlm_res_hash_dump(cfs_hash_t *hs, cfs_hash_bd_t *bd,
			      struct hlist_node *hnode, void *arg)
{
        struct ldlm_resource *res = cfs_hash_object(hs, hnode);
        int    level = (int)(unsigned long)arg;

        lock_res(res);
        ldlm_resource_dump(level, res);
        unlock_res(res);

        return 0;
}

/**
 * Print information about all locks in this namespace on this node to debug
 * log.
 */
void ldlm_namespace_dump(int level, struct ldlm_namespace *ns)
{
	if (!((libcfs_debug | D_ERROR) & level))
		return;

	CDEBUG(level, "--- Namespace: %s (rc: %d, side: %s)\n",
	       ldlm_ns_name(ns), atomic_read(&ns->ns_bref),
	       ns_is_client(ns) ? "client" : "server");

	if (cfs_time_before(cfs_time_current(), ns->ns_next_dump))
		return;

	cfs_hash_for_each_nolock(ns->ns_rs_hash,
				 ldlm_res_hash_dump,
				 (void *)(unsigned long)level);
	spin_lock(&ns->ns_lock);
	ns->ns_next_dump = cfs_time_shift(10);
	spin_unlock(&ns->ns_lock);
}
EXPORT_SYMBOL(ldlm_namespace_dump);

/**
 * Print information about all locks in this resource to debug log.
 */
void ldlm_resource_dump(int level, struct ldlm_resource *res)
{
	struct ldlm_lock *lock;
	unsigned int granted = 0;

	CLASSERT(RES_NAME_SIZE == 4);

	if (!((libcfs_debug | D_ERROR) & level))
		return;

	CDEBUG(level, "--- Resource: "DLDLMRES" (%p) refcount = %d\n",
	       PLDLMRES(res), res, atomic_read(&res->lr_refcount));

	if (!list_empty(&res->lr_granted)) {
		CDEBUG(level, "Granted locks (in reverse order):\n");
		list_for_each_entry_reverse(lock, &res->lr_granted,
						l_res_link) {
                        LDLM_DEBUG_LIMIT(level, lock, "###");
                        if (!(level & D_CANTMASK) &&
                            ++granted > ldlm_dump_granted_max) {
                                CDEBUG(level, "only dump %d granted locks to "
                                       "avoid DDOS.\n", granted);
                                break;
                        }
                }
        }
	if (!list_empty(&res->lr_converting)) {
                CDEBUG(level, "Converting locks:\n");
		list_for_each_entry(lock, &res->lr_converting, l_res_link)
                        LDLM_DEBUG_LIMIT(level, lock, "###");
        }
	if (!list_empty(&res->lr_waiting)) {
                CDEBUG(level, "Waiting locks:\n");
		list_for_each_entry(lock, &res->lr_waiting, l_res_link)
                        LDLM_DEBUG_LIMIT(level, lock, "###");
        }
}
EXPORT_SYMBOL(ldlm_resource_dump);

/**
 * Clears the lustre cache for the namespace \a ns.
 *
 * \param[in] ns the namespace to clear
 *
 * \retval 0 if all unused locks in \a ns are cleared
 * \retval -EINVAL if clearing all unused locks fails
 */
int ldlm_ns_drop_cache(struct ldlm_namespace *ns)
{
	unsigned long tmp;
	CDEBUG(D_DLMTRACE,
	       "dropping all unused locks from namespace %s\n",
	       ldlm_ns_name(ns));
	if (ns_connect_lru_resize(ns)) {
		int canceled, unused  = ns->ns_nr_unused;
		/* Try to cancel all @ns_nr_unused locks. */
		canceled = ldlm_cancel_lru(ns, unused, 0,
					   LDLM_CANCEL_PASSED |
					   LDLM_CANCEL_CLEANUP);
		if (canceled < unused) {
			CDEBUG(D_DLMTRACE,
			       "not all requested locks are canceled, "
			       "requested: %d, canceled: %d\n", unused,
			       canceled);
			return -EINVAL;
		}
	} else {
		tmp = ns->ns_max_unused;
		ns->ns_max_unused = 0;
		ldlm_cancel_lru(ns, 0, 0, LDLM_CANCEL_PASSED |
					  LDLM_CANCEL_CLEANUP);
		ns->ns_max_unused = tmp;
	}

	return 0;
}

/**
 * Indicates whether the workq is empty.  Note that this answer may change
 * between calling this function and the next instruction.
 *
 * \param[in] workq pointer to the workq
 * \retval 1 if \a workq is empty
 * \retval 0 if \a workq is nonempty
 */
int ldlm_dc_workq_empty(struct ldlm_dc_workq *workq)
{
	return atomic_read(&workq->dcwq_cur_index) < 0;
}

/**
 * Gets the next work item from the drop_caches work queue.  This is
 * thread-safe.  That is, no two threads will get the same work item, and
 * each work item is returned once.
 *
 * \param[in] workq pointer to the workq
 * \retval pointer to the next work item in \a workq
 * \retval NULL if \a workq is empty
 */
struct ldlm_dc_work_item *ldlm_dc_get_work_item(struct ldlm_dc_workq *workq)
{
	int cur = atomic_dec_return(&workq->dcwq_cur_index);
	if (cur < 0)
		return NULL;
	return &workq->dcwq_work_items[cur];
}

/**
 * Cache-clearing worker thread function.  Takes work items from the work
 * queue until it is empty.
 *
 * \param[in] arg pointer to ldlm_dc_ctl struct
 * \retval 0 always
 */
static int ldlm_drop_cachesd(void *arg)
{
	struct ldlm_dc_ctl *dc_ctl = arg;
	struct ldlm_dc_workq *workq = dc_ctl->dcc_workq;
	struct ldlm_dc_work_item *work_item;

	work_item = ldlm_dc_get_work_item(workq);

	while (work_item != NULL) {
		work_item->dcwi_rc = ldlm_ns_drop_cache(work_item->dcwi_ns);
		ldlm_namespace_put(work_item->dcwi_ns);
		work_item->dcwi_ns_needs_put = 0;
		work_item = ldlm_dc_get_work_item(workq);
	}

	complete(&dc_ctl->dcc_finished);
	/* Always return 0 since ldlm_drop_caches uses dcwi_rc of the work
	 * item instead of the actual return code from the thread. */
	return 0;
}

/**
 * Creates a work queue of namespaces for ldlm_drop_caches.
 *
 * \param[in] client indicates whether to drop cache for client or server
 * \retval pointer to the workq
 * \retval an ERR_PTR if there was a problem
 */
struct ldlm_dc_workq *ldlm_dc_get_workq(ldlm_side_t client)
{
	struct list_head *tmp;
	int num_namespaces = ldlm_namespace_nr_read(client);
	struct ldlm_dc_workq *workq;
	int workq_size = offsetof(struct ldlm_dc_workq,
				  dcwq_work_items[num_namespaces]);

	LIBCFS_ALLOC(workq, workq_size);
	if (workq == NULL) {
		CERROR("Failed to allocate %d bytes of memory for dc_workq.\n",
		       workq_size);
		return ERR_PTR(-ENOMEM);
	}

	workq->dcwq_size = workq_size;
	workq->dcwq_num_wi = 0;
	mutex_lock(ldlm_namespace_lock(client));

	/* This actually only iterates through the active namespace list. */
	list_for_each(tmp, ldlm_namespace_list(client)) {
		struct ldlm_namespace *ns;
		struct ldlm_dc_work_item *wi;
		/* The size of the namespace list may have increased since we
		 * allocated workq, so make sure not to write off the end. */
		if (workq->dcwq_num_wi >= num_namespaces) {
			CDEBUG(D_DLMTRACE,
			       "Number of namespaces increased from %d to %d. "
			       "Locks in some namespaces may not be cleared.\n",
			       num_namespaces, ldlm_namespace_nr_read(client));
			break;
		}

		wi = &workq->dcwq_work_items[workq->dcwq_num_wi];
		ns = list_entry(tmp, struct ldlm_namespace, ns_list_chain);

		/* Increment the ref count of the namespace so that it doesn't
		 * get freed before it is accessed by ldlm_drop_cachesd. */
		ldlm_namespace_get(ns);
		wi->dcwi_ns = ns;
		wi->dcwi_rc = 0;
		wi->dcwi_ns_needs_put = 1;

		workq->dcwq_num_wi++;
	}

	mutex_unlock(ldlm_namespace_lock(client));

	atomic_set(&workq->dcwq_cur_index, workq->dcwq_num_wi);

	return workq;
}

/**
 * Clears lustre caches for all namespaces.
 *
 * \param[in] client indicates whether to drop cache for client or server
 * \retval 0 if unused locks are cleared for all namespaces
 * \retval negative error code if there was a problem
 */
int ldlm_drop_caches(ldlm_side_t client)
{
	int i;
	int rc = 0;
	struct task_struct *task;
	int dc_ctls_size;
	struct ldlm_dc_ctl *dc_ctls;
	int num_threads = LDLM_DC_MAX_THREADS;
	int num_threads_created = 0;

	struct ldlm_dc_workq *workq;

	workq = ldlm_dc_get_workq(client);
	if (IS_ERR_VALUE(PTR_ERR(workq)))
		return PTR_ERR(workq);

	if (workq->dcwq_num_wi == 0)
		GOTO(out, rc = 0);

	if (num_threads > workq->dcwq_num_wi)
		num_threads = workq->dcwq_num_wi;

	dc_ctls_size = num_threads * sizeof(*dc_ctls);
	LIBCFS_ALLOC(dc_ctls, dc_ctls_size);
	if (dc_ctls == NULL) {
		CERROR("Failed to allocate %d bytes of memory for dc_ctls.\n",
		       dc_ctls_size);
		GOTO(out, rc = -ENOMEM);
	}

	for (i = 0; i < num_threads; i++) {
		init_completion(&dc_ctls[i].dcc_finished);
		dc_ctls[i].dcc_workq = workq;

		if (ldlm_dc_workq_empty(workq))
			break;

		task = kthread_run(ldlm_drop_cachesd, &dc_ctls[i],
				   "ldlm_drop_cachesd");

		if (IS_ERR(task)) {
			rc = PTR_ERR(task);
			CERROR("namespace cleanup thread %d/%d creation error: "
			       "rc = %d\n", i + 1, num_threads, rc);
			break;
		}
		num_threads_created++;
	}

	for (i = 0; i < num_threads_created; i++) 
		wait_for_completion(&dc_ctls[i].dcc_finished);

	for (i = 0; i < workq->dcwq_num_wi; i++) {
		if (workq->dcwq_work_items[i].dcwi_rc < 0 && rc == 0)
			rc = workq->dcwq_work_items[i].dcwi_rc;

		/* Make sure each namespace has its ref count decremented */
		if (workq->dcwq_work_items[i].dcwi_ns_needs_put)
			ldlm_namespace_put(workq->dcwq_work_items[i].dcwi_ns);
	}

	LIBCFS_FREE(dc_ctls, dc_ctls_size);

out:
	LIBCFS_FREE(workq, workq->dcwq_size);

	return rc;
}
EXPORT_SYMBOL(ldlm_drop_caches);
