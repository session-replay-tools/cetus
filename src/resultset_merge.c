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
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <stdio.h>
#include <math.h>

#include <mysqld_error.h>
#include "glib-ext.h"
#include "network-mysqld-packet.h"
#include "sys-pedantic.h"
#include "resultset_merge.h"
#include "sql-context.h"
#include "shard-plugin-con.h"
#include "server-session.h"
#include "chassis-event.h"
#include "sharding-query-plan.h"

const char EPOCH[] = "1970-01-01 00:00:00";
const char *type_name[] = {
    "FIELD_TYPE_DECIMAL",       //0x00   
    "FIELD_TYPE_TINY",          //0x01   
    "FIELD_TYPE_SHORT",         //0x02 
    "FIELD_TYPE_LONG",          //0x03 
    "FIELD_TYPE_FLOAT",         //0x04    
    "FIELD_TYPE_DOUBLE",        //0x05 
    "FIELD_TYPE_NULL",          //0x06 
    "FIELD_TYPE_TIMESTAMP",     //0x07 
    "FIELD_TYPE_LONGLONG",      //0x08   
    "FIELD_TYPE_INT24",         //0x09   
    "FIELD_TYPE_DATE",          //0x0a  
    "FIELD_TYPE_TIME",          //0x0b   
    "FIELD_TYPE_DATETIME",      //0x0c   
    "FIELD_TYPE_YEAR",          //0x0d   
    "FIELD_TYPE_NEWDATE",       //0x0e   
    "FIELD_TYPE_VARCHAR",       //0x0f 
    "FIELD_TYPE_BIT",           //0x10 
    "FIELD_TYPE_NEWDECIMAL",    //0xf6 
    "FIELD_TYPE_ENUM",          //0xf7   
    "FIELD_TYPE_SET",           //0xf8   
    "FIELD_TYPE_TINY_BLOB",     //0xf9   
    "FIELD_TYPE_MEDIUM_BLOB",   //0xfa   
    "FIELD_TYPE_LONG_BLOB",     //0xfb   
    "FIELD_TYPE_BLOB",          //0xfc   
    "FIELD_TYPE_VAR_STRING",    //0xfd   
    "FIELD_TYPE_STRING",        //0xfe  
    "FIELD_TYPE_GEOMETRY"       //0xff   
};

#define MAX_PACK_LEN 2048
#define MAX_COL_VALUE_LEN 512

#define PRIOR_TO 1
#define NOR_REL 0

typedef struct cetus_result_t {
    network_mysqld_proto_fielddefs_t *fielddefs;

    /**
     * if fielddefs != NULL, field_count equals fielddefs->len
     * sometime no need to parse fielddefs, we only want field_count
     */
    int field_count;
} cetus_result_t;

static void
cetus_result_destroy(cetus_result_t *res)
{
    if (res->fielddefs) {
        g_ptr_array_free(res->fielddefs, TRUE);
        res->fielddefs = NULL;
    }
}

static int compare_records_from_column(char *, char *, int, int, int *);

static int
check_str_num_supported(char *s, int len, char **p)
{
    if (len > 0) {
        if (s[0] == '+' || s[0] == '-') {
            s++;
            len--;
        }
    } else {
        return 1;
    }

    int i, is_valid = 1;
    int dot_cnt = 0, end = len - 1;

    for (i = 0; i < len; i++) {
        if (s[i] == '.') {
            dot_cnt++;
            if (i == 0 || i == end || dot_cnt > 1) {
                is_valid = 0;
            } else {
                if (p != NULL) {
                    *p = s + i;
                }
            }
        } else {
            if (s[i] < '0' || s[i] > '9') {
                is_valid = 0;
            }
        }
    }

    if (is_valid) {
        return 1;
    } else {
        return 0;
    }
}

static int
cmp_str_pos_num(char *s1, int len1, char *s2, int len2, int *compare_failed)
{
    char *p = NULL, *q = NULL;

    if (!check_str_num_supported(s1, len1, &p)) {
        g_warning("%s: str num is not supported:%s", G_STRLOC, s1);
        *compare_failed = 1;
        return -1;
    }

    if (!check_str_num_supported(s2, len2, &q)) {
        g_warning("%s: str num is not supported:%s", G_STRLOC, s2);
        *compare_failed = 1;
        return -1;
    }

    int s1_int_len;
    if (p == NULL) {
        s1_int_len = len1;
    } else {
        s1_int_len = p - s1;
    }

    int s2_int_len;
    if (q == NULL) {
        s2_int_len = len2;
    } else {
        s2_int_len = q - s2;
    }

    char *end = s1 + len1 - 1;
    while (s1[0] == '0' && s1[1] != '.' && s1 < end) {
        s1_int_len--;
        s1++;
        len1--;
    }

    end = s2 + len2 - 1;
    while (s2[0] == '0' && s2[1] != '.' && s2 < end) {
        s2_int_len--;
        s2++;
        len2--;
    }

    if (s1_int_len > s2_int_len) {
        return 1;
    } else if (s1_int_len < s2_int_len) {
        return -1;
    }

    int i;
    for (i = 0; i < s1_int_len; i++) {
        if (s1[i] > s2[i]) {
            return 1;
        } else if (s1[i] < s2[i]) {
            return -1;
        }
    }

    if (p == NULL && q == NULL) {
        return 0;
    }

    char *short1, *short2;
    if (p == NULL) {
        short2 = q + 1;
        while (short2[0] != '\0') {
            if (short2[0] != '0') {
                return -1;
            }
            short2++;
        }
        return 0;
    } else if (q == NULL) {
        short1 = p + 1;
        while (short1[0] != '\0') {
            if (short1[0] != '0') {
                return 1;
            }
            short1++;
        }
        return 0;
    } else {
        short1 = p + 1;
        short2 = q + 1;
    }

    int short_len = len1 - s1_int_len - 1;

    for (i = 0; i < short_len; i++) {
        if (short1[i] > short2[i]) {
            return 1;
        } else if (short1[i] < short2[i]) {
            return -1;
        }
    }

    return 0;
}

static int
cmp_str_num(char *s1, int len1, char *s2, int len2, int *compare_failed)
{
    if (len1 == 0 || len2 == 0) {
        g_critical("%s len in cmp_str_num is nil", G_STRLOC);
    }

    char *pos_s1 = s1, *pos_s2 = s2;
    int pos_len1 = len1, pos_len2 = len2;
    int neg_s1 = 0, neg_s2 = 0;

    if (s1[0] == '-') {
        pos_len1--;
        pos_s1++;
        neg_s1 = 1;
    } else if (s1[0] == '+') {
        pos_len1--;
        pos_s1++;
    }

    if (s2[0] == '-') {
        pos_len2--;
        pos_s2++;
        neg_s2 = 1;
    } else if (s2[0] == '+') {
        pos_len2--;
        pos_s2++;
    }

    if (neg_s1 && neg_s2) {
        return (-1) * cmp_str_pos_num(pos_s1, pos_len1, pos_s2, pos_len2, compare_failed);
    } else if (neg_s1) {
        return -1;
    } else if (neg_s2) {
        return 1;
    } else {
        return cmp_str_pos_num(pos_s1, pos_len1, pos_s2, pos_len2, compare_failed);
    }
}

static int
padding_zero(char *s1, int *p_len1, int size1, char *s2, int *p_len2, int size2)
{
    char *p = NULL, *q = NULL;

    int len1 = *p_len1;
    int len2 = *p_len2;

    if (len1 == 0) {
        s1[0] = '0';
        len1 = 1;
        *p_len1 = len1;
    } else {
        p = strchr(s1, '.');
    }

    if (len2 == 0) {
        s2[0] = '0';
        len2 = 1;
        *p_len2 = len2;
    } else {
        q = strchr(s2, '.');
    }

    if (p == NULL && q == NULL) {
        return 1;
    }

    int s1_int_len, s2_int_len;
    int short_len1 = 0, short_len2 = 0;
    char *short1, *short2;

    if (p == NULL) {
        s1_int_len = len1;
        s1[len1] = '.';
        *p_len1 = (*p_len1) + 1;
        short1 = s1 + len1 + 1;
    } else {
        s1_int_len = p - s1;
        short1 = p + 1;
        short_len1 = len1 - s1_int_len - 1;
    }

    if (q == NULL) {
        s2_int_len = len2;
        s2[len2] = '.';
        *p_len2 = (*p_len2) + 1;
        short2 = s2 + len2 + 1;
    } else {
        s2_int_len = q - s2;
        short2 = q + 1;
        short_len2 = len2 - s2_int_len - 1;
    }

    char *padding;
    int pad_len, new_str_len;
    if (short_len1 == short_len2) {
        return 1;
    } else if (short_len1 < short_len2) {
        padding = short1 + short_len1;
        pad_len = short_len2 - short_len1;
        new_str_len = (*p_len1) + pad_len;
        if (new_str_len >= size1) {
            return 0;
        }
        *p_len1 = new_str_len;
    } else {
        padding = short2 + short_len2;
        pad_len = short_len1 - short_len2;
        new_str_len = (*p_len2) + pad_len;
        if (new_str_len >= size2) {
            return 0;
        }
        *p_len2 = new_str_len;
    }

    int i;

    for (i = 0; i < pad_len; i++) {
        padding[i] = '0';
    }

    return 1;
}

static int
compare_str_num_value(char *str1, char *str2, int com_type, int *dest, int desc, int *compare_failed)
{
    int len1 = strlen(str1);
    int len2 = strlen(str2);

    if (!padding_zero(str1, &len1, MAX_COL_VALUE_LEN, str2, &len2, MAX_COL_VALUE_LEN)) {
        *compare_failed = 1;
        return 0;
    }

    int result = cmp_str_num(str1, len1, str2, len2, compare_failed);

    g_debug("%s:string str1:%s, str2:%s, cmp result:%d", G_STRLOC, str1, str2, result);

    if (result == 0) {
        return 1;
    }

    if ((desc && result > 0) || (!desc && result < 0)) {
        *dest = 1;
        return 0;
    }

    if ((desc && result < 0) || (!desc && result > 0)) {
        if (com_type == PRIOR_TO) {
            *dest = 0;
        } else {
            *dest = -1;
        }
        return 0;
    }

    g_critical("%s: reach the unreachable place", G_STRLOC);
    return 0;
}

static int
compare_date(char *str1, char *str2, int com_type, int *result, int desc)
{
    if (str1[0] == '\0') {
        strcpy(str1, EPOCH);
    }

    if (str2[0] == '\0') {
        strcpy(str2, EPOCH);
    }

    struct tm tm1 = { 0 };
    struct tm tm2 = { 0 };
    g_debug("%s:data1:%s, data2:%s", G_STRLOC, str1, str2);
    strptime(str1, "%Y-%m-%d", &tm1);
    strptime(str2, "%Y-%m-%d", &tm2);

    int diff[6];
    diff[0] = tm1.tm_year - tm2.tm_year;
    diff[1] = tm1.tm_mon - tm2.tm_mon;
    diff[2] = tm1.tm_mday - tm2.tm_mday;

    g_debug("%s:diff0:%d, diff1:%d, diff2:%d", G_STRLOC, diff[0], diff[1], diff[2]);
    int p;
    for (p = 0; p < 3; p++) {
        if ((desc && diff[p] > 0) || (!desc && diff[p] < 0)) {
            *result = 1;
            return 0;
        }

        if ((desc && diff[p] < 0) || (!desc && diff[p] > 0)) {
            if (com_type == PRIOR_TO) {
                *result = 0;
            } else {
                *result = -1;
            }
            return 0;
        }
    }

    return 1;
}

static int
compare_time(char *str1, char *str2, int com_type, int *result, int desc)
{
    if (str1[0] == '\0') {
        strcpy(str1, EPOCH);
    }

    if (str2[0] == '\0') {
        strcpy(str2, EPOCH);
    }

    struct tm tm1 = { 0 };
    struct tm tm2 = { 0 };

    strptime(str1, "%H:%M:%S", &tm1);
    strptime(str2, "%H:%M:%S", &tm2);

    int diff[3];
    diff[0] = tm1.tm_hour - tm2.tm_hour;
    diff[1] = tm1.tm_min - tm2.tm_min;
    diff[2] = tm1.tm_sec - tm2.tm_sec;

    int p;
    for (p = 0; p < 3; p++) {
        if ((desc && diff[p] > 0) || (!desc && diff[p] < 0)) {
            *result = 1;
            return 0;
        }

        if ((desc && diff[p] < 0) || (!desc && diff[p] > 0)) {
            if (com_type == PRIOR_TO) {
                *result = 0;
            } else {
                *result = -1;
            }

            return 0;
        }
    }

    return 1;
}

