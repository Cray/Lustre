/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Lustre Light common routines
 *
 *  Copyright (c) 2002-2004 Cluster File Systems, Inc.
 *
 *   This file is part of Lustre, http://www.lustre.org.
 *
 *   Lustre is free software; you can redistribute it and/or
 *   modify it under the terms of version 2 of the GNU General Public
 *   License as published by the Free Software Foundation.
 *
 *   Lustre is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Lustre; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/queue.h>

#include <sysio.h>
#include <fs.h>
#include <mount.h>
#include <inode.h>
#include <file.h>

#ifdef REDSTORM
#define CSTART_INIT
#endif

/* both sys/queue.h (libsysio require it) and portals/lists.h have definition
 * of 'LIST_HEAD'. undef it to suppress warnings
 */
#undef LIST_HEAD
#include <portals/ptlctl.h>

#include "lutil.h"
#include "llite_lib.h"

static int lllib_init(void)
{
        liblustre_set_nal_nid();

        if (liblustre_init_current("dummy") ||
            init_obdclass() ||
            init_lib_portals() ||
            ptlrpc_init() ||
            mdc_init() ||
            lov_init() ||
            osc_init())
                return -1;

        return _sysio_fssw_register("llite", &llu_fssw_ops);
}
 
#ifndef CRAY_PORTALS
#define LIBLUSTRE_NAL_NAME "tcp"
#elif defined REDSTORM
#define LIBLUSTRE_NAL_NAME "cray_qk_ernal"
#else
#define LIBLUSTRE_NAL_NAME "cray_pb_ernal"
#endif

int liblustre_process_log(struct config_llog_instance *cfg, int allow_recov)
{
        struct lustre_cfg lcfg;
        char  *peer = "MDS_PEER_UUID";
        struct obd_device *obd;
        struct lustre_handle mdc_conn = {0, };
        struct obd_export *exp;
        char  *name = "mdc_dev";
        class_uuid_t uuid;
        struct obd_uuid mdc_uuid;
        struct llog_ctxt *ctxt;
        ptl_nid_t nid = 0;
        int nal, err, rc = 0;
        ENTRY;

        generate_random_uuid(uuid);
        class_uuid_unparse(uuid, &mdc_uuid);

        if (ptl_parse_nid(&nid, g_zconf_mdsnid)) {
                CERROR("Can't parse NID %s\n", g_zconf_mdsnid);
                RETURN(-EINVAL);
        }

        nal = ptl_name2nal(LIBLUSTRE_NAL_NAME);
        if (nal <= 0) {
                CERROR("Can't parse NAL %s\n", LIBLUSTRE_NAL_NAME);
                RETURN(-EINVAL);
        }
        LCFG_INIT(lcfg, LCFG_ADD_UUID, name);
        lcfg.lcfg_nid = nid;
        lcfg.lcfg_inllen1 = strlen(peer) + 1;
        lcfg.lcfg_inlbuf1 = peer;
        lcfg.lcfg_nal = nal;
        err = class_process_config(&lcfg);
        if (err < 0)
                GOTO(out, err);

        LCFG_INIT(lcfg, LCFG_ATTACH, name);
        lcfg.lcfg_inlbuf1 = "mdc";
        lcfg.lcfg_inllen1 = strlen(lcfg.lcfg_inlbuf1) + 1;
        lcfg.lcfg_inlbuf2 = mdc_uuid.uuid;
        lcfg.lcfg_inllen2 = strlen(lcfg.lcfg_inlbuf2) + 1;
        err = class_process_config(&lcfg);
        if (err < 0)
                GOTO(out_del_uuid, err);

        LCFG_INIT(lcfg, LCFG_SETUP, name);
        lcfg.lcfg_inlbuf1 = g_zconf_mdsname;
        lcfg.lcfg_inllen1 = strlen(lcfg.lcfg_inlbuf1) + 1;
        lcfg.lcfg_inlbuf2 = peer;
        lcfg.lcfg_inllen2 = strlen(lcfg.lcfg_inlbuf2) + 1;
        err = class_process_config(&lcfg);
        if (err < 0)
                GOTO(out_detach, err);
        
        obd = class_name2obd(name);
        if (obd == NULL)
                GOTO(out_cleanup, err = -EINVAL);

        /* Disable initial recovery on this import */
        err = obd_set_info(obd->obd_self_export,
                           strlen("initial_recov"), "initial_recov",
                           sizeof(allow_recov), &allow_recov);

        err = obd_connect(&mdc_conn, obd, &mdc_uuid, 0);
        if (err) {
                CERROR("cannot connect to %s: rc = %d\n",
                        g_zconf_mdsname, err);
                GOTO(out_cleanup, err);
        }
        
        exp = class_conn2export(&mdc_conn);
        
        ctxt = exp->exp_obd->obd_llog_ctxt[LLOG_CONFIG_REPL_CTXT];
        rc = class_config_process_llog(ctxt, g_zconf_profile, cfg);
        if (rc)
                CERROR("class_config_process_llog failed: rc = %d\n", rc);

        err = obd_disconnect(exp, 0);

out_cleanup:
        LCFG_INIT(lcfg, LCFG_CLEANUP, name);
        err = class_process_config(&lcfg);
        if (err < 0)
                GOTO(out, err);

out_detach:
        LCFG_INIT(lcfg, LCFG_DETACH, name);
        err = class_process_config(&lcfg);
        if (err < 0)
                GOTO(out, err);

out_del_uuid:
        LCFG_INIT(lcfg, LCFG_DEL_UUID, name);
        lcfg.lcfg_inllen1 = strlen(peer) + 1;
        lcfg.lcfg_inlbuf1 = peer;
        err = class_process_config(&lcfg);

out:
        if (rc == 0)
                rc = err;
        
        RETURN(rc);
}

