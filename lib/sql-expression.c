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

#include "sql-expression.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <ctype.h>
#include <inttypes.h>
#include <glib.h>

#include "myparser.y.h"

char *
sql_token_dup(sql_token_t token)
{
    if (token.n == 0)
        return NULL;
    char *s = g_malloc0(token.n + 1);
    memcpy(s, token.z, token.n);
    sql_string_dequote(s);
    return s;
}

static int64_t
sql_token_to_int(sql_token_t token)
{
    /* TODO: HEX */
    int64_t value = 0;
    int sign = 1;
    const char *c = token.z;
    int i = 0;
    if (*c == '+' || *c == '-') {
        if (*c == '-')
            sign = -1;
        c++;
        i++;
    }
    while (isdigit(*c) && i++ < token.n) {
        value *= 10;
        value += (int)(*c - '0');
        c++;
    }
    return (value * sign);
}

void
sql_string_dequote(char *z)
{
    int quote;
    int i, j;
    if (z == 0)
        return;
    quote = z[0];
    switch (quote) {
    case '\'':
        break;
    case '"':
        break;
    case '`':
        break;                  /* For MySQL compatibility */
    case '[':
        quote = ']';
        break;                  /* For MS SqlServer compatibility */
    default:
        return;
    }
    for (i = 1, j = 0; z[i]; i++) {
        if (z[i] == quote) {
            if (z[i + 1] == quote) {    /* quote escape */
                z[j++] = quote;
                i++;
            } else {
                z[j++] = 0;
                break;
            }
        } else if (z[i] == '\\') {  /* slash escape */
            i++;
            z[j++] = z[i];
        } else {
            z[j++] = z[i];
        }
    }
}

sql_expr_t *
sql_expr_new(int op, const sql_token_t *token)
{
    int extra = 0;
    if (token && op != TK_INTEGER) {
        extra = token->n + 1;
    }
    sql_expr_t *expr = g_malloc0(sizeof(sql_expr_t) + extra);
    if (expr) {
        expr->op = op;
        if (token) {
            if (extra == 0) {
                expr->num_value = sql_token_to_int(*token);
                expr->token_text = token->z;
            } else {
                expr->token_text = (char *)&expr[1];
                assert(token != NULL);
                if (token->n) {
                    strncpy(expr->token_text, token->z, token->n);
                    if (op == TK_STRING || op == TK_ID) {
                        sql_string_dequote(expr->token_text);
                    }
                }
            }
            expr->start = token->z;
            expr->end = &token->z[token->n];
        }
        expr->height = 1;
        expr->var_scope = SCOPE_SESSION;
    }
    return expr;
}

/**
 * Only duplicate the root node
 */
sql_expr_t *
sql_expr_dup(const sql_expr_t *p)
{
    int extra = 0;
    if (p->token_text) {
        extra = strlen(p->token_text) + 1;
    }
    int size = sizeof(sql_expr_t) + extra;
    sql_expr_t *expr = g_malloc0(size);
    if (expr) {
        memcpy(expr, p, size);
        if(p->alias) {
            expr->alias = g_strdup(expr->alias);
            expr->left = 0;
            expr->right = 0;
        } else {
            expr->alias = 0;
            if (p->op == TK_DOT) {
                expr->left = sql_expr_dup(p->left);
                expr->right = sql_expr_dup(p->right);
            } else {
                expr->left = 0;
                expr->right = 0;
            }
        }
        expr->list = 0;
        expr->select = 0;
    }
    return expr;
}

static void
height_of_expr(sql_expr_t *p, int *height)
{
    if (p) {
        if (p->height > *height) {
            *height = p->height;
        }
    }
}

static void
expr_set_height(sql_expr_t *p)
{
    int height = 0;
    height_of_expr(p->left, &height);
    height_of_expr(p->right, &height);
    p->height = height + 1;
}

