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
 *
 * lnet/libcfs/darwin/darwin-curproc.c
 *
 * Lustre curproc API implementation for XNU kernel
 *
 * Author: Nikita Danilov <nikita@clusterfs.com>
 */

#define DEBUG_SUBSYSTEM S_LNET

#include <libcfs/libcfs.h>
#include <libcfs/kp30.h>

/*
 * Implementation of cfs_curproc API (see lnet/include/libcfs/curproc.h)
 * for XNU kernel.
 */

static inline struct ucred *curproc_ucred(void)
{
#ifdef __DARWIN8__
        return proc_ucred(current_proc());
#else
        return current_proc()->p_cred->pc_ucred;
#endif
}

uid_t  cfs_curproc_uid(void)
{
        return curproc_ucred()->cr_uid;
}

gid_t  cfs_curproc_gid(void)
{
        LASSERT(curproc_ucred()->cr_ngroups > 0);
        return curproc_ucred()->cr_groups[0];
}

uid_t  cfs_curproc_fsuid(void)
{
#ifdef __DARWIN8__
        return curproc_ucred()->cr_ruid;
#else
        return current_proc()->p_cred->p_ruid;
#endif
}

gid_t  cfs_curproc_fsgid(void)
{
#ifdef __DARWIN8__
        return curproc_ucred()->cr_rgid;
#else
        return current_proc()->p_cred->p_rgid;
#endif
}

pid_t  cfs_curproc_pid(void)
{
#ifdef __DARWIN8__
        /* no pid for each thread, return address of thread struct */
        return (pid_t)current_thread();
#else
        return current_proc()->p_pid;
#endif
}

int    cfs_curproc_groups_nr(void)
{
        LASSERT(curproc_ucred()->cr_ngroups > 0);
        return curproc_ucred()->cr_ngroups - 1;
}

int    cfs_curproc_is_in_groups(gid_t gid)
{
        int i;
        struct ucred *cr;

        cr = curproc_ucred();
        LASSERT(cr != NULL);

        for (i = 0; i < cr->cr_ngroups; ++ i) {
                if (cr->cr_groups[i] == gid)
                        return 1;
        }
        return 0;
}

void   cfs_curproc_groups_dump(gid_t *array, int size)
{
        struct ucred *cr;

        cr = curproc_ucred();
        LASSERT(cr != NULL);
        CLASSERT(sizeof array[0] == sizeof (__u32));

        size = min_t(int, size, cr->cr_ngroups);
        memcpy(array, &cr->cr_groups[1], size * sizeof(gid_t));
}

mode_t cfs_curproc_umask(void)
{
#ifdef __DARWIN8__
        /*
         * XXX Liang:
         *
         * fd_cmask is not available in kexts, so we just assume 
         * verything is permited.
         */
        return -1;
#else
        return current_proc()->p_fd->fd_cmask;
#endif
}

char  *cfs_curproc_comm(void)
{
#ifdef __DARWIN8__
        /*
         * Writing to proc->p_comm is not permited in Darwin8,
         * because proc_selfname() only return a copy of proc->p_comm,
         * so this function is not really working while user try to 
         * change comm of current process.
         */
        static char     pcomm[MAXCOMLEN+1];

        proc_selfname(pcomm, MAXCOMLEN+1);
        return pcomm;
#else
        return current_proc()->p_comm;
#endif
}

void cfs_cap_raise(cfs_cap_t cap) {}
void cfs_cap_lower(cfs_cap_t cap) {}

int cfs_cap_raised(cfs_cap_t cap)
{
        return 1;
}

void cfs_kernel_cap_pack(kernel_cap_t *kcap, cfs_cap_t cap) {}
void cfs_kernel_cap_unpack(kernel_cap_t *kcap, cfs_cap_t cap) {}

cfs_cap_t cfs_curproc_cap_pack(void) {
        return -1;
}

void cfs_curproc_cap_unpack(cfs_cap_t cap) {}

int cfs_capable(cfs_cap_t cap)
{
        return cap == CFS_CAP_SYS_BOOT ? is_suser(): is_suser1();
}

/*
 * Local variables:
 * c-indentation-style: "K&R"
 * c-basic-offset: 8
 * tab-width: 8
 * fill-column: 80
 * scroll-step: 1
 * End:
 */
