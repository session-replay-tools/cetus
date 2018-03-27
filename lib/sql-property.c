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

#include "sql-property.h"

#include <string.h>

enum property_parse_state_t {
    // PARSE_STATE_INIT,
    PARSE_STATE_KEY = 0,
    PARSE_STATE_VALUE,
    PARSE_STATE_EQ_SIGN,
    PARSE_STATE_ERROR,
};

enum {
    TYPE_INT,
    TYPE_STRING,
};

#define MAX_VALUE_LEN 50

gboolean
sql_property_is_valid(sql_property_t *p)
{
    if (p->table && p->group)   /* mutual exclusive */
        return FALSE;
    return TRUE;
}

void
sql_property_free(sql_property_t *p)
{
    if (p->group)
        g_free(p->group);
    if (p->table)
        g_free(p->table);
    if (p->key)
        g_free(p->key);
    g_free(p);
}

void
sql_property_parser_reset(sql_property_parser_t *parser)
{
    memset(parser, 0, sizeof(*parser));
}

static int
string_to_code(const char *str)
{
    struct code_map_t {
        char *name;
        int code;
    } map[] = {
        {
        "READWRITE", MODE_READWRITE}, {
        "READONLY", MODE_READONLY}, {
        "SCOPE_LOCAL", P_SCOPE_LOCAL}, {
        "SCOPE_GLOBAL", P_SCOPE_GLOBAL}, {
    "SINGLE_NODE", TRX_SINGLE_NODE},};
    int i;
    for (i = 0; i < sizeof(map) / sizeof(*map); ++i) {
        if (strcasecmp(map[i].name, str) == 0)
            return map[i].code;
    }
    return ERROR_VALUE;
}

static gboolean
parser_find_key(sql_property_parser_t *parser, const char *token, int len)
{
    static const struct property_desc_t {
        const char *name;
        size_t offset;
        int type;
        value_parse_func get_value;
    } desc[] = {
        {
        "mode", offsetof(struct sql_property_t, mode), TYPE_INT, string_to_code}, {
        "scope", offsetof(struct sql_property_t, scope), TYPE_INT, string_to_code}, {
        "transaction", offsetof(struct sql_property_t, transaction), TYPE_INT, string_to_code}, {
        "group", offsetof(struct sql_property_t, group), TYPE_STRING, NULL}, {
        "table", offsetof(struct sql_property_t, table), TYPE_STRING, NULL}, {
    "key", offsetof(struct sql_property_t, key), TYPE_STRING, NULL},};
    int i = 0;
    for (i = 0; i < sizeof(desc) / sizeof(*desc); ++i) {
        if (strcasecmp(token, desc[i].name) == 0) {
            parser->key_offset = desc[i].offset;
            parser->key_type = desc[i].type;
            parser->get_value = desc[i].get_value;
            parser->state = PARSE_STATE_EQ_SIGN;
            return TRUE;
        }
    }
    parser->state = PARSE_STATE_ERROR;
    return FALSE;
}

static gboolean
parser_set_value(sql_property_parser_t *parser, void *object, const char *token, int len)
{
    if (parser->key_type == TYPE_INT) {
        int *p = (int *)((unsigned char *)object + parser->key_offset);
        if (token[0] == '"' && token[len - 1] == '"') { /* might be quoted */
            char buf[MAX_VALUE_LEN] = { 0 };
            memcpy(buf, token + 1, len - 2);
            *p = parser->get_value(buf);
        } else {
            *p = parser->get_value(token);
        }
        parser->state = PARSE_STATE_KEY;
        return TRUE;
    } else if (parser->key_type == TYPE_STRING) {
        char **p = (char **)((unsigned char *)object + parser->key_offset);
        if (token[0] == '"' && token[len - 1] == '"') { /* might be quoted */
            *p = g_malloc0(len - 1);
            memcpy(*p, token + 1, len - 2);
        } else {
            *p = strndup(token, len);
        }
        parser->state = PARSE_STATE_KEY;
        return TRUE;
    }
    parser->state = PARSE_STATE_ERROR;
    return FALSE;
}

gboolean
sql_property_parser_parse(sql_property_parser_t *parser, const char *token, int len, sql_property_t *property)
{
    switch (parser->state) {

    case PARSE_STATE_KEY:
        return parser_find_key(parser, token, len);

    case PARSE_STATE_EQ_SIGN:
        if (len == 1 && token[0] == '=') {
            parser->state = PARSE_STATE_VALUE;
            return TRUE;
        } else {
            return FALSE;
        }

    case PARSE_STATE_VALUE:
        if (len > MAX_VALUE_LEN) {
            return FALSE;
        }
        return parser_set_value(parser, property, token, len);

    default:
        return FALSE;
    }
}
