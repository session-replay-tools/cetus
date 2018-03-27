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

#ifndef __CHASSIS_TIMINGS_H__
#define __CHASSIS_TIMINGS_H__

#include <sys/time.h>           /* struct timeval */
#include <glib.h>
#include "chassis-exports.h"

#define SECONDS ( 1)
#define MINUTES ( 60 * SECONDS)
#define HOURS ( 60 * MINUTES)

CHASSIS_API int chassis_epoch_from_string(const char *str, gboolean *ok);
CHASSIS_API gboolean chassis_timeval_from_double(struct timeval *dst, double t);
void chassis_epoch_to_string(time_t *epoch, char *str, int len);

#endif
