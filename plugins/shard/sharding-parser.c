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

#include "sharding-parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>

#include "glib-ext.h"
#include "sql-expression.h"
#include "sql-construction.h"
#include "sql-property.h"
#include "sharding-config.h"

static gboolean
is_compare_op(int op)
{
    return op == TK_EQ || op == TK_NE || op == TK_LT || op == TK_LE || op == TK_GT || op == TK_GE;
}

static gboolean
is_logical_op(int op)
{
    return op == TK_AND || op == TK_OR || op == TK_NOT;
}

static gboolean
is_arithmetic_op(int op)
{
    return op == TK_PLUS || op == TK_MINUS || op == TK_STAR || op == TK_SLASH || op == TK_REM;
}

static gboolean
expr_is_sharding_key(sql_expr_t *p, const sql_src_item_t *tb, const char *key)
{
    if (p->op == TK_DOT) {      /* table.colmun or db.table.col */
        assert(tb->table_name);
        assert(p->left && p->right);
        if (p->right->op == TK_DOT) {   /* db.table.col */
            if (tb->dbname && strcasecmp(p->left->token_text, tb->dbname) == 0
                && sql_expr_is_dotted_name(p->right, tb->table_name, key))
                return TRUE;
        } else if (strcasecmp(p->right->token_text, key) == 0) {
            char *prefix = p->left->token_text;
            if (strcasecmp(prefix, tb->table_name) == 0) {
                return TRUE;
            }
            if (tb->table_alias && strcmp(prefix, tb->table_alias) == 0) {
                return TRUE;
            }
        }
    } else if (p->op == TK_ID) {
        if (strcasecmp(p->token_text, key) == 0)
            return TRUE;
    }
    return FALSE;
}

static gboolean
expr_is_compound_value(sql_expr_t *p)
{
    if (!p)
        return FALSE;
    return p->op == TK_FUNCTION || p->op == TK_SELECT ||    /* subquery */
        p->op == TK_ID || p->op == TK_DOT ||    /* col, tbl.col */
        is_arithmetic_op(p->op);    /* 1+1, 3%2, 4 * 5, etc */
}

static unsigned int
supplemental_hash(unsigned int value)
{
    unsigned int tmp1 = value >> 20;
    unsigned int tmp2 = value >> 12;
    unsigned int tmp3 = tmp1 ^ tmp2;
    unsigned int h = value ^ tmp3;
    tmp1 = h >> 7;
    tmp2 = h >> 4;
    tmp3 = tmp1 ^ tmp2;
    h = h ^ tmp3;
    return h;
}

static unsigned int
cetus_str_hash(const unsigned char *key)
{
    int len = strlen((const char *)key);
    unsigned int hashcode_head = 0;
    int i = 0;
    int max = 8;

    if (max > len) {
        max = len;
    }
    for (; i < max; i++) {
        hashcode_head <<= 4;
        hashcode_head += key[i];
    }
    if (len > max) {
        i = len - 8;
        unsigned int hashcode_tail = 0;
        for (; i < len; i++) {
            hashcode_tail <<= 4;
            hashcode_tail += key[i];
        }
        return supplemental_hash(hashcode_head ^ hashcode_tail);
    } else {
        return supplemental_hash(hashcode_head);
    }
}

static void
prepare_for_sql_modify_limit(sql_select_t *select, guint64 *orig_limit, guint64 *orig_offset)
{
    if (select->offset && select->offset->op == TK_INTEGER) {
        *orig_offset = select->offset->num_value;
        select->offset->num_value = 0;
    }
    if (select->limit->op == TK_INTEGER) {
        *orig_limit = select->limit->num_value;
        select->limit->num_value += *orig_offset;
    }
}

static void
prepare_for_sql_modify_orderby(sql_select_t *select)
{
    sql_expr_list_t *columns = select->columns;
    sql_column_list_t *orderby = NULL;
    if (select->orderby_clause) {
        orderby = select->orderby_clause;
    }
    int i;
    for (i = 0; i < columns->len; ++i) {
        sql_expr_t *mcol = g_ptr_array_index(columns, i);
        if (!(mcol->flags & EP_ORDER_BY)) {
            if (mcol->op != TK_FUNCTION) {
                sql_column_t *ordcol = sql_column_new();
                ordcol->expr = sql_expr_dup(mcol);
                if (ordcol->expr->alias) {
                    ordcol->expr->op = TK_ID;
                    /* borrow text from alias */
                    ordcol->expr->token_text = ordcol->expr->alias;
                }
                orderby = sql_column_list_append(orderby, ordcol);
            }
        }
    }
    select->orderby_clause = orderby;
}

static GString *
modify_select(sql_context_t *context, having_condition_t *hav_condi, int is_groupby_need_reconstruct, int groups)
{
    sql_select_t *select = context->sql_statement;

    sql_expr_t *having = select->having_clause;
    if (having) {
        if (is_compare_op(having->op)) {
            sql_expr_t* hav_name = having->left;
            hav_condi->column_index =
                sql_expr_list_find_exact_aggregate(select->columns,
                        hav_name->start,
                        hav_name->end - hav_name->start);
            hav_condi->rel_type = having->op;
            sql_expr_t *val = having->right;
            if (hav_condi->condition_value) {
                g_free(hav_condi->condition_value);
                hav_condi->condition_value = NULL;
            }

            char *val_str = g_strndup(val->start, val->end - val->start);
            sql_string_dequote(val_str);
            hav_condi->condition_value = val_str;
            if (val->op == TK_UMINUS || val->op == TK_UPLUS) {
                hav_condi->data_type = TK_INTEGER;
            } else {
                hav_condi->data_type = val->op;
            }
        }
        select->having_clause = NULL;   /* temporarily remove HAVING */
    }

    gboolean need_reconstruct = FALSE;
    guint64 orig_offset = 0;
    guint64 orig_limit = 0;

    /* (LIMIT a, b) ==> (LIMIT 0, a+b) */
    if (groups > 1 && select->offset && select->offset->num_value > 0 && select->limit) {
        prepare_for_sql_modify_limit(select, &orig_limit, &orig_offset);
        need_reconstruct = TRUE;
    }

    if (is_groupby_need_reconstruct && select->groupby_clause != NULL && select->orderby_clause == NULL) {
        select->flags |= SF_REWRITE_ORDERBY;
    }

    if (select->flags & SF_REWRITE_ORDERBY) {
        prepare_for_sql_modify_orderby(select);
        select->flags = select->flags ^ SF_REWRITE_ORDERBY;
        need_reconstruct = TRUE;
    }

    GString *new_sql = NULL;
    if (having) {
        need_reconstruct = TRUE;
    } else if (context->sql_needs_reconstruct) {
        need_reconstruct = TRUE;
    }

    if (need_reconstruct) {
        new_sql = sql_construct_select(select, context->explain == TK_EXPLAIN ? 1:0);
        g_string_append_c(new_sql, ';');
        if (orig_offset != 0 || orig_limit != 0) {
            select->limit->num_value = orig_limit;
            select->offset->num_value = orig_offset;
        }
    }

    if (new_sql && select->prior) {
        sql_select_t *sub_select = select->prior;
        GString *union_sql = g_string_new(NULL);
        while (sub_select) {
            GString *sql = sql_construct_select(sub_select, 0);
            g_string_append(union_sql, sql->str);
            g_string_append(union_sql, " UNION ");
            g_string_free(sql, TRUE);
            sub_select = sub_select->prior;
        }
        g_string_append(union_sql, new_sql->str);
        g_string_free(new_sql, TRUE);
        new_sql = union_sql;
    }

    select->having_clause = having; /* get HAVING back */

    return new_sql;
}

GString *
sharding_modify_sql(sql_context_t *context, having_condition_t *hav_condi, int is_groupby_need_reconstruct, int partition_mode, int groups)
{
    if (!partition_mode) {
        if (context->stmt_type == STMT_SELECT) {
            if (context->sql_statement) {
                return modify_select(context, hav_condi, is_groupby_need_reconstruct, groups);
            }
        }
    } else {
        switch (context->stmt_type) {
            case STMT_SELECT:
                if (context->sql_statement) {
                    return modify_select(context, hav_condi, is_groupby_need_reconstruct, groups);
                }
                break;
            case STMT_UPDATE:
                if (context->sql_statement) {
                    return sql_construct_update(context->sql_statement);
                }
                break;
            case STMT_DELETE:
                if (context->sql_statement) {
                    return sql_construct_delete(context->sql_statement);
                }
                break;
            case STMT_INSERT: {
                if (context->sql_statement) {
                    GString *s = g_string_sized_new(512);
                    sql_construct_insert(partition_mode, s, context->sql_statement, NULL);
                    return s;
                }
                break;
            }
            default:
                break;
        }
    }

    return NULL;
}

