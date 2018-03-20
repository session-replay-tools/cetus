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

#ifndef  TC_LOG_INCLUDED
#define  TC_LOG_INCLUDED

#include "chassis-exports.h"

#define LOG_STDERR            0
#define LOG_EMERG             1
#define LOG_ALERT             2
#define LOG_CRIT              3
#define LOG_ERR               4
#define LOG_WARN              5
#define LOG_NOTICE            6
#define LOG_INFO              7
#define LOG_DEBUG             8

#define LOG_MAX_LEN 2048

int tc_log_init(const char *);
int tc_get_log_hour();
void tc_log_end(void);

void tc_log_info(int level, int err, const char *fmt, ...);

#if (TC_DEBUG)

#define tc_log_debug0(level, err, fmt)                                       \
    tc_log_info(level, err, (const char *) fmt)

#define tc_log_debug1(level, err, fmt, a1)                                   \
    tc_log_info(level, err, (const char *) fmt, a1)

#define tc_log_debug2(level, err, fmt, a1, a2)                               \
    tc_log_info(level, err, (const char *) fmt, a1, a2)

#define tc_log_debug3(level, err, fmt, a1, a2, a3)                           \
    tc_log_info(level, err, (const char *) fmt, a1, a2, a3)

#define tc_log_debug4(level, err, fmt, a1, a2, a3, a4)                       \
    tc_log_info(level, err, (const char *) fmt, a1, a2, a3, a4)

#define tc_log_debug5(level, err, fmt, a1, a2, a3, a4, a5)                   \
    tc_log_info(level, err, (const char *) fmt, a1, a2, a3, a4, a5)

#define tc_log_debug6(level, err, fmt, a1, a2, a3, a4, a5, a6)               \
    tc_log_info(level, err, (const char *) fmt, a1, a2, a3, a4, a5, a6)

#define tc_log_debug7(level, err, fmt, a1, a2, a3, a4, a5, a6, a7)           \
    tc_log_info(level, err, (const char *) fmt, a1, a2, a3, a4, a5, a6, a7)

#define tc_log_debug8(level, err, fmt, a1, a2, a3, a4, a5, a6, a7, a8)       \
    tc_log_info(level, err, (const char *) fmt, a1, a2, a3, a4, a5, a6, a7, a8)

#else

#define tc_log_debug0(level, err, fmt)
#define tc_log_debug1(level, err, fmt, a1)
#define tc_log_debug2(level, err, fmt, a1, a2)
#define tc_log_debug3(level, err, fmt, a1, a2, a3)
#define tc_log_debug4(level, err, fmt, a1, a2, a3, a4)
#define tc_log_debug5(level, err, fmt, a1, a2, a3, a4, a5)
#define tc_log_debug6(level, err, fmt, a1, a2, a3, a4, a5, a6)
#define tc_log_debug7(level, err, fmt, a1, a2, a3, a4, a5, a6, a7)
#define tc_log_debug8(level, err, fmt, a1, a2, a3, a4, a5, a6, a7, a8)
#define tc_log_debug_trace(level, err, flag, ip, tcp)

#endif /* TC_DEBUG */

#endif /* TC_LOG_INCLUDED */
