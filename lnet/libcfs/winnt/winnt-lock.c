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


# define DEBUG_SUBSYSTEM S_LNET

#include <libcfs/libcfs.h>


#if _X86_

void __declspec (naked) FASTCALL
atomic_add(
    int i,
    atomic_t *v
    )
{
    // ECX = i
    // EDX = v ; [EDX][0] = v->counter

    __asm {
        lock add dword ptr [edx][0], ecx
        ret
    }
}

void __declspec (naked) FASTCALL
atomic_sub(
    int i,
    atomic_t *v
   ) 
{
    // ECX = i
    // EDX = v ; [EDX][0] = v->counter

    __asm {
        lock sub dword ptr [edx][0], ecx
        ret
    }
}

void __declspec (naked) FASTCALL
atomic_inc(
    atomic_t *v
    )
{
    //InterlockedIncrement((PULONG)(&((v)->counter)));

    //` ECX = v ; [ECX][0] = v->counter

    __asm {
        lock inc dword ptr [ecx][0]
        ret
    }
}

void __declspec (naked) FASTCALL
atomic_dec(
    atomic_t *v
    )
{
    // ECX = v ; [ECX][0] = v->counter

    __asm {
        lock dec dword ptr [ecx][0]
        ret
    }
}

int __declspec (naked) FASTCALL 
atomic_sub_and_test(
    int i,
    atomic_t *v
    )
{

    // ECX = i
    // EDX = v ; [EDX][0] = v->counter

    __asm {
        xor eax, eax
        lock sub dword ptr [edx][0], ecx
        sete al
        ret
    }
}

int __declspec (naked) FASTCALL
atomic_inc_and_test(
    atomic_t *v
    )
{
    // ECX = v ; [ECX][0] = v->counter

    __asm {
        xor eax, eax
        lock inc dword ptr [ecx][0]
        sete al
        ret
    }
}

int __declspec (naked) FASTCALL
atomic_dec_and_test(
    atomic_t *v
    )
{
    // ECX = v ; [ECX][0] = v->counter

    __asm {
        xor eax, eax
        lock dec dword ptr [ecx][0]
        sete al
        ret
    }
}

#else

void FASTCALL
atomic_add(
    int i,
    atomic_t *v
    )
{
    InterlockedExchangeAdd( (PULONG)(&((v)->counter)) , (LONG) (i));
}

void FASTCALL
atomic_sub(
    int i,
    atomic_t *v
   ) 
{
    InterlockedExchangeAdd( (PULONG)(&((v)->counter)) , (LONG) (-1*i));
}

void FASTCALL
atomic_inc(
    atomic_t *v
    )
{
   InterlockedIncrement((PULONG)(&((v)->counter)));
}

void FASTCALL
atomic_dec(
    atomic_t *v
    )
{
    InterlockedDecrement((PULONG)(&((v)->counter)));
}

int FASTCALL 
atomic_sub_and_test(
    int i,
    atomic_t *v
    )
{
    int counter, result;

    do {

        counter = v->counter;
        result = counter - i;

    } while ( InterlockedCompareExchange(
                &(v->counter),
                result,
                counter) !=  counter);

    return (result == 0);
}

int FASTCALL
atomic_inc_and_test(
    atomic_t *v
    )
{
    int counter, result;

    do {

        counter = v->counter;
        result = counter + 1;

    } while ( InterlockedCompareExchange(
                &(v->counter),
                result,
                counter) !=  counter);

    return (result == 0);
}

int FASTCALL
atomic_dec_and_test(
    atomic_t *v
    )
{
    int counter, result;

    do {

        counter = v->counter;
        result = counter + 1;

    } while ( InterlockedCompareExchange(
                &(v->counter),
                result,
                counter) !=  counter);

    return (result == 0);
}

#endif


/*
 * rw spinlock
 */


void
rwlock_init(rwlock_t * rwlock)
{
    spin_lock_init(&rwlock->guard);
    rwlock->count = 0;
}

void
rwlock_fini(rwlock_t * rwlock)
{
}

void
read_lock(rwlock_t * rwlock)
{
    cfs_task_t * task = cfs_current();
    PTASK_SLOT   slot = NULL;

    if (!task) {
        /* should bugchk here */
        cfs_enter_debugger();
        return;
    }

    slot = CONTAINING_RECORD(task, TASK_SLOT, task);
    ASSERT(slot->Magic == TASKSLT_MAGIC);
   
    slot->irql = KeRaiseIrqlToDpcLevel();

    while (TRUE) {
	    spin_lock(&rwlock->guard);
        if (rwlock->count >= 0)
            break;
        spin_unlock(&rwlock->guard);
    }

	rwlock->count++;
	spin_unlock(&rwlock->guard);
}

void
read_unlock(rwlock_t * rwlock)
{
    cfs_task_t * task = cfs_current();
    PTASK_SLOT   slot = NULL;

    if (!task) {
        /* should bugchk here */
        cfs_enter_debugger();
        return;
    }

    slot = CONTAINING_RECORD(task, TASK_SLOT, task);
    ASSERT(slot->Magic == TASKSLT_MAGIC);
   
    spin_lock(&rwlock->guard);
	ASSERT(rwlock->count > 0);
    rwlock->count--;
    if (rwlock < 0) {
        cfs_enter_debugger();
    }
	spin_unlock(&rwlock->guard);

    KeLowerIrql(slot->irql);
}

void
write_lock(rwlock_t * rwlock)
{
    cfs_task_t * task = cfs_current();
    PTASK_SLOT   slot = NULL;

    if (!task) {
        /* should bugchk here */
        cfs_enter_debugger();
        return;
    }

    slot = CONTAINING_RECORD(task, TASK_SLOT, task);
    ASSERT(slot->Magic == TASKSLT_MAGIC);
   
    slot->irql = KeRaiseIrqlToDpcLevel();

    while (TRUE) {
	    spin_lock(&rwlock->guard);
        if (rwlock->count == 0)
            break;
        spin_unlock(&rwlock->guard);
    }

	rwlock->count = -1;
	spin_unlock(&rwlock->guard);
}

void
write_unlock(rwlock_t * rwlock)
{
    cfs_task_t * task = cfs_current();
    PTASK_SLOT   slot = NULL;

    if (!task) {
        /* should bugchk here */
        cfs_enter_debugger();
        return;
    }

    slot = CONTAINING_RECORD(task, TASK_SLOT, task);
    ASSERT(slot->Magic == TASKSLT_MAGIC);
   
    spin_lock(&rwlock->guard);
	ASSERT(rwlock->count == -1);
    rwlock->count = 0;
	spin_unlock(&rwlock->guard);

    KeLowerIrql(slot->irql);
}