static int
compare_year(char *str1, char *str2, int com_type, int *result, int desc)
{
    if (str1[0] == '\0') {
        strcpy(str1, EPOCH);
    }

    if (str2[0] == '\0') {
        strcpy(str2, EPOCH);
    }

    long y1 = atol(str1);
    long y2 = atol(str2);

    if (y1 == y2) {
        return 1;
    }

    if (desc) {
        *result = y1 > y2;
    } else {
        *result = y1 < y2;
    }

    if (com_type != PRIOR_TO) {
        if (*result == 0) {
            *result = -1;
        }
    }

    return 0;
}

/* skip some column (lenenc_str or NULL) */
static inline gint
skip_field(network_packet *packet, guint skip)
{
    guint iter;

    for (iter = 0; iter < skip; iter++) {
        guint8 first = 0;
        if (network_mysqld_proto_peek_int8(packet, &first) == -1) {
            return -1;
        }

        if (first == MYSQLD_PACKET_NULL) {
            network_mysqld_proto_skip(packet, 1);
        } else {
            if (network_mysqld_proto_skip_lenenc_str(packet) == -1) {
                return -1;
            }
        }
    }
    return 0;
}

#define MAX_ORDER_BY_ITEMS 32

/**
 * @breif For all ORDER-BY columns, get their offsets inside Row-Packet
 * @return At most 4 offset values embedded in a 64-bit integer: 4 * int16_t --> int64_t
 */
static guint64
get_field_offsets(network_packet *packet, ORDER_BY order_array[], int order_array_size)
{
    int i, max_pos = 0;
    int orderby_count = MIN(order_array_size, 4);

    for (i = 0; i < orderby_count; i++) {
        if (order_array[i].pos > max_pos) {
            max_pos = order_array[i].pos;
        }
    }

    if (max_pos > MAX_ORDER_BY_ITEMS) {
        return 0;
    }

    unsigned char map[MAX_ORDER_BY_ITEMS] = { 0 };  /* field pos ==> order by pos */
    for (i = 0; i < orderby_count; i++) {
        /* hack: add 1 for existence probing */
        map[order_array[i].pos] = i + 1;
    }

    guint64 value = 0;
    if (max_pos == 0) {
        value = NET_HEADER_SIZE;
    }

    guint iter;
    for (iter = 0; iter < max_pos; iter++) {
        if (packet->data->str[packet->offset] == (char)MYSQLD_PACKET_NULL) {
            network_mysqld_proto_skip(packet, 1);
        } else {
            if (network_mysqld_proto_skip_lenenc_str(packet) == -1) {
                return 0;
            }
        }
        size_t next_iter = iter + 1;
        if (next_iter < MAX_ORDER_BY_ITEMS && map[next_iter]) {
            int seq = map[next_iter] - 1;
            guint64 new_value = packet->offset;
            g_debug("offset value:%ld, seq=%d, iter=%d %p", new_value, seq, iter, packet->data);
            if (new_value <= 0xFFFF) {
                int j;
                for (j = 0; j < seq; j++) {
                    new_value = new_value << 16;
                }

                value += new_value;
            }
            g_debug("part all value set:%ld for %p", value, packet->data);
        }
    }

    return value;
}

/**
 *      OK Packet                   0x00
 *      Error Packet                0xff  255
 *      Result Set Packet           1-250 (first byte of Length-Coded Binary)
 *      Field Packet                1-250 ("")
 *      Row Data Packet             1-250 ("")
 *      EOF Packet                  0xfe  254
 */
static inline guchar
get_pkt_type(GString *pkt)
{
    return (unsigned char)pkt->str[NET_HEADER_SIZE];
}

static char *
retrieve_aggr_value(GString *data, int pos, char *str)
{
    network_packet packet;
    packet.data = data;
    packet.offset = NET_HEADER_SIZE;
    skip_field(&packet, pos);
    network_mysqld_proto_get_column(&packet, str, MAX_COL_VALUE_LEN);

    return str;
}

static char *
str_int_add(char *merged_int, char *s1, int len1, char *s2, int len2, int carry)
{
    char *large = s2;
    int len = len1;
    int max_len = len2;

    if (len1 > len2) {
        len = len2;
        max_len = len1;
        large = s1;
    }

    char *p = s1 + len1 - 1;
    char *q = s2 + len2 - 1;
    char *dest = merged_int + max_len;
    int tmp = carry;
    int i;
    for (i = len - 1; i >= 0; i--) {
        tmp += *p - '0' + *q - '0';
        *dest = tmp % 10 + '0';
        tmp = tmp / 10;
        p--;
        q--;
        dest--;
    }

    len = max_len - len;
    if (len == 0) {
        *dest = tmp + '0';
    } else {
        large = large + len - 1;
        for (i = len - 1; i >= 0; i--) {
            tmp += *large - '0';
            *dest = tmp % 10 + '0';
            tmp = tmp / 10;
            large--;
            dest--;
        }

        *dest = tmp + '0';
    }

    return merged_int;
}

static char *
str_int_sub(char *merged_int, char *s1, int len1, char *s2, int len2, int borrow)
{
    char *p = s1 + len1 - 1;
    char *q = s2 + len2 - 1;
    char *dest = merged_int + len1 - 1;
    int tmp = 0;
    int i;
    for (i = len2 - 1; i >= 0; i--) {
        tmp = (*p - '0') - (*q - '0') - borrow;
        if (tmp < 0) {
            borrow = 1;
            tmp = tmp + 10;
        } else {
            borrow = 0;
        }

        *dest = tmp + '0';
        p--;
        q--;
        dest--;
    }

    int len = len1 - len2;
    if (len != 0) {
        char *large = s1 + len - 1;
        for (i = len - 1; i >= 0; i--) {
            tmp = (*large - '0') - borrow;
            if (tmp < 0) {
                borrow = 1;
                tmp = tmp + 10;
            } else {
                borrow = 0;
            }
            *dest = tmp + '0';
            large--;
            dest--;
        }
    }

    return merged_int;
}

static char *
str_decimal_add(char *merged_value, char *s1, int len1, char *s2, int len2)
{
    char *p, *q;

    p = strchr(s1, '.');
    q = strchr(s2, '.');

    if (p != NULL && q != NULL) {
        p[0] = '\0';
        q[0] = '\0';

        int s1_int_len = p - s1;
        int s2_int_len = q - s2;

        int int_len;
        if (s1_int_len < s2_int_len) {
            int_len = s2_int_len;
        } else {
            int_len = s1_int_len;
        }

        char *short1 = p + 1;
        char *short2 = q + 1;
        char *merged_short = merged_value + int_len + 1;

        merged_short[0] = '.';
        merged_short++;

        int short_len = len1 - s1_int_len - 1;

        int i;

        int tmp = 0;
        for (i = short_len - 1; i >= 0; i--) {
            tmp += short1[i] - '0' + short2[i] - '0';
            merged_short[i] = tmp % 10 + '0';
            tmp = tmp / 10;
        }

        str_int_add(merged_value, s1, s1_int_len, s2, s2_int_len, tmp);

        p[0] = '.';
        q[0] = '.';

    } else {
        str_int_add(merged_value, s1, len1, s2, len2, 0);
    }

    return merged_value;
}

static char *
str_decimal_sub(char *merged_value, char *s1, int len1, char *s2, int len2)
{
    char *p, *q;

    p = strchr(s1, '.');
    q = strchr(s2, '.');

    if (p != NULL && q != NULL) {
        p[0] = '\0';
        q[0] = '\0';

        int s1_int_len = p - s1;
        int s2_int_len = q - s2;

        int int_len;
        if (s1_int_len < s2_int_len) {
            int_len = s2_int_len;
        } else {
            int_len = s1_int_len;
        }

        char *short1 = p + 1;
        char *short2 = q + 1;
        char *merged_short = merged_value + int_len;

        merged_short[0] = '.';
        merged_short++;

        int short_len = len1 - s1_int_len - 1;

        int i;

        int borrow = 0;
        for (i = short_len - 1; i >= 0; i--) {
            int tmp = (short1[i] - '0') - (short2[i] - '0') - borrow;
            if (tmp < 0) {
                borrow = 1;
                tmp = tmp + 10;
            } else {
                borrow = 0;
            }
            merged_short[i] = tmp + '0';
        }

        str_int_sub(merged_value, s1, s1_int_len, s2, s2_int_len, borrow);

        p[0] = '.';
        q[0] = '.';

    } else {
        str_int_sub(merged_value, s1, len1, s2, len2, 0);
    }

    return merged_value;
}

static void
trim_zero(char *s)
{
    char *p = s;
    char *q = NULL;
    int depth = 0;
    while (p[0] == '0') {
        if (p[1] >= '0' && p[1] <= '9') {
            depth++;
            p++;
            if (p[0] == '.') {
                q = p - 1;
            } else if (p[0] != '0') {
                q = p;
            }
        } else {
            break;
        }
    }

    if (depth > 0) {
        if (q == NULL) {
            q = p;
        }
    }

    if (q != NULL) {
        p = s;
        while (q[0] != '\0') {
            *p++ = *q++;
        }
        *p = '\0';
    }
}

static int
dispose_sign_add(int is_integer, char *merged_value, char *s1, int len1, char *s2, int len2, int *supported)
{
    if (s1[0] != '-' && s2[0] != '-') {
        if (is_integer) {
            str_int_add(merged_value, s1, len1, s2, len2, 0);
        } else {
            str_decimal_add(merged_value, s1, len1, s2, len2);
        }
        trim_zero(merged_value);
    } else if (s1[0] == '-' && s2[0] == '-') {
        if (is_integer) {
            str_int_add(merged_value + 1, s1 + 1, len1 - 1, s2 + 1, len2 - 1, 0);
        } else {
            str_decimal_add(merged_value + 1, s1 + 1, len1 - 1, s2 + 1, len2 - 1);
        }
        trim_zero(merged_value + 1);
        merged_value[0] = '-';
    } else if (s1[0] == '-') {
        int result = cmp_str_num(s1 + 1, len1 - 1, s2, len2, supported);
        if (result > 0) {
            if (is_integer) {
                str_int_sub(merged_value + 1, s1 + 1, len1 - 1, s2, len2, 0);
            } else {
                str_decimal_sub(merged_value + 1, s1 + 1, len1 - 1, s2, len2);
            }
            trim_zero(merged_value + 1);
            merged_value[0] = '-';
        } else if (result < 0) {
            if (is_integer) {
                str_int_sub(merged_value, s2, len2, s1 + 1, len1 - 1, 0);
            } else {
                str_decimal_sub(merged_value, s2, len2, s1 + 1, len1 - 1);
            }
            trim_zero(merged_value);
        } else {
            merged_value[0] = '0';
            return 0;
        }
    } else if (s2[0] == '-') {
        int result = cmp_str_num(s1, len1, s2 + 1, len2 - 1, supported);
        if (result > 0) {
            if (is_integer) {
                str_int_sub(merged_value, s1 + 1, len1 - 1, s2, len2, 0);
            } else {
                str_decimal_sub(merged_value, s1, len1, s2 + 1, len2 - 1);
            }
            trim_zero(merged_value);
        } else if (result < 0) {
            if (is_integer) {
                str_int_sub(merged_value + 1, s2 + 1, len2 - 1, s1, len1, 0);
            } else {
                str_decimal_sub(merged_value + 1, s2 + 1, len2 - 1, s1, len1);
            }
            trim_zero(merged_value + 1);
            merged_value[0] = '-';
        } else {
            merged_value[0] = '0';
            return 0;
        }
    }

    return 1;
}

static char *
str_add(int type, char *merged_value, char *s1, int len1, char *s2, int len2, int *merge_failed)
{
    int is_result_padding = 0, is_integer = 0, is_need_check = 0;

    switch (type) {
    case FIELD_TYPE_TINY:
    case FIELD_TYPE_SHORT:
    case FIELD_TYPE_LONG:
    case FIELD_TYPE_LONGLONG:
    case FIELD_TYPE_INT24:
        is_integer = 1;
        break;
    case FIELD_TYPE_DOUBLE:
    case FIELD_TYPE_FLOAT:
        is_need_check = 1;
        break;
    case FIELD_TYPE_NEWDECIMAL:
    case FIELD_TYPE_DECIMAL:
        is_result_padding = 1;
        break;
    default:
        g_warning("%s: unknown type for add:%d", G_STRLOC, type);
        *merge_failed = 1;
        return NULL;
    }

    if (!is_integer) {
        if (is_need_check) {
            if (!check_str_num_supported(s1, len1, NULL)) {
                g_warning("%s: str num is not supported:%s", G_STRLOC, s1);
                *merge_failed = 1;
                return NULL;
            }
            if (!check_str_num_supported(s2, len2, NULL)) {
                g_warning("%s: str num is not supported:%s", G_STRLOC, s2);
                *merge_failed = 1;
                return NULL;
            }
        }

        if (!padding_zero(s1, &len1, MAX_COL_VALUE_LEN, s2, &len2, MAX_COL_VALUE_LEN)) {
            *merge_failed = 1;
            return NULL;
        }
    }

    int result = dispose_sign_add(is_integer, merged_value, s1,
                                  len1, s2, len2, merge_failed);
    if (*merge_failed) {
        return NULL;
    }

    if ((!result) && is_result_padding) {
        int merged_len = strlen(merged_value);
        if (!padding_zero(merged_value, &merged_len, 2 * MAX_COL_VALUE_LEN, s2, &len2, MAX_COL_VALUE_LEN)) {
            *merge_failed = 1;
            return NULL;
        }
    }

    return merged_value;
}

