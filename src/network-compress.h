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
