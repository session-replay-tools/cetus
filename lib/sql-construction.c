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

#include "sql-construction.h"
#include <inttypes.h>

#include "myparser.y.h"

static void sql_expr_traverse(GString *s, sql_expr_t *expr);

static void
string_append_quoted(GString *s, const char *p, char quote)
{
    g_string_append_c(s, quote);
    for (; *p != '\0'; ++p) {
        if (*p == quote) {      /* escape quote inside string */
            g_string_append_c(s, '\\');
        }
        g_string_append_c(s, *p);
    }
    g_string_append_c(s, quote);
}

/* sql construction */
static void
sql_append_expr(GString *s, sql_expr_t *p)
{
    size_t len;
    switch (p->op) {
    case TK_ID:
        len = s->len;
        if (len > 0) {
            if (s->str[len - 1] != ' ') {
                g_string_append(s, " ");
            }
        }
        g_string_append(s, p->token_text);
        break;
    case TK_EQ:
        g_string_append(s, "=");
        break;
    case TK_LT:
        g_string_append(s, "<");
        break;
    case TK_GT:
        g_string_append(s, ">");
        break;
    case TK_LE:
        g_string_append(s, "<=");
        break;
    case TK_GE:
        g_string_append(s, ">=");
        break;
    case TK_NE:
        g_string_append(s, "<>");
        break;
    case TK_AND:
        g_string_append(s, " AND ");
        break;
    case TK_OR:
        g_string_append(s, " OR ");
        break;
    case TK_DOT:
        g_string_append(s, " ");
        g_string_append(s, p->left->token_text);
        g_string_append(s, ".");
        g_string_append(s, p->right->token_text);
        break;
    case TK_UPLUS:
    case TK_UMINUS:
    case TK_INTEGER:{
        char valstr[32] = { 0 };
        char *pstr = valstr;
        if (p->op == TK_UMINUS) {
            *pstr = '-';
            ++pstr;
        }
        sprintf(pstr, "%" PRIu64, p->num_value);
        g_string_append(s, valstr);
        break;
    }
    case TK_STRING:
        if (sql_is_quoted_string(p->token_text)) {  /* TODO: dequote all */
            g_string_append(s, " ");
            g_string_append(s, p->token_text);
        } else {
            g_string_append_c(s, ' ');
            string_append_quoted(s, p->token_text, '\'');
        }
        break;
    case TK_FUNCTION:{
        g_string_append(s, " ");
        g_string_append(s, p->token_text);
        g_string_append(s, "(");
        sql_expr_list_t *args = p->list;
        if (args) {
            int i = 0;
            for (i = 0; i < args->len; ++i) {
                sql_expr_t *arg = g_ptr_array_index(args, i);
                sql_expr_traverse(s, arg);
                if (i < args->len - 1) {
                    g_string_append(s, ",");
                }
            }
        }
        g_string_append(s, ")");
        break;
    }
    case TK_BETWEEN:{
        sql_append_expr(s, p->left);
        g_string_append(s, " BETWEEN ");
        sql_expr_list_t *args = p->list;
        if (args && args->len == 2) {
            sql_expr_t *low = g_ptr_array_index(args, 0);
            sql_expr_t *high = g_ptr_array_index(args, 1);
            sql_expr_traverse(s, low);
            g_string_append(s, " AND ");
            sql_expr_traverse(s, high);
        }
        break;
    }
    case TK_IN:{
        sql_append_expr(s, p->left);
        g_string_append(s, " IN (");
        if (p->list) {
            sql_expr_list_t *args = p->list;
            int i;
            for (i = 0; args && i < args->len; ++i) {
                sql_expr_t *arg = g_ptr_array_index(args, i);
                sql_append_expr(s, arg);
                if (i < args->len - 1) {
                    g_string_append_c(s, ',');
                }
            }
        } else if (p->select) {
            GString *sel = sql_construct_select(p->select, 0);
            if (sel) {
                g_string_append(s, sel->str);
                g_string_free(sel, TRUE);
            }
        }
        g_string_append(s, ")");
        break;
    }
    case TK_EXISTS:{
        g_string_append(s, " EXISTS (");
        GString *sel = sql_construct_select(p->select, 0);
        if (sel) {
            g_string_append(s, sel->str);
            g_string_free(sel, TRUE);
        }
        g_string_append(s, ")");
        break;
    }
    case TK_LIKE_KW:
        if (p->list) {
            sql_expr_list_t *args = p->list;
            if (args->len > 0) {
                sql_expr_t *arg = g_ptr_array_index(args, 0);
                sql_expr_traverse(s, arg);
            }
            g_string_append(s, " LIKE ");
            if (args->len > 1) {
                sql_expr_t *arg = g_ptr_array_index(args, 1);
                sql_append_expr(s, arg);
            }
            if (args->len > 2) {
                sql_expr_t *arg = g_ptr_array_index(args, 2);
                g_string_append(s, " ESCAPE ");
                sql_append_expr(s, arg);
            }
        }
        break;
    case TK_NOT:
        g_string_append(s, " NOT(");
        sql_expr_traverse(s, p->left);
        g_string_append_c(s, ')');
        break;
    case TK_SELECT:{           /* subselect as an expression */
        g_string_append(s, "(");
        GString *sel = sql_construct_select(p->select, 0);
        if (sel) {
            g_string_append(s, sel->str);
            g_string_free(sel, TRUE);
        }
        g_string_append(s, ")");
        break;
    }
    case TK_IS:
        g_string_append(s, " IS ");
        break;
    case TK_ISNOT:
        g_string_append(s, " IS NOT ");
        break;
    case TK_PLUS:
        g_string_append_c(s, '+');
        break;
    case TK_MINUS:
        g_string_append_c(s, '-');
        break;
    default:
        g_string_append(s, " ");
        g_string_append(s, p->token_text);
    }
}