static gboolean
sql_select_contains_sharding_table(sql_select_t *select, char **current_db /* in_out */ , char **table /* out */ )
{
    char *db = *current_db;
    while (select) {
        sql_src_list_t *sources = select->from_src;
        int i;
        for (i = 0; sources && i < sources->len; ++i) {
            sql_src_item_t *src = g_ptr_array_index(sources, i);
            if (src->dbname) {
                db = src->dbname;
            }
            if (src->table_name) {
                if (shard_conf_is_shard_table(db, src->table_name)) {
                    *current_db = db;
                    *table = src->table_name;
                    return TRUE;
                }
            }
        }
        select = select->prior;
    }
    return FALSE;
}

struct condition_t {
    int op;
    union {
        gint64 num;
        const char *str;
    } v;
};

/**
 * ! we don't handle negative `b`
 */
static int32_t modulo(int64_t a, int32_t b)
{
    int32_t r = a % b;
    return r < 0 ? r + b : r;
}

/**
 * check if the group satisfies an inequation
 *   suppose condition is "Greater Than 42", (op = TK_GT, v.num = 42)
 *   if exists X -> (low, high] that satifies X > 42, return true
 *   if it doesn't exist such an X, return false;
 */
static gboolean
partition_satisfies(sharding_partition_t *partition, struct condition_t cond)
{
    /* partition value -> (low, high] */
    if (partition->method == SHARD_METHOD_HASH) {
        int64_t hash_value = (partition->key_type == SHARD_DATA_TYPE_STR)
            ? cetus_str_hash((const unsigned char *)cond.v.str) : cond.v.num;

        int32_t hash_mod = modulo(hash_value, partition->hash_count);

        if (cond.op == TK_EQ) {
            return sharding_partition_contain_hash(partition, hash_mod);
        } else {
            return TRUE;
        }
    }
    /* vvv SHARD_METHOD_RANGE vvv */
    if (partition->key_type == SHARD_DATA_TYPE_STR) {
        const char *low = partition->low_value;
        const char *high = partition->value;    // high is NULL means unlimited
        const char *val = cond.v.str;
        switch (cond.op) {
        case TK_EQ:
            return (low == NULL || strcmp(val, low) > 0) && (high == NULL || strcmp(val, high) <= 0);
        case TK_GT:
            return high == NULL || strcmp(val, high) < 0;
        case TK_LT:
            return low == NULL || strcmp(val, low) > 0;
        case TK_GE:
            return high == NULL || strcmp(val, high) <= 0;
        case TK_LE:
            return low == NULL || strcmp(val, low) > 0;
        case TK_NE:
            return TRUE;
        default:
            g_warning(G_STRLOC ":error condition");
        }
    } else {                    /* int and datetime */
        int64_t low = (int64_t) partition->low_value;
        int64_t high = (int64_t) partition->value; /* high range OR hash value */
        int64_t val = cond.v.num;
        switch (cond.op) {
        case TK_EQ:
            return val > low && val <= high;
        case TK_GT:
            return val < high;
        case TK_LT:
            return val > low + 1;
        case TK_GE:
            return val <= high;
        case TK_LE:
            return val > low;
        case TK_NE:
            if (val == high && high == (low + 1)) {
                return FALSE;
            } else {
                return TRUE;
            }
        default:
            g_warning(G_STRLOC ":error condition");
            return FALSE;
        }
    }

    g_warning(G_STRLOC ":error reach here");
    return FALSE;
}

/* filter out those which not satisfy cond */
static void
partitions_filter(GPtrArray *partitions, struct condition_t cond)
{
    int i;
    for (i = 0; i < partitions->len; ++i) {
        sharding_partition_t *gp = g_ptr_array_index(partitions, i);
        if (!partition_satisfies(gp, cond)) {
            g_ptr_array_remove_index(partitions, i);
            --i;
        }
    }
}

/* collect those which satisfy cond */
static void
partitions_collect(GPtrArray *from_partitions, struct condition_t cond, GPtrArray *to_partitions)
{
    int i;
    for (i = 0; i < from_partitions->len; ++i) {
        sharding_partition_t *gp = g_ptr_array_index(from_partitions, i);
        if (partition_satisfies(gp, cond)) {
            g_ptr_array_add(to_partitions, gp);
        }
    }
}

/* get first group that satisfies cond */
sharding_partition_t *
partitions_get(GPtrArray *from_partitions, struct condition_t cond)
{
    int i;
    for (i = 0; i < from_partitions->len; ++i) {
        sharding_partition_t *gp = g_ptr_array_index(from_partitions, i);
        if (partition_satisfies(gp, cond)) {
            return gp;
        }
    }
    return NULL;
}

static void
partitions_merge(GPtrArray *partitions, GPtrArray *other)
{
    int i;
    for (i = 0; i < other->len; ++i) {
        sharding_partition_t *gp = g_ptr_array_index(other, i);
        g_ptr_array_add(partitions, gp);
        /* duplication check is performed later */
    }
}

static GPtrArray *
partitions_dup(GPtrArray *partitions)
{
    GPtrArray *dup = g_ptr_array_new();
    int i;
    for (i = 0; i < partitions->len; ++i) {
        sharding_partition_t *gp = g_ptr_array_index(partitions, i);
        g_ptr_array_add(dup, gp);
    }
    return dup;
}

/**
 * parse cond.v from a string
 */
static int
string_to_sharding_value(const char *str, int expected, struct condition_t *cond)
{
    assert(cond);
    if (expected == SHARD_DATA_TYPE_STR) {
        cond->v.str = str;
    } else if (expected == SHARD_DATA_TYPE_DATE || expected == SHARD_DATA_TYPE_DATETIME) {
        gboolean ok;
        cond->v.num = chassis_epoch_from_string(str, &ok);
        if (!ok) {
            g_warning(G_STRLOC ":error datetime format: %s", str);
            return PARSE_ERROR;
        }
    } else if (expected == SHARD_DATA_TYPE_INT) {
        char *endptr = NULL;
        errno = 0;
        cond->v.num = g_ascii_strtoll(str, &endptr, 10);
        if (errno == ERANGE) {
            g_warning(G_STRLOC ":too large for int: %s", str);
            return PARSE_ERROR;
        }

        if (str == endptr || *endptr != '\0') {
            g_warning(G_STRLOC ":cannot get INT from string token: %s", str);
            return PARSE_ERROR;
        }
    } else {
        g_warning(G_STRLOC ":unexpected key string: %s", str);
        return PARSE_ERROR;
    }
    return PARSE_OK;
}

/**
 * parse cond.v from sql expression
 */
static int
expr_parse_sharding_value(sql_expr_t *p, int expected, struct condition_t *cond)
{
    assert(p);
    assert(cond);
    gint64 intval;
    if (sql_expr_get_int(p, &intval) && expected == SHARD_DATA_TYPE_INT) {
        cond->v.num = intval;
    } else if (p->op == TK_STRING) {
        return string_to_sharding_value(p->token_text, expected, cond);
    } else if (expr_is_compound_value(p)) {
        g_debug(G_STRLOC ":compound value, use all shard");
        return PARSE_UNRECOGNIZED;
    } else {
        g_warning(G_STRLOC ":unexpected token type: %d, %s", p->op, p->token_text);
        return PARSE_ERROR;
    }
    return PARSE_OK;
}

static int
partitions_filter_inequation_expr(GPtrArray *partitions, sql_expr_t *expr)
{
    g_assert(partitions);
    sharding_partition_t *gp;
    if (partitions->len > 0) {
        gp = g_ptr_array_index(partitions, 0);
    } else {
        return PARSE_OK;
    }

    struct condition_t cond = { 0 };
    cond.op = expr->op;
    int rc = expr_parse_sharding_value(expr->right, gp->key_type, &cond);
    if (rc != PARSE_OK)
        return rc;
    partitions_filter(partitions, cond);
    return PARSE_OK;
}

static int
partitions_filter_BETWEEN_expr(GPtrArray *partitions, sql_expr_t *expr)
{
    const sharding_partition_t *gp = NULL;
    g_assert(partitions);
    if (partitions->len > 0) {
        gp = g_ptr_array_index(partitions, 0);
    } else {
        return PARSE_OK;
    }

    struct condition_t cond = { 0 };
    sql_expr_list_t *btlist = expr->list;
    if (btlist && btlist->len == 2) {
        sql_expr_t *low = g_ptr_array_index(btlist, 0);
        sql_expr_t *high = g_ptr_array_index(btlist, 1);
        cond.op = TK_GE;
        int rc = expr_parse_sharding_value(low, gp->key_type, &cond);
        if (rc != PARSE_OK)
            return rc;
        partitions_filter(partitions, cond);

        cond.op = TK_LE;
        rc = expr_parse_sharding_value(high, gp->key_type, &cond);
        if (rc != PARSE_OK)
            return rc;
        partitions_filter(partitions, cond);
    }
    return PARSE_OK;
}

