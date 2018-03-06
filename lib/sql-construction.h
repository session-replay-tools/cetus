#ifndef _SQL_CONSTRUCTION_
#define _SQL_CONSTRUCTION_

#include "sql-expression.h"


GString *sql_construct_expr(sql_expr_t *expr);

/* TODO: don't alloc inside function */
GString *sql_construct_select(sql_select_t *);

void sql_construct_insert(GString *, sql_insert_t *);

#endif /*_SQL_CONSTRUCTION_*/