static int
merge_aggr_value(int fun_type, int type, char *merged_value,
                 char *str1, char *str2, int len1, int len2, int *merge_failed)
{
    if (len1 == 0 || len2 == 0) {
        if (len1 == 0 && len2 == 0) {
            return 0;
        } else if (len1 == 0) {
            strncpy(merged_value, str2, len2);
            merged_value[len2] = '\0';
            return 1;
        } else {
            strncpy(merged_value, str1, len1);
            merged_value[len1] = '\0';
            return 1;
        }
    }

    int is_str_type = 0;
    switch (type) {
    case FIELD_TYPE_TIME:
    case FIELD_TYPE_TIMESTAMP:
    case FIELD_TYPE_DATETIME:
    case FIELD_TYPE_YEAR:
    case FIELD_TYPE_NEWDATE:
    case FIELD_TYPE_DATE:
    case FIELD_TYPE_VAR_STRING:
    case FIELD_TYPE_STRING:
        is_str_type = 1;
        break;
    default:
        break;
    }

    switch (fun_type) {
    case FT_SUM:
        if (!str_add(type, merged_value, str1, len1, str2, len2, merge_failed)) {
            return 0;
        }
        break;
    case FT_MAX:
        if (is_str_type) {
            if (strcmp(str1, str2) >= 0) {
                return 0;
            } else {
                strncpy(merged_value, str2, len2);
                merged_value[len2] = '\0';
            }
        } else {
            if (cmp_str_num(str1, len1, str2, len2, merge_failed) > 0) {
                return 0;
            } else {
                strncpy(merged_value, str2, len2);
                merged_value[len2] = '\0';
            }
        }
        break;
    case FT_MIN:
        if (is_str_type) {
            if (strcmp(str1, str2) <= 0) {
                return 0;
            } else {
                strncpy(merged_value, str2, len2);
                merged_value[len2] = '\0';
            }
        } else {
            if (cmp_str_num(str1, len1, str2, len2, merge_failed) <= 0) {
                return 0;
            } else {
                strncpy(merged_value, str2, len2);
                merged_value[len2] = '\0';
            }
        }
        break;
    case FT_COUNT:
        if (!str_add(type, merged_value, str1, len1, str2, len2, merge_failed)) {
            return 0;
        }
        break;
    }

    return 1;
}

static int
modify_record(GList *cand1, group_aggr_t * aggr,
              network_packet *packet1, network_packet *packet2, int *orig_packet_len, int *merge_failed)
{
    char str1[MAX_COL_VALUE_LEN] = { 0 };
    char str2[MAX_COL_VALUE_LEN] = { 0 };
    char merged_value[2 * MAX_COL_VALUE_LEN] = { 0 };

    char *before, *hit, *after;

    GString *pkt1 = packet1->data;

    before = (char *)(pkt1->str + packet1->offset);
    skip_field(packet1, aggr->pos);
    skip_field(packet2, aggr->pos);

    hit = (char *)(pkt1->str + packet1->offset);
    network_mysqld_proto_get_column(packet1, str1, MAX_COL_VALUE_LEN);
    network_mysqld_proto_get_column(packet2, str2, MAX_COL_VALUE_LEN);

    after = (char *)(pkt1->str + packet1->offset);

    char buffer[MAX_PACK_LEN] = { 0 };
    char *buf_pos = buffer;
    int len = hit - before;
    memcpy(buf_pos, before, len);
    buf_pos = buf_pos + len + 1;

    int len1 = strlen(str1);
    int len2 = strlen(str2);
    if (!merge_aggr_value(aggr->fun_type, aggr->type, merged_value, str1, str2, len1, len2, merge_failed)) {
        return 0;
    }

    len = strlen(merged_value);
    if (aggr->type == FIELD_TYPE_DOUBLE || aggr->type == FIELD_TYPE_FLOAT) {
        char *p = strchr(merged_value, '.');
        if (p != NULL) {
            int i = len - 1;
            while (i >= 1 && merged_value[i] == '0') {
                len--;
                i--;
            }

            if (merged_value[i] == '.') {
                len--;
            }
        }
    }

    if (len == len1) {
        memcpy(hit + 1, merged_value, len);
    } else {
        int packet_len = (*orig_packet_len) - len1 + len;

        if (packet_len > MAX_PACK_LEN) {
            *merge_failed = 1;
            return 0;
        }

        strncpy(buf_pos, merged_value, len);
        *(buf_pos - 1) = (char)len;
        buf_pos = buf_pos + len;
        memcpy(buf_pos, after, (*orig_packet_len) - (after - before));

        *orig_packet_len = packet_len;
        GString *packet = g_string_sized_new(calculate_alloc_len(NET_HEADER_SIZE + packet_len));
        packet->len = NET_HEADER_SIZE;
        g_string_append_len(packet, buffer, packet_len);
        network_mysqld_proto_set_packet_len(packet, packet_len);

        g_string_free(cand1->data, TRUE);
        cand1->data = packet;
        packet1->data = packet;
    }

    return 1;
}

static gint
combine_aggr_record(GList *cand1, GList *cand2, aggr_by_group_para_t *para, int *merge_failed)
{
    int i;
    network_packet packet1;
    network_packet packet2;
    packet1.data = cand1->data;
    packet2.data = cand2->data;

    int orig_packet_len = network_mysqld_proto_get_packet_len(packet1.data);

    if (orig_packet_len >= MAX_PACK_LEN) {
        g_warning("%s record too long for group by", G_STRLOC);
        *merge_failed = 1;
        return 0;
    }

    short aggr_num = para->aggr_num;

    for (i = 0; i < aggr_num; i++) {
        group_aggr_t *aggr = para->aggr_array + (aggr_num - 1 - i);

        packet1.offset = NET_HEADER_SIZE;
        packet2.offset = NET_HEADER_SIZE;

        /* if equal, combined */
        switch (aggr->type) {
        case FIELD_TYPE_TINY:
        case FIELD_TYPE_SHORT:
        case FIELD_TYPE_LONG:
        case FIELD_TYPE_LONGLONG:
        case FIELD_TYPE_INT24:
            modify_record(cand1, aggr, &packet1, &packet2, &orig_packet_len, merge_failed);
            break;
        case FIELD_TYPE_NEWDECIMAL:
        case FIELD_TYPE_DECIMAL:
        case FIELD_TYPE_FLOAT:
        case FIELD_TYPE_DOUBLE:
            modify_record(cand1, aggr, &packet1, &packet2, &orig_packet_len, merge_failed);
            break;
        case FIELD_TYPE_TIME:
        case FIELD_TYPE_TIMESTAMP:
        case FIELD_TYPE_DATETIME:
        case FIELD_TYPE_YEAR:
        case FIELD_TYPE_NEWDATE:
        case FIELD_TYPE_DATE:
        case FIELD_TYPE_VAR_STRING:
        case FIELD_TYPE_STRING:
            switch (aggr->fun_type) {
            case FT_MAX:
            case FT_MIN:
                break;
            default:
                g_warning("%s: string is not valid for aggr fun:%d", G_STRLOC, aggr->fun_type);
                *merge_failed = 1;
                return 0;
            }

            modify_record(cand1, aggr, &packet1, &packet2, &orig_packet_len, merge_failed);
            break;

        default:
            *merge_failed = 1;
            g_warning("%s:unknown Field Type: %d", G_STRLOC, para->group_array[i].type);
            return 1;
        }
    }
    return 0;
}

static gint
cal_aggr_rec_rel(GList *cand1, GList *cand2, aggr_by_group_para_t *para, int *merge_failed)
{
    int i, disp_flag;
    char str1[MAX_COL_VALUE_LEN] = { 0 };
    char str2[MAX_COL_VALUE_LEN] = { 0 };
    network_packet packet1;
    network_packet packet2;
    packet1.data = cand1->data;
    packet2.data = cand2->data;

    group_by_t *group_array = para->group_array;

    for (i = 0; i < para->group_array_size; i++) {
        disp_flag = 0;

        packet1.offset = NET_HEADER_SIZE;
        packet2.offset = NET_HEADER_SIZE;

        skip_field(&packet1, group_array[i].pos);
        skip_field(&packet2, group_array[i].pos);

        network_mysqld_proto_get_column(&packet1, str1, MAX_COL_VALUE_LEN);
        network_mysqld_proto_get_column(&packet2, str2, MAX_COL_VALUE_LEN);

        switch (group_array[i].type) {
        case FIELD_TYPE_DATE:
            if (!compare_date(str1, str2, NOR_REL, &disp_flag, group_array[i].desc)) {
                return disp_flag;
            }
            break;
        case FIELD_TYPE_TIME:
            if (!compare_time(str1, str2, NOR_REL, &disp_flag, group_array[i].desc)) {
                return disp_flag;
            }
            break;
        case FIELD_TYPE_YEAR:
            if (!compare_year(str1, str2, NOR_REL, &disp_flag, group_array[i].desc)) {
                return disp_flag;
            }
            break;
        case FIELD_TYPE_NEWDATE:
            return 1;
            /* case FIELD_TYPE_VARCHAR: */
        case FIELD_TYPE_VAR_STRING:
        case FIELD_TYPE_TIMESTAMP:
        case FIELD_TYPE_DATETIME:
        case FIELD_TYPE_STRING:
            if (!compare_records_from_column(str1, str2, NOR_REL, group_array[i].desc, &disp_flag)) {
                return disp_flag;
            }

            break;
        case FIELD_TYPE_TINY:
        case FIELD_TYPE_SHORT:
        case FIELD_TYPE_LONG:
        case FIELD_TYPE_LONGLONG:
        case FIELD_TYPE_INT24:
        case FIELD_TYPE_DOUBLE:
        case FIELD_TYPE_NEWDECIMAL:
        case FIELD_TYPE_DECIMAL:
        case FIELD_TYPE_FLOAT:
            if (!compare_str_num_value(str1, str2, NOR_REL, &disp_flag, group_array[i].desc, merge_failed)) {
                return disp_flag;
            }
            break;

        default:
            g_warning("%s:unknown Field Type: %d", G_STRLOC, group_array[i].type);
            *merge_failed = 1;
            return 1;
        }
    }
    return combine_aggr_record(cand1, cand2, para, merge_failed);
}

static int heap_count = 0;

static int
compare_value_from_records(network_packet *packet1, network_packet *packet2,
                           ORDER_BY *ob, int type, int *result, int *compare_failed)
{
    char str1[MAX_COL_VALUE_LEN] = { 0 };
    char str2[MAX_COL_VALUE_LEN] = { 0 };

    skip_field(packet1, ob->pos);
    skip_field(packet2, ob->pos);
    network_mysqld_proto_get_column(packet1, str1, MAX_COL_VALUE_LEN);
    network_mysqld_proto_get_column(packet2, str2, MAX_COL_VALUE_LEN);

    switch (type) {
    case FIELD_TYPE_LONG:
    case FIELD_TYPE_DOUBLE:
        return compare_str_num_value(str1, str2, PRIOR_TO, result, ob->desc, compare_failed);
    case FIELD_TYPE_DATE:
        return compare_date(str1, str2, PRIOR_TO, result, ob->desc);
    case FIELD_TYPE_TIME:
        return compare_time(str1, str2, PRIOR_TO, result, ob->desc);
    case FIELD_TYPE_YEAR:
        return compare_year(str1, str2, PRIOR_TO, result, ob->desc);
    }
    return 1;
}

static int
compare_records_from_column(char *str1, char *str2, int com_type, int desc, int *result)
{
    g_debug("%s: str1:%s, str2:%s", G_STRLOC, str1, str2);
    int ret = 0;
    if (str1[0] == '\0' && str2[0] != '\0') {
        ret = -1;
    } else if (str1[0] != '\0' && str2[0] == '\0') {
        ret = 1;
    } else if (str1[0] != '\0' && str2[0] != '\0') {
        ret = strcasecmp(str1, str2);
    } else {
        ret = 0;
    }

    if (ret == 0) {
        return 1;
    }

    if ((desc && ret > 0) || (!desc && ret < 0)) {
        *result = 1;
        return 0;
    }

    if ((desc && ret < 0) || (!desc && ret > 0)) {
        if (com_type == PRIOR_TO) {
            *result = 0;
        } else {
            *result = -1;
        }
        return 0;
    }

    g_critical("%s: reach the unreachable place", G_STRLOC);

    return 0;
}