static int
partitions_collect_IN_expr(GPtrArray *partitions, sql_expr_t *expr)
{
    const sharding_partition_t *part = NULL;
    g_assert(partitions);
    if (partitions->len > 0) {
        part = g_ptr_array_index(partitions, 0);
    } else {
        return PARSE_OK;
    }

    struct condition_t cond = { 0 };
    if (expr->list && expr->list->len > 0) {
        GPtrArray *collected = g_ptr_array_new();

        sql_expr_list_t *args = expr->list;
        int i;
        for (i = 0; i < args->len; ++i) {
            sql_expr_t *arg = g_ptr_array_index(args, i);
            cond.op = TK_EQ;
            int rc = expr_parse_sharding_value(arg, part->key_type, &cond);
            if (rc != PARSE_OK) {
                g_ptr_array_free(collected, TRUE);
                return rc;
            }
            partitions_collect(partitions, cond, collected);
        }

        /* transfer collected to partitions as output */
        g_ptr_array_remove_range(partitions, 0, partitions->len);

        for (i = 0; i < collected->len; ++i) {
            gpointer *grp = g_ptr_array_index(collected, i);
            g_ptr_array_add(partitions, grp);
        }
        g_ptr_array_free(collected, TRUE);
        return PARSE_OK;

    } else {
        return PARSE_UNRECOGNIZED;
    }
}

static int
partitions_filter_expr(GPtrArray *partitions, sql_expr_t *expr)
{
    g_assert(partitions);
    if (partitions->len == 0) {
        return PARSE_OK;
    }

    GQueue *stack = g_queue_new();
    int rc = PARSE_OK;
    g_queue_push_head(stack, expr);

    /* for each condition in where clause */
    while (!g_queue_is_empty(stack)) {  /* TODO: NOT op is not supported */
        sql_expr_t *p = g_queue_pop_head(stack);
        if (p->op == TK_OR) {
            GPtrArray *new_partitions = partitions_dup(partitions);
            rc = partitions_filter_expr(partitions, p->left);
            if (rc != PARSE_OK) {
                g_ptr_array_free(new_partitions, TRUE);
                break;
            }
            rc = partitions_filter_expr(new_partitions, p->right);
            if (rc != PARSE_OK) {
                g_ptr_array_free(new_partitions, TRUE);
                break;
            }
            partitions_merge(partitions, new_partitions);
            g_ptr_array_free(new_partitions, TRUE);
            continue;
        }
        if (p->op == TK_AND) {
            if (p->right)
                g_queue_push_head(stack, p->right);
            if (p->left)
                g_queue_push_head(stack, p->left);
            continue;
        }
        if (p->flags & EP_SHARD_COND) { /* HACK: SHARD_COND under TK_NOT is skipped */
            if (is_compare_op(p->op) && !(p->flags & EP_JOIN_LINK)) {
                rc = partitions_filter_inequation_expr(partitions, p);
            } else if (p->op == TK_BETWEEN) {
                rc = partitions_filter_BETWEEN_expr(partitions, p);
            } else if (p->op == TK_IN) {
                rc = partitions_collect_IN_expr(partitions, p);
            }
        }
        if (rc != PARSE_OK) {
            break;
        }
    }
    g_queue_free(stack);
    return rc;
}

static int
flip_compare_op(int op)
{                               /* flip horizontally */
    switch (op) {
    case TK_LE:
        return TK_GE;
    case TK_GE:
        return TK_LE;
    case TK_LT:
        return TK_GT;
    case TK_GT:
        return TK_LT;
    default:
        return op;
    }
}

/**
 *  1.mark conditions that contains sharding key
 *  2.switch const field to right side ( 3<x --> x>3)
 *  //3.check if we can handle the value of sharding key
 * @param src the sharding table
 * @param field the sharding column name
 * @return number of occurance of sharding key
 */
static int
optimize_sharding_condition(sql_expr_t *where, const sql_src_item_t *src, const char *field)
{
    if (!where) {
        return FALSE;
    }
    GQueue *stack = g_queue_new();
    g_queue_push_head(stack, where);
    gboolean key_occur = 0;

    while (!g_queue_is_empty(stack)) {  /* TODO: NOT op is not supported */
        sql_expr_t *p = g_queue_pop_head(stack);
        if (is_logical_op(p->op)) {
            if (p->right)
                g_queue_push_head(stack, p->right);
            if (p->left)
                g_queue_push_head(stack, p->left);
            continue;
        }
        if (is_compare_op(p->op) && !(p->flags & EP_JOIN_LINK)) {
            /* the key might be on either side, unify as field = CONST */
            sql_expr_t *lhs = NULL, *rhs = NULL;
            if (p->left->op == TK_ID || p->left->op == TK_DOT) {
                lhs = p->left;
                rhs = p->right;
            } else if (p->right->op == TK_ID || p->right->op == TK_DOT) {
                lhs = p->right;
                rhs = p->left;
            } else {
                g_debug(G_STRLOC ":both sides aren't ID, neglect");
                continue;
            }
            if (expr_is_sharding_key(lhs, src, field)) {
                if (p->left != lhs) {
                    p->left = lhs;
                    p->right = rhs;
                    p->op = flip_compare_op(p->op);
                }
                p->flags |= EP_SHARD_COND;
                key_occur += 1;
            }
        } else if (p->op == TK_BETWEEN) {
            if (expr_is_sharding_key(p->left, src, field)) {
                p->flags |= EP_SHARD_COND;
                key_occur += 1;
            }
        } else if (p->op == TK_IN) {
            if (expr_is_sharding_key(p->left, src, field)) {
                p->flags |= EP_SHARD_COND;
                key_occur += 1;
            }
        }
    }
    g_queue_free(stack);
    return key_occur;
}

static void
partitions_get_group_names(GPtrArray *partitions, GPtrArray *groups)
{
    int i;
    for (i = 0; i < partitions->len; ++i) {
        sharding_partition_t *gp = g_ptr_array_index(partitions, i);
        g_ptr_array_add(groups, gp->group_name);
    }
}

/**
 * find out which 2 tables are connected by expression 'p', then
 * 1. record it in linkage array
 * 2. mark the expression 'p' as EP_JOIN_LINK;
 */
static void
find_linkage(sql_expr_t *p, const GPtrArray *tables, const GPtrArray *keys, gint8 * linkage)
{
    int i, j;
    int N = tables->len;
    for (i = 0; i < N; ++i) {
        sql_src_item_t *t1 = g_ptr_array_index(tables, i);
        const char *key1 = g_ptr_array_index(keys, i);

        for (j = 0; j < N; ++j) {
            if (i == j)
                continue;
            sql_src_item_t *t2 = g_ptr_array_index(tables, j);
            const char *key2 = g_ptr_array_index(keys, j);
            if (expr_is_sharding_key(p->left, t1, key1)
                && expr_is_sharding_key(p->right, t2, key2)) {
                p->flags |= EP_JOIN_LINK;
                /* only record linkage from lower index to higher index */
                i < j ? (linkage[i * N + j] = 1) : (linkage[j * N + i] = 1);
                return;
            }
        }
    }
}

/**
 * 1.check if all the tables belong to same VDB
 * 2.check if there exists tableA.key = tableB.key,
 * 3.and mark that equation with EP_JOIN_LINK
 */
static gboolean
join_on_sharding_key(char *default_db, GPtrArray *sharding_tables, sql_expr_t *where)
{
    int first_vdb_id;
    GPtrArray *sharding_keys = g_ptr_array_new();   /* array<const char *> */
    int i;
    for (i = 0; i < sharding_tables->len; ++i) {
        sql_src_item_t *src = g_ptr_array_index(sharding_tables, i);
        char *db = src->dbname ? src->dbname : default_db;
        sharding_table_t *tinfo = shard_conf_get_info(db, src->table_name);
        if (!tinfo) {
            g_ptr_array_free(sharding_keys, TRUE);
            g_warning(G_STRLOC "%s.%s is not sharding table", db, src->table_name);
            return FALSE;
        }
        if (i == 0) {
            first_vdb_id = tinfo->vdb_id;
        }
        if (tinfo->vdb_id != first_vdb_id) {
            g_ptr_array_free(sharding_keys, TRUE);
            return FALSE;
        }
        g_ptr_array_add(sharding_keys, tinfo->pkey->str);
    }

    /* 2-d array of table linkage */
    int N = sharding_tables->len;
    gint8 *linkage = g_new0(gint8, N * N);

    /* for each equation in WHERE clause */
    GQueue *stack = g_queue_new();
    if (where) {
        g_queue_push_head(stack, where);
    }
    /* for each equation in ON clause */
    for (i = 0; i < sharding_tables->len; ++i) {
        sql_src_item_t *src = g_ptr_array_index(sharding_tables, i);
        if (src->on_clause) {
            g_queue_push_head(stack, src->on_clause);
        }
    }

    while (!g_queue_is_empty(stack)) {  /* TODO: NOT op is not supported */
        sql_expr_t *p = g_queue_pop_head(stack);
        if (is_logical_op(p->op)) {
            if (p->right)
                g_queue_push_head(stack, p->right);
            if (p->left)
                g_queue_push_head(stack, p->left);
            continue;
        }
        if (p->op == TK_EQ && sql_expr_is_field_name(p->left)
            && sql_expr_is_field_name(p->right)) {
            find_linkage(p, sharding_tables, sharding_keys, linkage);
        }
    }
    g_queue_free(stack);

    int num_linkage = 0;
    for (i = 0; i < N * N; ++i)
        num_linkage += linkage[i];
    g_free(linkage);
    g_ptr_array_free(sharding_keys, TRUE);
    return num_linkage + 1 == sharding_tables->len;
}

