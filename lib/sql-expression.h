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

#ifndef SQL_EXPRESSION_H
#define SQL_EXPRESSION_H

#include <stdint.h>
#include <stdbool.h>
#include <glib.h>
#include <stdio.h>

enum sql_join_type_t {
    JT_INNER = 0x01,            /* Any kind of inner or cross join */
    JT_CROSS = 0x02,            /* Explicit use of the CROSS keyword */
    JT_NATURAL = 0x04,          /* True for a "natural" join */
    JT_LEFT = 0x08,             /* Left outer join */
    JT_RIGHT = 0x10,            /* Right outer join */
    JT_OUTER = 0x20,            /* The "OUTER" keyword is present */
    JT_ERROR = 0x40,            /* unknown or unsupported join type */
};

#define MAX_AGGR_FUNS 8

typedef struct group_aggr_t {
    uint8_t type;
    int pos;
    unsigned int fun_type;
} group_aggr_t;

typedef struct sql_token_t sql_token_t;
typedef struct sql_expr_t sql_expr_t;
typedef GPtrArray sql_expr_list_t;
typedef GPtrArray sql_id_list_t;
typedef GPtrArray sql_src_list_t;
typedef struct sql_src_item_t sql_src_item_t;
typedef struct sql_expr_span_t sql_expr_span_t;

typedef struct sql_select_t sql_select_t;
typedef struct sql_delete_t sql_delete_t;
typedef struct sql_update_t sql_update_t;
typedef struct sql_insert_t sql_insert_t;
typedef enum sql_stmt_type_t sql_stmt_type_t;
typedef struct sql_column_t sql_column_t;
typedef GPtrArray sql_column_list_t;
typedef struct sql_drop_database_t sql_drop_database_t;

enum sql_stmt_type_t {
    STMT_UNKOWN,
    STMT_SELECT,
    STMT_INSERT,
    STMT_UPDATE,
    STMT_DELETE,

    STMT_COMMON_DDL,

    STMT_SHOW,
    STMT_SET,
    STMT_SET_NAMES,
    STMT_SET_TRANSACTION,
    STMT_ROLLBACK,
    STMT_COMMIT,
    STMT_CALL,
    STMT_START,
    STMT_EXPLAIN_TABLE,
    STMT_USE,
    STMT_SAVEPOINT,
    STMT_SHOW_COLUMNS,
    STMT_SHOW_CREATE,
    STMT_SHOW_WARNINGS,

    STMT_DROP_DATABASE,
};
struct sql_token_t {
    char *z;              /* pointer to token text, not NUL-terminated */
    uint32_t n;                 /* length of token text in bytes */
};

enum sql_var_scope_t {
    SCOPE_GLOBAL,
    SCOPE_SESSION,
    SCOPE_USER,

    /* SET TRANSACTION ...without explicit scope keyword, 
       only affect next transaction, it's transient */
    SCOPE_TRANSIENT,
    SCOPE_NONE
};

enum sql_trx_feature_t {
    TF_READ_ONLY = 1,
    TF_READ_WRITE,

    /* isolation levels */
    TF_SERIALIZABLE,
    TF_REPEATABLE_READ,
    TF_READ_COMMITTED,
    TF_READ_UNCOMMITTED,
};

enum sql_clause_flag_t {
    CF_READ = 0x01,
    CF_WRITE = 0x02,
    CF_FORCE_MASTER = 0x04,
    CF_FORCE_SLAVE = 0x08,
    CF_DDL = 0x10,
    CF_LOCAL_QUERY = 0x20,
    CF_DISTINCT_AGGR = 0x40,
    CF_SUBQUERY = 0x80,
    CF_AGGREGATE = 0x0100,
};

enum sql_sort_order_t {
    SQL_SO_ASC,
    SQL_SO_DESC,
};

enum sql_expr_flags_t {
    EP_FUNCTION = 0x01,
    EP_INTERVAL = 0x02,
    EP_EXISTS = 0x04,
    EP_BETWEEN = 0x08,
    EP_ATOMIC = 0x10,
    EP_LAST_INSERT_ID = 0x20,
    EP_DISTINCT = 0x40,
    EP_SHARD_COND = 0x80,
    EP_JOIN_LINK = 0x0100,
    EP_NOT = 0x0200,
    EP_CASE_WHEN = 0x0400,
    EP_ORDER_BY = 0x0800,
    EP_AGGREGATE = 0x1000,
};