/* short means 16-bit integer */
static guint16 get_nth_short(guint64 base, int n)
{
    g_assert(n < 4);
    int i;
    guint64 mask = 0xFFFF;
    for (i = 0; i < n; i++) {
        mask = mask << 16;
    }
    guint64 value = mask & base;

    for (i = 0; i < n; i++) {
        value = value >> 16;
    }
    return value;
}

static int
compare_records_by_str(network_packet *packet1, network_packet *packet2,
                       order_by_para_t *para, int pkt1_index, int pkt2_index, int i, int *result)
{
    char str1[MAX_COL_VALUE_LEN] = { 0 };
    char str2[MAX_COL_VALUE_LEN] = { 0 };
    ORDER_BY *ob = &(para->order_array[i]);

    uint64_t offsets_1, offsets_2;
    if (para->field_offsets_cache[pkt1_index]) {
        offsets_1 = para->field_offsets_cache[pkt1_index];
    } else {
        offsets_1 = get_field_offsets(packet1, para->order_array, para->order_array_size);
        para->field_offsets_cache[pkt1_index] = offsets_1;
    }

    guint offset = get_nth_short(offsets_1, i);
    if (offset == 0) {
        offset = NET_HEADER_SIZE;
    }
    packet1->offset = offset;

    if (para->field_offsets_cache[pkt2_index]) {
        offsets_2 = para->field_offsets_cache[pkt2_index];
    } else {
        offsets_2 = get_field_offsets(packet2, para->order_array, para->order_array_size);
        para->field_offsets_cache[pkt2_index] = offsets_2;
    }

    offset = get_nth_short(offsets_2, i);
    if (offset == 0) {
        offset = NET_HEADER_SIZE;
    }
    packet2->offset = offset;

    network_mysqld_proto_get_column(packet1, str1, MAX_COL_VALUE_LEN);
    network_mysqld_proto_get_column(packet2, str2, MAX_COL_VALUE_LEN);

    return compare_records_from_column(str1, str2, PRIOR_TO, ob->desc, result);
}

/**
 *  is_prior_to Relation(record_A *record_B) defined ORDER BY
 *  return 1 if record A is prior to record B  else 0
 */
static gint
is_prior_to(GString *pkt1, GString *pkt2, order_by_para_t *para,
            int pkt1_index, int pkt2_index, int *is_record_equal, int *compare_failed)
{
    int i, equal_field_cnt, result;
    network_packet packet1;
    network_packet packet2;

    packet1.data = pkt1;
    packet2.data = pkt2;

    equal_field_cnt = 0;

    g_debug("%s: call is_prior_to, index1:%d, index2:%d, count:%d, pkt1:%p, pkt2:%p",
            G_STRLOC, pkt1_index, pkt2_index, ++heap_count, pkt1, pkt2);

    for (i = 0; i < para->order_array_size; i++) {
        result = 0;
        packet1.offset = NET_HEADER_SIZE;
        packet2.offset = NET_HEADER_SIZE;

        ORDER_BY *order = &(para->order_array[i]);

        switch (order->type) {
        case FIELD_TYPE_TINY:
        case FIELD_TYPE_SHORT:
        case FIELD_TYPE_LONG:
        case FIELD_TYPE_LONGLONG:
        case FIELD_TYPE_INT24:
            if (!compare_value_from_records(&packet1, &packet2, order, FIELD_TYPE_LONG, &result, compare_failed)) {
                return result;
            }
            equal_field_cnt++;
            break;
        case FIELD_TYPE_NEWDECIMAL:
        case FIELD_TYPE_DECIMAL:
        case FIELD_TYPE_FLOAT:
        case FIELD_TYPE_DOUBLE:
            if (!compare_value_from_records(&packet1, &packet2, order, FIELD_TYPE_DOUBLE, &result, compare_failed)) {
                return result;
            }
            equal_field_cnt++;
            break;
        case FIELD_TYPE_DATE:
            if (!compare_value_from_records(&packet1, &packet2, order, FIELD_TYPE_DATE, &result, compare_failed)) {
                return result;
            }

            equal_field_cnt++;
            break;
        case FIELD_TYPE_TIME:
            if (!compare_value_from_records(&packet1, &packet2, order, FIELD_TYPE_DATE, &result, compare_failed)) {
                return result;
            }
            equal_field_cnt++;
            break;
        case FIELD_TYPE_YEAR:
            if (!compare_value_from_records(&packet1, &packet2, order, FIELD_TYPE_DATE, &result, compare_failed)) {
                return result;
            }
            equal_field_cnt++;
            break;
        case FIELD_TYPE_NEWDATE:
            return 1;
            /* case FIELD_TYPE_VARCHAR: */
        case FIELD_TYPE_TIMESTAMP:
        case FIELD_TYPE_DATETIME:
        case FIELD_TYPE_VAR_STRING:
        case FIELD_TYPE_STRING:
            if (!compare_records_by_str(&packet1, &packet2, para, pkt1_index, pkt2_index, i, &result)) {
                return result;
            }
            equal_field_cnt++;
            break;
        case FIELD_TYPE_NULL:
        case FIELD_TYPE_BIT:
        case FIELD_TYPE_ENUM:
        case FIELD_TYPE_SET:
        case FIELD_TYPE_TINY_BLOB:
        case FIELD_TYPE_MEDIUM_BLOB:
        case FIELD_TYPE_LONG_BLOB:
        case FIELD_TYPE_BLOB:
        case FIELD_TYPE_GEOMETRY:
            return 1;
        default:
            *compare_failed = 1;
            g_warning("%s:unknown Field Type: %d", G_STRLOC, order->type);
            return 1;
        }
    }

    if (equal_field_cnt == para->order_array_size) {
        if (is_record_equal) {
            *is_record_equal = 1;
        }
    }

    return 1;
}

/* find index of field by name, the name might be an alias */
static int
cetus_result_find_fielddef(cetus_result_t *res, const char *table, const char *field)
{
    int i;
    for (i = 0; i < res->fielddefs->len; ++i) {
        network_mysqld_proto_fielddef_t *fdef = g_ptr_array_index(res->fielddefs, i);
        if (table && table[0] != '\0') {
            if ((fdef->table && strcmp(table, fdef->table) == 0)
                || (fdef->org_table && strcmp(table, fdef->org_table) == 0)) {
                if ((fdef->name && strcmp(field, fdef->name) == 0)
                    || (fdef->org_name && strcmp(field, fdef->org_name) == 0))
                    return i;
            }
        } else {
            if ((fdef->name && strcmp(field, fdef->name) == 0)
                || (fdef->org_name && strcmp(field, fdef->org_name) == 0))
                return i;
        }
    }
    return -1;
}

static gboolean
cetus_result_parse_fielddefs(cetus_result_t *res_merge, GQueue *input)
{
    if (res_merge->field_count >= input->length) {
        g_critical("%s: res_merge->field_count:%d, queue length:%d",
                G_STRLOC, res_merge->field_count, input->length);
        return FALSE;
    }

    network_packet packet = { 0 };

    res_merge->fielddefs = g_ptr_array_new_with_free_func((GDestroyNotify)network_mysqld_proto_fielddef_free);
    int i;
    for (i = 0; i < res_merge->field_count; ++i) {
        /* TODO g_queue_peek_nth is not efficient*/
        packet.data = g_queue_peek_nth(input, i + 1);
        packet.offset = 0;
        network_mysqld_proto_skip_network_header(&packet);
        network_mysqld_proto_fielddef_t *fdef;
        fdef = network_mysqld_proto_fielddef_new();
        int err = network_mysqld_proto_get_fielddef(&packet, fdef, CLIENT_PROTOCOL_41);
        if (err) {
            network_mysqld_proto_fielddef_free(fdef);
            g_ptr_array_free(res_merge->fielddefs, TRUE);
            res_merge->fielddefs = NULL;
            return FALSE;
        }
        g_ptr_array_add(res_merge->fielddefs, fdef);
    }

    return TRUE;
}

static gboolean
cetus_result_retrieve_field_count(GQueue *input, guint64 *p_field_count)
{
    g_debug("%s:call cetus_result_retrieve_field_count", G_STRLOC);
    int packet_count = g_queue_get_length(input);
    network_packet packet = { 0 };
    packet.data = g_queue_peek_head(input); /* Number-of-Field packet */
    int err = network_mysqld_proto_skip_network_header(&packet);
    guint64 field_count;
    err |= network_mysqld_proto_get_lenenc_int(&packet, &field_count);
    if (err || field_count >= packet_count) {
        return FALSE;
    }

    *p_field_count = field_count;

    return TRUE;
}

/**
 * Get order_array.pos, order_array.type
 */
static gboolean
get_order_by_fields(cetus_result_t *res_merge, ORDER_BY *order_array,
                    guint order_array_size, result_merge_t *merged_result)
{
    int i;
    for (i = 0; i < order_array_size; ++i) {
        ORDER_BY *orderby = &(order_array[i]);

        if (orderby->pos == -1) {
            int index = cetus_result_find_fielddef(res_merge,
                                                   orderby->table_name, orderby->name);
            if (index == -1) {
                merged_result->status = RM_FAIL;
                char msg[128] = { 0 };
                snprintf(msg, sizeof(msg), "order by:no %s in field list", orderby->name);
                merged_result->detail = g_string_new(msg);
                return FALSE;
            }
            orderby->pos = index;
        }
        network_mysqld_proto_fielddef_t *fdef = g_ptr_array_index(res_merge->fielddefs, orderby->pos);
        orderby->type = fdef->type;
    }
    return TRUE;
}

static gboolean
get_group_by_fields(cetus_result_t *res_merge, group_by_t *group_array, guint group_array_size,
                    result_merge_t *merged_result)
{
    int i;
    for (i = 0; i < group_array_size; ++i) {
        group_by_t *groupby = &(group_array[i]);
        if (groupby->pos == -1) {
            int index = cetus_result_find_fielddef(res_merge,
                                                   groupby->table_name, groupby->name);
            if (index == -1) {
                merged_result->status = RM_FAIL;
                char msg[128] = { 0 };
                snprintf(msg, sizeof(msg), "group by: no %s in field list", groupby->name);
                merged_result->detail = g_string_new(msg);
                return FALSE;
            }
            groupby->pos = index;
        }
        network_mysqld_proto_fielddef_t *fdef = g_ptr_array_index(res_merge->fielddefs, groupby->pos);
        groupby->type = fdef->type;
    }
    return TRUE;
}

static gboolean
fulfill_condi(char *aggr_value, having_condition_t *hav_condi, result_merge_t *merged_result)
{
    int is_num = 0;
    switch (hav_condi->data_type) {
    case TK_INTEGER:
        is_num = 1;
        break;
    case TK_FLOAT:
        is_num = 1;
        break;
    default:
        break;
    }

    int len1 = strlen(aggr_value);
    int len2 = strlen(hav_condi->condition_value);
    int result;

    if (is_num) {
        if (len2 == 0) {
            return TRUE;
        }

        if (len1 == 0) {
            return FALSE;
        }

        int num_unsupported = 0;
        result = cmp_str_num(aggr_value, len1, hav_condi->condition_value, len2, &num_unsupported);
        if (num_unsupported) {
            merged_result->status = RM_FAIL;
            g_warning("%s:merge_failed,num_unsupported", G_STRLOC);
            return FALSE;
        }

    } else {
        result = strcmp(aggr_value, hav_condi->condition_value);
    }

    switch (hav_condi->rel_type) {
    case TK_LE:
        if (result <= 0) {
            return TRUE;
        }
        break;
    case TK_GE:
        if (result >= 0) {
            return TRUE;
        }
        break;
    case TK_LT:
        if (result < 0) {
            return TRUE;
        }
        break;
    case TK_GT:
        if (result > 0) {
            return TRUE;
        }
        break;
    case TK_EQ:
        if (result == 0) {
            return TRUE;
        }
        break;
    case TK_NE:
        if (result != 0) {
            return TRUE;
        }
        break;
    }

    return FALSE;
}

