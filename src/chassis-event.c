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

#include <glib.h>
#include <string.h>
#include <errno.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_UNISTD_H
#include <unistd.h>             /* for write() */
#endif

#include <sys/socket.h>         /* for SOCK_STREAM and AF_UNIX/AF_INET */

#include <event.h>

#include "chassis-event.h"
#include "glib-ext.h"
#include "cetus-util.h"

#define E_NET_CONNRESET ECONNRESET
#define E_NET_CONNABORTED ECONNABORTED
#define E_NET_INPROGRESS EINPROGRESS
#if EWOULDBLOCK == EAGAIN
/**
 * some system make EAGAIN == EWOULDBLOCK which would lead to a
 * error in the case handling
 *
 * set it to -1 as this error should never happen
 */
#define E_NET_WOULDBLOCK -1
#else
#define E_NET_WOULDBLOCK EWOULDBLOCK
#endif

extern sig_atomic_t    cetus_reap;
extern sig_atomic_t    cetus_change_binary;
extern sig_atomic_t    cetus_quit;
extern sig_atomic_t    cetus_noaccept;

void
chassis_event_add_with_timeout(chassis *chas, struct event *ev, struct timeval *tv)
{
    event_base_set(chas->event_base, ev);
    event_add(ev, tv);
    g_debug("%s:event add ev:%p", G_STRLOC, ev);
}

/**
 * add a event asynchronously
 *
 * @see network_mysqld_con_handle()
 */
void
chassis_event_add(chassis *chas, struct event *ev)
{
    chassis_event_add_with_timeout(chas, ev, NULL);
}

chassis_event_loop_t *
chassis_event_loop_new()
{
    return event_base_new();
}

void
chassis_event_loop_free(chassis_event_loop_t *event)
{
    event_base_free(event);
}

void *
chassis_event_loop(chassis_event_loop_t *loop, int *mutex)
{

    /**
     * check once a second if we shall shutdown the proxy
     */
    while ((mutex != NULL && (*mutex) != 0) || !chassis_is_shutdown()) {
        if (cetus_reap || cetus_change_binary || cetus_quit || cetus_noaccept) {
            if (cetus_quit) {
                g_message("%s: cetus_quit is true", G_STRLOC);
            }

            if (cetus_noaccept) {
                g_message("%s: cetus_noaccept is true", G_STRLOC);
            }

            if (cetus_reap) {
                g_message("%s: cetus_reap is true", G_STRLOC);
            }
            break;
        }

        struct timeval timeout;
        int r;

        timeout.tv_sec = 0;
        timeout.tv_usec = 256000;

        r = event_base_loopexit(loop, &timeout);
        if (r == -1) {
            g_critical("%s: leaving chassis_event_loop early. failed", G_STRLOC);
            break;
        }

        r = event_base_dispatch(loop);

        if (r == -1) {
            g_debug("%s: after event_base_dispatch:%d, errno:%d, str:%s",
                    G_STRLOC, r, errno, strerror(errno));
            if (errno == EINTR) {
                g_message("%s: EINTR is met", G_STRLOC);
                continue;
            }
            g_critical("%s: leaving chassis_event_loop early, errno != EINTR was: %s (%d)",
                    G_STRLOC, g_strerror(errno), errno);
            break;
        }
    }
        

    return NULL;
}
