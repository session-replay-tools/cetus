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

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "network-mysqld-proto.h"

#include "sys-pedantic.h"
#include "glib-ext.h"
#include "cetus-util.h"

/**
 * decode a length-encoded integer from a network packet
 *
 * _off is incremented on success 
 *
 * @param packet   the MySQL-packet to decode
 * @param v        destination of the integer
 * @return 0 on success, non-0 on error 
 *
 */
int
network_mysqld_proto_get_lenenc_int(network_packet *packet, guint64 *v)
{
    guint off = packet->offset;
    guint64 ret = 0;
    unsigned char *bytestream = (unsigned char *)packet->data->str;

    if (off >= packet->data->len) {
        g_critical("%s: offset is too large:%d, packet len:%d", G_STRLOC, (int)off, (int)packet->data->len);

        return -1;
    }

    if (bytestream[off] < 251) {    /* */
        ret = bytestream[off];
    } else if (bytestream[off] == 252) {    /* 2 byte length */
        if (off + 2 >= packet->data->len)
            return -1;
        ret = (bytestream[off + 1] << 0) | (bytestream[off + 2] << 8);
        off += 2;
    } else if (bytestream[off] == 253) {    /* 3 byte */
        if (off + 3 >= packet->data->len)
            return -1;
        ret = (bytestream[off + 1] << 0) | (bytestream[off + 2] << 8) | (bytestream[off + 3] << 16);

        off += 3;
    } else if (bytestream[off] == 254) {    /* 8 byte */
        if (off + 8 >= packet->data->len)
            return -1;
        ret = (unsigned int)((bytestream[off + 5] << 0) |
                             (bytestream[off + 6] << 8) | (bytestream[off + 7] << 16) | (bytestream[off + 8] << 24));
        ret <<= 32;

        ret += (unsigned int)((bytestream[off + 1] << 0) |
                              (bytestream[off + 2] << 8) | (bytestream[off + 3] << 16) | (bytestream[off + 4] << 24));

        off += 8;
    } else {
        /* if we hit this place we complete have no idea about the protocol */
        if (bytestream[off] != 251) {
            g_critical("%s: bytestream[%d] is %d", G_STRLOC, off, bytestream[off]);
        }

        /* either ERR (255) or NULL (251) */
        packet->offset = off + 1;

        return -1;

    }
    off += 1;

    packet->offset = off;

    *v = ret;

    return 0;
}

/**
 * skip bytes in the network packet
 *
 * a assertion makes sure that we can't skip over the end of the packet 
 *
 * @param packet the MySQL network packet
 * @param size   bytes to skip
 *
 */
int
network_mysqld_proto_skip(network_packet *packet, gsize size)
{
    if (packet->offset + size > packet->data->len)
        return -1;

    packet->offset += size;

    return 0;
}

int
network_mysqld_proto_skip_lenenc_str(network_packet *packet)
{
    guint64 len;

    if (packet->offset >= packet->data->len) {
        g_debug_hexdump(G_STRLOC, S(packet->data));
        return -1;
    }
    if (packet->offset >= packet->data->len) {
        return -1;
    }

    if (network_mysqld_proto_get_lenenc_int(packet, &len))
        return -1;

    if (packet->offset + len > packet->data->len)
        return -1;

    packet->offset += len;
    return 0;
}

/**
 * get a fixed-length integer from the network packet 
 *
 * @param packet the MySQL network packet
 * @param v      destination of the integer
 * @param size   byte-len of the integer to decode
 * @return a the decoded integer
 */
int
network_mysqld_proto_peek_int_len(network_packet *packet, guint64 *v, gsize size)
{
    gsize i;
    int shift;
    guint32 r_l = 0, r_h = 0;
    guchar *bytes = (guchar *) packet->data->str + packet->offset;

    if (packet->offset > packet->data->len) {
        return -1;
    }
    if (packet->offset + size > packet->data->len) {
        return -1;
    }

    /**
     * for some reason left-shift > 32 leads to negative numbers 
     */
    for (i = 0, shift = 0; i < size && i < 4; i++, shift += 8, bytes++) {
        r_l |= ((*bytes) << shift);
    }

    for (shift = 0; i < size; i++, shift += 8, bytes++) {
        r_h |= ((*bytes) << shift);
    }

    *v = (((guint64)r_h << 32) | r_l);

    return 0;
}