void
sql_expr_attach_subtrees(sql_expr_t *root, sql_expr_t *left, sql_expr_t *right)
{
    if (root == NULL) {
        sql_expr_free(left);
        sql_expr_free(right);
    } else {
        if (left) {
            root->left = left;
            root->start = left->start;
        }
        if (right) {
            root->right = right;
            root->end = right->end;
        }
        expr_set_height(root);
    }
}

void
sql_expr_free(void *p)
{
    if (p) {
        sql_expr_t *exp = (sql_expr_t *)p;
        if (exp->left)
            sql_expr_free(exp->left);
        if (exp->right)
            sql_expr_free(exp->right);
        if (exp->list)
            g_ptr_array_free(exp->list, TRUE);
        if (exp->select)
            sql_select_free(exp->select);
        if (exp->alias)
            g_free(exp->alias);
        g_free(exp);
    }
}

gboolean
sql_expr_get_int(sql_expr_t *p, gint64 *value)
{
    gboolean rc = FALSE;
    if (p == NULL)
        return FALSE;
    switch (p->op) {
    case TK_INTEGER:
        if (value) {
            *value = p->num_value;
        }
        rc = TRUE;
        break;
    case TK_UMINUS:
        rc = sql_expr_get_int(p->left, value);
        *value = -(*value);
        break;
    case TK_UPLUS:
        rc = sql_expr_get_int(p->left, value);
        break;
    default:
        rc = FALSE;
    }
    return rc;
}

gboolean
sql_expr_is_boolean(sql_expr_t *p, gboolean *value)
{
    gint64 int_val = -1;
    if (sql_expr_get_int(p, &int_val)) {
        *value = (int_val != 0);
        return TRUE;
    } else if (sql_expr_is_id(p, "off") || sql_expr_is_id(p, "false")) {
        *value = FALSE;
        return TRUE;
    } else if (sql_expr_is_id(p, "on") || sql_expr_is_id(p, "true")) {
        *value = TRUE;
        return TRUE;
    } else {
        return FALSE;
    }
}

/**
 * @param name NULL or expected token name
 * @return
 *   if name is NULL:
 *     return whether p is of type TK_ID
 *   if name not NULL:
 *     return TRUE only if p is TK_ID and has same name
 */
gboolean
sql_expr_is_id(const sql_expr_t *p, const char *name)
{
    const char *id = sql_expr_id(p);
    if (name == NULL) {
        return id != NULL;
    } else {
        if (id) {
            return strcasecmp(id, name) == 0;
        }
        return FALSE;
    }
}

gboolean
sql_expr_is_function(const sql_expr_t *p, const char *name)
{
    const char *func = NULL;
    if (p && p->op == TK_FUNCTION) {
        func = p->token_text;
    }
    if (name == NULL) {
        return func != NULL;
    } else {
        if (func && strcasecmp(func, name) == 0) {
            return TRUE;
        }
        if (p && p->alias && strcmp(p->alias, name) == 0) {
            return TRUE;
        }
        return FALSE;
    }
}

/**
 * prefix and suffix is NULLable
 * @return
 * if prefix or suffix is NULL, return true if the expression is of type:
 * TK_DOT
 *  |--TK_ID
 *  |--TK_ID
 *
 * if prefix or suffix not NULL, return ture only if expr is "prefix.suffix"
 */
gboolean
sql_expr_is_dotted_name(const sql_expr_t *p, const char *prefix, const char *suffix)
{
    if (p && p->op == TK_DOT) {
        return sql_expr_is_id(p->left, prefix)
            && sql_expr_is_id(p->right, suffix);
    }
    return FALSE;
}

void sql_expr_get_dotted_names(const sql_expr_t *p, char *db, int db_len,
                               char *table, int tb_len,
                               char *col, int col_len)
{
    if (p && p->op == TK_DOT) {
        const sql_expr_t *dot = p;
        if (p->right->op == TK_DOT) { /* db.table.col */
            if (db) {
                strncpy(db, p->left->token_text, db_len);
            }
            dot = p->right;
        }

        if (table && dot->left->token_text) {
            strncpy(table, dot->left->token_text, tb_len);
        }
        if (col && dot->right->token_text) {
            strncpy(col, dot->right->token_text, col_len);
        }
    }
}