/* parse host:/mdsname/profile string */
int ll_parse_mount_target(const char *target, char **mdsnid,
                          char **mdsname, char **profile)
{
        static char buf[256];
        char *s;

        buf[255] = 0;
        strncpy(buf, target, 255);

        if ((s = strchr(buf, ':'))) {
                *mdsnid = buf;
                *s = '\0';
                                                                                                                        
                while (*++s == '/')
                        ;
                *mdsname = s;
                if ((s = strchr(*mdsname, '/'))) {
                        *s = '\0';
                        *profile = s + 1;
                        return 0;
                }
        }

        return -1;
}

/*
 * early liblustre init
 * called from C startup in catamount apps, before main()
 *
 * The following is a skeleton sysio startup sequence,
 * as implemented in C startup (skipping error handling).
 * In this framework none of these calls need be made here
 * or in the apps themselves.  The NAMESPACE_STRING specifying
 * the initial set of fs ops (creates, mounts, etc.) is passed
 * as an environment variable.
 * 
 *      _sysio_init();
 *      _sysio_incore_init();
 *      _sysio_native_init();
 *      _sysio_lustre_init();
 *      _sysio_boot(NAMESPACE_STRING);
 *
 * the name _sysio_lustre_init() follows the naming convention
 * established in other fs drivers from libsysio:
 *  _sysio_incore_init(), _sysio_native_init()
 *
 * _sysio_lustre_init() must be called before _sysio_boot()
 * to enable libsysio's processing of namespace init strings containing
 * lustre filesystem operations
 */
int _sysio_lustre_init(void)
{
        int err;

#if 0
        portal_debug = -1;
        portal_subsystem_debug = -1;
#endif

        liblustre_init_random();

        err = lllib_init();
        if (err) {
                perror("init llite driver");
        }       
        return err;
}