int
network_mysqld_proto_get_int_len(network_packet *packet, guint64 *v, gsize size)
{
    int err = 0;

    err = err || network_mysqld_proto_peek_int_len(packet, v, size);

    if (err)
        return -1;

    packet->offset += size;

    return 0;
}

/**
 * get a 8-bit integer from the network packet
 *
 * @param packet the MySQL network packet
 * @param v      dest for the number
 * @return 0 on success, non-0 on error
 * @see network_mysqld_proto_get_int_len()
 */
int
network_mysqld_proto_get_int8(network_packet *packet, guint8 *v)
{
    guint64 v64;

    if (network_mysqld_proto_get_int_len(packet, &v64, 1))
        return -1;

    g_assert_cmpint(v64 & 0xff, ==, v64);   /* check that we really only got one byte back */

    *v = v64 & 0xff;

    return 0;
}

/**
 * get a 8-bit integer from the network packet
 *
 * @param packet the MySQL network packet
 * @param v      dest for the number
 * @return 0 on success, non-0 on error
 * @see network_mysqld_proto_get_int_len()
 */
int
network_mysqld_proto_peek_int8(network_packet *packet, guint8 *v)
{
    guint64 v64;

    if (network_mysqld_proto_peek_int_len(packet, &v64, 1))
        return -1;

    /* check that we really only got one byte back */
    g_assert_cmpint(v64 & 0xff, ==, v64);

    *v = v64 & 0xff;

    return 0;
}

/**
 * get a 16-bit integer from the network packet
 *
 * @param packet the MySQL network packet
 * @param v      dest for the number
 * @return 0 on success, non-0 on error
 * @see network_mysqld_proto_get_int_len()
 */
int
network_mysqld_proto_get_int16(network_packet *packet, guint16 *v)
{
    guint64 v64;

    if (network_mysqld_proto_get_int_len(packet, &v64, 2))
        return -1;

    /* check that we really only got two byte back */
    g_assert_cmpint(v64 & 0xffff, ==, v64);

    *v = v64 & 0xffff;

    return 0;
}

/**
 * get a 16-bit integer from the network packet
 *
 * @param packet the MySQL network packet
 * @param v      dest for the number
 * @return 0 on success, non-0 on error
 * @see network_mysqld_proto_get_int_len()
 */
int
network_mysqld_proto_peek_int16(network_packet *packet, guint16 *v)
{
    guint64 v64;

    if (network_mysqld_proto_peek_int_len(packet, &v64, 2))
        return -1;

    /* check that we really only got two byte back */
    g_assert_cmpint(v64 & 0xffff, ==, v64);

    *v = v64 & 0xffff;

    return 0;
}

/**
 * get a 24-bit integer from the network packet
 *
 * @param packet the MySQL network packet
 * @param v      dest for the number
 * @return 0 on success, non-0 on error
 * @see network_mysqld_proto_get_int_len()
 */
int
network_mysqld_proto_get_int24(network_packet *packet, guint32 *v)
{
    guint64 v64;

    if (network_mysqld_proto_get_int_len(packet, &v64, 3))
        return -1;

    /* check that we really only got two byte back */
    g_assert_cmpint(v64 & 0x00ffffff, ==, v64);

    *v = v64 & 0x00ffffff;

    return 0;
}

/**
 * get a 32-bit integer from the network packet
 *
 * @param packet the MySQL network packet
 * @param v      dest for the number
 * @return 0 on success, non-0 on error
 * @see network_mysqld_proto_get_int_len()
 */
int
network_mysqld_proto_get_int32(network_packet *packet, guint32 *v)
{
    guint64 v64;

    if (network_mysqld_proto_get_int_len(packet, &v64, 4))
        return -1;

    *v = v64 & 0xffffffff;

    return 0;
}

/**
 * get a string from the network packet
 *
 * @param packet the MySQL network packet
 * @param s      dest of the string
 * @param len    length of the string
 * @return       0 on success, non-0 otherwise
 * @return the string (allocated) or NULL of len is 0
 */
int
network_mysqld_proto_get_str_len(network_packet *packet, gchar **s, gsize len)
{
    gchar *str;

    if (len == 0) {
        *s = NULL;
        return 0;
    }

    if (packet->offset > packet->data->len) {
        return -1;
    }
    if (packet->offset + len > packet->data->len) {
        g_critical("%s: packet-offset out of range: %u + " F_SIZE_T " > " F_SIZE_T,
                   G_STRLOC, packet->offset, len, packet->data->len);

        return -1;
    }

    str = g_malloc(len + 1);
    memcpy(str, packet->data->str + packet->offset, len);
    str[len] = '\0';

    packet->offset += len;

    *s = str;

    return 0;
}