gboolean
sql_expr_is_field_name(const sql_expr_t *p)
{
    if (p) {
        if (p->op == TK_ID) {
            return TRUE;
        } else if (p->op == TK_DOT) {   /* qualified name */
            if (p->right->op == TK_DOT) {   /* db.table.col */
                return sql_expr_is_id(p->left, 0)
                    && sql_expr_is_dotted_name(p->right, 0, 0);
            } else {            /* table.col */
                return sql_expr_is_dotted_name(p, 0, 0);
            }
        }
    }
    return FALSE;
}

sql_expr_list_t *
sql_expr_list_append(sql_expr_list_t *list, sql_expr_t *expr)
{
    if (expr == NULL)
        return list;
    if (list == NULL) {
        list = g_ptr_array_new_with_free_func(sql_expr_free);
    }
    g_ptr_array_add(list, expr);
    return list;
}

sql_expr_t *
sql_expr_list_find(sql_expr_list_t *list, const char *name)
{
    int i = 0;
    for (i = 0; i < list->len; ++i) {
        sql_expr_t *col = g_ptr_array_index(list, i);
        if (sql_expr_is_id(col, name)) {
            return col;
        }
    }
    return NULL;
}

sql_expr_t *
sql_expr_list_find_fullname(sql_expr_list_t *list, const sql_expr_t *expr)
{
    if (sql_expr_is_dotted_name(expr, NULL, NULL)) {
        const char *prefix = expr->left->token_text;
        const char *suffix = expr->right->token_text;
        int i;
        for (i = 0; i < list->len; ++i) {
            sql_expr_t *col = g_ptr_array_index(list, i);
            if (sql_expr_is_dotted_name(col, prefix, suffix)) {
                return col;
            }
        }
    }
    return NULL;
}

/**
 * Find first aggregate named `target`, only match function name
 * Example: target=max will match max(a) or max(b)
 * if `target` is NULL, find first occurance of any aggregate
 */
int sql_expr_list_find_aggregate(sql_expr_list_t *list, const char *target)
{
    int i;
    for (i = 0; i < list->len; ++i) {
        sql_expr_t *p = g_ptr_array_index(list, i);
        if (sql_expr_is_function(p, target)
            && sql_aggregate_type(p->token_text) != FT_UNKNOWN) {
            return i;
        }
    }
    return -1;
}

/**
 * Similar with `sql_exp_list_find_aggregate`, but will also
 *  match function arguments, and target cannot be NULL
 * Example: target=max(a) will match max(a)
 */
int sql_expr_list_find_exact_aggregate(sql_expr_list_t *list, const char *target, int len)
{
    int i;
    for (i = 0; i < list->len; ++i) {
        sql_expr_t *p = g_ptr_array_index(list, i);
        if (p->op == TK_FUNCTION
            && sql_aggregate_type(p->token_text) != FT_UNKNOWN) {
            if (strncasecmp(p->start, target, len) == 0) {
                return i;
            }
            if (p->alias && strncmp(p->alias, target, len) == 0) {
                return i;
            }
        }
    }
    return -1;
}

int
sql_expr_list_find_aggregates(sql_expr_list_t *list, group_aggr_t * aggr_array)
{
    int i, index = 0;
    enum sql_aggregate_type_t type;
    for (i = 0; i < list->len; ++i) {
        sql_expr_t *p = g_ptr_array_index(list, i);
        if (p->op == TK_FUNCTION) {
            type = sql_aggregate_type(p->token_text);
            if (type != FT_UNKNOWN) {
                if (index < MAX_AGGR_FUNS) {
                    aggr_array[index].pos = i;
                    aggr_array[index].fun_type = type;
                    index++;
                }
            }
        }
    }
    return index;
}