/* env variables */
#define ENV_LUSTRE_MNTPNT               "LIBLUSTRE_MOUNT_POINT"
#define ENV_LUSTRE_MNTTGT               "LIBLUSTRE_MOUNT_TARGET"
#define ENV_LUSTRE_TIMEOUT              "LIBLUSTRE_TIMEOUT"
#define ENV_LUSTRE_DUMPFILE             "LIBLUSTRE_DUMPFILE"
#define ENV_LUSTRE_DEBUG_MASK           "LIBLUSTRE_DEBUG_MASK"
#define ENV_LUSTRE_DEBUG_SUBSYS         "LIBLUSTRE_DEBUG_SUBSYS"

extern int _sysio_native_init();
extern unsigned int obd_timeout;

static char *lustre_path = NULL;

/* global variables */
char   *g_zconf_mdsname = NULL; /* mdsname, for zeroconf */
char   *g_zconf_mdsnid = NULL;  /* mdsnid, for zeroconf */
char   *g_zconf_profile = NULL; /* profile, for zeroconf */


void __liblustre_setup_(void)
{
        char *target = NULL;
        char *timeout = NULL;
        char *debug_mask = NULL;
        char *debug_subsys = NULL;
        char *root_driver = "native";
        char *lustre_driver = "llite";
        char *root_path = "/";
        unsigned mntflgs = 0;
	int err;

	lustre_path = getenv(ENV_LUSTRE_MNTPNT);
	if (!lustre_path) {
                lustre_path = "/mnt/lustre";
	}

        /* mount target */
        target = getenv(ENV_LUSTRE_MNTTGT);
        if (!target) {
                printf("LibLustre: no mount target specified\n");
                exit(1);
        }
        if (ll_parse_mount_target(target,
                                  &g_zconf_mdsnid,
                                  &g_zconf_mdsname,
                                  &g_zconf_profile)) {
                CERROR("mal-formed target %s \n", target);
                exit(1);
        }
        if (!g_zconf_mdsnid || !g_zconf_mdsname || !g_zconf_profile) {
                printf("Liblustre: invalid target %s\n", target);
                exit(1);
        }
        printf("LibLustre: mount point %s, target %s\n",
                lustre_path, target);

        timeout = getenv(ENV_LUSTRE_TIMEOUT);
        if (timeout) {
                obd_timeout = (unsigned int) strtol(timeout, NULL, 0);
                printf("LibLustre: set obd timeout as %u seconds\n",
                        obd_timeout);
        }

        /* debug masks */
        debug_mask = getenv(ENV_LUSTRE_DEBUG_MASK);
        if (debug_mask)
                portal_debug = (unsigned int) strtol(debug_mask, NULL, 0);

        debug_subsys = getenv(ENV_LUSTRE_DEBUG_SUBSYS);
        if (debug_subsys)
                portal_subsystem_debug =
                                (unsigned int) strtol(debug_subsys, NULL, 0);

#ifndef CSTART_INIT
        /* initialize libsysio & mount rootfs */
	if (_sysio_init()) {
		perror("init sysio");
		exit(1);
	}
        _sysio_native_init();

	err = _sysio_mount_root(root_path, root_driver, mntflgs, NULL);
	if (err) {
		perror(root_driver);
		exit(1);
	}

        if (_sysio_lustre_init())
		exit(1);
#endif

        err = mount("/", lustre_path, lustre_driver, mntflgs, NULL);
	if (err) {
		errno = -err;
		perror(lustre_driver);
		exit(1);
	}
}

void __liblustre_cleanup_(void)
{
        /* user app might chdir to a lustre directory, and leave busy pnode
         * during finaly libsysio cleanup. here we chdir back to "/".
         * but it can't fix the situation that liblustre is mounted
         * at "/".
         */
        chdir("/");
#if 0
        umount(lustre_path);
#endif
        /* we can't call umount here, because libsysio will not cleanup
         * opening files for us. _sysio_shutdown() will cleanup fds at
         * first but which will also close the sockets we need for umount
         * liblutre. this delima lead to another hack in
         * libsysio/src/file_hack.c FIXME
         */
        _sysio_shutdown();
        cleanup_lib_portals();
        PtlFini();
}