static void sql_expr_find_subqueries(sql_expr_t *where, GList **queries)
{
    if (!where) {
        return;
    }
    GQueue *stack = g_queue_new();
    g_queue_push_head(stack, where);

    while (!g_queue_is_empty(stack)) {  /* TODO: NOT op is not supported */
        sql_expr_t *p = g_queue_pop_head(stack);
        if (is_logical_op(p->op)) {
            if (p->right)
                g_queue_push_head(stack, p->right);
            if (p->left)
                g_queue_push_head(stack, p->left);
            continue;
        } else if (p->op == TK_IN || p->op == TK_EXISTS) {
            if (p->select) {
                *queries = g_list_append(*queries, p->select);
            }
        }
    }
    g_queue_free(stack);
}

static void
sql_select_get_single_tables(sql_select_t *select, char *current_db, GList **single_tables /*out */ )
{
    char *db = current_db;
    while (select) {
        sql_src_list_t *sources = select->from_src;
        int i;
        for (i = 0; sources && i < sources->len; ++i) {
            sql_src_item_t *src = g_ptr_array_index(sources, i);
            if (src->dbname) {
                db = src->dbname;
            }
            if (src->table_name && shard_conf_is_single_table(0, db, src->table_name)) {
                *single_tables = g_list_append(*single_tables, src);
            }
        }
        select = select->prior;
    }
}

static void
dup_groups(sql_src_item_t *table, GPtrArray *groups)
{
    sql_src_item_t *src = table;
   if (src->table_name == NULL) {
       if (src->select) {
           sql_src_list_t *sources = src->select->from_src;
           int i; 
           for (i = 0; sources && i < sources->len; ++i) {
               src = g_ptr_array_index(sources, i);
               dup_groups(src, groups);
           }
           return;
       } else {
           return;
       }
   }

   src->groups = g_ptr_array_new();
    int i;
    for (i = 0; i < groups->len; i++) {
        GString *group = g_ptr_array_index(groups, i);
        g_ptr_array_add(table->groups, group);
    }
}

static void 
sql_select_check_and_set_shard_table(sql_expr_t *where, sql_select_t *select, char *current_db, GPtrArray *groups)
{
    char *db = current_db;
    while (select) {
        sql_src_list_t *sources = select->from_src;
        int i; 
        for (i = 0; sources && i < sources->len; ++i) {
            sql_src_item_t *src = g_ptr_array_index(sources, i);
            if (src->dbname) {
                db = src->dbname;
            }
            if (src->table_name && shard_conf_is_shard_table(db, src->table_name)) {
                dup_groups(src, groups);
                if (where) {
                    where->modify_flag = 1;
                }
            }
        }
        select = select->prior;
    }
}


static gboolean
sql_select_has_single_table(sql_select_t *select, char *current_db)
{
    char *db = current_db;
    while (select) {
        sql_src_list_t *sources = select->from_src;
        int i;
        for (i = 0; sources && i < sources->len; ++i) {
            sql_src_item_t *src = g_ptr_array_index(sources, i);
            if (src->dbname) {
                db = src->dbname;
            }
            if (src->table_name && shard_conf_is_single_table(0, db, src->table_name)) {
                return TRUE;
            }
        }
        select = select->prior;
    }
    return FALSE;
}


static void
dup_groups_for_partition(sql_expr_t *where, sql_src_list_t *sources, GList *subqueries,
        sql_select_t * prior, char *default_db, GPtrArray *groups)
{
    if (sources) {
        int i;
        for (i = 0; i < sources->len; ++i) {
            sql_src_item_t *src = g_ptr_array_index(sources, i);
            if (src->groups  == NULL || src->groups->len == 0) {
                dup_groups(src, groups);
            }
        }
    }
    
    GList *l;
    for (l = subqueries; l; l = l->next) {
        sql_select_check_and_set_shard_table(where, l->data, default_db, groups);
    }

    if (prior) {
        sql_select_check_and_set_shard_table(NULL, prior, default_db, groups);
    }
}

static int
routing_select(sql_context_t *context, const sql_select_t *select,
               char *default_db, guint32 fixture, query_stats_t *stats, GPtrArray *groups /* out */, int partition_mode)
{
    sql_src_list_t *sources = select->from_src;
    if (!sources) {
        shard_conf_get_fixed_group(partition_mode, groups, fixture);
        return USE_NON_SHARDING_TABLE;
    }
    
    GList *subqueries = NULL;
    sql_expr_find_subqueries(select->where_clause, &subqueries);

    GPtrArray *sharding_tables = g_ptr_array_new();
    GList *single_tables = NULL;
    int i;
    for (i = 0; i < sources->len; ++i) {
        char *db = default_db;
        sql_src_item_t *src = g_ptr_array_index(sources, i);
        char *table = NULL;
        if (src->select && sql_select_contains_sharding_table(src->select, &db, &table)) {
            sharding_filter_sql(context);   /* sharding table inside sub-query, should be filterd */
            if (context->rc == PARSE_NOT_SUPPORT) {
                g_ptr_array_free(sharding_tables, TRUE);
                g_list_free(subqueries);
                return ERROR_UNPARSABLE;
            }
            shard_conf_get_table_groups(groups, db, table);
            if (partition_mode) {
                dup_groups_for_partition(select->where_clause, sources, subqueries, select->prior, default_db, groups);
                context->sql_needs_reconstruct = 1;
            }
            g_list_free(subqueries);
            g_ptr_array_free(sharding_tables, TRUE);
            return USE_ALL_SHARDINGS;
        }
        if (!partition_mode && src->select) {      /* subquery not contain sharding table, try to find single table */
            sql_select_get_single_tables(src->select, db, &single_tables);
        }
        db = src->dbname ? src->dbname : default_db;
        if (src->table_name) {
            if (shard_conf_is_shard_table(db, src->table_name)) {
                g_ptr_array_add(sharding_tables, src);
            } else if ((!partition_mode) && shard_conf_is_single_table(0, db, src->table_name)) {
                single_tables = g_list_append(single_tables, src);
            }
        }
    }

    /* handle single table */
    if (single_tables) {
        if (sharding_tables->len > 0) {
            g_ptr_array_free(sharding_tables, TRUE);
            g_list_free(single_tables);
            g_list_free(subqueries);
            sql_context_append_msg(context, "(cetus) JOIN single-table WITH sharding-table");
            return ERROR_UNPARSABLE;
        }

        GList *l;
        for (l = single_tables; l; l = l->next) {
            sql_src_item_t *src = l->data;
            char *db = src->dbname ? src->dbname : default_db;
            shard_conf_get_single_table_distinct_group(groups, db, src->table_name);
        }
        if (groups->len > 1) {
            g_ptr_array_free(sharding_tables, TRUE);
            g_list_free(single_tables);
            g_list_free(subqueries);
            sql_context_append_msg(context, "(cetus)JOIN multiple single-tables not allowed");
            return ERROR_UNPARSABLE;
        } else {
            g_ptr_array_free(sharding_tables, TRUE);
            g_list_free(single_tables);
            g_list_free(subqueries);
            return USE_NON_SHARDING_TABLE;
        }
    }

    /* handle subquery in where clause */
    if (!partition_mode) {
        GList *l;
        for (l = subqueries; l; l = l->next) {
            if (sql_select_has_single_table(l->data, default_db)) {
                g_ptr_array_free(sharding_tables, TRUE);
                g_list_free(subqueries);
                sql_context_append_msg(context, "(cetus) Found single-table in subquery, not allowed");
                return ERROR_UNPARSABLE;
            }
        }
    }

    if (sharding_tables->len == 0) {
        shard_conf_get_fixed_group(partition_mode, groups, fixture);
        g_ptr_array_free(sharding_tables, TRUE);
        g_list_free(subqueries);
        return USE_NON_SHARDING_TABLE;
    }

    if (sharding_tables->len >= 2) {
        if (!join_on_sharding_key(default_db, sharding_tables, select->where_clause)) {
            g_ptr_array_free(sharding_tables, TRUE);
            g_list_free(subqueries);
            sql_context_append_msg(context, "(proxy)JOIN must inside VDB and have explicit join-on condition");
            return ERROR_UNPARSABLE;
        }
    }

    gboolean has_sharding_key = FALSE;
    for (i = 0; i < sharding_tables->len; ++i) {
        sql_src_item_t *shard_table = g_ptr_array_index(sharding_tables, i);
        char *db = shard_table->dbname ? shard_table->dbname : default_db;

        sharding_table_t *shard_info = shard_conf_get_info(db, shard_table->table_name);

        /* join tables have same sharding key, we are graunteed tableA.key = tableB.key
           so tableA.key = x also applies to tableB */
        if (optimize_sharding_condition(select->where_clause, shard_table, shard_info->pkey->str)) {
            has_sharding_key = TRUE;
        }
    }

    if (has_sharding_key) {
        for (i = 0; i < sharding_tables->len; ++i) {
            GPtrArray *partitions = g_ptr_array_new();  /* GPtrArray<sharding_partition_t *> */
            sql_src_item_t *shard_table = g_ptr_array_index(sharding_tables, i);
            char *db = shard_table->dbname ? shard_table->dbname : default_db;

            shard_conf_table_partitions(partitions, db, shard_table->table_name);
            int rc = partitions_filter_expr(partitions, select->where_clause);
            if (rc == PARSE_ERROR) {
                g_warning(G_STRLOC ":unrecognized key ranges");
                g_ptr_array_free(partitions, TRUE);
                g_ptr_array_free(sharding_tables, TRUE);
                g_list_free(subqueries);
                sql_context_append_msg(context, "(proxy)sharding key parse error");
                return ERROR_UNPARSABLE;
            } else if (rc == PARSE_UNRECOGNIZED) {
                g_ptr_array_free(partitions, TRUE);
                g_ptr_array_free(sharding_tables, TRUE);
                shard_conf_get_table_groups(groups, db, shard_table->table_name);
                if (partition_mode) {
                    dup_groups_for_partition(select->where_clause, sources, subqueries, select->prior, default_db, groups);
                    context->sql_needs_reconstruct = 1;
                }
                g_list_free(subqueries);
                return USE_ALL_SHARDINGS;
            }
            partitions_get_group_names(partitions, groups);
            g_ptr_array_free(partitions, TRUE);
            if (partition_mode) {
                dup_groups(shard_table, groups);
            }
        }
    }

    if (groups->len > 0) {
        g_ptr_array_free(sharding_tables, TRUE);
        if (partition_mode) {
            dup_groups_for_partition(select->where_clause, NULL, subqueries, select->prior, default_db, groups);
            context->sql_needs_reconstruct = 1;
        }
        g_list_free(subqueries);
        return USE_SHARDING;
    } else {
        /* has sharding table, but no sharding key
           OR sharding key filter out all groups */
        for (i = 0; i < sharding_tables->len; ++i) {
            sql_src_item_t *shard_table = g_ptr_array_index(sharding_tables, i);
            char *db = shard_table->dbname ? shard_table->dbname : default_db;
            shard_conf_get_table_groups(groups, db, shard_table->table_name);
            if (partition_mode) {
                dup_groups(shard_table, groups);
            }
        }
        g_ptr_array_free(sharding_tables, TRUE);
        if (partition_mode) {
            dup_groups_for_partition(select->where_clause, NULL, subqueries, select->prior, default_db, groups);
            context->sql_needs_reconstruct = 1;
        }
        g_list_free(subqueries);

        return USE_ALL_SHARDINGS;
    }
}

