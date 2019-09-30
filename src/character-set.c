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

#include "character-set.h"

#include <string.h>
#include <assert.h>

int
charset_get_number(const char *name)
{
    if (!name)
        return DEFAULT_CHARSET;
    static struct _charset_number_t {
        const char *name;
        int number;
    } map[] = {
        {
        "latin1", 0x08}, {
        "big5", 0x01}, {
        "gb2312", 0x18}, {
        "gbk", 0x1c}, {
        "utf8", 0x21}, {
        "utf8mb4", 0x2d}, {
        "utf8_unicode_ci", 0x21}, {
        "utf8mb4_unicode_ci", 0x2d}, {
        "binary", 0x3f}
    };
    int map_len = sizeof(map) / sizeof(struct _charset_number_t);
    int i = 0;
    while (i < map_len) {
        if (strcasecmp(name, map[i].name) == 0) {
            return map[i].number;
        }
        ++i;
    }
    return DEFAULT_CHARSET;
}

const char *
charset_get_name(int number)
{
    static const char *charset[256] = {
        0, "big5", 0, 0, 0, 0, 0, 0, "latin1", 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, "gb2312", 0, 0, 0, "gbk", 0, 0, 0,
        0, "utf8", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, "utf8mb4", 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, "binary",
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        "utf8", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        "utf8mb4", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    };

    if (number >= (sizeof(charset) / sizeof(charset[0]))) {
        return NULL;
    }

    return charset[number];
}