static int
network_mysqld_proto_get_str_len2(network_packet *packet, gchar *s, gsize len)
{
    if (len == 0) {
        return 0;
    }

    if (packet->offset > packet->data->len) {
        return -1;
    }
    if (packet->offset + len > packet->data->len) {
        g_critical("%s: packet-offset out of range: %u + " F_SIZE_T " > " F_SIZE_T,
                   G_STRLOC, packet->offset, len, packet->data->len);

        return -1;
    }

    if (len) {
        memcpy(s, packet->data->str + packet->offset, len);
        s[len] = '\0';
    }

    packet->offset += len;

    return 0;
}

/**
 * get a variable-length string from the network packet
 *
 * variable length strings are prefixed with variable-length integer 
 * defining the length of the string
 *
 * @param packet the MySQL network packet
 * @param s      destination of the decoded string
 * @param _len    destination of the length of the decoded string, if len is non-NULL
 * @return 0 on success, non-0 on error
 * @see network_mysqld_proto_get_str_len(), network_mysqld_proto_get_lenenc_int()
 */
int
network_mysqld_proto_get_lenenc_str(network_packet *packet, gchar **s, guint64 *_len)
{
    guint64 len;

    if (packet->offset >= packet->data->len) {
        g_debug_hexdump(G_STRLOC, S(packet->data));
        return -1;
    }
    if (packet->offset >= packet->data->len) {
        return -1;
    }

    if (network_mysqld_proto_get_lenenc_int(packet, &len))
        return -1;

    if (len > PACKET_LEN_MAX)
        return -1;

    if (packet->offset + len > packet->data->len)
        return -1;

    if (_len)
        *_len = len;

    return network_mysqld_proto_get_str_len(packet, s, len);
}

int
network_mysqld_proto_get_column(network_packet *packet, gchar *s, gsize s_size)
{
    guint64 len;
    int err = network_mysqld_proto_get_lenenc_int(packet, &len);
    if (err) {
        return -1;
    }

    if (s_size <= len) {
        g_critical("%s: column too long:%ld, buffer size:%ld", G_STRLOC, len, s_size);
        return -1;
    }

    if (packet->offset + len > packet->data->len)
        return -1;

    return network_mysqld_proto_get_str_len2(packet, s, len);
}

/**
 * get a NUL-terminated string from the network packet
 *
 * @param packet the MySQL network packet
 * @param s      dest of the string
 * @return       0 on success, non-0 otherwise
 * @see network_mysqld_proto_get_str_len()
 */
int
network_mysqld_proto_get_string(network_packet *packet, gchar **s)
{
    guint64 len;
    int err = 0;

    for (len = 0; packet->offset + len < packet->data->len && *(packet->data->str + packet->offset + len); len++) ;

    if (*(packet->data->str + packet->offset + len) != '\0') {
        /* this has to be a \0 */
        return -1;
    }

    if (len > 0) {
        if (packet->offset >= packet->data->len) {
            return -1;
        }
        if (packet->offset + len > packet->data->len) {
            return -1;
        }

        /**
         * copy the string w/o the NUL byte 
         */
        err = err || network_mysqld_proto_get_str_len(packet, s, len);
    }

    err = err || network_mysqld_proto_skip(packet, 1);

    return err ? -1 : 0;
}

/**
 * get a GString from the network packet
 *
 * @param packet the MySQL network packet
 * @param len    bytes to copy
 * @param out    a GString which carries the string
 * @return       0 on success, -1 on error
 */
int
network_mysqld_proto_get_gstr_len(network_packet *packet, gsize len, GString *out)
{
    int err = 0;

    if (!out)
        return -1;

    g_string_truncate(out, 0);

    if (!len)
        return 0;               /* nothing to copy */

    err = err || (packet->offset >= packet->data->len); /* the offset is already too large */
    err = err || (packet->offset + len > packet->data->len);    /* offset would get too large */

    if (!err) {
        g_string_append_len(out, packet->data->str + packet->offset, len);
        packet->offset += len;
    }

    return err ? -1 : 0;
}

/**
 * get a NUL-terminated GString from the network packet
 *
 * @param packet the MySQL network packet
 * @param out    a GString which carries the string
 * @return       a pointer to the string in out
 *
 * @see network_mysqld_proto_get_gstr_len()
 */
