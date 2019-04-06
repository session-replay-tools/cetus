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

#ifndef _GLIB_EXT_STRING_LEN_H_
#define _GLIB_EXT_STRING_LEN_H_

#include <inttypes.h>

/**
 * simple macros to get the data and length of a "string"
 *
 * C() is for constant strings like "foo"
 * S() is for GString's 
 */
#define C(x) x, sizeof(x) - 1
#define S(x) (x) ? (x)->str : NULL, (x) ? (x)->len : 0
#define L(x) x, strlen(x)

typedef int32_t BitArray;
#define SetBit(A,k)     (A[(k/32)] |= (1 << (k%32)))
#define ClearBit(A,k)   (A[(k/32)] &= ~(1 << (k%32)))
#define TestBit(A,k)    (A[(k/32)] & (1 << (k%32)))

#define KB 1024
#define MB 1024 * KB
#define GB 1024 * MB

gboolean try_get_int_value(const gchar *option_value, gint *return_value);
gboolean try_get_long_value(const gchar *option_value, long long *return_value);
gboolean try_get_double_value(const gchar *option_value, gdouble *return_value);

int make_iso8601_timestamp(char *buf, uint64_t utime);
guint64 get_timer_microseconds();

void bytes_to_hex_str(char* pin, int len, char* pout);

#endif