static gboolean
expr_same_with_sharding_cond(sql_expr_t *equation, sql_expr_t *where)
{
    GQueue *stack = g_queue_new();
    g_queue_push_head(stack, where);
    gboolean is_same = FALSE;
    /* for each condition in where clause */
    while (!g_queue_is_empty(stack)) {
        sql_expr_t *p = g_queue_pop_head(stack);
        if (p->op == TK_OR || p->op == TK_AND) {
            if (p->right)
                g_queue_push_head(stack, p->right);
            if (p->left)
                g_queue_push_head(stack, p->left);
            continue;
        }
        /* HACK: SHARD_COND under TK_NOT is skipped */
        if ((p->flags & EP_SHARD_COND) && p->op == TK_EQ) {
            if (sql_expr_equals(equation->left, p->left)
                && sql_expr_equals(equation->right, p->right)) {
                is_same = TRUE;
            }
        }
    }
    g_queue_free(stack);
    return is_same;
}

static int
routing_update(sql_context_t *context, sql_update_t *update,
               char *default_db, sharding_plan_t *plan, GPtrArray *groups, guint32 fixture)
{
    char *db = default_db;
    sql_src_list_t *tables = update->table_reference->table_list;
    sql_src_item_t *table = g_ptr_array_index(tables, 0);
    if (table->dbname) {
        db = table->dbname;
    }

    if (!shard_conf_is_shard_table(db, table->table_name)) {
        if (plan->is_partition_mode) {
            plan->table_type = GLOBAL_TABLE;
            sharding_plan_add_group(plan, partition_get_super_group());
            return USE_NON_SHARDING_TABLE;
        }

        if (shard_conf_is_single_table(0, db, table->table_name)) {
            plan->table_type = SINGLE_TABLE;
            shard_conf_get_single_table_distinct_group(groups, db, table->table_name);
            return USE_NON_SHARDING_TABLE;
        }

        shard_conf_get_all_groups(groups);
        plan->table_type = GLOBAL_TABLE;
        if (groups->len > 1) {
            return USE_DIS_TRAN;
        }
        return USE_NON_SHARDING_TABLE;
    }

    if (plan->is_partition_mode) {
        context->sql_needs_reconstruct = 1;
    }
    plan->table_type = SHARDED_TABLE;
    sharding_table_t *shard_info = shard_conf_get_info(db, table->table_name);
    int key_occur = optimize_sharding_condition(update->where_clause,
                                                table, shard_info->pkey->str);
    int i;
    /* update sharding key is not allowed */
    for (i = 0; update->set_list && i < update->set_list->len; ++i) {
        sql_expr_t *equation = g_ptr_array_index(update->set_list, i);
        if (!equation || !equation->left) {
            sql_context_append_msg(context, "(proxy)syntax error");
            return ERROR_UNPARSABLE;
        }
        if (expr_is_sharding_key(equation->left, table, shard_info->pkey->str)) {
            if (!(key_occur == 1    /* "update set k = 1 where k = 1" is legal */
                  && expr_same_with_sharding_cond(equation, update->where_clause))) {
                sql_context_append_msg(context, "(proxy)update of sharding key is not allowed");
                return ERROR_UNPARSABLE;
            }
        }
    }

    GPtrArray *partitions = g_ptr_array_new();
    shard_conf_table_partitions(partitions, db, table->table_name);
    if (key_occur) {
        int rc = partitions_filter_expr(partitions, update->where_clause);
        if (rc == PARSE_ERROR || rc == PARSE_UNRECOGNIZED) {
            g_warning(G_STRLOC ":unrecognized key ranges");
            g_ptr_array_free(partitions, TRUE);
            sql_context_append_msg(context, "(proxy)sharding key parse error");
            return ERROR_UNPARSABLE;
        }
    }
    partitions_get_group_names(partitions, groups);
    g_ptr_array_free(partitions, TRUE);

    int ret = USE_DIS_TRAN;
    if (groups->len == 1) {
        ret = USE_SHARDING;
    } else if (groups->len == 0) {
        shard_conf_get_table_groups(groups, db, table->table_name);
    }

    if (plan->is_partition_mode) {
        dup_groups(table, groups);
    }

    return ret;
}

static void
group_insert_values(GHashTable *groups, sharding_partition_t *part, sql_select_t *new_values)
{
    sql_select_t *values = g_hash_table_lookup(groups, part);
    if (values) {
        new_values->prior = values;
        g_hash_table_insert(groups, part, new_values);
    } else {
        g_hash_table_insert(groups, part, new_values);
    }
}

static sql_select_t *
merge_insert_values(GHashTable *groups, sql_select_t *residual)
{
    sql_select_t *merged_values = NULL;
    GList *values_list = g_hash_table_get_values(groups);
    if (residual) {
        values_list = g_list_append(values_list, residual);
    }
    GList *l;
    for (l = values_list; l != NULL; l = l->next) {
        sql_select_t *val = l->data;
        sql_select_t *tail = val;
        while (tail->prior) {
            tail = tail->prior;
        }
        if (merged_values) {
            tail->prior = merged_values;
            merged_values = val;
        } else {
            merged_values = val;
        }
    }
    g_list_free(values_list);
    return merged_values;
}

static int
insert_multi_value(sql_context_t *context, sql_insert_t *insert,
                   const char *db, const char *table,
                   sharding_table_t *shard_info, int shard_key_index, sharding_plan_t *plan)
{
    int rc = 0;

    GPtrArray *partitions = g_ptr_array_new();
    shard_conf_table_partitions(partitions, db, table);

    GHashTable *value_groups = g_hash_table_new(g_direct_hash, g_direct_equal);

    sql_select_t *values = insert->sel_val;
    insert->sel_val = NULL;     /* take away from insert AST */

    while (values) {
        if (values->columns->len <= shard_key_index) {
            g_warning("%s:col list values not match", G_STRLOC);
            sql_context_append_msg(context, "(proxy)no sharding key");
            rc = ERROR_UNPARSABLE;
            goto out;
        }
        struct condition_t cond = { TK_EQ, {0} };
        sql_expr_t *val = g_ptr_array_index(values->columns, shard_key_index);
        int rc = expr_parse_sharding_value(val, shard_info->shard_key_type, &cond);
        if (rc != PARSE_OK) {
            sql_context_append_msg(context, "(proxy)sharding key parse error");
            rc = ERROR_UNPARSABLE;
            goto out;
        }
        sharding_partition_t *part = partitions_get(partitions, cond);
        if (!part) {
            rc = ERROR_UNPARSABLE;
            goto out;
        }
        sql_select_t *node = values;
        values = values->prior;
        node->prior = NULL;     /* must be single values node */
        group_insert_values(value_groups, part, node);
    }
    GHashTableIter iter;
    sharding_partition_t *part;
    sql_select_t *values_list;
    g_hash_table_iter_init(&iter, value_groups);
    while (g_hash_table_iter_next(&iter, (void **)&part, (void **)&values_list)) {
        GString *sql = g_string_new(NULL);
        insert->sel_val = values_list;
        sql_construct_insert(plan->is_partition_mode, sql, insert, part->group_name);
        sharding_plan_add_group_sql(plan, part->group_name, sql);
    }
    rc = plan->groups->len > 1 ? USE_DIS_TRAN : USE_NON_SHARDING_TABLE;
    plan->is_sql_rewrite_completely = 1;

  out:
    /* restore the INSERT-AST */
    insert->sel_val = merge_insert_values(value_groups, values);

    g_hash_table_destroy(value_groups);
    g_ptr_array_free(partitions, TRUE);
    return rc;
}