void
sql_expr_list_free(sql_expr_list_t *list)
{
    if (list)
        g_ptr_array_free(list, TRUE);
}

enum sql_aggregate_type_t
sql_aggregate_type(const char *s)
{
    if (strncasecmp(s, "count", 5) == 0)
        return FT_COUNT;
    else if (strncasecmp(s, "sum", 3) == 0)
        return FT_SUM;
    else if (strncasecmp(s, "avg", 3) == 0)
        return FT_AVG;
    else if (strncasecmp(s, "max", 3) == 0)
        return FT_MAX;
    else if (strncasecmp(s, "min", 3) == 0)
        return FT_MIN;
    else
        return FT_UNKNOWN;
}

sql_column_t *
sql_column_new()
{
    return g_new0(struct sql_column_t, 1);
}

void
sql_column_free(void *p)
{
    if (!p)
        return;
    sql_column_t *col = (sql_column_t *)p;
    if (col->expr)
        sql_expr_free(col->expr);
    if (col->alias)
        g_free(col->alias);
    if (col->type)
        g_free(col->type);
    g_free(col);
}

sql_column_list_t *
sql_column_list_append(sql_column_list_t *list, sql_column_t *col)
{
    if (col == NULL)
        return list;
    if (list == NULL) {
        list = g_ptr_array_new_with_free_func(sql_column_free);
    }
    g_ptr_array_add(list, col);
    return list;
}

void
sql_column_list_free(sql_column_list_t *list)
{
    if (list)
        g_ptr_array_free(list, TRUE);
}

sql_select_t *
sql_select_new()
{
    sql_select_t *p = g_new0(sql_select_t, 1);
    return p;
}

void
sql_select_free(sql_select_t *p)
{
    if (!p)
        return;
    if (p->columns)             /* The fields of the result */
        sql_expr_list_free(p->columns);
    if (p->from_src)            /* The FROM clause */
        sql_src_list_free(p->from_src);
    if (p->where_clause)        /* The WHERE clause */
        sql_expr_free(p->where_clause);
    if (p->groupby_clause)      /* The GROUP BY clause */
        sql_expr_list_free(p->groupby_clause);
    if (p->having_clause)       /* The HAVING clause */
        sql_expr_free(p->having_clause);
    if (p->orderby_clause)      /* The ORDER BY clause */
        sql_expr_list_free(p->orderby_clause);
    if (p->prior)               /* Prior select in a compound select statement */
        sql_select_free(p->prior);
    /* sql_select_t *pNext;         Next select to the left in a compound */
    if (p->limit)               /* LIMIT expression. NULL means not used. */
        sql_expr_free(p->limit);
    if (p->offset)              /* OFFSET expression. NULL means not used. */
        sql_expr_free(p->offset);
    g_free(p);
}

sql_delete_t *
sql_delete_new()
{
    sql_delete_t *p = g_new0(sql_delete_t, 1);
    return p;
}

void
sql_delete_free(sql_delete_t *p)
{
    if (!p)
        return;
    if (p->from_src)            /* The FROM clause */
        sql_src_list_free(p->from_src);
    if (p->where_clause)        /* The WHERE clause */
        sql_expr_free(p->where_clause);
    if (p->orderby_clause)
        sql_expr_list_free(p->orderby_clause);  /* The ORDER BY clause */
    if (p->limit)               /* LIMIT expression. NULL means not used. */
        sql_expr_free(p->limit);
    if (p->offset)              /* OFFSET expression. NULL means not used. */
        sql_expr_free(p->offset);
    g_free(p);
}

sql_update_t *
sql_update_new()
{
    sql_update_t *p = g_new0(sql_update_t, 1);
    return p;
}