static int
aggr_by_group(aggr_by_group_para_t *para, GList **candidates, guint *pkt_count, result_merge_t *merged_result)
{
    guint cand_index = 0;
    GList *candidate = NULL;
    size_t row_cnter = 0;
    size_t off_pos = 0;
    size_t iter;

    GPtrArray *recv_queues = para->recv_queues;

    while (row_cnter < para->limit->row_count) {
        if (!candidate || get_pkt_type((GString *)candidate->data) == MYSQLD_PACKET_EOF) {
            for (iter = 0; iter < recv_queues->len; iter++) {
                GString *item = candidates[iter]->data;
                if (item != NULL && get_pkt_type(item) != MYSQLD_PACKET_EOF) {
                    cand_index = iter;
                    candidate = candidates[iter];
                    break;
                }
            }
        }
        /* if candidate is still NULL, all possible candidates have been exhausted */
        if (candidate == NULL || get_pkt_type((GString *)candidate->data) == MYSQLD_PACKET_EOF) {
            break;
        }

        /* to obtain candidate ptr and its index in recv_queues by scanning candidates once */
        for (iter = 0; iter < recv_queues->len; iter++) {
            /* don't compare with itself */
            if (iter == cand_index) {
                continue;
            } else {
                GList *tmp_list = candidates[iter];
                /* some recv_queue may be shorter than others */
                if (tmp_list == NULL || get_pkt_type((GString *)tmp_list->data) == MYSQLD_PACKET_EOF) {
                    continue;
                }

                /* 1, output; 0, wait; -1, change index */

                g_debug("record1:%p, record2:%p", candidate, tmp_list);
                int merge_failed = 0;
                int result = cal_aggr_rec_rel(candidate, tmp_list, para, &merge_failed);

                if (merge_failed) {
                    merged_result->status = RM_FAIL;
                    g_warning("%s:merge_failed", G_STRLOC);
                    return 0;
                }

                if (result == -1) {
                    candidate = tmp_list;
                    cand_index = iter;
                } else if (result == 0) {
                    candidates[iter] = tmp_list->next;
                    continue;
                }
            }
        }

        g_debug("candidate:%p, off_pos:%d, para->limit->offset:%d",
                candidate, (int) off_pos, (int) para->limit->offset);
        if (off_pos < para->limit->offset) {
            off_pos++;
            candidates[cand_index] = candidate->next;
            candidate = candidate->next;
            continue;
        } else {
            char aggr_value[MAX_COL_VALUE_LEN] = { 0 };
            retrieve_aggr_value(candidate->data, para->hav_condi->column_index, aggr_value);

            if (!para->hav_condi->rel_type || fulfill_condi(aggr_value, para->hav_condi, merged_result)) {
                ((GString *)candidate->data)->str[3] = (*pkt_count) + 1;
                ++(*pkt_count);
                row_cnter++;
                network_queue_append(para->send_queue, (GString *)candidate->data);
            } else {
                g_string_free((GString *)candidate->data, TRUE);
            }

            candidates[cand_index] = candidate->next;
            GList *ptr_to_unlink = candidate;
            candidate = candidate->next;
            network_queue *recv_queue = recv_queues->pdata[cand_index];
            g_queue_delete_link(recv_queue->chunks, ptr_to_unlink);
        }
    }

    return 1;
}

void
heap_adjust(heap_type *heap, int s, int m, int *compare_failed)
{
    g_debug("%s: call heap_adjust, s:%d, m:%d", G_STRLOC, s, m);

    s = s - 1;
    m = m - 1;

    heap_element *rc = heap->element[s];

    int j, k, is_dup;

    for (j = (s << 1) + 1; j <= m; j = (j << 1) + 1) {
        if (j < m) {
            if (heap->element[j]->is_over && heap->element[j + 1]->is_over) {
                g_debug("%s: in adjust, break here:%d", G_STRLOC, j);
                break;
            }

            if (heap->element[j]->is_over) {
                j++;
            } else if (!heap->element[j + 1]->is_over) {
                if (!heap->element[j]->refreshed && !heap->element[j + 1]->refreshed && heap->element[j]->is_prior_to == -1) {
                    j++;
                } else {
                    is_dup = 0;
                    k = j;
                    if (!is_prior_to(heap->element[j]->record->data,
                                     heap->element[j + 1]->record->data, &(heap->order_para),
                                     heap->element[j]->index, heap->element[j + 1]->index, &is_dup, compare_failed)) {
                        j++;
                        heap->element[j]->is_prior_to = -1;

                    } else {
                        if (is_dup) {
                            heap->element[j + 1]->is_dup = 1;
                        } else {
                        }

                        heap->element[j]->is_prior_to = 1;
                    }

                    heap->element[k]->refreshed = 0;
                    heap->element[k + 1]->refreshed = 0;
                }
            }
        } else {
            if (heap->element[j]->is_over) {
                g_debug("%s: call over here, break j:%d", G_STRLOC, j);
                break;
            }
        }

        if (!rc->is_over) {
            is_dup = 0;
            if (is_prior_to(rc->record->data, heap->element[j]->record->data,
                            &(heap->order_para), rc->index, heap->element[j]->index, &is_dup, compare_failed)) {
                if (is_dup) {
                    if (heap->element[j]->is_dup) {
                        heap->element[s]->is_dup = 1;
                    } else {
                        heap->element[j]->is_dup = 1;
                    }
                }

                break;
            }
        }

        heap->element[s] = heap->element[j];
        heap->element[s]->refreshed = 1;

        s = j;
    }

    heap->element[s] = rc;
    heap->element[s]->refreshed = 1;
}

static void
check_server_sess_wait_for_event(network_mysqld_con *con, int ss_index, short ev_type, struct timeval *timeout)
{
    size_t i;
    for (i = 0; i < con->servers->len; i++) {
        server_session_t *ss = g_ptr_array_index(con->servers, i);
        if (ss_index >= 0) {
            if (ss_index != i) {
                continue;
            }
        }

        if (!ss->server->is_read_finished) {
            if (ss->server->is_waiting) {
                g_debug("%s: ss %d is waiting", G_STRLOC, (int)i);
                continue;
            }
            con->num_read_pending++;
            ss->read_cal_flag = 0;
            g_debug("%s: ss %d is not read finished, read pending:%d, fd:%d, ss index:%d",
                    G_STRLOC, (int)i, con->num_read_pending, ss->server->fd, ss->index);
            event_set(&(ss->server->event), ss->server->fd, ev_type, server_session_con_handler, ss);
            chassis_event_add_with_timeout(con->srv, &(ss->server->event), timeout);
            g_debug("%s: call chassis_event_add_with_timeout", G_STRLOC);
            ss->server->is_waiting = 1;
        } else {
            g_debug("%s: ss %d is read finished", G_STRLOC, (int)i);
        }
    }
}

static int
check_after_limit(network_mysqld_con *con, merge_parameters_t *data, int is_finished)
{
    GPtrArray *recv_queues = data->recv_queues;
    GList **candidates = data->candidates;
    gboolean is_more_to_read = FALSE;
    GList *candidate = NULL;
    size_t iter;

    g_debug("%s: call check_after_limit", G_STRLOC);

    for (iter = 0; iter < recv_queues->len; iter++) {
        gboolean is_over = FALSE;
        do {
            candidate = candidates[iter];
            if (candidate == NULL || candidate->data == NULL) {
                con->partially_merged = 1;
                is_more_to_read = TRUE;
                g_debug("%s: item is nil, index:%d", G_STRLOC, (int)iter);
                break;
            }

            GString *item = candidate->data;
            guchar pkt_type = get_pkt_type(item);
            if (pkt_type == MYSQLD_PACKET_EOF || pkt_type == MYSQLD_PACKET_ERR) {
                g_debug("%s: is over true:%d, pkt type:%d", G_STRLOC, (int)iter, (int)pkt_type);
                is_over = TRUE;
                break;
            }

            candidates[iter] = candidate->next;
            g_debug("%s: free packet addr:%p, iter:%d, pkt_type:%d", G_STRLOC,
                    candidate->data, (int)iter, (int)pkt_type);
            g_string_free((GString *)candidate->data, TRUE);
            network_queue *recv_queue = recv_queues->pdata[iter];
            g_queue_delete_link(recv_queue->chunks, candidate);
        } while (!is_over);
    }

    if (is_finished && is_more_to_read) {
        g_warning("%s: finished reading, but needs more read:%p", G_STRLOC, con);
    } else if ((!is_finished) && (!is_more_to_read)) {
        g_warning("%s: not finished reading, but is_more_to_read false:%p", G_STRLOC, con);
    }

    if (is_more_to_read) {
        g_debug("%s: need more reading for:%p", G_STRLOC, con);
        check_server_sess_wait_for_event(con, -1, EV_READ, &con->read_timeout);
        return 0;
    }

    return 1;
}

static int
do_simple_merge(network_mysqld_con *con, merge_parameters_t *data, int is_finished)
{
    network_queue *send_queue = data->send_queue;
    GPtrArray *recv_queues = data->recv_queues;
    GList **candidates = data->candidates;
    limit_t *limit = &(data->limit);
    int *row_cnter = &(data->row_cnter);
    int *off_pos = &(data->off_pos);

    GList *candidate = NULL;
    size_t iter;

    heap_count = 0;

    int merged_output_size = con->srv->merged_output_size;
    if (con->is_client_compressed) {
        merged_output_size = con->srv->compressed_merged_output_size;
    }

    g_debug("%s: call do_simple_merge", G_STRLOC);

    gboolean shortaged = FALSE;
    for (iter = 0; iter < recv_queues->len; iter++) {
        candidate = candidates[iter];
        network_queue *recv_queue = recv_queues->pdata[iter];

        while (candidate != NULL) {
            if (candidate->data == NULL) {
                g_debug("%s: candidate data is nil:%d", G_STRLOC, (int)iter);
                break;
            }

            if ((*row_cnter) == limit->row_count) {
                g_debug("%s: reach limit:%d", G_STRLOC, (int)iter);
                break;
            }

            guchar pkt_type = get_pkt_type((GString *)candidate->data);
            if (pkt_type == MYSQLD_PACKET_EOF) {
                g_debug("%s: MYSQLD_PACKET_EOF here:%d", G_STRLOC, (int)iter);
                break;
            }

            if (pkt_type == MYSQLD_PACKET_ERR) {
                data->is_pack_err = 1;
                data->err_pack = candidate->data;
                g_debug("%s: MYSQLD_PACKET_ERR here:%d", G_STRLOC, (int)iter);
                break;
            }

            if ((*off_pos) < limit->offset) {
                (*off_pos)++;
                g_string_free((GString *)candidate->data, TRUE);
            } else {

                int packet_len = network_mysqld_proto_get_packet_len(candidate->data);
                data->aggr_output_len += packet_len;

                ((GString *)candidate->data)->str[3] = data->pkt_count + 1;
                ++(data->pkt_count);
                network_queue_append(send_queue, (GString *)candidate->data);
                if (data->aggr_output_len >= merged_output_size) {
                    g_debug("%s: send_part_content_to_client:%d, iter:%d", G_STRLOC, data->aggr_output_len, (int)iter);
                    send_part_content_to_client(con);
                    data->aggr_output_len = 0;
                }
                (*row_cnter)++;
            }

            candidates[iter] = candidate->next;
            g_queue_delete_link(recv_queue->chunks, candidate);
            candidate = candidates[iter];
        }

        if (candidate == NULL || candidate->data == NULL) {
            if (con->servers) {
                server_session_t *ss = g_ptr_array_index(con->servers, iter);
                if (ss->server->is_waiting) {
                    g_debug("%s: is_waiting true:%d", G_STRLOC, (int)iter);
                    continue;
                }
                candidates[iter] = NULL;
                g_debug("%s: candidate is nil for i:%d, recv_queues:%p",
                        G_STRLOC, (int)iter, recv_queues);
                shortaged = TRUE;
            } else {
                g_warning("%s: return part of responses", G_STRLOC);
            }
        }
    }

    if (shortaged) {
        con->partially_merged = 1;
        g_debug("%s: need more reading for:%p", G_STRLOC, con);
        if (data->aggr_output_len >= merged_output_size) {
            send_part_content_to_client(con);
            data->aggr_output_len = 0;
        }
        check_server_sess_wait_for_event(con, -1, EV_READ, &con->read_timeout);
        return 0;
    } else {
        if (!is_finished) {
            g_debug("%s: check limit for:%p", G_STRLOC, con);
            if (limit->row_count > 0 && (*row_cnter) >= limit->row_count) {
                g_debug("%s: do call check_after_limit for:%p", G_STRLOC, con);
                if (check_after_limit(con, data, is_finished) == FALSE) {
                    g_debug("%s: call check_after_limit over for:%p", G_STRLOC, con);
                    return 0;
                }
            }
        }
    }

    return 1;
}