static int
routing_insert(sql_context_t *context, sql_insert_t *insert, char *default_db, sharding_plan_t *plan, guint32 fixture)
{
    sql_src_list_t *src_list = insert->table;
    assert(src_list && src_list->len > 0);
    sql_src_item_t *src = g_ptr_array_index(src_list, 0);
    char *db = src->dbname ? src->dbname : default_db;
    char *table = src->table_name;
    g_debug(G_STRLOC ":db:%s, table:%s", db, table);

    sharding_table_t *shard_info = shard_conf_get_info(db, table);
    if (shard_info == NULL) {
        if (plan->is_partition_mode) {
            plan->table_type = GLOBAL_TABLE;
            sharding_plan_add_group(plan, partition_get_super_group());
            return USE_NON_SHARDING_TABLE;
        }
        GPtrArray *groups = g_ptr_array_new();
        if (shard_conf_is_single_table(0, db, table)) {
            shard_conf_get_single_table_distinct_group(groups, db, table);
            sharding_plan_add_groups(plan, groups);
            plan->table_type = SINGLE_TABLE;
            g_ptr_array_free(groups, TRUE);
            return USE_NON_SHARDING_TABLE;
        }

        if (insert->sel_val) {
            gboolean is_success = TRUE;
            sql_src_list_t *from = insert->sel_val->from_src;
            if (from) {
                char *db = default_db;
                sql_src_item_t *table = g_ptr_array_index(from, 0);
                if (table->dbname)
                    db = table->dbname;

                if (shard_conf_is_shard_table(db, table->table_name)) {
                    is_success = FALSE;
                } else if ((!plan->is_partition_mode) && shard_conf_is_single_table(0, db, table->table_name)) {
                    is_success = FALSE;
                }
            }

            if (is_success == FALSE) {
                g_warning("%s:unsupported INSERT format", G_STRLOC);
                sql_context_append_msg(context, "(proxy) unsupported INSERT format");
                g_ptr_array_free(groups, TRUE);
                return ERROR_UNPARSABLE;
            }
        }

        shard_conf_get_all_groups(groups);
        plan->table_type = GLOBAL_TABLE;
        if (groups->len > 1) {
            sharding_plan_add_groups(plan, groups);
            g_ptr_array_free(groups, TRUE);
            return USE_DIS_TRAN;
        }
        sharding_plan_add_groups(plan, groups);
        g_ptr_array_free(groups, TRUE);
        return USE_NON_SHARDING_TABLE;
    }

    if (plan->is_partition_mode) {
        context->sql_needs_reconstruct = 1;
    }
    plan->table_type = SHARDED_TABLE;
    sql_id_list_t *cols = insert->columns;
    if (cols == NULL) {
        g_warning("%s:unsupported INSERT format", G_STRLOC);
        sql_context_append_msg(context, "(proxy)INSERT must use explicit column names");
        return ERROR_UNPARSABLE;
    }
    const char *shard_key = shard_info->pkey->str;
    int shard_key_index = -1;
    int i;
    for (i = 0; i < cols->len; ++i) {
        char *col = (char *)g_ptr_array_index(cols, i);
        if (strcasecmp(col, shard_key) == 0) {
            shard_key_index = i;
            break;
        }
    }
    if (shard_key_index == -1) {
        g_warning(G_STRLOC ":cannot find sharding colomn %s", shard_key);
        sql_context_append_msg(context, "(proxy)INSERTion into sharding table must use sharding key");
        return ERROR_UNPARSABLE;
    }
    sql_select_t *sel_val = insert->sel_val;
    if (!sel_val || !sel_val->columns) {
        g_warning("%s:could not find insert values", G_STRLOC);
        sql_context_append_msg(context, "(proxy)no VALUES");
        return ERROR_UNPARSABLE;
    }
    if (sel_val->flags & SF_MULTI_VALUE) {
        return insert_multi_value(context, insert, db, table, shard_info, shard_key_index, plan);
    }

    /* SINGLE VALUE */
    sql_expr_list_t *values = sel_val->columns;
    if (values->len <= shard_key_index) {
        g_warning("%s:col list values not match", G_STRLOC);
        sql_context_append_msg(context, "(proxy)no sharding key");
        return ERROR_UNPARSABLE;
    }

    struct condition_t cond = { TK_EQ, {0} };
    sql_expr_t *val = g_ptr_array_index(values, shard_key_index);
    int rc = expr_parse_sharding_value(val, shard_info->shard_key_type, &cond);
    if (rc != PARSE_OK) {
        sql_context_append_msg(context, "(proxy)sharding key parse error");
        return ERROR_UNPARSABLE;
    }

    GPtrArray *partitions = g_ptr_array_new();
    shard_conf_table_partitions(partitions, db, table);
    partitions_filter(partitions, cond);

    GPtrArray *groups = g_ptr_array_new();
    partitions_get_group_names(partitions, groups);
    if (plan->is_partition_mode) {
        dup_groups(src, groups);
    }

    g_ptr_array_free(partitions, TRUE);
    sharding_plan_add_groups(plan, groups);
    g_ptr_array_free(groups, TRUE);

    if (plan->groups->len == 0) {
        /* TODO: return code when pkey out of range; */
        return USE_NON_SHARDING_TABLE;
    } else {
        if (plan->groups->len != 1) {   /* can't happen */
            return ERROR_UNPARSABLE;
        }
        return USE_SHARDING;
    }
}

static int
routing_delete(sql_context_t *context, sql_delete_t *delete,
               char *default_db, sharding_plan_t *plan, GPtrArray *groups, guint32 fixture)
{
    if (!delete) {
        g_warning(G_STRLOC ":delete ast error");
        sql_context_append_msg(context, "(proxy)no ast");
        return ERROR_UNPARSABLE;
    }
    char *db = default_db;
    sql_src_list_t *from = delete->from_src;
    sql_src_item_t *table = g_ptr_array_index(from, 0);
    if (table->dbname)
        db = table->dbname;

    if (!shard_conf_is_shard_table(db, table->table_name)) {
        if (plan->is_partition_mode) {
            plan->table_type = GLOBAL_TABLE;
            sharding_plan_add_group(plan, partition_get_super_group());
            return USE_NON_SHARDING_TABLE;
        }

        if (shard_conf_is_single_table(0, db, table->table_name)) {
            shard_conf_get_single_table_distinct_group(groups, db, table->table_name);
            plan->table_type = SINGLE_TABLE;
            return USE_NON_SHARDING_TABLE;
        }

        shard_conf_get_all_groups(groups);
        plan->table_type = GLOBAL_TABLE;
        if (groups->len > 1) {
            return USE_DIS_TRAN;
        }
        return USE_NON_SHARDING_TABLE;
    }

    if (plan->is_partition_mode) {
        context->sql_needs_reconstruct = 1;
    }
    plan->table_type = SHARDED_TABLE;
    if (!delete->where_clause) {
        shard_conf_get_table_groups(groups, db, table->table_name);
        if (plan->is_partition_mode) {
            dup_groups(table, groups);
        }

        if (groups->len == 1) {
            return USE_SHARDING;
        } else if (groups->len > 1) {
            return USE_DIS_TRAN;
        }
    }

    sharding_table_t *shard_info = shard_conf_get_info(db, table->table_name);
    GPtrArray *partitions = g_ptr_array_new();
    shard_conf_table_partitions(partitions, db, table->table_name);
    gboolean has_sharding_key = optimize_sharding_condition(delete->where_clause, table, shard_info->pkey->str);
    int rc = PARSE_OK;
    if (has_sharding_key) {
        rc = partitions_filter_expr(partitions, delete->where_clause);
    }

    if (rc == PARSE_ERROR || rc == PARSE_UNRECOGNIZED) {
        g_warning(G_STRLOC ":unrecognized key");
        g_ptr_array_free(partitions, TRUE);
        sql_context_append_msg(context, "(proxy)sharding key parse error");
        return ERROR_UNPARSABLE;
    }
    partitions_get_group_names(partitions, groups);
    g_ptr_array_free(partitions, TRUE);

    int ret = USE_DIS_TRAN;
    if (groups->len == 1) {
        ret = USE_SHARDING;
    } else if (groups->len == 0) {
        shard_conf_get_table_groups(groups, db, table->table_name);
    }

    if (plan->is_partition_mode) {
        dup_groups(table, groups);
    }

    return ret;
}