int
network_mysqld_proto_get_gstr(network_packet *packet, GString *out)
{
    guint64 len;
    int err = 0;

    for (len = 0; packet->offset + len < packet->data->len &&
         *(packet->data->str + packet->offset + len) != '\0'; len++) ;

    if (packet->offset + len == packet->data->len) {    /* havn't found a trailing \0 */
        return -1;
    }

    if (len > 0) {
        g_assert(packet->offset < packet->data->len);
        g_assert(packet->offset + len <= packet->data->len);

        err = err || network_mysqld_proto_get_gstr_len(packet, len, out);
    }

    /* skip the \0 */
    err = err || network_mysqld_proto_skip(packet, 1);

    return err ? -1 : 0;
}

/**
 * create a empty field for a result-set definition
 *
 * @return a empty MYSQL_FIELD
 */
MYSQL_FIELD *
network_mysqld_proto_fielddef_new()
{
    MYSQL_FIELD *field;

    field = g_new0(MYSQL_FIELD, 1);

    return field;
}

void
network_mysqld_mysql_field_row_free(GPtrArray *row)
{
    if (row) {
        g_ptr_array_free(row, TRUE);
    }
}

/**
 * free a MYSQL_FIELD and its components
 *
 * @param field  the MYSQL_FIELD to free
 */
void
network_mysqld_proto_fielddef_free(MYSQL_FIELD *field)
{
    if (field->catalog)
        g_free(field->catalog);
    if (field->db)
        g_free(field->db);
    if (field->name)
        g_free(field->name);
    if (field->org_name)
        g_free(field->org_name);
    if (field->table)
        g_free(field->table);
    if (field->org_table)
        g_free(field->org_table);

    g_free(field);
}

/**
 * create a array of MYSQL_FIELD 
 *
 * @return a empty array of MYSQL_FIELD
 */
GPtrArray *
network_mysqld_proto_fielddefs_new(void)
{
    GPtrArray *fields;

    fields = g_ptr_array_new();

    return fields;
}

/**
 * free a array of MYSQL_FIELD 
 *
 * @param fields  array of MYSQL_FIELD to free
 * @see network_mysqld_proto_field_free()
 */
void
network_mysqld_proto_fielddefs_free(GPtrArray *fields)
{
    guint i;
    for (i = 0; i < fields->len; i++) {
        MYSQL_FIELD *field = fields->pdata[i];
        if (field)
            g_free(field);
    }

    g_ptr_array_free(fields, TRUE);
}

/**
 * set length of the packet in the packet header
 *
 * each MySQL packet is 
 *  - is prefixed by a 4 byte packet header
 *  - length is max 16Mbyte (3 Byte)
 *  - sequence-id (1 Byte) 
 *
 * To encode a packet of more then 16M clients have to send multiple 16M frames
 *
 * the sequence-id is incremented for each related packet and wrapping from 255 to 0
 *
 * @param header  string of at least 4 byte to write the packet header to
 * @param length  length of the packet
 * @param id      sequence-id of the packet
 * @return 0
 */
int
network_mysqld_proto_set_packet_len(GString *_header, guint32 length)
{
    unsigned char *header = (unsigned char *)_header->str;

    g_assert_cmpint(length, <=, PACKET_LEN_MAX);

    header[0] = (length >> 0) & 0xFF;
    header[1] = (length >> 8) & 0xFF;
    header[2] = (length >> 16) & 0xFF;

    return 0;
}

int
network_mysqld_proto_set_packet_id(GString *_header, guint8 id)
{
    unsigned char *header = (unsigned char *)_header->str;

    header[3] = id;

    return 0;
}

int network_mysqld_proto_set_compressed_packet_len(GString *_header, guint32 length,
                                                   guint32 len_before)
{
    unsigned char *header = (unsigned char *)_header->str;

    g_assert_cmpint(length, <=, PACKET_LEN_MAX);

    header[0] = (length >> 0) & 0xFF;
    header[1] = (length >> 8) & 0xFF;
    header[2] = (length >> 16) & 0xFF;

    header[4] = (len_before >> 0) & 0xFF;
    header[5] = (len_before >> 8) & 0xFF;
    header[6] = (len_before >> 16) & 0xFF;
    return 0;
}

int
network_mysqld_proto_append_packet_len(GString *_header, guint32 length)
{
    return network_mysqld_proto_append_int24(_header, length);
}

