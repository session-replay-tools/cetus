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

#ifndef SQL_PROPERTY_H
#define SQL_PROPERTY_H

#include <glib.h>

#define MAX_PROPERTY_LEN 64

enum {
    ERROR_VALUE = -1,
    MODE_READWRITE = 1,
    MODE_READONLY,
    P_SCOPE_LOCAL,
    P_SCOPE_GLOBAL,
    TRX_SINGLE_NODE,
};

typedef struct sql_property_t {
    int mode;
    int scope;
    int transaction;
    char *group;
    char *table;
    char *key;
} sql_property_t;

void sql_property_free(sql_property_t *);
gboolean sql_property_is_valid(sql_property_t *);

typedef int (*value_parse_func) (const char *);

typedef struct sql_property_parser_t {
    int key_offset;
    value_parse_func get_value;
    int key_type;
    int state;

    gboolean is_parsing;

} sql_property_parser_t;

void sql_property_parser_reset(sql_property_parser_t *parser);

gboolean sql_property_parser_parse(sql_property_parser_t *parser, const char *token, int len, sql_property_t *property);

#endif /* SQL_PROPERTY_H */