/* these expr will be processed as leaf node */
static gboolean
sql_expr_is_leaf_node(sql_expr_t *expr)
{
    return (expr->op == TK_DOT && expr->left && expr->right)    /* db.table */
        ||(expr->op == TK_UMINUS && expr->left) /* -3 */
        ||(expr->op == TK_UPLUS && expr->left)  /* +4 */
        ||(expr->op == TK_BETWEEN)
        || (expr->op == TK_NOT)
        || (expr->op == TK_EXISTS)
        || (expr->op == TK_IN);
}

static void
sql_expr_traverse(GString *s, sql_expr_t *expr)
{
    if (!expr)
        return;
    if (sql_expr_is_leaf_node(expr)) {
        sql_append_expr(s, expr);
        return;
    }
    if (expr->left) {
        gboolean parenth = (expr->op == TK_AND && expr->left->op == TK_OR);
        if (parenth)
            g_string_append_c(s, '(');

        sql_expr_traverse(s, expr->left);
        if (parenth)
            g_string_append_c(s, ')');
    }

    sql_append_expr(s, expr);

    if (expr->right) {
        gboolean parenth = (expr->op == TK_AND && expr->right->op == TK_OR);
        if (parenth)
            g_string_append_c(s, '(');

        sql_expr_traverse(s, expr->right);
        if (parenth)
            g_string_append_c(s, ')');
    }
}

static void
sql_construct_join(GString *s, int flag)
{
    if (flag == JT_INNER) {
        g_string_append(s, " JOIN ");
        return;
    }
    if (flag & JT_INNER) {
        g_string_append(s, "INNER ");
    }
    if (flag & JT_CROSS) {
        g_string_append(s, "CROSS ");
    }
    if (flag & JT_NATURAL) {
        g_string_append(s, "NATURAL ");
    }
    if (flag & JT_LEFT) {
        g_string_append(s, "LEFT ");
    }
    if (flag & JT_RIGHT) {
        g_string_append(s, "RIGHT ");
    }
    if (flag & JT_OUTER) {
        g_string_append(s, "OUTER ");
    }
    g_string_append(s, "JOIN ");
}

static inline void
append_sql_expr(GString *s, sql_expr_t *expr)
{
    g_string_append_len(s, expr->start, expr->end - expr->start);
}

