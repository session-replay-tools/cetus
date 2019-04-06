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

#include "sql-operation.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>

#include "sql-expression.h"
#include "sql-filter-variables.h"
#include "myparser.y.h"

void
sql_select(sql_context_t *st, sql_select_t *select)
{
    st->rw_flag |= CF_READ;
    sql_context_add_stmt(st, STMT_SELECT, select);
}

void
sql_delete(sql_context_t *st, sql_delete_t *del)
{
    if (st->rc != PARSE_OK) {
        g_warning("Delete Parse Error");
    }
    st->rw_flag |= CF_WRITE;
    sql_context_add_stmt(st, STMT_DELETE, del);
}

void
sql_update(sql_context_t *st, sql_update_t *update)
{
    if (st->rc != PARSE_OK) {
        g_warning("Update Parse Error");
    }
    st->rw_flag |= CF_WRITE;
    sql_context_add_stmt(st, STMT_UPDATE, update);
}

void
sql_insert(sql_context_t *st, sql_insert_t *insert)
{
    if (st->rc != PARSE_OK) {
        g_warning("Insert Parse Error");
    }
    st->rw_flag |= CF_WRITE;
    sql_context_add_stmt(st, STMT_INSERT, insert);
}

void
sql_start_transaction(sql_context_t *st)
{
    if (st->rc != PARSE_OK) {
        g_warning("Start transaction parse Error");
    }
    st->rw_flag |= CF_WRITE;
    sql_context_add_stmt(st, STMT_START, NULL);
}

void
sql_commit_transaction(sql_context_t *st)
{
    if (st->rc != PARSE_OK) {
        g_warning("COMMIT transaction parse Error");
    }
    st->rw_flag |= CF_WRITE;
    sql_context_add_stmt(st, STMT_COMMIT, NULL);
}

void
sql_rollback_transaction(sql_context_t *st)
{
    if (st->rc != PARSE_OK) {
        g_warning("ROLLBACK transaction parse Error");
    }
    st->rw_flag |= CF_WRITE;
    sql_context_add_stmt(st, STMT_ROLLBACK, NULL);
}

void
sql_savepoint(sql_context_t *st, int tk, char *name)
{
    if (st->rc != PARSE_OK) {
        g_warning("SAVE POINT parse Error");
    }
    st->rw_flag |= CF_WRITE;
    sql_context_add_stmt(st, STMT_SAVEPOINT, name);
}

void
sql_drop_database(sql_context_t *st, sql_drop_database_t *drop_database)
{
    st->rw_flag |= CF_WRITE;
    sql_context_add_stmt(st, STMT_DROP_DATABASE, drop_database);
}

static gboolean
string_array_contains(const char **sa, int size, const char *str)
{
    int i = 0;
    for (; i < size; ++i) {
        if (strcasecmp(sa[i], str) == 0)
            return TRUE;
    }
    return FALSE;
}

void
sql_set_variable(sql_context_t *ps, sql_expr_list_t *exps)
{
    if (ps->property) {
        sql_context_set_error(ps, PARSE_NOT_SUPPORT, "Commanding comment is not allowed in SET clause");
        goto out;
    }
    g_assert(exps);

    int i = 0;
    for (i = 0; i < exps->len; ++i) {
        sql_expr_t *p = g_ptr_array_index(exps, i);
        if (!p || p->op != TK_EQ || !sql_expr_is_id(p->left, NULL)) {
            sql_context_set_error(ps, PARSE_SYNTAX_ERR, "syntax error in SET");
            goto out;
        }
        if (p->left && p->left->var_scope == SCOPE_GLOBAL) {
            sql_context_set_error(ps, PARSE_NOT_SUPPORT, "Only session scope SET is supported now");
            goto out;
        }
        const char *var_name = p->left->token_text;
        const char *value = p->right->token_text;
        if (strcasecmp(var_name, "sql_mode") == 0) {
            gboolean supported = sql_filter_vars_is_allowed(var_name, value);
            if (!supported) {
                sql_context_set_error(ps, PARSE_NOT_SUPPORT, "This sql_mode is not supported");
                goto out;
            }
        } else if (!sql_filter_vars_is_allowed(var_name, value)) {
            char msg[128];
            snprintf(msg, 128, "SET of %s is not supported", var_name);
            sql_context_set_error(ps, PARSE_NOT_SUPPORT, msg);
            goto out;
        }
    }
  out:
    sql_context_add_stmt(ps, STMT_SET, exps);
}

void
sql_set_names(sql_context_t *ps, char *val)
{
    if (ps->property) {
        sql_context_set_error(ps, PARSE_NOT_SUPPORT, "Commanding comment is not allowed in SET clause");
        g_free(val);
        return;
    }
    const char *charsets[] = { "latin1", "ascii", "gb2312", "gbk", "utf8", "utf8mb4", "big5" };
    int cs_size = sizeof(charsets) / sizeof(char *);
    if (!string_array_contains(charsets, cs_size, val)) {
        char msg[128] = { 0 };
        snprintf(msg, 128, "Unknown character set: %s", val);
        sql_context_set_error(ps, PARSE_NOT_SUPPORT, msg);
        g_free(val);
        return;
    }
    sql_context_add_stmt(ps, STMT_SET_NAMES, val);
}

void
sql_set_transaction(sql_context_t *ps, int scope, int rw_feature, int level)
{
    if (ps->property) {
        sql_context_set_error(ps, PARSE_NOT_SUPPORT, "Commanding comment is not allowed in SET clause");
        return;
    }
    if (scope == SCOPE_GLOBAL) {
        sql_context_set_error(ps, PARSE_NOT_SUPPORT, "GLOBAL scope SET TRANSACTION is not supported now");
        return;
    }
    sql_set_transaction_t *set_tran = g_new0(sql_set_transaction_t, 1);
    set_tran->scope = scope;
    set_tran->rw_feature = rw_feature;
    set_tran->level = level;
    sql_context_add_stmt(ps, STMT_SET_TRANSACTION, set_tran);
}

void
sql_use_database(sql_context_t *ps, char *val)
{
    ps->rw_flag |= CF_READ;
    sql_context_add_stmt(ps, STMT_USE, val);
}

void
sql_explain_table(sql_context_t *ps, sql_src_list_t *table)
{
    ps->rw_flag |= CF_READ;
    sql_context_add_stmt(ps, STMT_EXPLAIN_TABLE, table);
}