static int
do_sort_merge(network_mysqld_con *con, merge_parameters_t *data, int is_finished, int *compare_failed)
{
    network_queue *send_queue = data->send_queue;
    GPtrArray *recv_queues = data->recv_queues;
    GList **candidates = data->candidates;
    limit_t *limit = &(data->limit);
    heap_type *heap = data->heap;
    int *row_cnter = &(data->row_cnter);
    int *off_pos = &(data->off_pos);
    uint64_t *field_offsets = heap->order_para.field_offsets_cache;

    GList *candidate = NULL;

    int merged_output_size = con->srv->merged_output_size;
    if (con->is_client_compressed) {
        merged_output_size = con->srv->compressed_merged_output_size;
    }
    int last_output_index = -1;
    while ((*row_cnter) < limit->row_count) {
        if (heap->element[0]->is_over) {
            if (heap->is_err) {
                data->is_pack_err = 1;
            }
            break;
        }

        int cand_index = heap->element[0]->index;

        if (heap->element[0]->record == NULL) {
            if (candidates[cand_index] == NULL) {
                g_debug("%s: cand_index:%d is nil", G_STRLOC, cand_index);
                check_server_sess_wait_for_event(con, cand_index, EV_READ, &con->read_timeout);
                return 0;
            } else {
                heap->element[0]->record = candidates[cand_index];
                g_debug("%s: record:%p for index:%d", G_STRLOC, heap->element[0]->record, cand_index);
                heap_adjust(heap, 1, recv_queues->len, compare_failed);
                if (*compare_failed) {
                    return 0;
                }
                cand_index = heap->element[0]->index;
            }
        }

        candidate = heap->element[0]->record;
        field_offsets[cand_index] = 0;

        g_debug("%s: row counter:%d", G_STRLOC, (int)(*row_cnter));

        if (data->is_distinct && heap->element[0]->is_dup && cand_index != last_output_index) {
            g_debug("%s: dup element at:%d", G_STRLOC, cand_index);
            g_string_free((GString *)candidate->data, TRUE);
        } else if ((*off_pos) < limit->offset) {
            (*off_pos)++;
            g_string_free((GString *)candidate->data, TRUE);
            g_debug("%s: off pos here:%d", G_STRLOC, (int)(*off_pos));
        } else {

            int packet_len = network_mysqld_proto_get_packet_len(candidate->data);
            data->aggr_output_len += packet_len;
            ((GString *)candidate->data)->str[3] = data->pkt_count + 1;
            ++(data->pkt_count);
            network_queue_append(send_queue, (GString *)candidate->data);
            (*row_cnter)++;
            last_output_index = cand_index;

            if (data->aggr_output_len >= merged_output_size) {
                g_debug("%s: send_part_content_to_client:%d", G_STRLOC, data->aggr_output_len);
                send_part_content_to_client(con);
                data->aggr_output_len = 0;
            }
        }

        heap->element[0]->is_dup = 0;
        heap->element[0]->is_err = 0;
        heap->element[0]->refreshed = 0;
        heap->element[0]->is_prior_to = 0;
        heap->element[0]->record = heap->element[0]->record->next;

        candidates[cand_index] = candidate->next;
        network_queue *recv_queue = recv_queues->pdata[cand_index];
        g_debug("%s: remove candidate:%p for queue:%p, ss:%d", G_STRLOC, candidate, recv_queue, cand_index);
        g_queue_delete_link(recv_queue->chunks, candidate);

        if (candidates[cand_index] == NULL || candidates[cand_index]->data == NULL) {
            con->partially_merged = 1;
            g_debug("%s: item is nil, index:%d", G_STRLOC, cand_index);
            if (data->aggr_output_len >= merged_output_size) {
                send_part_content_to_client(con);
                g_debug("%s: send_part_content_to_client:%d", G_STRLOC, data->aggr_output_len);
                data->aggr_output_len = 0;
            }
            check_server_sess_wait_for_event(con, cand_index, EV_READ, &con->read_timeout);
            return 0;
        }

        GString *item = candidates[cand_index]->data;
        guchar pkt_type = get_pkt_type(item);

        if (pkt_type == MYSQLD_PACKET_EOF || pkt_type == MYSQLD_PACKET_ERR) {
            g_debug("%s: index is over:%d", G_STRLOC, cand_index);
            heap->element[0]->is_over = 1;
            if (pkt_type == MYSQLD_PACKET_ERR) {
                heap->element[0]->is_err = 1;
                heap->is_err = 1;
                data->err_pack = item;
            }
        } else {
            heap->element[0]->is_over = 0;
        }

        g_debug("%s: candidate:%p for queue:%p, ss:%d", G_STRLOC, candidates[cand_index], recv_queue, cand_index);

        heap_adjust(heap, 1, recv_queues->len, compare_failed);
        if (*compare_failed) {
            return 0;
        }
    }

    if (limit->row_count > 0 && (*row_cnter) >= limit->row_count) {
        if (check_after_limit(con, data, is_finished) == FALSE) {
            return 0;
        }
    }

    return 1;
}

static int
create_heap_for_merge_sort(network_mysqld_con *con, merge_parameters_t *data, int *compare_failed)
{
    GList **candidates = data->candidates;
    heap_type *heap = data->heap;
    size_t iter;

    for (iter = 0; iter < heap->len; iter++) {
        heap->element[iter]->is_dup = 0;
        heap->element[iter]->is_err = 0;
        heap->element[iter]->refreshed = 0;
        heap->element[iter]->is_prior_to = 0;

        if (candidates[iter] != NULL) {
            GString *item = candidates[iter]->data;
            if (item != NULL) {
                guchar pkt_type = get_pkt_type(item);
                if (pkt_type == MYSQLD_PACKET_EOF || pkt_type == MYSQLD_PACKET_ERR) {
                    heap->element[iter]->is_over = 1;
                    if (pkt_type == MYSQLD_PACKET_ERR) {
                        heap->element[iter]->is_err = 1;
                        heap->is_err = 1;
                        data->err_pack = item;
                    }
                } else {
                    heap->element[iter]->record = candidates[iter];
                    heap->element[iter]->index = iter;
                    heap->element[iter]->is_over = 0;
                }
            } else {
                con->partially_merged = 1;
                check_server_sess_wait_for_event(con, iter, EV_READ, &con->read_timeout);
                return 0;
            }
        }
    }

    for (iter = heap->len / 2; iter > 0; --iter) {
        heap_adjust(heap, iter, heap->len, compare_failed);
        if (*compare_failed) {
            return 0;
        }
    }

    g_debug("%s: create heap over", G_STRLOC);

    return 1;
}

int
callback_merge(network_mysqld_con *con, merge_parameters_t *data, int is_finished)
{
    int merge_failed = 0;
    network_queue *send_queue = data->send_queue;
    heap_type *heap = data->heap;
    int order_array_size = heap->order_para.order_array_size;

    if (order_array_size > 0) {
        int result = do_sort_merge(con, data, is_finished, &merge_failed);
        if (merge_failed) {
            return RM_FAIL;
        }
        if (!result) {
            return RM_SUCCESS;
        }
    } else {
        if (!do_simple_merge(con, data, is_finished)) {
            return RM_SUCCESS;
        }
    }

    if (is_finished) {
        g_debug("%s: finished is true", G_STRLOC);
        if (data->is_pack_err) {
            if (data->err_pack == NULL) {
                g_warning("%s: packet err while p is nil", G_STRLOC);
                return RM_FAIL;
            }
            data->pack_err_met = 1;
            data->err_pack->str[3] = data->pkt_count + 1;
            ++(data->pkt_count);
            guchar pkt_type = get_pkt_type(data->err_pack);
            if (pkt_type == MYSQLD_PACKET_ERR) {
                g_message("%s: Add err packet to send queue", G_STRLOC);
            } else {
                g_warning("%s: Add non err packet info to send queue when err pack is met", G_STRLOC);
            }
            GString *new_err_pack = g_string_new_len(data->err_pack->str, data->err_pack->len);
            network_queue_append(send_queue, new_err_pack);
        } else {
            /* TODO if in trans, then needs to set 'in transaction' flag ? */
            GString *eof_pkt = g_string_new_len("\x05\x00\x00\x07\xfe\x00\x00\x02\x00", 9);
            eof_pkt->str[3] = (data->pkt_count) + 1;
            network_queue_append(send_queue, eof_pkt);
        }
    }
    g_debug("%s: send queue len:%d", G_STRLOC, send_queue->chunks->length);

    return RM_SUCCESS;
}

static int
do_merge(network_mysqld_con *con, merge_parameters_t *data, int *merge_failed)
{
    heap_type *heap = data->heap;
    int is_finished = 0;

    if (con->num_pending_servers == 0) {
        is_finished = 1;
    }

    if (heap->order_para.order_array_size > 0) {
        if (!create_heap_for_merge_sort(con, data, merge_failed)) {
            return 1;
        }

        if (!do_sort_merge(con, data, is_finished, merge_failed)) {
            return 1;
        }
    } else {
        if (!do_simple_merge(con, data, is_finished)) {
            return 1;
        }
    }

    if (data->is_pack_err) {
        if (data->err_pack == NULL) {
            g_warning("%s: packet err while p is nil", G_STRLOC);
            return 0;
        }
        data->pack_err_met = 1;
        data->err_pack->str[3] = data->pkt_count + 1;
        ++(data->pkt_count);
        guchar pkt_type = get_pkt_type(data->err_pack);
        if (pkt_type == MYSQLD_PACKET_ERR) {
            g_message("%s: Add err packet to send queue", G_STRLOC);
        } else {
            g_warning("%s: Add non err packet info to send queue when err pack is met", G_STRLOC);
        }
        GString *new_err_pack = g_string_new_len(data->err_pack->str, data->err_pack->len);
        network_queue_append(data->send_queue, new_err_pack);
    }

    return 1;
}

gint
check_dist_tran_resultset(network_queue *recv_queue, network_mysqld_con *con)
{
    int fail = 0;
    GList *pkt = recv_queue->chunks->head;
    /* only check the first packet in each recv_queue */
    if (pkt != NULL && pkt->data != NULL && ((GString *)pkt->data)->len > NET_HEADER_SIZE) {
        guchar pkt_type = get_pkt_type((GString *)pkt->data);
        g_debug("%s: pkt type:%d", G_STRLOC, pkt_type);
        if (pkt_type == MYSQLD_PACKET_ERR) {
            network_packet packet;
            packet.offset = NET_HEADER_SIZE;
            packet.data = pkt->data;
            network_mysqld_err_packet_t *err_packet;
            err_packet = network_mysqld_err_packet_new();
            if (!network_mysqld_proto_get_err_packet(&packet, err_packet)) {
                int checked = 0;
                switch (err_packet->errcode) {
                case ER_XA_RBROLLBACK:
                    g_message("%s: ER_XA_RBROLLBACK for con:%p", G_STRLOC, con);
                    fail = 1;
                    checked = 1;
                    break;
                case ER_XA_RBDEADLOCK:
                    g_message("%s: ER_XA_RBDEADLOCK for con:%p", G_STRLOC, con);
                    fail = 1;
                    checked = 1;
                    break;
                case ER_XA_RBTIMEOUT:
                    g_message("%s: ER_XA_RBTIMEOUT for con:%p", G_STRLOC, con);
                    fail = 1;
                    checked = 1;
                    break;
                case ER_LOCK_DEADLOCK:
                case ER_LOCK_WAIT_TIMEOUT:
                    fail = 1;
                    checked = 1;
                    break;
                case ER_DUP_ENTRY:
                    g_message("%s: ER_DUP_ENTRY here:%d", G_STRLOC, con->last_resp_num);
                    if (con->last_resp_num > 1) {
                        fail = 1;
                    } else {
                        fail = 0;
                    }
                    checked = 1;
                    break;
                default:
                    fail = 0;
                    break;
                }
                if (!checked) {
                    if (strncasecmp(err_packet->sqlstate->str, "XA", 2) == 0) {
                        fail = 1;
                    }
                }
            } else {
                g_warning("%s: network_mysqld_proto_get_err_packet err", G_STRLOC);
            }
            network_mysqld_err_packet_free(err_packet);
        } else if (pkt_type == MYSQLD_PACKET_EOF) {
            fail = 1;
        }
    } else {
        fail = 1;
    }

    if (fail) {
        return -1;
    }

    return 0;
}