GString *
sql_construct_select(sql_select_t *select, int explain)
{
    int i = 0;
    GString *s = g_string_sized_new(512);
    if (explain) {
        g_string_append(s, "EXPLAIN ");
    }
    g_string_append(s, "SELECT ");
    if (select->columns) {
        if (select->flags & SF_DISTINCT) {
            g_string_append(s, "DISTINCT ");
        }
        for (i = 0; i < select->columns->len; ++i) {
            sql_expr_t *expr = g_ptr_array_index(select->columns, i);
            append_sql_expr(s, expr);
            if (expr->alias) {
                g_string_append(s, " AS ");
                g_string_append(s, expr->alias);
            }
            if (i != select->columns->len - 1)
                g_string_append(s, ",");
        }
    }
    if (select->from_src) {
        g_string_append(s, " FROM ");
        for (i = 0; i < select->from_src->len; ++i) {
            sql_src_item_t *src = g_ptr_array_index(select->from_src, i);
            if (src->table_name) {
                if (src->dbname) {
                    g_string_append(s, src->dbname);
                    g_string_append(s, ".");
                }
                if (src->groups && src->groups->len > 0) {
                    g_string_append(s, src->table_name);
                    int index = src->group_index++;
                    index = index % src->groups->len;
                    GString *group_name = src->groups->pdata[index];
                    g_string_append(s, "_");
                    g_string_append(s, group_name->str);
                } else {
                    g_string_append(s, src->table_name);
                }
                g_string_append(s, " ");
            } else if (src->select) {
                GString *sub = sql_construct_select(src->select, 0);
                g_string_append_c(s, '(');
                g_string_append_len(s, sub->str, sub->len);
                g_string_append_c(s, ')');
                g_string_free(sub, TRUE);
            }

            if (src->table_alias) {
                g_string_append(s, " AS ");
                g_string_append(s, src->table_alias);
                g_string_append(s, " ");
            }

            if (src->index_hint) {
                switch (src->index_hint->type) {
                    case IH_USE_INDEX: {
                        g_string_append(s, " USE INDEX ( ");
                        break;
                    }
                    case IH_USE_KEY: {
                        g_string_append(s, " USE KEY ( ");
                        break;
                    }
                    case IH_IGNORE_INDEX: {
                        g_string_append(s, " IGNORE INDEX ( ");
                        break;
                    }
                    case IH_IGNORE_KEY: {
                        g_string_append(s, " IGNORE KEY ( ");
                        break;
                    }
                    case IH_FORCE_INDEX: {
                        g_string_append(s, " FORCE INDEX ( ");
                        break;
                    }
                    case IH_FORCE_KEY: {
                        g_string_append(s, " FORCE KEY ( ");
                        break;
                    }
                }
                gint len = src->index_hint->names->len;
                gint i = 0;
                for(i=0; i<len; i++) {
                    g_string_append(s, g_ptr_array_index(src->index_hint->names, i));
                    if(i != (len -1)) {
                        g_string_append(s, " , ");
                    }
                }
                g_string_append(s, " ) ");
            }

            if (src->on_clause) {
                g_string_append(s, " ON ");
                append_sql_expr(s, src->on_clause);
                g_string_append_c(s, ' ');
            }
            if (src->jointype != 0) {
                sql_construct_join(s, src->jointype);
            }
        }
    }
    if (select->where_clause) {
        g_string_append(s, " WHERE ");
        sql_expr_t *expr = select->where_clause;
        if (expr->modify_flag) {
            sql_expr_traverse(s, expr);
        } else {
            append_sql_expr(s, expr);
        }
    }
    if (select->groupby_clause) {
        sql_expr_list_t *groupby = select->groupby_clause;
        g_string_append(s, " GROUP BY ");
        for (i = 0; i < groupby->len; ++i) {
            sql_expr_t *expr = g_ptr_array_index(groupby, i);
            append_sql_expr(s, expr);
            if (i < groupby->len - 1) {
                g_string_append(s, ",");
            }
        }
    }
    if (select->having_clause) {
        g_string_append(s, " HAVING ");
        append_sql_expr(s, select->having_clause);
    }
    if (select->orderby_clause) {
        sql_expr_list_t *orderby = select->orderby_clause;
        g_string_append(s, " ORDER BY ");
        for (i = 0; i < orderby->len; ++i) {
            sql_column_t *col = g_ptr_array_index(orderby, i);

            /* this might be duped from column, @see sql_modify_orderby
               not using append_sql_expr() to reserve possible alias */
            sql_expr_t *expr = col->expr;
            sql_expr_traverse(s, expr);
            if (col->sort_order && col->sort_order == SQL_SO_DESC) {
                g_string_append(s, " DESC ");
            }
            if (i < orderby->len - 1) {
                g_string_append(s, ",");
            }
        }
    }

    /* don't use append_sql_expr() for LIMIT/OFFSET, the expression has changed */
    if (select->limit) {
        g_string_append(s, " LIMIT ");
        sql_expr_traverse(s, select->limit);
    }
    if (select->offset) {
        g_string_append(s, " OFFSET ");
        sql_expr_traverse(s, select->offset);
    }
    if (select->lock_read) {
        g_string_append(s, " FOR UPDATE");
    }

    if (select->prior) {
        sql_select_t *sub_select = select->prior;
        GString *union_sql = g_string_new(NULL);
        while (sub_select) {
            GString *sql = sql_construct_select(sub_select, 0);
            g_string_append(union_sql, sql->str);
            g_string_append(union_sql, " UNION ");
            g_string_free(sql, TRUE);
            sub_select = sub_select->prior;
        }

        g_string_append(union_sql, s->str);
        g_string_free(s, TRUE);
        s = union_sql;
    }

    return s;
}