enum sql_aggregate_type_t {
    FT_UNKNOWN = 0,
    FT_COUNT,
    FT_SUM,
    FT_AVG,
    FT_MAX,
    FT_MIN,
};

enum sql_index_hint_type_t {
    IH_USE_INDEX,
    IH_USE_KEY,
    IH_IGNORE_INDEX,
    IH_IGNORE_KEY,
    IH_FORCE_INDEX,
    IH_FORCE_KEY
};

struct sql_index_hint_t {
    uint8_t type;
    sql_id_list_t *names;
};

typedef struct sql_index_hint_t sql_index_hint_t;

struct sql_table_reference_t {
    sql_src_list_t* table_list;
    sql_index_hint_t *index_hint;
};

typedef struct sql_table_reference_t sql_table_reference_t;

struct sql_expr_t {
    uint16_t height;                 /* Height of the tree headed by this node */
    uint16_t op;                /* Operation performed by this node */
    unsigned int modify_flag:1;
    char *token_text;           /* Token value. Zero terminated and dequoted */
    int64_t num_value;
    sql_expr_t *left;
    sql_expr_t *right;

    sql_expr_list_t *list;      /* op = IN, EXISTS, SELECT, CASE, FUNCTION, BETWEEN */
    sql_select_t *select;       /* EP_xIsSelect and op = IN, EXISTS, SELECT */

    char *alias;
    enum sql_expr_flags_t flags;
    enum sql_var_scope_t var_scope; /* variable scope: SESSION(default) or GLOBAL */
    const char *start;          /* first char of expr in original sql */
    const char *end;            /* one char past the end of expr in orig sql */
};

enum select_flag_t {
    SF_DISTINCT = 0x01,
    SF_ALL = 0x02,
    SF_CALC_FOUND_ROWS = 0x04,
    SF_MULTI_VALUE = 0x08,
    SF_REWRITE_ORDERBY = 0x10,
};

struct sql_select_t {
    uint8_t op;                 /* One of: TK_UNION TK_ALL TK_INTERSECT TK_EXCEPT */
    uint32_t flags;             /* Various SF_* values */
    sql_expr_list_t *columns;   /* The fields of the result */
    sql_src_list_t *from_src;   /* The FROM clause */
    sql_expr_t *where_clause;
    sql_expr_list_t *groupby_clause;
    sql_expr_t *having_clause;
    sql_column_list_t *orderby_clause;
    sql_select_t *prior;        /* Prior select in a compound select statement */
    sql_select_t *next;         /* Next select to the left in a compound */
    sql_expr_t *limit;          /* LIMIT expression. NULL means not used. */
    sql_expr_t *offset;         /* OFFSET expression. NULL means not used. */
    int lock_read;
};

struct sql_delete_t {
    sql_src_list_t *from_src;   /* The FROM clause */
    sql_expr_t *where_clause;
    sql_column_list_t *orderby_clause;
    sql_expr_t *limit;          /* LIMIT expression. NULL means not used. */
    sql_expr_t *offset;         /* OFFSET expression. NULL means not used. */
};

struct sql_update_t {
    sql_table_reference_t *table_reference;
    sql_expr_list_t *set_list;
    sql_expr_t *where_clause;
    sql_column_list_t *orderby_clause;
    sql_expr_t *limit;          /* LIMIT expression. NULL means not used. */
    sql_expr_t *offset;         /* OFFSET expression. NULL means not used. */

};

struct sql_insert_t {
    int is_replace;
    sql_src_list_t *table;
    sql_select_t *sel_val;      /* [1] select...  [2] values(...) */
    sql_id_list_t *columns;
    sql_expr_list_t *update_list; /* ON DUPLICATE KEY UPDATE ... */
    const char *columns_start; /* [start, end) span of columns */
    const char *columns_end;
};

struct sql_column_t {
    sql_expr_t *expr;
    char *type;
    enum sql_sort_order_t sort_order;
    char *alias;
};

struct sql_src_item_t {
    char *dbname;               /* Name of database holding this table */
    char *table_name;           /* Name of the table */
    GPtrArray *groups;
    sql_index_hint_t *index_hint;
    char *table_alias;          /* The "B" part of a "A AS B" phrase.  zName is the "A" */
    sql_select_t *select;       /* A SELECT statement used in place of a table name */