static int
merge_for_modify(sql_context_t *context, network_queue *send_queue, GPtrArray *recv_queues,
                 network_mysqld_con *con, cetus_result_t *res_merge, result_merge_t *merged_result)
{
    /* INSERT/UPDATE/DELETE expecting OK packet */
    int total_affected_rows = 0;
    int total_warnings = 0;
    int i;

    for (i = 0; i < recv_queues->len; i++) {
        network_queue *recv_q = g_ptr_array_index(recv_queues, i);
        GString *pkt = g_queue_peek_head(recv_q->chunks);
        /* only check the first packet in each recv_queue */
        if (!pkt || pkt->len <= NET_HEADER_SIZE) {
            cetus_result_destroy(res_merge);
            merged_result->status = RM_FAIL;
            g_warning("%s:pkt is wrong", G_STRLOC);
            return 0;
        }

        guchar pkt_type = get_pkt_type(pkt);
        switch (pkt_type) {
        case MYSQLD_PACKET_OK:{
            network_packet packet = { pkt, 0 };
            network_mysqld_ok_packet_t one_ok;
            network_mysqld_proto_skip_network_header(&packet);
            if (!network_mysqld_proto_get_ok_packet(&packet, &one_ok)) {
                total_affected_rows += one_ok.affected_rows;
                total_warnings += one_ok.warnings;
            }
            break;
        }
        case MYSQLD_PACKET_EOF:
            break;
        case MYSQLD_PACKET_ERR:
            network_queue_append(send_queue, pkt);
            g_queue_remove(recv_q->chunks, pkt);
            cetus_result_destroy(res_merge);
            merged_result->status = RM_FAIL;
            g_warning("%s:MYSQLD_PACKET_ERR is met", G_STRLOC);
            return 0;
        default:
            break;
        }
    }

    if (total_affected_rows > 0) {
        con->last_record_updated = 1;
    }

    if (con->sharding_plan && con->sharding_plan->table_type == GLOBAL_TABLE && recv_queues->len) {
        total_affected_rows /= recv_queues->len;
    }

    if (con->srv->candidate_config_changed) {
        if (total_affected_rows > 0) {
            con->srv->config_changed = 1;
        } else {
            con->srv->config_changed = 0;
        }
    }
    network_mysqld_con_send_ok_full(con->client, total_affected_rows, 0, 0x02, total_warnings);

    return 1;
}

static gboolean
disp_orderby_info(sql_column_list_t *sel_orderby, cetus_result_t *res_merge,
                  ORDER_BY *order_array, int order_array_size, result_merge_t *merged_result)
{
    int i;

    for (i = 0; i < sel_orderby->len; ++i) {
        sql_column_t *col = g_ptr_array_index(sel_orderby, i);
        sql_expr_t *expr = col->expr;
        ORDER_BY *ord_col = &(order_array[i]);
        ord_col->pos = -1;      /* initial invalid value: -1 */
        if (expr->op == TK_ID) {
            strncpy(ord_col->name, expr->token_text, MAX_NAME_LEN - 1);
        } else if (expr->op == TK_INTEGER) {
            gint64 v;
            sql_expr_get_int(expr, &v);
            ord_col->pos = (v > 0 && v <= res_merge->field_count) ? (int)v - 1 : -1;
        } else if (expr->op == TK_FUNCTION) {
            strncpy(ord_col->name, expr->start, expr->end - expr->start);
        } else if (expr->op == TK_DOT) {
            strncpy(ord_col->table_name, expr->left->token_text, MAX_NAME_LEN - 1);
            strncpy(ord_col->name, expr->right->token_text, MAX_NAME_LEN - 1);
        } else {
            g_warning("order by name error");
        }
        ord_col->desc = (col->sort_order == SQL_SO_DESC);
    }

    if (!get_order_by_fields(res_merge, order_array, order_array_size, merged_result)) {
        cetus_result_destroy(res_merge);
        return FALSE;
    }

    return TRUE;
}

static gboolean
disp_groupby_info(sql_column_list_t *sel_groupby, cetus_result_t *res_merge,
                  group_by_t *group_array, int group_array_size, result_merge_t *merged_result)
{
    int i;
    for (i = 0; i < sel_groupby->len; ++i) {
        sql_expr_t *expr = g_ptr_array_index(sel_groupby, i);
        group_by_t *group_col = &(group_array[i]);
        group_col->pos = -1;    /* initial invalid value: -1 */
        if (expr->op == TK_ID) {    /* TODO: wrap sql_expr_t in list */
            strncpy(group_col->name, expr->token_text, MAX_NAME_LEN - 1);
        } else if (expr->op == TK_DOT) {
            sql_expr_get_dotted_names(expr, 0, 0,
                                      group_col->table_name, MAX_NAME_LEN-1,
                                      group_col->name, MAX_NAME_LEN-1);
        } else if (expr->op == TK_INTEGER) {
            gint64 v;
            sql_expr_get_int(expr, &v);
            group_col->pos = (v > 0 && v <= res_merge->field_count) ? (int)v - 1 : -1;
        } else if (expr->op == TK_FUNCTION) {
            strncpy(group_col->name, expr->start, expr->end - expr->start);
        } else {
            g_warning("group by name error");
        }
    }

    if (!get_group_by_fields(res_merge, group_array, group_array_size, merged_result)) {
        cetus_result_destroy(res_merge);
        return FALSE;
    }

    return TRUE;
}

static gboolean
retrieve_orderby_info_from_groupby_info(sql_column_list_t *sel_groupby, cetus_result_t *res_merge,
                                        ORDER_BY *order_array, int order_array_size, result_merge_t *merged_result)
{
    int i;

    for (i = 0; i < sel_groupby->len; ++i) {
        sql_expr_t *expr = g_ptr_array_index(sel_groupby, i);
        ORDER_BY *ord_col = &(order_array[i]);
        ord_col->pos = -1;      /* initial invalid value: -1 */
        if (expr->op == TK_ID) {
            strncpy(ord_col->name, expr->token_text, MAX_NAME_LEN - 1);
        } else if (expr->op == TK_INTEGER) {
            gint64 v;
            sql_expr_get_int(expr, &v);
            ord_col->pos = (v > 0 && v <= res_merge->field_count) ? (int)v - 1 : -1;
        } else if (expr->op == TK_FUNCTION) {
            strncpy(ord_col->name, expr->start, expr->end - expr->start);
        } else if (expr->op == TK_DOT) {
            sql_expr_get_dotted_names(expr, 0, 0,
                                      ord_col->table_name, MAX_NAME_LEN-1,
                                      ord_col->name, MAX_NAME_LEN-1);
        } else {
            g_warning("order by name error");
        }
    }

    if (!get_order_by_fields(res_merge, order_array, order_array_size, merged_result)) {
        cetus_result_destroy(res_merge);
        return FALSE;
    }

    return TRUE;
}


static int
check_network_packet_err(network_mysqld_con *con, GList **candidates, GPtrArray *recv_queues,
                         network_queue *send_queue, cetus_result_t *res_merge, result_merge_t *merged_result)
{
    int i;
    for (i = 0; i < recv_queues->len; i++) {
        GString *pkt = candidates[i]->data;
        /* only check the first packet in incoming row packets */
        if (pkt != NULL && pkt->len > NET_HEADER_SIZE) {
            guchar pkt_type = get_pkt_type(pkt);
            if (pkt_type == MYSQLD_PACKET_ERR) {
                network_queue_append(send_queue, pkt);
                network_queue *recv_queue = g_ptr_array_index(recv_queues, i);
                g_queue_remove(recv_queue->chunks, pkt);
                cetus_result_destroy(res_merge);
                if (con->num_pending_servers) {
                    merged_result->status = RM_FAIL;
                    g_warning("%s:MYSQLD_PACKET_ERR met, num_pending_servers:%d", G_STRLOC, con->num_pending_servers);
                } else {
                    merged_result->status = RM_SUCCESS;
                }
                return 0;
            }
        } else {
            cetus_result_destroy(res_merge);
            merged_result->status = RM_FAIL;
            g_warning("%s:pkt is wrong", G_STRLOC);
            return 0;
        }
    }

    return 1;
}

static int
check_field_count_consistant(GPtrArray *recv_queues, result_merge_t *merged_result, guint64 *field_count)
{
    int i;
    guint64 last_field_count = 0;
    for (i = 0; i < recv_queues->len; i++) {
        network_queue *queue = g_ptr_array_index(recv_queues, i);
        if (cetus_result_retrieve_field_count(queue->chunks, field_count) == FALSE) {
            g_warning("%s:cetus_result_retrieve_field_count failure", G_STRLOC);
            merged_result->status = RM_FAIL;
            return 0;
        }

        if (last_field_count) {
            if (last_field_count != (*field_count)) {
                g_warning("%s:field count different, field_count1:%d, field_count2:%d",
                          G_STRLOC, (int)last_field_count, (int)(*field_count));
                merged_result->status = RM_FAIL;
                return 0;
            }
        }

        last_field_count = *field_count;
    }

    return 1;
}

static int
prepare_for_row_process(GList **candidates, GPtrArray *recv_queues, network_queue *send_queue,
                        guint pkt_count, result_merge_t *merged_result)
{
    int i, candidate_iter = 0;

    /* insert 'ROW packets' to candidates */
    for (i = 0; i < recv_queues->len; i++) {
        network_queue *recv_q = g_ptr_array_index(recv_queues, i);
        GList *rows_start = NULL;

        g_debug("%s: pkt_count:%d", G_STRLOC, pkt_count);

        int j;
        /* sending result header, field-defs and EOF once */
        for (j = pkt_count; j > 0; --j) {
            GString *packet = g_queue_pop_head(recv_q->chunks);
            if (packet == NULL) {
                g_warning("%s:packet null, enlarge max_header_size, pkt cnt:%d", G_STRLOC, pkt_count);
                merged_result->status = RM_FAIL;
                return 0;
            }

            if (i == 0) {
                network_queue_append(send_queue, packet);
            } else {
                g_string_free(packet, TRUE);
            }

            /* check if the last packet is field EOF packet */
        }

        rows_start = g_queue_peek_head_link(recv_q->chunks);

        if (rows_start) {
            candidates[candidate_iter] = rows_start;
            candidate_iter++;
        } else {
            g_warning("%s:rows start null, enlarge max_header_size, pkt cnt:%d", G_STRLOC, pkt_count);
            merged_result->status = RM_FAIL;
            return 0;
        }
    }

    return 1;
}

static int
merge_for_show_warnings(network_queue *send_queue, GPtrArray *recv_queues,
                        network_mysqld_con *con, cetus_result_t *res_merge, result_merge_t *merged_result)
{
    guint64 field_count = 0;
    if (!check_field_count_consistant(recv_queues, merged_result, &field_count)) {
        return 0;
    }
    res_merge->field_count = field_count;

    GList **candidates = g_new0(GList *, recv_queues->len);
    /* field-count-packet + eof-packet */
    guint pkt_count = res_merge->field_count + 2;

    if (!prepare_for_row_process(candidates, recv_queues, send_queue, pkt_count, merged_result)) {
        g_warning("%s:prepare_for_row_process failed", G_STRLOC);
        g_free(candidates);
        return 0;
    }

    if (!check_network_packet_err(con, candidates, recv_queues, send_queue, res_merge, merged_result)) {
        g_warning("%s:packet err is met", G_STRLOC);
        g_free(candidates);
        return 0;
    }

    merge_parameters_t *data = g_new0(merge_parameters_t, 1);

    data->send_queue = send_queue;
    data->recv_queues = recv_queues;
    data->candidates = candidates;
    data->pkt_count = pkt_count;
    data->limit.offset = 0;
    data->limit.row_count = G_MAXINT32;

    data->pack_err_met = 0;

    con->data = data;

    if (!do_simple_merge(con, con->data, 1)) {
        g_warning("%s:merge failed", G_STRLOC);
        merged_result->status = RM_FAIL;
        return 0;
    }

    pkt_count = data->pkt_count;

    /* after adding all packets we don't need candidate list anymore */

    /* need to append EOF after all Row Data Packets?? Yes
     * update packet number in header 
     */

    g_debug("%s: append here", G_STRLOC);
    /* TODO if in trans, then needs to set 'in transaction' flag ? */
    GString *eof_pkt = g_string_new_len("\x05\x00\x00\x07\xfe\x00\x00\x02\x00", 9);
    eof_pkt->str[3] = pkt_count + 1;
    network_queue_append(send_queue, eof_pkt);

    return 1;
}

static int
merge_for_admin(network_queue *send_queue, GPtrArray *recv_queues,
        network_mysqld_con *con, cetus_result_t *res_merge, result_merge_t *merged_result)
{
    int p;

    for (p = 0; p < recv_queues->len; p++) {
        network_queue *recv_q = g_ptr_array_index(recv_queues, p);
        GString *pkt = g_queue_peek_head(recv_q->chunks);
        /* only check the first packet in each recv_queue */
        if (pkt != NULL && pkt->len > NET_HEADER_SIZE) {
            guchar pkt_type = get_pkt_type(pkt);
            if (pkt_type == MYSQLD_PACKET_ERR || pkt_type == MYSQLD_PACKET_EOF) {
                network_queue_append(send_queue, pkt);
                network_mysqld_proto_set_packet_id(pkt, 1);
                g_queue_pop_head(recv_q->chunks);
                return 0;
            }
        }
    }

    guint64 field_count = 0;
    if (!check_field_count_consistant(recv_queues, merged_result, &field_count)) {
        return 0;
    }
    res_merge->field_count = field_count;

    GList **candidates = g_new0(GList *, recv_queues->len);
    /* field-count-packet + eof-packet */
    guint pkt_count = res_merge->field_count + 2;

    if (!prepare_for_row_process(candidates, recv_queues, send_queue, pkt_count, merged_result)) {
        g_warning("%s:prepare_for_row_process failed", G_STRLOC);
        g_free(candidates);
        return 0;
    }

    merge_parameters_t *data = g_new0(merge_parameters_t, 1);

    data->send_queue = send_queue;
    data->recv_queues = recv_queues;
    data->candidates = candidates;
    data->pkt_count = pkt_count;
    data->limit.offset = 0;
    data->limit.row_count = G_MAXINT32;

    data->pack_err_met = 0;

    con->data = data;

    if (!do_simple_merge(con, con->data, 1)) {
        g_warning("%s:merge failed", G_STRLOC);
        merged_result->status = RM_FAIL;
        return 0;
    }

    pkt_count = data->pkt_count;

    /* after adding all packets we don't need candidate list anymore */

    /* need to append EOF after all Row Data Packets?? Yes
     * update packet number in header 
     */

    g_debug("%s: append here", G_STRLOC);
    /* TODO if in trans, then needs to set 'in transaction' flag ? */
    GString *eof_pkt = g_string_new_len("\x05\x00\x00\x07\xfe\x00\x00\x02\x00", 9);
    eof_pkt->str[3] = pkt_count + 1;
    network_queue_append(send_queue, eof_pkt);

    return 1;
}