void
sql_update_free(sql_update_t *p)
{
    if (!p)
        return;
    if (p->table_reference)
        sql_table_reference_free(p->table_reference);
    if (p->set_list)
        sql_expr_list_free(p->set_list);
    if (p->where_clause)        /* The WHERE clause */
        sql_expr_free(p->where_clause);
    if (p->orderby_clause)
        sql_expr_list_free(p->orderby_clause);  /* The ORDER BY clause */
    if (p->limit)               /* LIMIT expression. NULL means not used. */
        sql_expr_free(p->limit);
    if (p->offset)              /* OFFSET expression. NULL means not used. */
        sql_expr_free(p->offset);
    g_free(p);
}

sql_insert_t *
sql_insert_new()
{
    sql_insert_t *p = g_new0(sql_insert_t, 1);
    return p;
}

void
sql_insert_free(sql_insert_t *p)
{
    if (!p)
        return;
    if (p->table)
        sql_src_list_free(p->table);
    if (p->sel_val)
        sql_select_free(p->sel_val);
    if (p->columns)
        sql_id_list_free(p->columns);
    if (p->update_list)
        sql_expr_list_free(p->update_list);
    g_free(p);
}

void
sql_src_item_free(void *p)
{
    if (!p)
        return;
    struct sql_src_item_t *item = (struct sql_src_item_t *)p;
    if (item->table_name)
        g_free(item->table_name);
    if (item->index_hint)
        sql_index_hint_free(item->index_hint);
    if (item->table_alias)
        g_free(item->table_alias);
    if (item->dbname)
        g_free(item->dbname);
    if (item->select)
        sql_select_free(item->select);
    if (item->on_clause)
        sql_expr_free(item->on_clause);
    if (item->pUsing)
        sql_id_list_free(item->pUsing);
    if (item->func_arg)
        sql_expr_list_free(item->func_arg);
    if (item->groups) {
        g_ptr_array_free(item->groups, TRUE);
    }
    g_free(item);
}

sql_drop_database_t *
sql_drop_database_new()
{
    sql_drop_database_t *p = g_new0(sql_drop_database_t, 1);
    return p;
}

void
sql_drop_database_free(sql_drop_database_t *p)
{
    if(!p) return;
    if(p && p->schema_name) {
        g_free(p->schema_name);
    }
    g_free(p);
}

sql_src_list_t *
sql_src_list_append(sql_src_list_t *p, sql_token_t *tname,
                    sql_token_t *dbname, sql_index_hint_t *index_hint, sql_token_t *alias, sql_select_t *subquery,
                    sql_expr_t *on_clause, sql_id_list_t *using_clause)
{
    struct sql_src_item_t *item = g_new0(sql_src_item_t, 1);
    if (item) {
        item->table_name = tname ? sql_token_dup(*tname) : NULL;
        item->index_hint = index_hint;
        item->table_alias = alias ? sql_token_dup(*alias) : NULL;
        item->dbname = dbname ? sql_token_dup(*dbname) : NULL;
        item->select = subquery;
        item->on_clause = on_clause;
        item->pUsing = using_clause;
    }
    if (!p) {
        p = g_ptr_array_new_with_free_func(sql_src_item_free);
    }
    g_ptr_array_add(p, item);
    return p;
}

void
sql_src_list_free(sql_src_list_t *p)
{
    if (p)
        g_ptr_array_free(p, TRUE);
}

sql_id_list_t *
sql_id_list_append(sql_id_list_t *p, sql_token_t *id_name)
{
    if (!p) {
        p = g_ptr_array_new_with_free_func(g_free);
    }
    if (id_name)
        g_ptr_array_add(p, sql_token_dup(*id_name));
    return p;
}

void
sql_id_list_free(sql_id_list_t *p)
{
    if (p)
        g_ptr_array_free(p, TRUE);
}

