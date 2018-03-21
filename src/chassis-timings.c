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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <glib.h>
#include <time.h>
#include <math.h>

#include "chassis-timings.h"
#include "glib-ext.h"

int
chassis_epoch_from_string(const char *str, gboolean *ok)
{
    if (!str) {
        *ok = FALSE;
        return 0;
    }
    struct tm t = { 0 };
    if (strptime(str, "%Y-%m-%d %H:%M:%S", &t)) {   /* %Y-%m-%d will fail */
        if (ok)
            *ok = TRUE;
        return mktime(&t);
    }
    struct tm d = { 0 };
    if (strptime(str, "%Y-%m-%d", &d)) {    /* %Y-%m-%d %H:%M:%S will also pass, with 'd' set wrong */
        if (ok)
            *ok = TRUE;
        return mktime(&d);
    }
    if (ok)
        *ok = FALSE;
    return 0;
}

gboolean
chassis_timeval_from_double(struct timeval *dst, double t)
{
    g_return_val_if_fail(dst != NULL, FALSE);
    g_return_val_if_fail(t >= 0, FALSE);

    dst->tv_sec = floor(t);
    dst->tv_usec = floor((t - dst->tv_sec) * 1000000);

    return TRUE;
}

void
chassis_epoch_to_string(time_t *epoch, char *str, int len)
{
    struct tm *local = localtime(epoch);
    if (local) {
        strftime(str, len, "%Y-%m-%d %H:%M:%S", local);
    }
}