int
network_mysqld_proto_append_packet_id(GString *_header, guint8 id)
{
    return network_mysqld_proto_append_int8(_header, id);
}

/**
 * decode the packet length from a packet header
 *
 * @param header the first 3 bytes of the network packet
 * @return the packet length
 * @see network_mysqld_proto_set_header()
 */
guint32
network_mysqld_proto_get_packet_len(GString *_header)
{
    unsigned char *header = (unsigned char *)_header->str;

    return (header[0]) | (header[1] << 8) | (header[2] << 16);
}

/**
 * decode the packet id from a packet header
 *
 * @param header the first 4 bytes of the network packet
 * @return the packet id
 * @see network_mysqld_proto_set_packet_id()
 */
guint8
network_mysqld_proto_get_packet_id(GString *_header)
{
    unsigned char *header = (unsigned char *)_header->str;

    return header[3];
}

/**
 * append the variable-length integer to the packet
 *
 * @param packet  the MySQL network packet
 * @param length  integer to encode
 * @return        0
 */
int
network_mysqld_proto_append_lenenc_int(GString *packet, guint64 length)
{
    if (length < 251) {
        g_string_append_c(packet, length);
    } else if (length < 65536) {
        g_string_append_c(packet, (gchar)252);
        g_string_append_c(packet, (length >> 0) & 0xff);
        g_string_append_c(packet, (length >> 8) & 0xff);
    } else if (length < 16777216) {
        g_string_append_c(packet, (gchar)253);
        g_string_append_c(packet, (length >> 0) & 0xff);
        g_string_append_c(packet, (length >> 8) & 0xff);
        g_string_append_c(packet, (length >> 16) & 0xff);
    } else {
        g_string_append_c(packet, (gchar)254);

        g_string_append_c(packet, (length >> 0) & 0xff);
        g_string_append_c(packet, (length >> 8) & 0xff);
        g_string_append_c(packet, (length >> 16) & 0xff);
        g_string_append_c(packet, (length >> 24) & 0xff);

        g_string_append_c(packet, (length >> 32) & 0xff);
        g_string_append_c(packet, (length >> 40) & 0xff);
        g_string_append_c(packet, (length >> 48) & 0xff);
        g_string_append_c(packet, (length >> 56) & 0xff);
    }

    return 0;
}

/**
 * encode a GString in to a MySQL len-encoded string 
 *
 * @param packet  the MySQL network packet
 * @param s       string to encode
 * @param length  length of the string to encode
 * @return 0
 */
int
network_mysqld_proto_append_lenenc_str_len(GString *packet, const char *s, guint64 length)
{
    if (!s) {
        g_string_append_c(packet, (gchar)251); /** this is NULL */
    } else {
        network_mysqld_proto_append_lenenc_int(packet, length);
        g_string_append_len(packet, s, length);
    }

    return 0;
}

/**
 * encode a GString in to a MySQL len-encoded string 
 *
 * @param packet  the MySQL network packet
 * @param s       string to encode
 *
 * @see network_mysqld_proto_append_lenenc_str_len()
 */
int
network_mysqld_proto_append_lenenc_str(GString *packet, const char *s)
{
    return network_mysqld_proto_append_lenenc_str_len(packet, s, s ? strlen(s) : 0);
}

/**
 * encode fixed length integer in to a network packet
 *
 * @param packet  the MySQL network packet
 * @param num     integer to encode
 * @param size    byte size of the integer
 * @return        0
 */
static int
network_mysqld_proto_append_int_len(GString *packet, guint64 num, gsize size)
{
    gsize i;

    for (i = 0; i < size; i++) {
        g_string_append_c(packet, num & 0xff);
        num >>= 8;
    }

    return 0;
}

/**
 * encode 8-bit integer in to a network packet
 *
 * @param packet  the MySQL network packet
 * @param num     integer to encode
 *
 * @see network_mysqld_proto_append_int_len()
 */
int
network_mysqld_proto_append_int8(GString *packet, guint8 num)
{
    return network_mysqld_proto_append_int_len(packet, num, 1);
}

/**
 * encode 16-bit integer in to a network packet
 *
 * @param packet  the MySQL network packet
 * @param num     integer to encode
 *
 * @see network_mysqld_proto_append_int_len()
 */
int
network_mysqld_proto_append_int16(GString *packet, guint16 num)
{
    return network_mysqld_proto_append_int_len(packet, num, 2);
}