char *
sql_get_token_name(int op)
{
    static struct token_list_s {
        int code;
        char *name;
    } token_list[] = {
        {
        TK_SEMI, "TK_SEMI      "}, {
        TK_CREATE, "TK_CREATE    "}, {
        TK_TABLE, "TK_TABLE     "}, {
        TK_IF, "TK_IF        "}, {
        TK_NOT, "TK_NOT       "}, {
        TK_EXISTS, "TK_EXISTS    "}, {
        TK_TEMP, "TK_TEMP      "}, {
        TK_LP, "TK_LP        "}, {
        TK_RP, "TK_RP        "}, {
        TK_AS, "TK_AS        "}, {
        TK_WITHOUT, "TK_WITHOUT   "}, {
        TK_COMMA, "TK_COMMA     "}, {
        TK_OR, "TK_OR        "}, {
        TK_AND, "TK_AND       "}, {
        TK_IS, "TK_IS        "}, {
        TK_MATCH, "TK_MATCH     "}, {
        TK_LIKE_KW, "TK_LIKE_KW   "}, {
        TK_BETWEEN, "TK_BETWEEN   "}, {
        TK_IN, "TK_IN        "},
            /*    {TK_ISNULL      ,"TK_ISNULL    "}, */
            /*    {TK_NOTNULL     ,"TK_NOTNULL   "}, */
        {
        TK_NE, "TK_NE        "}, {
        TK_EQ, "TK_EQ        "}, {
        TK_GT, "TK_GT        "}, {
        TK_LE, "TK_LE        "}, {
        TK_LT, "TK_LT        "}, {
        TK_GE, "TK_GE        "}, {
        TK_ESCAPE, "TK_ESCAPE    "}, {
        TK_BITAND, "TK_BITAND    "}, {
        TK_BITOR, "TK_BITOR     "}, {
        TK_LSHIFT, "TK_LSHIFT    "}, {
        TK_RSHIFT, "TK_RSHIFT    "}, {
        TK_PLUS, "TK_PLUS      "}, {
        TK_MINUS, "TK_MINUS     "}, {
        TK_STAR, "TK_STAR      "}, {
        TK_SLASH, "TK_SLASH     "}, {
        TK_REM, "TK_REM       "}, {
        TK_CONCAT, "TK_CONCAT    "}, {
        TK_COLLATE, "TK_COLLATE   "}, {
        TK_BITNOT, "TK_BITNOT    "}, {
        TK_ID, "TK_ID        "}, {
        TK_ABORT, "TK_ABORT     "}, {
        TK_ACTION, "TK_ACTION    "}, {
        TK_AFTER, "TK_AFTER     "}, {
        TK_ANALYZE, "TK_ANALYZE   "}, {
        TK_ASC, "TK_ASC       "}, {
        TK_ATTACH, "TK_ATTACH    "}, {
        TK_BEFORE, "TK_BEFORE    "}, {
        TK_BEGIN, "TK_BEGIN     "}, {
        TK_BY, "TK_BY        "}, {
        TK_CASCADE, "TK_CASCADE   "}, {
        TK_CAST, "TK_CAST      "}, {
        TK_COLUMNKW, "TK_COLUMNKW  "}, {
        TK_CONFLICT, "TK_CONFLICT  "}, {
        TK_DATABASE, "TK_DATABASE  "}, {
        TK_DESC, "TK_DESC      "}, {
        TK_DETACH, "TK_DETACH    "}, {
        TK_EACH, "TK_EACH      "}, {
        TK_END, "TK_END       "}, {
        TK_FAIL, "TK_FAIL      "}, {
        TK_FOR, "TK_FOR       "}, {
        TK_IGNORE, "TK_IGNORE    "}, {
        TK_INITIALLY, "TK_INITIALLY "}, {
        TK_INSTEAD, "TK_INSTEAD   "}, {
        TK_NO, "TK_NO        "}, {
        TK_PLAN, "TK_PLAN      "}, {
        TK_QUERY, "TK_QUERY     "}, {
        TK_KEY, "TK_KEY       "}, {
        TK_OF, "TK_OF        "}, {
        TK_TO, "TK_TO"}, {
        TK_OFFSET, "TK_OFFSET    "}, {
        TK_PRAGMA, "TK_PRAGMA    "}, {
        TK_RAISE, "TK_RAISE     "}, {
        TK_RECURSIVE, "TK_RECURSIVE "}, {
        TK_RELEASE, "TK_RELEASE   "}, {
        TK_REPLACE, "TK_REPLACE   "}, {
        TK_RESTRICT, "TK_RESTRICT  "}, {
        TK_ROW, "TK_ROW       "}, {
        TK_TRANSACTION, "TK_TRANSACTION"}, {
        TK_START, "TK_START"}, {
        TK_COMMIT, "TK_COMMIT"}, {
        TK_ROLLBACK, "TK_ROLLBACK  "}, {
        TK_SAVEPOINT, "TK_SAVEPOINT "}, {
        TK_TRIGGER, "TK_TRIGGER   "}, {
        TK_VACUUM, "TK_VACUUM    "}, {
        TK_VIEW, "TK_VIEW      "}, {
        TK_VIRTUAL, "TK_VIRTUAL   "}, {
        TK_WITH, "TK_WITH      "}, {
        TK_RENAME, "TK_RENAME    "}, {
        TK_ANY, "TK_ANY       "}, {
        TK_STRING, "TK_STRING    "}, {
        TK_JOIN_KW, "TK_JOIN_KW   "}, {
        TK_INTEGER, "TK_INTEGER   "}, {
        TK_FLOAT, "TK_FLOAT     "}, {
        TK_CONSTRAINT, "TK_CONSTRAINT"}, {
        TK_DEFAULT, "TK_DEFAULT   "}, {
        TK_CHECK, "TK_CHECK     "}, {
        TK_AUTO_INCREMENT, "TK_AUTO_INCREMENT  "}, {
        TK_PRIMARY, "TK_PRIMARY   "}, {
        TK_UNIQUE, "TK_UNIQUE    "}, {
        TK_FOREIGN, "TK_FOREIGN   "}, {
        TK_DROP, "TK_DROP      "}, {
        TK_SELECT, "TK_SELECT    "}, {
        TK_VALUES, "TK_VALUES    "}, {
        TK_DISTINCT, "TK_DISTINCT  "}, {
        TK_DOT, "TK_DOT       "}, {
        TK_FROM, "TK_FROM      "}, {
        TK_JOIN, "TK_JOIN      "}, {
        TK_ON, "TK_ON        "}, {
        TK_USING, "TK_USING     "}, {
        TK_ORDER, "TK_ORDER     "}, {
        TK_GROUP, "TK_GROUP     "}, {
        TK_HAVING, "TK_HAVING    "}, {
        TK_LIMIT, "TK_LIMIT     "}, {
        TK_DELETE, "TK_DELETE    "}, {
        TK_WHERE, "TK_WHERE     "}, {
        TK_UPDATE, "TK_UPDATE    "}, {
        TK_SET, "TK_SET       "}, {
        TK_INTO, "TK_INTO      "}, {
        TK_INSERT, "TK_INSERT    "}, {
        TK_NULL, "TK_NULL      "}, {
        TK_BLOB, "TK_BLOB      "}, {
        TK_CASE, "TK_CASE      "}, {
        TK_WHEN, "TK_WHEN      "}, {
        TK_THEN, "TK_THEN      "}, {
        TK_ELSE, "TK_ELSE      "}
    };
    int len = sizeof(token_list) / sizeof(struct token_list_s);
    int i;
    for (i = 0; i < len; ++i) {
        if (token_list[i].code == op)
            return token_list[i].name;
    }
    return "NotFound";
}