/* format as " expr1,expr2,expr3 "*/
void
sql_append_expr_list(GString *s, sql_expr_list_t *exprlist)
{
    int i = 0;
    for (i = 0; i < exprlist->len; ++i) {
        sql_expr_t *expr = g_ptr_array_index(exprlist, i);
        append_sql_expr(s, expr);
        if (i != exprlist->len - 1) {
            g_string_append_c(s, ',');
        }
    }
}

void
sql_construct_insert(int is_partition_mode, GString *s, sql_insert_t *p, GString *group)
{
    g_string_append(s, "INSERT INTO ");
    if (p->table && p->table->len > 0) {
        sql_src_item_t *src = g_ptr_array_index(p->table, 0);
        if (src->dbname) {
            g_string_append(s, src->dbname);
            g_string_append_c(s, '.');
        }

        g_string_append(s, src->table_name);
        if (is_partition_mode) {
            if (group) {
                g_string_append(s, "_");
                g_string_append(s, group->str);
            } else if (src->groups && src->groups->len > 0) {
                int index = src->group_index++;
                index = index % src->groups->len;
                GString *group_name = src->groups->pdata[index];
                g_string_append(s, "_");
                g_string_append(s, group_name->str);
            }
        }
        g_string_append_c(s, ' ');
    }
    if (p->columns && p->columns->len > 0) {
        g_string_append_len(s, p->columns_start,
                            p->columns_end - p->columns_start);
        g_string_append_c(s, ' ');
    }
    if (p->sel_val) {
        if (p->sel_val->from_src) {
            /* select as values */
            GString *select = sql_construct_select(p->sel_val, 0);
            g_string_append(s, select->str);
            g_string_free(select, TRUE);
        } else {
            /* expression values */
            g_string_append(s, "VALUES");
            sql_select_t *values = p->sel_val;
            for (; values; values = values->prior) {
                sql_expr_list_t *cols = values->columns;
                g_string_append_c(s, '(');
                sql_append_expr_list(s, cols);
                g_string_append(s, "),");
            }
            s->str[s->len - 1] = ' ';   /* no comma at the end */
        }
    }
    if (p->update_list) {
        g_string_append(s, " ON DUPLICATE KEY UPDATE ");
        sql_append_expr_list(s, p->update_list);
    }
}
    
GString *
sql_construct_update(sql_update_t *p)
{
    GString *s = g_string_sized_new(512);

    g_string_append(s, "UPDATE ");

    sql_src_list_t *tables = p->table_reference->table_list;
    sql_src_item_t *src = g_ptr_array_index(tables, 0);

    if (src->dbname) {
        g_string_append(s, src->dbname);
        g_string_append_c(s, '.');
    }
    if (src->groups && src->groups->len > 0) {
        g_string_append(s, src->table_name);
        int index = src->group_index++;
        index = index % src->groups->len;
        GString *group_name = src->groups->pdata[index];
        g_string_append(s, "_");
        g_string_append(s, group_name->str);
    } else {
        g_string_append(s, src->table_name);
    }

    g_string_append(s, " SET ");

    int i;
    for (i = 0; p->set_list && i < p->set_list->len; ++i) {
        sql_expr_t *expr = g_ptr_array_index(p->set_list, i);
        append_sql_expr(s, expr);
        if ((i + 1) < p->set_list->len) {
            g_string_append(s, ",");
        }
    }

    if (p->where_clause) {
        g_string_append(s, " WHERE ");
        append_sql_expr(s, p->where_clause);
    }

    return s;
}

GString *
sql_construct_delete(sql_delete_t *p)
{
    GString *s = g_string_new(NULL);

    g_string_append(s, "DELETE FROM ");

    sql_src_item_t *src = g_ptr_array_index(p->from_src, 0);

    if (src->dbname) {
        g_string_append(s, src->dbname);
        g_string_append_c(s, '.');
    }
    if (src->groups && src->groups->len > 0) {
        g_string_append(s, src->table_name);
        int index = src->group_index++;
        index = index % src->groups->len;
        GString *group_name = src->groups->pdata[index];
        g_string_append(s, "_");
        g_string_append(s, group_name->str);
    } else {
        g_string_append(s, src->table_name);
    }

    if (p->where_clause) {
        g_string_append(s, " WHERE ");
        append_sql_expr(s, p->where_clause);
    }

    return s;
}

