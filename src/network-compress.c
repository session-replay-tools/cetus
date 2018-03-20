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

#include <stdio.h>
#include <string.h>
#include <zlib.h>
#include "network-compress.h"

#define CHUNK 16384

int
cetus_compress_init(z_stream *strm)
{
    /* allocate deflate state */
    strm->zalloc = Z_NULL;
    strm->zfree = Z_NULL;
    strm->opaque = Z_NULL;

    int level = Z_DEFAULT_COMPRESSION;

    return deflateInit(strm, level);
}

void
cetus_compress_end(z_stream *strm)
{
    (void)deflateEnd(strm);
}

int
cetus_compress(z_stream *strm, GString *dst, char *src, int src_len, int end)
{
    int flush;
    unsigned char out[CHUNK];

    strm->avail_in = src_len;
    flush = end ? Z_FINISH : Z_NO_FLUSH;
    strm->next_in = (Bytef *) src;

    /* run deflate() on input until output buffer not full, finish
       compression if all of source has been read in */
    do {
        strm->avail_out = CHUNK;
        strm->next_out = out;
        deflate(strm, flush);   /* no bad return value */
        unsigned int have = CHUNK - strm->avail_out;
        if (have > 0) {
            g_string_append_len(dst, (const gchar *)out, have);
        }

    } while (strm->avail_out == 0);

    return Z_OK;
}

static int
cetus_uncompress_init(z_stream *strm)
{
    /* allocate inflate state */
    strm->zalloc = Z_NULL;
    strm->zfree = Z_NULL;
    strm->opaque = Z_NULL;
    strm->avail_in = 0;
    strm->next_in = Z_NULL;

    return inflateInit(strm);
}

static void
cetus_uncompress_end(z_stream *strm)
{
    (void)inflateEnd(strm);
}

int
cetus_uncompress(GString *uncompressed_packet, unsigned char *src, int len)
{
    int ret;
    z_stream strm;
    unsigned char out[CHUNK];

    cetus_uncompress_init(&strm);

    /* decompress until deflate stream ends or end of file */
    strm.avail_in = len;
    strm.next_in = src;

    /* run inflate() on input until output buffer not full */
    do {
        strm.avail_out = CHUNK;
        strm.next_out = out;
        ret = inflate(&strm, Z_NO_FLUSH);
        switch (ret) {
        case Z_NEED_DICT:
            ret = Z_DATA_ERROR; /* and fall through */
        case Z_DATA_ERROR:
        case Z_MEM_ERROR:
            (void)inflateEnd(&strm);
            return ret;
        }
        unsigned int have = CHUNK - strm.avail_out;

        g_string_append_len(uncompressed_packet, (const gchar *)out, have);

    } while (strm.avail_out == 0);

    cetus_uncompress_end(&strm);

    return ret == Z_STREAM_END ? Z_OK : Z_DATA_ERROR;
}