/* print the syntax tree, used for debug */
void
sql_expr_print(sql_expr_t *p, int depth)
{
    if (p) {
        int INDENT = 2;
        static const char *spaces = "                                               ";
        printf("%.*s", depth * INDENT, spaces);
        printf("%s ", sql_get_token_name(p->op));
        if (p->op != TK_INTEGER && p->token_text)
            printf("%s\n", p->token_text);
        else if (p->op == TK_INTEGER)
            printf("%" PRIu64 "\n", p->num_value);
        else
            printf("\n");

        if (p->left)
            sql_expr_print(p->left, depth + 1);
        else if (p->right)
            printf("%.*s[nul]\n", (depth + 1) * INDENT, spaces);
        else;
        if (p->right)
            sql_expr_print(p->right, depth + 1);
        else if (p->left)
            printf("%.*s[nul]\n", (depth + 1) * INDENT, spaces);
        else;
    }
}

void
sql_statement_free(void *clause, sql_stmt_type_t stmt_type)
{
    if (!clause)
        return;
    switch (stmt_type) {
    case STMT_SELECT:
        sql_select_free(clause);
        break;
    case STMT_UPDATE:
        sql_update_free(clause);
        break;
    case STMT_INSERT:
        sql_insert_free(clause);
        break;
    case STMT_DELETE:
        sql_delete_free(clause);
        break;
    case STMT_SHOW:
        break;
    case STMT_SHOW_COLUMNS:
    case STMT_SHOW_CREATE:
    case STMT_EXPLAIN_TABLE:
        sql_src_list_free(clause);
        break;
    case STMT_SET:
        sql_expr_list_free(clause);
        break;
    case STMT_SET_TRANSACTION:
    case STMT_SET_NAMES:
    case STMT_USE:
    case STMT_SAVEPOINT:
        g_free(clause);
        break;
    case STMT_START:
    case STMT_COMMIT:
    case STMT_ROLLBACK:
    case STMT_COMMON_DDL:
        break;
    case STMT_DROP_DATABASE:
        sql_drop_database_free(clause); 
        break;
    case STMT_CALL:
        sql_expr_free(clause);
        break;
    default:
        g_warning(G_STRLOC ":not supported clause type, caution mem leak");
    }
}