static int
check_fail_met(sql_context_t *context, network_queue *send_queue, GPtrArray *recv_queues,
               network_mysqld_con *con, uint64_t *uniq_id, result_merge_t *merged_result)
{
    int p;
    char *orig_sql = con->orig_sql->str;
    for (p = 0; p < recv_queues->len; p++) {
        network_queue *recv_q = g_ptr_array_index(recv_queues, p);
        GString *pkt = g_queue_peek_head(recv_q->chunks);
        /* only check the first packet in each recv_queue */
        if (pkt != NULL && pkt->len > NET_HEADER_SIZE) {
            guchar pkt_type = get_pkt_type(pkt);
            if (pkt_type == MYSQLD_PACKET_ERR || pkt_type == MYSQLD_PACKET_EOF) {
                server_session_t *ss = g_ptr_array_index(con->servers, p);
                if (pkt_type == MYSQLD_PACKET_ERR) {
                    g_warning("%s: failed query:%s, server:%s", G_STRLOC, orig_sql, ss->server->dst->name->str);
                }

                network_queue_append(send_queue, pkt);
                g_queue_pop_head(recv_q->chunks);
                if (context->rw_flag & CF_DDL) {
                    g_warning("%s: failed ddl query:%s, server:%s",
                            G_STRLOC, orig_sql, ss->server->dst->name->str);
                }
                return 0;
            }
        } else {
            g_warning("%s: merge failed for con:%p", G_STRLOC, con);
            merged_result->status = RM_FAIL;
            return 0;
        }
    }

    return 1;
}


void
admin_resultset_merge(network_mysqld_con *con, network_queue *send_queue, GPtrArray *recv_queues,
        result_merge_t *merged_result)
{
    cetus_result_t res_merge = { 0 };

    g_debug("%s: sql:%s", G_STRLOC, con->orig_sql->str);
    if (con->admin_read_merge) {
        g_debug("%s: call merge_for_admin", G_STRLOC);
        if (!merge_for_admin(send_queue, recv_queues, con, &res_merge, merged_result)) {
            cetus_result_destroy(&res_merge);
            return;
        }
    } else {
        g_debug("%s: call merge_for_modify", G_STRLOC);
        if (!merge_for_modify(NULL, send_queue, recv_queues, con, &res_merge, merged_result)) {
            cetus_result_destroy(&res_merge);
            return;
        }
    }

    cetus_result_destroy(&res_merge);
    merged_result->status = RM_SUCCESS;
}

static int
merge_for_select(sql_context_t *context, network_queue *send_queue, GPtrArray *recv_queues,
                 network_mysqld_con *con, cetus_result_t *res_merge, result_merge_t *merged_result)
{
    sql_select_t *select = (sql_select_t *)context->sql_statement;

    guint64 field_count = 0;
    if (!check_field_count_consistant(recv_queues, merged_result, &field_count)) {
        return 0;
    }
    res_merge->field_count = field_count;

    group_aggr_t aggr_array[MAX_AGGR_FUNS] = {{0}};

    int aggr_num = sql_expr_list_find_aggregates(select->columns, aggr_array);
    sql_column_list_t *sel_orderby = select->orderby_clause;
    sql_expr_list_t *sel_groupby = select->groupby_clause;
    if (sel_orderby || sel_groupby || aggr_num > 0) {
        network_queue *first_queue = g_ptr_array_index(recv_queues, 0);
        gboolean ok = cetus_result_parse_fielddefs(res_merge, first_queue->chunks);
        if (!ok) {
            g_warning("%s:parse_fielddefs failed:%s", G_STRLOC, con->orig_sql->str);
            merged_result->status = RM_FAIL;
            return 0;
        }
    }

    ORDER_BY order_array[MAX_ORDER_COLS];
    int order_array_size = 0;   /* number of ORDER_BY Columns */
    if (sel_orderby && sel_orderby->len > 0) {
        memset(order_array, 0, sizeof(ORDER_BY) * MAX_ORDER_COLS);
        order_array_size = MIN(MAX_ORDER_COLS, sel_orderby->len);
        if (!disp_orderby_info(sel_orderby, res_merge, order_array, order_array_size, merged_result)) {
            g_warning("%s:disp_orderby_info failed:%s", G_STRLOC, con->orig_sql->str);
            return 0;
        }
    }

    group_by_t group_array[MAX_GROUP_COLS];
    int group_array_size = 0;   /* number of group_by_t Columns */
    if (sel_groupby && sel_groupby->len > 0) {
        memset(group_array, 0, sizeof(group_by_t) * MAX_GROUP_COLS);
        group_array_size = MIN(MAX_GROUP_COLS, sel_groupby->len);
        if (!disp_groupby_info(sel_groupby, res_merge, group_array, group_array_size, merged_result)) {
            g_warning("%s:disp_groupby_info failed:%s", G_STRLOC, con->orig_sql->str);
            return 0;
        }
        if (sel_orderby && sel_orderby->len > 0) {
            /* if order by & group by both appears, it's guaranteed they have
               only one same column */
            group_array[0].desc = order_array[0].desc;
        } else {
            if (field_count == group_array_size) {
                memset(order_array, 0, sizeof(ORDER_BY) * MAX_ORDER_COLS);
                order_array_size = group_array_size;
                if (!retrieve_orderby_info_from_groupby_info(sel_groupby, res_merge,
                                                             order_array, order_array_size, merged_result)) {
                    g_warning("%s:disp_orderby_info from group by failed:%s", G_STRLOC, con->orig_sql->str);
                    return 0;
                }
            }
        }
    }

    int i, index = 0;
    for (i = 0; i < aggr_num; i++) {
        network_mysqld_proto_fielddef_t *fdef = g_ptr_array_index(res_merge->fielddefs, aggr_array[index].pos);
        aggr_array[index].type = fdef->type;
        index++;
    }

    GList **candidates = g_new0(GList *, recv_queues->len);
    /* field-count-packet + eof-packet */
    guint pkt_count = res_merge->field_count + 2;

    if (!prepare_for_row_process(candidates, recv_queues, send_queue, pkt_count, merged_result)) {
        g_warning("%s:prepare_for_row_process failed", G_STRLOC);
        g_free(candidates);
        return 0;
    }

    if (!check_network_packet_err(con, candidates, recv_queues, send_queue, res_merge, merged_result)) {
        g_warning("%s:packet err is met", G_STRLOC);
        g_free(candidates);
        return 0;
    }

    limit_t limit;
    limit.offset = 0;
    limit.row_count = G_MAXINT32;
    sql_expr_get_int(select->limit, &limit.row_count);
    sql_expr_get_int(select->offset, &limit.offset);
    int pack_err_met = 0;
    having_condition_t *hav_condi = &(con->hav_condi);

    if (aggr_num > 0) {
        aggr_by_group_para_t para;

        para.send_queue = send_queue;
        para.recv_queues = recv_queues;
        para.limit = &limit;
        para.group_array = group_array;
        para.aggr_array = aggr_array;
        para.hav_condi = hav_condi;
        para.group_array_size = group_array_size;
        para.aggr_num = aggr_num;

        if (!aggr_by_group(&para, candidates, &pkt_count, merged_result)) {
            g_free(candidates);
            g_warning("%s:aggr_by_group error", G_STRLOC);
            return 0;
        }
        g_free(candidates);
    } else {
        merge_parameters_t *data = g_new0(merge_parameters_t, 1);
        heap_type *heap = g_new0(heap_type, 1);
        data->heap = heap;
        heap->len = recv_queues->len;
        heap_element *elements = g_new0(heap_element, heap->len);
        data->elements = elements;

        size_t iter;
        for (iter = 0; iter < recv_queues->len; iter++) {
            heap->element[iter] = elements + iter;
            heap->element[iter]->is_dup = 0;
            heap->element[iter]->is_err = 0;
            heap->element[iter]->refreshed = 0;
            heap->element[iter]->is_prior_to = 0;
        }

        data->send_queue = send_queue;
        data->recv_queues = recv_queues;
        data->candidates = candidates;
        data->pkt_count = pkt_count;
        data->limit.offset = limit.offset;
        data->limit.row_count = limit.row_count;

        for (iter = 0; iter < order_array_size; iter++) {
            memcpy((heap->order_para).order_array + iter, order_array + iter, sizeof(ORDER_BY));
        }

        data->pack_err_met = 0;
        heap->order_para.order_array_size = order_array_size;
        data->heap = heap;

        con->data = data;

        if ((select->flags & SF_DISTINCT) || field_count == group_array_size) {
            data->is_distinct = 1;
        }

        int merge_failed = 0;
        int result = do_merge(con, con->data, &merge_failed);

        if (merge_failed) {
            g_warning("%s:merge failed", G_STRLOC);
            merged_result->status = RM_FAIL;
            return 0;
        }

        if (!result) {
            g_warning("%s:result is zero", G_STRLOC);
            return 0;
        }

        pkt_count = data->pkt_count;
        pack_err_met = data->pack_err_met;
    }

    /* after adding all packets we don't need candidate list anymore */

    /* need to append EOF after all Row Data Packets?? Yes
     * update packet number in header 
     */

    if (!con->partially_merged) {
        if (pack_err_met == 0) {
            g_debug("%s: append here", G_STRLOC);
            /* TODO if in trans, then needs to set 'in transaction' flag ? */
            GString *eof_pkt = g_string_new_len("\x05\x00\x00\x07\xfe\x00\x00\x02\x00", 9);
            eof_pkt->str[3] = pkt_count + 1;
            network_queue_append(send_queue, eof_pkt);
        } else {
            g_debug("%s: err packet is met", G_STRLOC);
        }
    }

    return 1;
}

void
resultset_merge(network_queue *send_queue, GPtrArray *recv_queues,
                network_mysqld_con *con, uint64_t *uniq_id, result_merge_t *merged_result)
{
    shard_plugin_con_t *st = con->plugin_con_state;
    sql_context_t *context = st->sql_context;

    cetus_result_t res_merge = { 0 };

    if (!send_queue || !recv_queues || recv_queues->len <= 0 || !context) {
        g_warning("%s: packet->str[NET_HEADER_SIZE] != MYSQLD_PACKET_EOF", G_STRLOC);
        merged_result->status = RM_FAIL;
        return;
    }

    if (!check_fail_met(context, send_queue, recv_queues, con, uniq_id, merged_result)) {
        return;
    }

    if (context->explain) {
        network_queue *first_q = g_ptr_array_index(recv_queues, 0);
        if (first_q != NULL && first_q->chunks != NULL) {
            GString *packet = NULL;
            while ((packet = g_queue_pop_head(first_q->chunks)) != NULL)
                network_queue_append(send_queue, packet);
        }
        merged_result->status = RM_SUCCESS;
        return;
    }

    switch (context->stmt_type) {
    case STMT_SHOW_WARNINGS:
        if (!merge_for_show_warnings(send_queue, recv_queues, con, &res_merge, merged_result)) {
            cetus_result_destroy(&res_merge);
            return;
        }
        break;
    case STMT_SELECT:
        if (!merge_for_select(context, send_queue, recv_queues, con, &res_merge, merged_result)) {
            cetus_result_destroy(&res_merge);
            return;
        }
        break;
    case STMT_INSERT:
    case STMT_UPDATE:
    case STMT_DELETE:
    case STMT_SET:
    case STMT_START:
    case STMT_COMMIT:
    case STMT_ROLLBACK:
    default:
        if (!merge_for_modify(context, send_queue, recv_queues, con, &res_merge, merged_result)) {
            cetus_result_destroy(&res_merge);
            return;
        }
        break;
    }

    cetus_result_destroy(&res_merge);
    merged_result->status = RM_SUCCESS;
}
