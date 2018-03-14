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

#ifndef _NETWORK_COMPRESS_H_
#define _NETWORK_COMPRESS_H_

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <sys/types.h>
#include <unistd.h>
#include <mysql.h>
#include <glib.h>
#include <zlib.h>
#include "network-exports.h"

#define MIN_COMPRESS_LENGTH  50

NETWORK_API int cetus_uncompress(GString *uncompressed_packet, unsigned char *src, int len);
NETWORK_API int cetus_compress(z_stream *strm, GString *dst, char *src, int src_len, int end);
NETWORK_API void cetus_compress_end(z_stream *strm);
NETWORK_API int cetus_compress_init(z_stream *strm);

#endif