int
check_property_has_groups(sql_context_t *context)
{
    sql_property_t *property = context->property;

    if (!property) {
        return FALSE;
    }
    if (!sql_property_is_valid(property)) {
        return FALSE;
    }

    if (property->group) {
        return TRUE;
    } else {
        return FALSE;
    }
}

int
routing_by_property(sql_context_t *context, sql_property_t *property, char *default_db, GPtrArray *groups /* out */ )
{
    if (property->table) {
        char *db = default_db;
        char *table = property->table;

        /* check for dotted name "db.table" */
        char *p = strrchr(property->table, '.');
        if (p) {
            *p = '\0';
            db = property->table;
            table = p + 1;
        }
        GPtrArray *partitions = g_ptr_array_new();
        shard_conf_table_partitions(partitions, db, table);
        if (property->key) {
            struct condition_t cond = { TK_EQ, {0} };
            sharding_table_t *info = shard_conf_get_info(db, table);
            if (!info) {
                g_ptr_array_free(partitions, TRUE);
                sql_context_append_msg(context, "(cetus)no such table");
                return ERROR_UNPARSABLE;
            }
            int rc = string_to_sharding_value(property->key, info->shard_key_type, &cond);
            if (rc != PARSE_OK) {
                g_ptr_array_free(partitions, TRUE);
                sql_context_append_msg(context, "(proxy)comment error: key");
                return ERROR_UNPARSABLE;
            }
            partitions_filter(partitions, cond);
        }
        partitions_get_group_names(partitions, groups);
        g_ptr_array_free(partitions, TRUE);
        enum sql_clause_flag_t f = context->rw_flag;
        if ((f & CF_WRITE) && !(f & CF_DDL) && groups->len > 1) {
            return USE_DIS_TRAN;
        }
        return USE_SHARDING;

    } else if (property->group) {
        shard_conf_find_groups(groups, property->group);
        if (groups->len > 0) {
            enum sql_clause_flag_t f = context->rw_flag;
            if ((f & CF_WRITE) && !(f & CF_DDL) && groups->len > 1) {
                return USE_DIS_TRAN;
            }
            return USE_SHARDING;
        } else {
            char msg[256] = { 0 };
            snprintf(msg, 256, "no group: %s for db: %s", property->group, default_db);
            g_warning("%s", msg);
            sql_context_append_msg(context, msg);
            return ERROR_UNPARSABLE;
        }
    }
    sql_context_append_msg(context, "(proxy)comment error, unknown property");
    return ERROR_UNPARSABLE;
}

int
sharding_parse_groups_by_property(GString *default_db, sql_context_t *context, sharding_plan_t *plan)
{
    GPtrArray *groups = g_ptr_array_new();

    context->sql_needs_reconstruct = 0;

    int rc = routing_by_property(context, context->property, default_db->str, groups);
    sharding_plan_add_groups(plan, groups);
    g_ptr_array_free(groups, TRUE);

    return rc;
}


int
sharding_parse_groups(GString *default_db, sql_context_t *context, query_stats_t *stats,
                      guint64 fixture, sharding_plan_t *plan)
{
    GPtrArray *groups = g_ptr_array_new();
    if (context == NULL) {
        g_warning("%s:sql is not parsed", G_STRLOC);
        shard_conf_get_fixed_group(plan->is_partition_mode, groups, fixture);
        sharding_plan_add_groups(plan, groups);
        g_ptr_array_free(groups, TRUE);
        return USE_NON_SHARDING_TABLE;
    }

    context->sql_needs_reconstruct = 0;

    char *db = default_db->str;
    g_debug(G_STRLOC ":default db:%s", db);

    if (!plan->is_partition_mode) {
        if (sql_context_has_sharding_property(context)) {
            int rc = routing_by_property(context, context->property, db, groups);
            sharding_plan_add_groups(plan, groups);
            g_ptr_array_free(groups, TRUE);
            return rc;
        }
    }

    int rc = ERROR_UNPARSABLE;
    switch (context->stmt_type) {
    case STMT_SELECT:{
        sql_select_t *select = context->sql_statement;
        while (select) {
            rc = routing_select(context, select, db, fixture, stats, groups, plan->is_partition_mode);
            if (rc < 0) {
                break;
            }
            select = select->prior; /* select->prior UNION select */
        }
        sharding_plan_add_groups(plan, groups);
        g_ptr_array_free(groups, TRUE);

        if ((rc == USE_SHARDING || rc == USE_ALL_SHARDINGS) && plan->groups->len > 1) {
            sharding_filter_sql(context);   /* only filter queries with sharding table */
            if (context->rc == PARSE_NOT_SUPPORT) {
                sharding_plan_clear_group(plan);
                return ERROR_UNPARSABLE;
            }
        }
        return rc;              /* TODO: result of first select */
    }
    case STMT_UPDATE:
        rc = routing_update(context, context->sql_statement, db, plan, groups, fixture);
        sharding_plan_add_groups(plan, groups);
        g_ptr_array_free(groups, TRUE);
        return rc;
    case STMT_INSERT:
        rc = routing_insert(context, context->sql_statement, db, plan, fixture);
        g_ptr_array_free(groups, TRUE);
        return rc;
    case STMT_DELETE:
        rc = routing_delete(context, context->sql_statement, db, plan, groups, fixture);
        sharding_plan_add_groups(plan, groups);
        g_ptr_array_free(groups, TRUE);
        return rc;
    case STMT_SHOW_WARNINGS:
        g_ptr_array_free(groups, TRUE);
        return USE_PREVIOUS_WARNING_CONN;
    case STMT_SHOW_COLUMNS:
    case STMT_SHOW_CREATE:
    case STMT_EXPLAIN_TABLE:{  /* DESCRIBE tablename; */
        sql_src_list_t *tables = context->sql_statement;
        sql_src_item_t *src_item = g_ptr_array_index(tables, 0);
        g_assert(src_item);
        if (src_item->dbname)
            db = src_item->dbname;
        if (shard_conf_is_shard_table(db, src_item->table_name)) {
            shard_conf_get_any_group(groups, db, src_item->table_name);
            sharding_plan_add_groups(plan, groups);
            g_ptr_array_free(groups, TRUE);
            return USE_ANY_SHARDINGS;
        }
        if ((!plan->is_partition_mode) && shard_conf_is_single_table(0, db, src_item->table_name)) {
            shard_conf_get_single_table_distinct_group(groups, db, src_item->table_name);
            sharding_plan_add_groups(plan, groups);
            g_ptr_array_free(groups, TRUE);
            return USE_NON_SHARDING_TABLE;
        }
        shard_conf_get_fixed_group(plan->is_partition_mode, groups, fixture);
        sharding_plan_add_groups(plan, groups);
        g_ptr_array_free(groups, TRUE);
        return USE_NON_SHARDING_TABLE;
    }
    case STMT_SET:
        if (sql_context_is_autocommit_on(context)) {
            g_ptr_array_free(groups, TRUE);
            return USE_SAME;
        } else if (sql_context_is_autocommit_off(context)) {
            g_ptr_array_free(groups, TRUE);
            return USE_NONE;
        } else {
            shard_conf_get_all_groups(groups);
            sharding_plan_add_groups(plan, groups);
            g_ptr_array_free(groups, TRUE);
            return USE_ALL;
        }
    case STMT_START:
        g_ptr_array_free(groups, TRUE);
        return USE_NONE;
    case STMT_COMMIT:
    case STMT_ROLLBACK:
        g_ptr_array_free(groups, TRUE);
        return USE_PREVIOUS_TRAN_CONNS;
    case STMT_CALL:
        g_ptr_array_free(groups, TRUE);
        return rc;
    case STMT_DROP_DATABASE:
    case STMT_COMMON_DDL:      /* ddl without comments sent to all */
        if (plan->is_partition_mode) {
            g_ptr_array_free(groups, TRUE);
            sql_context_set_error(context, PARSE_NOT_SUPPORT, "(cetus) DDL is not allowed for partition until now");
            return ERROR_UNPARSABLE;
        } else {
            shard_conf_get_all_groups(groups);
            sharding_plan_add_groups(plan, groups);
            g_ptr_array_free(groups, TRUE);
            return USE_ALL;
        }
    case STMT_SHOW:
        shard_conf_get_fixed_group(plan->is_partition_mode, groups, fixture);
        sharding_plan_add_groups(plan, groups);
        g_ptr_array_free(groups, TRUE);
        return USE_NON_SHARDING_TABLE;
    default:
        g_debug("unrecognized query, using default master db, sql:%s", plan->orig_sql->str);
        shard_conf_get_fixed_group(plan->is_partition_mode, groups, fixture);
        sharding_plan_add_groups(plan, groups);
        g_ptr_array_free(groups, TRUE);
        return USE_NON_SHARDING_TABLE;
    }
}

