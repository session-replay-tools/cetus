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

#ifndef _CHASSIS_EVENT_H_
#define _CHASSIS_EVENT_H_

#include <glib.h>               /* GPtrArray */

#include "chassis-exports.h"
#include "chassis-mainloop.h"

#define CHECK_PENDING_EVENT(ev) \
    if (event_pending((ev), EV_READ|EV_WRITE|EV_TIMEOUT, NULL)) {       \
        event_del(ev);  \
    }

CHASSIS_API void chassis_event_add(chassis *chas, struct event *ev);
CHASSIS_API void chassis_event_add_with_timeout(chassis *chas, struct event *ev, struct timeval *tv);

typedef struct event_base chassis_event_loop_t;

CHASSIS_API chassis_event_loop_t *chassis_event_loop_new();
CHASSIS_API void chassis_event_loop_free(chassis_event_loop_t *e);
CHASSIS_API void chassis_event_set_event_base(chassis_event_loop_t *e, struct event_base *event_base);
CHASSIS_API void *chassis_event_loop(chassis_event_loop_t *, int *);

#endif
