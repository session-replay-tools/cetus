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

#ifndef SQL_CONTEXT_H
#define SQL_CONTEXT_H

#include "sql-expression.h"
#include "myparser.y.h"

enum sql_parse_state_code_t {
    PARSE_NOT_SUPPORT = -3,
    PARSE_SYNTAX_ERR = -2,
    PARSE_ERROR = -1,
    PARSE_OK = 0,
    PARSE_HEAD,                 /* only extract head, leave the tail unparsed */
    PARSE_UNRECOGNIZED,         /* confused, send to backend for further validation */
};

enum sql_parsing_place_t {
    SELECT_BEGIN = 0,
    SELECT_OPTION,
    SELECT_COLUMN,
    SELECT_FROM,
    SELECT_WHERE,
    SELECT_DONE,
};

struct sql_property_t;

typedef struct sql_context_t {
    enum sql_parse_state_code_t rc;
    char *message;
    int explain;
    void *user_data;
    void *sql_statement;        /* opaque statement pointer */
    sql_stmt_type_t stmt_type;
    short stmt_count;

    enum sql_clause_flag_t rw_flag;
    enum sql_clause_flag_t clause_flags;
    enum sql_expr_flags_t where_flags;
    enum sql_parsing_place_t parsing_place;

    struct sql_property_t *property;
    unsigned int is_parsing_subquery:1;
    unsigned int allow_subquery_nesting:1;
    unsigned int sql_needs_reconstruct:1;
} sql_context_t;

void sql_context_init(sql_context_t *);

void sql_context_reset(sql_context_t *);

void sql_context_destroy(sql_context_t *);

void sql_context_append_msg(sql_context_t *, char *msg);

void sql_context_set_error(sql_context_t *, int err, char *msg);

void sql_context_add_stmt(sql_context_t *, enum sql_stmt_type_t, void *);

gboolean sql_context_using_property(sql_context_t *);

gboolean sql_context_has_sharding_property(sql_context_t *p);

void sql_context_parse_len(sql_context_t *, GString *sql);

gboolean sql_context_is_autocommit_on(sql_context_t *);

gboolean sql_context_is_autocommit_off(sql_context_t *);

gboolean sql_context_is_single_node_trx(sql_context_t *);

gboolean sql_context_is_cacheable(sql_context_t *);

#endif /* SQL_CONTEXT_H */