gboolean
sql_is_quoted_string(const char *s)
{
    if (!s)
        return FALSE;
    int len = strlen(s);
    if (len < 2)
        return FALSE;
    if (s[0] != s[len - 1])
        return FALSE;
    return s[0] == '\'' || s[0] == '"' || s[0] == '`';
}

int
sql_join_type(sql_token_t kw)
{
    static struct {
        char *name;
        uint8_t code;
    } _map[] = {
        {
        "INNER", JT_INNER}, {
        "CROSS", JT_CROSS}, {
        "NATURAL", JT_NATURAL}, {
        "LEFT", JT_LEFT}, {
        "RIGHT", JT_RIGHT}, {
    "OUTER", JT_OUTER},};
    int ret = JT_ERROR;
    char *kw_str = sql_token_dup(kw);
    int i = 0;
    while (i < 6) {
        if (strcasecmp(_map[i].name, kw_str) == 0) {
            ret = _map[i].code;
            break;
        }
        ++i;
    }
    g_free(kw_str);
    return ret;
}

/**
 * !! support only ID, INTGER and STRING
 */
gboolean
sql_expr_equals(const sql_expr_t *p1, const sql_expr_t *p2)
{
    if (p1 && p2 && p1->op == p2->op) {
        if (p1->op == TK_ID || p1->op == TK_STRING) {
            return strcmp(p1->token_text, p2->token_text) == 0;
        } else if (p1->op == TK_INTEGER) {
            return p1->num_value == p2->num_value;
        }
    }
    return FALSE;
}

void
sql_index_hint_free(sql_index_hint_t* p)
{
    if (p && p->names) {
        sql_id_list_free(p->names);
    }
    if (p)
        g_free(p);
}

sql_index_hint_t*
sql_index_hint_new()
{
    return g_new0(sql_index_hint_t, 1);    
}

void
sql_table_reference_free(sql_table_reference_t* p)
{
    if (!p) {
        return;
    }
    if (p->table_list) {
        sql_src_list_free(p->table_list);
    }
    if (p->index_hint) {
        sql_index_hint_free(p->index_hint);
    }
    g_free(p);
}

sql_table_reference_t *
sql_table_reference_new()
{
    return g_new0(sql_table_reference_t, 1);
}
