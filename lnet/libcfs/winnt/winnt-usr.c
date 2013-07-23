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

#ifndef __KERNEL__

#include <stdio.h>
#include <stdlib.h>
#include <io.h>
#include <time.h>
#include <windows.h>

void portals_debug_msg(int subsys, int mask, char *file, const char *fn,
                              const int line, unsigned long stack,
                              char *format, ...) {
    }

int cfs_proc_mknod(const char *path, unsigned short  mode,  unsigned int dev)
{
    return 0;
}


void print_last_error(char* Prefix)
{
    LPVOID lpMsgBuf;

    FormatMessage( 
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        GetLastError(),
        0,
        (LPTSTR) &lpMsgBuf,
        0,
        NULL
        );

    printf("%s %s", Prefix, (LPTSTR) lpMsgBuf);

    LocalFree(lpMsgBuf);
}

//
// The following declarations are defined in io.h of VC
// sys/types.h will conflict with io.h, so we need place
// these declartions here.

#ifdef __cplusplus
extern "C" {
#endif
    void
    __declspec (naked) __cdecl _chkesp(void)
    {
#if _X86_
        __asm {  jz      exit_chkesp     };
        __asm {  int     3               };
    exit_chkesp:
        __asm {  ret                     };
#endif
    }
#ifdef __cplusplus
}
#endif

unsigned int sleep (unsigned int seconds)
{
    Sleep(seconds * 1000);
    return 0;
}

int gethostname(char * name, int namelen)
{
    return 0;
}

int ioctl (
    int handle,
    int cmd,
    void *buffer
    )
{
    printf("hello, world\n");
    return 0;
}

#endif /* __KERNEL__ */
