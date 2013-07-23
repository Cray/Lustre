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
#define DEBUG_SUBSYSTEM S_LNET

#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/highmem.h>
#include <libcfs/libcfs.h>

static unsigned int cfs_alloc_flags_to_gfp(u_int32_t flags)
{
	unsigned int mflags = 0;

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,5,0))
        if (flags & CFS_ALLOC_ATOMIC)
                mflags |= __GFP_HIGH;
        else if (flags & CFS_ALLOC_WAIT)
                mflags |= __GFP_WAIT;
        else
                mflags |= (__GFP_HIGH | __GFP_WAIT);
        if (flags & CFS_ALLOC_IO)
                mflags |= __GFP_IO | __GFP_HIGHIO;
#else
        if (flags & CFS_ALLOC_ATOMIC)
                mflags |= __GFP_HIGH;
        else
                mflags |= __GFP_WAIT;
        if (flags & CFS_ALLOC_NOWARN)
                mflags |= __GFP_NOWARN;
        if (flags & CFS_ALLOC_IO)
                mflags |= __GFP_IO;
#endif
        if (flags & CFS_ALLOC_FS)
                mflags |= __GFP_FS;
        if (flags & CFS_ALLOC_HIGH)
                mflags |= __GFP_HIGH;
        return mflags;
}

void *
cfs_alloc(size_t nr_bytes, u_int32_t flags)
{
	void *ptr = NULL;
        int *ltd = JOURNAL_ENTER();

	ptr = kmalloc(nr_bytes, cfs_alloc_flags_to_gfp(flags));
	if (ptr != NULL && (flags & CFS_ALLOC_ZERO))
		memset(ptr, 0, nr_bytes);

        JOURNAL_EXIT(ltd);

	return ptr;
}

void
cfs_free(void *addr)
{
	kfree(addr);
}

void *
cfs_alloc_large(size_t nr_bytes)
{
	return vmalloc(nr_bytes);
}

void
cfs_free_large(void *addr)
{
	vfree(addr);
}

cfs_page_t *cfs_alloc_pages(unsigned int flags, unsigned int order)
{
        cfs_page_t *pages = NULL;
        int *ltd = JOURNAL_ENTER();

        /*
         * XXX nikita: do NOT call portals_debug_msg() (CDEBUG/ENTRY/EXIT)
         * from here: this will lead to infinite recursion.
         */
        pages = alloc_pages(cfs_alloc_flags_to_gfp(flags), order);

        JOURNAL_EXIT(ltd);

        return pages;
}

void __cfs_free_pages(cfs_page_t *page, unsigned int order)
{
        __free_pages(page, order);
}

cfs_mem_cache_t *
cfs_mem_cache_create (const char *name, size_t size, size_t offset,
                      unsigned long flags)
{
#ifdef HAVE_KMEM_CACHE_CREATE_DTOR
        return kmem_cache_create(name, size, offset, flags, NULL, NULL);
#else
        return kmem_cache_create(name, size, offset, flags, NULL);
#endif
}

int
cfs_mem_cache_destroy (cfs_mem_cache_t * cachep)
{
#ifdef HAVE_KMEM_CACHE_DESTROY_INT
        return kmem_cache_destroy(cachep);
#else
        kmem_cache_destroy(cachep);
        return 0;
#endif
}

void *
cfs_mem_cache_alloc(cfs_mem_cache_t *cachep, int flags)
{
        void *obj = NULL;
        int *ltd = JOURNAL_ENTER();

        obj = kmem_cache_alloc(cachep, cfs_alloc_flags_to_gfp(flags));

        JOURNAL_EXIT(ltd);

        return obj;
}

void
cfs_mem_cache_free(cfs_mem_cache_t *cachep, void *objp)
{
        return kmem_cache_free(cachep, objp);
}

EXPORT_SYMBOL(cfs_alloc);
EXPORT_SYMBOL(cfs_free);
EXPORT_SYMBOL(cfs_alloc_large);
EXPORT_SYMBOL(cfs_free_large);
EXPORT_SYMBOL(cfs_alloc_pages);
EXPORT_SYMBOL(__cfs_free_pages);
EXPORT_SYMBOL(cfs_mem_cache_create);
EXPORT_SYMBOL(cfs_mem_cache_destroy);
EXPORT_SYMBOL(cfs_mem_cache_alloc);
EXPORT_SYMBOL(cfs_mem_cache_free);
