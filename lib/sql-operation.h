#ifndef SQL_OPERATION_H
#define SQL_OPERATION_H

#include "sql-context.h"
#include "sql-expression.h"

void sql_set_variable(sql_context_t *, sql_expr_list_t *);

void sql_set_names(sql_context_t *, char *);

void sql_set_transaction(sql_context_t *, int scope, int rwfeature, int level);

void sql_select(sql_context_t *st, sql_select_t *select);

void sql_delete(sql_context_t *st, sql_delete_t *del);

void sql_update(sql_context_t *st, sql_update_t *p);

void sql_insert(sql_context_t *st, sql_insert_t *p);

void sql_statement_free(void *stmt, sql_stmt_type_t stmt_type);

void sql_start_transaction(sql_context_t *st);

void sql_commit_transaction(sql_context_t *st);

void sql_rollback_transaction(sql_context_t *st);

void sql_savepoint(sql_context_t *st, int, char *);

void sql_explain_table(sql_context_t *st, sql_src_list_t *table);

void sql_use_database(sql_context_t *ps, char *val);

#endif /* SQL_OPERATION_H */
