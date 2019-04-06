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

#ifndef _CHASSIS_REMOTE_CONFIGS_H_
#define _CHASSIS_REMOTE_CONFIGS_H_

#include <glib.h>               /* GPtrArray */
#include "chassis-mainloop.h"

typedef struct cetus_monitor_t cetus_monitor_t;

typedef enum {
    MONITOR_TYPE_CHECK_ALIVE,
    MONITOR_TYPE_CHECK_DELAY,
} monitor_type_t;

typedef void (*monitor_callback_fn) (int, short, void *);

cetus_monitor_t *cetus_monitor_new();

void cetus_monitor_free(cetus_monitor_t *);

void cetus_monitor_open(cetus_monitor_t *, monitor_type_t);

void cetus_monitor_start_thread(cetus_monitor_t *, chassis *data);

void cetus_monitor_stop_thread(cetus_monitor_t *);

#endif
