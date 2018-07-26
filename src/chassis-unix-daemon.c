/* $%BEGINLICENSE%$
 Copyright (c) 2007, 2012, Oracle and/or its affiliates. All rights reserved.

 This program is free software; you can redistribute it and/or
 modify it under the terms of the GNU General Public License as
 published by the Free Software Foundation; version 2 of the
 License.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 02110-1301  USA

 $%ENDLICENSE%$ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_SYS_WAIT_H
#include <sys/wait.h>           /* wait4 */
#endif
#include <sys/stat.h>
#ifdef HAVE_SYS_RESOURCE_H
#include <sys/resource.h>       /* getrusage */
#endif

#include <stdlib.h>
#include <string.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>

#include <glib.h>

#include "glib-ext.h"
#include "chassis-unix-daemon.h"

/**
 * start the app in the background 
 * 
 * UNIX-version
 */
void
chassis_unix_daemonize(void)
{
#ifdef SIGTTOU
    signal(SIGTTOU, SIG_IGN);
#endif
#ifdef SIGTTIN
    signal(SIGTTIN, SIG_IGN);
#endif
#ifdef SIGTSTP
    signal(SIGTSTP, SIG_IGN);
#endif
    if (fork() != 0)
        exit(0);

    if (setsid() == -1)
        exit(0);

    signal(SIGHUP, SIG_IGN);

    if (fork() != 0)
        exit(0);

    umask(0);
}

/**
 * forward the signal to the process group, but not us
 */
static void
chassis_unix_signal_forward(int sig)
{
    signal(sig, SIG_IGN);       /* we don't want to create a loop here */

    kill(0, sig);
}