/* is ORDERBY column a subset of SELECT column */
static gboolean
select_compare_orderby(sql_select_t *select)
{
    sql_expr_list_t *columns = select->columns;
    sql_column_list_t *ord_cols = select->orderby_clause;

    if (ord_cols == NULL) {
        sql_expr_t *mcol = g_ptr_array_index(columns, 0);
        if (mcol->op == TK_STAR) {
            return FALSE;       /* reject: select DISTINCT *where .. */
        } else if (mcol->op == TK_DOT && mcol->right && mcol->right->op == TK_STAR) {
            return FALSE;       /* reject: select DISTINCT t.* where .. */
        }
        select->flags |= SF_REWRITE_ORDERBY;
        return TRUE;            /* accept: select DISTINCT a, b where .., rewrite later */
    }

    if (columns->len >= ord_cols->len) {
        int i;
        for (i = 0; i < ord_cols->len; ++i) {
            sql_column_t *ordcol = g_ptr_array_index(ord_cols, i);
            sql_expr_t *ord = ordcol->expr;
            if (sql_expr_is_id(ord, NULL)) {
                sql_expr_t *match = sql_expr_list_find(columns, ord->token_text);
                if (match) {
                    match->flags |= EP_ORDER_BY;
                } else {
                    return FALSE;   /* reject: select DISTINCT a,b,c ORDER BY x,y */
                }
            } else if (sql_expr_is_dotted_name(ord, NULL, NULL)) {
                sql_expr_t *match = sql_expr_list_find_fullname(columns, ord);
                if (match) {
                    match->flags |= EP_ORDER_BY;
                } else {
                    return FALSE;   /* reject: select DISTINCT a,b,c ORDER BY x,y */
                }
            } else {
                g_warning(G_STRLOC ":unrecognized order by column");
                return FALSE;
            }
        }
        if (columns->len != ord_cols->len)
            select->flags |= SF_REWRITE_ORDERBY;
        return TRUE;            /* accept: 1) select DISTINCT a,b,c,d ORDER BY b,c,a. rewrite later */
        /*         2) select DISTINCT a,b,c,d ORDER BY a,c,b,d. no rewrite */
    }
    return FALSE;               /* reject: DISTINCT a ORDER BY a,b,c. might be syntax error */
}

static gboolean
select_check_HAVING_column(sql_select_t *select)
{
    sql_expr_t *having = select->having_clause;
    if (!(having && having->left)) {    /* no HAVING is alright */
        return TRUE;
    }

    const char *having_func = having->left->token_text;
    if (!having_func) {
        return FALSE;
    }

    /* find having cond in columns */
    sql_expr_list_t *columns = select->columns;
    int index = sql_expr_list_find_exact_aggregate(columns, having->left->start,
                                       having->left->end - having->left->start);
    return index != -1;
}

static gboolean
select_has_distincted_aggregate(sql_select_t *select, int has_subquery, char **aggr_name)
{
    int i;
    for (i = 0; select->columns && i < select->columns->len; ++i) {
        sql_expr_t *expr = g_ptr_array_index(select->columns, i);
        if (expr->op == TK_FUNCTION && expr->flags & EP_DISTINCT) {
            if (strcasecmp(expr->token_text, "count") == 0
                || strcasecmp(expr->token_text, "sum") == 0 || strcasecmp(expr->token_text, "avg") == 0) {
                *aggr_name = expr->token_text;
                return TRUE;
            }
        }
    }
    if (has_subquery && select->from_src) {
        for (i = 0; i < select->from_src->len; ++i) {
            sql_src_item_t *src = g_ptr_array_index(select->from_src, i);
            if (src->select)
                return select_has_distincted_aggregate(src->select, 0, aggr_name);
        }
    }
    return FALSE;
}

static gboolean
select_has_sub_select_aggregate(sql_select_t *select, int is_analyze)
{
    int i;
    if (is_analyze) {
        for (i = 0; select->columns && i < select->columns->len; ++i) {
            sql_expr_t *expr = g_ptr_array_index(select->columns, i);
            if (expr->op == TK_FUNCTION && expr->flags & EP_AGGREGATE) {
                if (strcasecmp(expr->token_text, "count") == 0
                        || strcasecmp(expr->token_text, "sum") == 0 || strcasecmp(expr->token_text, "avg") == 0) {
                    return TRUE;
                }
            }
        }
    }
    if (select->from_src) {
        for (i = 0; i < select->from_src->len; ++i) {
            sql_src_item_t *src = g_ptr_array_index(select->from_src, i);
            if (src->select)
                return select_has_sub_select_aggregate(src->select, 1);
        }
    }

    return FALSE;
}

static gboolean
select_has_AVG(sql_select_t *select)
{
    int i;
    for (i = 0; select->columns && i < select->columns->len; ++i) {
        sql_expr_t *expr = g_ptr_array_index(select->columns, i);
        if (expr->op == TK_FUNCTION && expr->flags & EP_AGGREGATE) {
            if (strcasecmp(expr->token_text, "avg") == 0)
                return TRUE;
        }
    }
    return FALSE;
}

/* group by & order by have only 1 column, and they are same */
static gboolean
select_groupby_orderby_have_same_column(sql_select_t *select)
{
    g_assert(select->groupby_clause && select->orderby_clause);
    sql_expr_list_t *grp = select->groupby_clause;
    sql_column_list_t *ord = select->orderby_clause;
    if (grp->len != ord->len) {
        return FALSE;
    }

    int i;

    for (i = 0; i < grp->len; i++) {
        sql_expr_t *grp_expr = g_ptr_array_index(grp, i);
        sql_column_t *ord_col = g_ptr_array_index(ord, i);
        if (g_strcmp0(ord_col->expr->token_text, grp_expr->token_text) != 0) {
            return FALSE;
        }
    }

    return TRUE;
}

void
sharding_filter_sql(sql_context_t *context)
{                               /* TODO:should be in sql-operations.c */
    if (context->stmt_type == STMT_SELECT) {
        sql_select_t *select = context->sql_statement;
        if (select->flags & SF_DISTINCT) {
            gboolean same = select_compare_orderby(select);
            if (!same) {
                sql_context_set_error(context, PARSE_NOT_SUPPORT,
                                      "(proxy)ORDER BY columns must be a subset of DISTINCT columns");
                return;
            }
        }

        /* grauntee HAVING condition show up in column */
        if (select->having_clause) {
            if (!is_compare_op(select->having_clause->op)) {
                sql_context_set_error(context, PARSE_NOT_SUPPORT, "(cetus) Only support simple HAVING condition");
                return;
            }
            if (!select_check_HAVING_column(select)) {
                sql_context_set_error(context, PARSE_NOT_SUPPORT,
                                      "(cetus) HAVING condition must show up in column");
                return;
            }
            if (select->limit) {
                sql_context_set_error(context, PARSE_NOT_SUPPORT,
                                      "(cetus) Only support HAVING condition without limit");
                return;
            }
        }

        if (select->groupby_clause) {
            sql_expr_list_t *groupby = select->groupby_clause;
            int i;
            for (i = 0; i < groupby->len; ++i) {
                sql_expr_t *col = g_ptr_array_index(groupby, i);
                if (col->op == TK_CASE) {
                    sql_context_set_error(context, PARSE_NOT_SUPPORT, "(proxy) group by CASE-WHEN not supported");
                    return;
                }
            }
        }
        if (context->clause_flags & CF_AGGREGATE) {
            if (select_has_AVG(select)) {
                sql_context_set_error(context, PARSE_NOT_SUPPORT,
                                      "(cetus)this AVG would be routed to multiple shards, not allowed");
                return;
            }
            /* if we can't find simple aggregates, it's inside complex expressions */
            if (sql_expr_list_find_aggregate(select->columns, NULL) == -1) {
                sql_context_set_error(context, PARSE_NOT_SUPPORT,
                                      "(cetus) Complex aggregate function not allowed on sharded sql");
                return;
            }
        }
        if (select->groupby_clause && select->orderby_clause && !select_groupby_orderby_have_same_column(select)) {
            sql_context_set_error(context, PARSE_NOT_SUPPORT,
                                  "(cetus) can't ORDER BY and GROUP BY different columns on sharded sql");
            return;
        }
        /* reject SELECT COUNT(DISTINCT) / SUM(DISTINCT) / AVG(DISTINCT) */
        if (context->clause_flags & CF_DISTINCT_AGGR) {
            char *aggr_name = NULL;
            int subquery = context->clause_flags & CF_SUBQUERY;
            if (select_has_distincted_aggregate(select, subquery, &aggr_name)) {
                char msg[128];
                snprintf(msg, 128, "(proxy) %s(DISTINCT ...) not supported", aggr_name);
                sql_context_set_error(context, PARSE_NOT_SUPPORT, msg);
                return;
            }
        }

        if ((!context->allow_subquery_nesting) && (context->clause_flags & CF_SUBQUERY)) {
            if (select_has_sub_select_aggregate(select, 0) == TRUE) {
                sql_context_set_error(context, PARSE_NOT_SUPPORT, "(proxy) sub select aggregate functions not supported");
                return;
            }
        }
    }
}