/**
 * encode 24-bit integer in to a network packet
 *
 * @param packet  the MySQL network packet
 * @param num     integer to encode
 *
 * @see network_mysqld_proto_append_int_len()
 */
int
network_mysqld_proto_append_int24(GString *packet, guint32 num)
{
    return network_mysqld_proto_append_int_len(packet, num, 3);
}

/**
 * encode 32-bit integer in to a network packet
 *
 * @param packet  the MySQL network packet
 * @param num     integer to encode
 *
 * @see network_mysqld_proto_append_int_len()
 */
int
network_mysqld_proto_append_int32(GString *packet, guint32 num)
{
    return network_mysqld_proto_append_int_len(packet, num, 4);
}

/**
 * hash the password as MySQL 4.1 and later assume
 *
 *   SHA1(password)
 *
 * @see network_mysqld_proto_scramble
 */
int
network_mysqld_proto_password_hash(GString *response, const char *password, gsize password_len)
{
    GChecksum *cs;

    /* first round: SHA1(password) */
    cs = g_checksum_new(G_CHECKSUM_SHA1);

    g_checksum_update(cs, (guchar *) password, password_len);

    g_string_set_size(response, g_checksum_type_get_length(G_CHECKSUM_SHA1));
    /* will be overwritten with the right value in the next step */
    response->len = response->allocated_len;
    g_checksum_get_digest(cs, (guchar *) response->str, &(response->len));

    g_checksum_free(cs);

    return 0;
}

/**
 * scramble the hashed password with the challenge
 *
 * @param response         dest 
 * @param challenge        the challenge string as sent by the mysql-server
 * @param challenge_len    length of the challenge
 * @param hashed_pwd  hashed password
 * @param hashed_pwd_len length of the hashed password
 *
 * @see network_mysqld_proto_password_hash
 */
int
network_mysqld_proto_password_scramble(GString *response,
                                       const char *challenge, gsize challenge_len,
                                       const char *hashed_pwd, gsize hashed_pwd_len)
{
    int i;
    GChecksum *cs;
    GString *step2;

    g_return_val_if_fail(NULL != challenge, -1);
    g_return_val_if_fail(20 == challenge_len || 21 == challenge_len, -1);
    g_return_val_if_fail(NULL != hashed_pwd, -1);
    g_return_val_if_fail(20 == hashed_pwd_len || 21 == hashed_pwd_len, -1);

    /**
     * we have to run
     *
     *   XOR(SHA1(password), SHA1(challenge + SHA1(SHA1(password)))
     *
     * where SHA1(password) is the hashed_pwd and
     *       challenge      is ... challenge
     *
     *   XOR(hashed_pwd, SHA1(challenge + SHA1(hashed_pwd)))
     *
     */

    if (hashed_pwd_len == 21)
        hashed_pwd_len--;

    /* 1. SHA1(hashed_pwd) */
    step2 = g_string_new(NULL);
    network_mysqld_proto_password_hash(step2, hashed_pwd, hashed_pwd_len);

    /* 2. SHA1(challenge + SHA1(hashed_pwd) */
    cs = g_checksum_new(G_CHECKSUM_SHA1);

    /* if the challenge is 21 bytes long it means we're behind a 5.5.7 or up server
     * that supports authentication plugins. After auth-plugin-data-2 the protocol adds
     * a spacing character to split it from the next part of the packet: auth-plugin-name.
     * That spacing char '\0' is the 21th byte.
     *
     * We assume that auth-plugin-data is always 20 bytes, 
     * on this scnenario it is 21 so we need
     * to ignore the last byte: the trailing '\0'.
     */
    if (challenge_len == 21)
        challenge_len--;

    g_checksum_update(cs, (guchar *) challenge, challenge_len);
    g_checksum_update(cs, (guchar *) step2->str, step2->len);

    g_string_set_size(response, g_checksum_type_get_length(G_CHECKSUM_SHA1));
    response->len = response->allocated_len;
    g_checksum_get_digest(cs, (guchar *) response->str, &(response->len));

    g_checksum_free(cs);

    /* XOR the hashed_pwd with SHA1(challenge + SHA1(hashed_pwd)) */
    for (i = 0; i < 20; i++) {
        response->str[i] = (guchar) response->str[i] ^ (guchar) hashed_pwd[i];
    }

    g_string_free(step2, TRUE);

    return 0;
}

int
network_mysqld_proto_skip_network_header(network_packet *packet)
{
    return network_mysqld_proto_skip(packet, NET_HEADER_SIZE);
}

/*@}*/