    sql_expr_t *on_clause;      /* The ON clause of a join */
    sql_id_list_t *pUsing;      /* The USING clause of a join */

    sql_expr_list_t *func_arg;  /* Arguments to table-valued-function */
    int group_index;
    uint8_t jointype;           /* Type of join between this table and the previous */
};                              /* One entry for each identifier on the list */

struct sql_drop_database_t {
    char *schema_name;
    uint8_t ifexists;
};

typedef struct sql_set_transaction_t {
    enum sql_var_scope_t scope;
    enum sql_trx_feature_t rw_feature;
    enum sql_trx_feature_t level;
} sql_set_transaction_t;

char *sql_token_dup(sql_token_t);

void sql_string_dequote(char *str);

sql_expr_t *sql_expr_new(int op, const sql_token_t *token);

sql_expr_t *sql_expr_dup(const sql_expr_t *);

void sql_expr_attach_subtrees(sql_expr_t *p, sql_expr_t *left, sql_expr_t *right);

void sql_expr_free(void *p);

void sql_expr_print(sql_expr_t *p, int depth);

gboolean sql_expr_get_int(sql_expr_t *p, gint64 *value);

gboolean sql_expr_is_boolean(sql_expr_t *p, gboolean *value);

#define sql_expr_id(A) \
    (A ? ((A->op == TK_ID)?A->token_text:NULL) : NULL)

gboolean sql_expr_is_id(const sql_expr_t *p, const char *name);

gboolean sql_expr_is_function(const sql_expr_t *p, const char *func_name);

gboolean sql_expr_is_dotted_name(const sql_expr_t *p, const char *prefix, const char *suffix);

void sql_expr_get_dotted_names(const sql_expr_t *p, char *db, int db_len,
                               char *table, int tb_len,
                               char *col, int col_len);

gboolean sql_expr_is_field_name(const sql_expr_t *p);

sql_expr_list_t *sql_expr_list_append(sql_expr_list_t *list, sql_expr_t *expr);

sql_expr_t *sql_expr_list_find(sql_expr_list_t *list, const char *name);

sql_expr_t *sql_expr_list_find_fullname(sql_expr_list_t *list, const sql_expr_t *expr);

int sql_expr_list_find_aggregates(sql_expr_list_t *list, group_aggr_t * aggr_array);
int sql_expr_list_find_aggregate(sql_expr_list_t *list, const char *target);

int sql_expr_list_find_exact_aggregate(sql_expr_list_t *list, const char *target, int len);

void sql_expr_list_free(sql_expr_list_t *list);

sql_src_list_t *sql_src_list_append(sql_src_list_t *, sql_token_t *tname,
                                    sql_token_t *dbname, sql_index_hint_t *index_hint,
                                    sql_token_t *alias, sql_select_t *subquery,
                                    sql_expr_t *on_clause, sql_id_list_t *using_clause);

void sql_src_list_free(sql_src_list_t *p);

sql_id_list_t *sql_id_list_append(sql_id_list_t *p, sql_token_t *id_name);

void sql_id_list_free(sql_id_list_t *p);

sql_select_t *sql_select_new();

void sql_select_free(sql_select_t *);

sql_delete_t *sql_delete_new();

void sql_delete_free(sql_delete_t *);

sql_update_t *sql_update_new();

void sql_update_free(sql_update_t *);

sql_insert_t *sql_insert_new();

void sql_insert_free(sql_insert_t *);

char *sql_get_token_name(int op);

sql_column_list_t *sql_column_list_append(sql_column_list_t *, sql_column_t *);

void sql_column_list_free(sql_column_list_t *);

sql_column_t *sql_column_new();

void sql_column_free(void *);

sql_drop_database_t *sql_drop_database_new();
void sql_drop_database_free(sql_drop_database_t *);

int sql_join_type(sql_token_t kw);

void sql_statement_free(void *clause, sql_stmt_type_t stmt_type);

gboolean sql_is_quoted_string(const char *s);

enum sql_aggregate_type_t sql_aggregate_type(const char *s);

gboolean sql_expr_equals(const sql_expr_t *, const sql_expr_t *);

void sql_index_hint_free(sql_index_hint_t* p);
sql_index_hint_t *sql_index_hint_new();

void sql_table_reference_free(sql_table_reference_t* p);
sql_table_reference_t *sql_table_reference_new();

#endif /* SQL_EXPRESSION_H */
