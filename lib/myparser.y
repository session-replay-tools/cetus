// All token codes are small integers with #defines that begin with "TK_"
%token_prefix TK_

// The type of the data attached to each token is Token.  This is also the
// default type for non-terminals.
//
%token_type {sql_token_t}
%default_type {sql_token_t}

// The generated parser function takes a 4th argument as follows:
%extra_argument {sql_context_t *context}

// This code runs whenever there is a syntax error
//
%syntax_error {
    UNUSED_PARAMETER(yymajor);  /* Silence some compiler warnings */
    #define MAX_LEN 256
    char msg[MAX_LEN] = {0};
    snprintf(msg, MAX_LEN, "near \"%s\": syntax error", TOKEN.z);
    sql_context_set_error(context, PARSE_SYNTAX_ERR, msg);
}

%stack_overflow {
    context->rc = PARSE_ERROR;
    sql_context_append_msg(context, "parser stack overflow");
}

// The name of the generated procedure that implements the parser
// is as follows:
%name sqlParser

// The following text is included near the beginning of the C source
// code file that implements the parser.
//
%include {
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include "sql-context.h"
#include "sql-expression.h"
#include "sql-operation.h"

#define UNUSED_PARAMETER(x) (void)(x)
/*
** Disable all error recovery processing in the parser push-down
** automaton.
*/
#define YYNOERRORRECOVERY 1

/*
** Make yytestcase() the same as testcase()
*/
/*#define yytestcase(X) testcase(X)*/

/*
** Indicate that sqlParserFree() will never be called with a null
** pointer.
*/
#define YYPARSEFREENEVERNULL 1

/*
** Alternative datatype for the argument to the malloc() routine passed
** into sqlParserAlloc().  The default is size_t.
*/
#define YYMALLOCARGTYPE  uint64_t

/*
** An instance of this structure holds information about the
** LIMIT clause of a SELECT statement.
*/
struct LimitVal {
  sql_expr_t *pLimit;    /* The LIMIT expression.  NULL if there is no limit */
  sql_expr_t *pOffset;   /* The OFFSET expression.  NULL if there is none */
};

/*
** An instance of this structure is used to store the LIKE,
** GLOB, NOT LIKE, and NOT GLOB operators.
*/
struct LikeOp {
  sql_token_t eOperator;  /* "like" or "glob" or "regexp" */
  int bNot;         /* True if the NOT keyword is present */
};

struct transact_feature_t {
    int rw_feature;
    int isolation_level;
};

struct idlist_opt_t {
  sql_id_list_t* list;
  const char* span_start;
  const char* span_end;
};

} // end %include

// Input is a single SQL command
input ::= cmdlist.
cmdlist ::= cmdlist ecmd.
cmdlist ::= ecmd.
ecmd ::= SEMI.
ecmd ::= cmdx SEMI. {
    context->stmt_count += 1;
    if (context->stmt_count > 1) {
        sql_context_set_error(context, PARSE_NOT_SUPPORT,
                                    "multi-statement not support");
    }
}

cmdx ::= cmd.
cmdx ::= select_stmt.
cmdx ::= update_stmt.
cmdx ::= delete_stmt.
cmdx ::= insert_stmt.
cmdx ::= drop_database_stmt.

///////////////////// EXPLAIN syntax ////////////////////////////
cmd ::= explain fullname(X) opt_col_name. {
    context->rw_flag |= CF_READ;
    sql_context_add_stmt(context, STMT_EXPLAIN_TABLE, X);
}
opt_col_name ::= ID|STRING.
opt_col_name ::= .

cmd ::= explain explainable_stmt.
explainable_stmt ::= select_stmt.
explainable_stmt ::= insert_stmt.
explainable_stmt ::= update_stmt.
explainable_stmt ::= delete_stmt.

explain ::= EXPLAIN. {context->explain = TK_EXPLAIN;}
explain ::= DESCRIBE. {context->explain = TK_EXPLAIN;}
explain ::= DESC. {context->explain = TK_EXPLAIN;}

//////////////////// SHARD_EXPLAIN syntax //////////////////////
cmd ::= SHARD_EXPLAIN explainable_stmt. {
    context->explain = TK_SHARD_EXPLAIN;
    context->clause_flags |= CF_LOCAL_QUERY;
}

///////////////////// SHOW syntax //////////////////////////////
cmd_head ::= SHOW opt_full. {
    context->rw_flag |= CF_READ;
    sql_context_add_stmt(context, STMT_SHOW, NULL);
    context->rc = PARSE_HEAD;
}
cmd ::= SHOW opt_full COLUMNS|FIELDS FROM fullname(X) opt_db opt_wild_or_where. {
    context->rw_flag |= CF_READ;
    sql_context_add_stmt(context, STMT_SHOW_COLUMNS, X);
}
cmd ::= SHOW CREATE VIEW|TABLE fullname(X). {
    context->rw_flag |= CF_READ;
    sql_context_add_stmt(context, STMT_SHOW_CREATE, X);
}
cmd ::= SHOW WARNINGS. {
    context->rw_flag |= CF_READ;
    sql_context_add_stmt(context, STMT_SHOW_WARNINGS, NULL);
}

opt_full ::= .
opt_full ::= JOIN_KW.

opt_db ::= .
opt_db ::= FROM|IN ID.

opt_wild_or_where ::= .
opt_wild_or_where ::= LIKE_KW STRING.
opt_wild_or_where ::= WHERE expr. /* destructor expr */

cmd ::= USE ID(X). {sql_use_database(context, sql_token_dup(X));}
cmd ::= CALL expr(X). {
    context->rw_flag |= CF_WRITE;
    sql_context_add_stmt(context, STMT_CALL, X);
}

cmd ::= XA RECOVER.
cmd ::= XA COMMIT STRING.
cmd ::= XA ROLLBACK STRING.

///////////////////// SET Command ///////////////////////////////
cmd ::= SET option_value_list(X). {
    sql_set_variable(context, X);
}
cmd ::= SET NAMES ID|STRING(X) opt_collate. {
    sql_set_names(context, sql_token_dup(X));
}
opt_collate ::= COLLATE ID|STRING.
opt_collate ::= .

cmd ::= SET opt_var_scope TRANSACTION transact_feature. {
    sql_context_add_stmt(context, STMT_SET_TRANSACTION, NULL);
}

%type transact_feature { struct transact_feature_t }
transact_feature(A) ::= READ ONLY. {
    A.rw_feature = TF_READ_ONLY;
    A.isolation_level = 0;
}
transact_feature(A) ::= READ WRITE. {
    A.rw_feature = TF_READ_WRITE;
    A.isolation_level = 0;
}
transact_feature(A) ::= ISOLATION LEVEL isolation_level(X). {
    A.isolation_level = X;
    A.rw_feature = 0;
}
%type isolation_level {int}
isolation_level(A) ::= REPEATABLE READ. {A = TF_REPEATABLE_READ;}
isolation_level(A) ::= READ COMMITTED. {A = TF_READ_COMMITTED;}
isolation_level(A) ::= READ UNCOMMITTED. {A = TF_READ_UNCOMMITTED;}
isolation_level(A) ::= SERIALIZABLE. {A = TF_SERIALIZABLE;}
%type opt_var_scope {enum sql_var_scope_t}
opt_var_scope(A) ::= GLOBAL. { A = SCOPE_GLOBAL; }
opt_var_scope(A) ::= SESSION. { A = SCOPE_SESSION; }
opt_var_scope(A) ::= . { A = SCOPE_TRANSIENT; }


%type option_type {enum sql_var_scope_t}
option_type(A) ::= GLOBAL. { A = SCOPE_GLOBAL; }
option_type(A) ::= SESSION. { A = SCOPE_SESSION; }
option_type(A) ::= LOCAL. {A = SCOPE_SESSION;}

%type opt_var_ident_type {enum sql_var_scope_t}
opt_var_ident_type(A) ::= AT_SIGN AT_SIGN. {A = SCOPE_SESSION;}
opt_var_ident_type(A) ::= AT_SIGN AT_SIGN GLOBAL DOT. {A = SCOPE_GLOBAL;}
opt_var_ident_type(A) ::= AT_SIGN AT_SIGN SESSION DOT. {A = SCOPE_SESSION;}
opt_var_ident_type(A) ::= AT_SIGN AT_SIGN LOCAL DOT. {A = SCOPE_SESSION;}

%type option_value {sql_expr_t*} // the [option = value] pair
%destructor option_value { sql_expr_free($$); }

// set var=xx
option_value(A) ::= internal_variable_name(X) EQ set_expr_or_default(Y). {
    X->var_scope = SCOPE_SESSION;
    A=spanBinaryExpr(TK_EQ, X, Y);
}
// set SCOPE var=xx
option_value(A) ::= option_type(S) internal_variable_name(X) EQ set_expr_or_default(Y).{
    X->var_scope = S;
    A=spanBinaryExpr(TK_EQ, X, Y);
}
// set @user_var=xx
option_value(A) ::= AT_SIGN ID(X) EQ expr(Y). {
    sql_expr_t* lhs = sql_expr_new(TK_ID, &X);
    lhs->var_scope = SCOPE_USER;
    A=spanBinaryExpr(TK_EQ, lhs, Y);
}
// set @@scope.var=xx
option_value(A) ::= opt_var_ident_type(S) internal_variable_name(X) EQ set_expr_or_default(Y). {
    X->var_scope = S;
    A=spanBinaryExpr(TK_EQ, X, Y);
}

%type internal_variable_name {sql_expr_t*}
%destructor internal_variable_name { sql_expr_free($$); }
internal_variable_name(A) ::= ID(X). { A = sql_expr_new(@X, &X); }
internal_variable_name(A) ::= ID(X) DOT ID(Y). {
    sql_expr_t* p1 = sql_expr_new(TK_ID, &X);
    sql_expr_t* p2 = sql_expr_new(TK_ID, &Y);
    A = sql_expr_new(TK_DOT, 0);
    sql_expr_attach_subtrees(A, p1, p2);
}

%type set_expr_or_default {sql_expr_t*}
%destructor set_expr_or_default { sql_expr_free($$); }
set_expr_or_default(A) ::= expr(A).
set_expr_or_default(A) ::= DEFAULT(X). { A = sql_expr_new(@X, &X); }
set_expr_or_default(A) ::= BINARY(X). { A = sql_expr_new(@X, &X); }
set_expr_or_default(A) ::= ON(X). {A = sql_expr_new(@X, &X);} // set xx=on
set_expr_or_default(A) ::= ALL(X). { A = sql_expr_new(@X, &X); }

%type option_value_list {sql_expr_list_t*}
%destructor option_value_list {sql_expr_list_free($$);}

option_value_list(A) ::= option_value_list(A) COMMA option_value(Y).
    {A = sql_expr_list_append(A,Y);}
option_value_list(A) ::= option_value(Y).
    {A = sql_expr_list_append(0,Y); /*A-overwrites-Y*/}

//////////////////////// KILL ! not supported ! ////////////////////////////
cmd ::= cmd_head ANY.
cmd_head ::= KILL. {
    sql_context_set_error(context, PARSE_NOT_SUPPORT,
                          "KILL not yet supported by proxy");
}

///////////////////// Begin and end transactions. ////////////////////////////
//
cmd ::= START trans_opt.  {sql_start_transaction(context);}
trans_opt ::= .
trans_opt ::= TRANSACTION.
trans_opt ::= TRANSACTION nm.

cmd ::= BEGIN.  {sql_start_transaction(context);}

cmd ::= COMMIT trans_opt.      {sql_commit_transaction(context);}
cmd ::= ROLLBACK trans_opt.    {sql_rollback_transaction(context);}

savepoint_opt ::= SAVEPOINT.
savepoint_opt ::= .
cmd ::= SAVEPOINT nm(X). {
  sql_savepoint(context, TK_SAVEPOINT, sql_token_dup(X));
}
cmd ::= RELEASE savepoint_opt nm(X). {
  sql_savepoint(context, TK_RELEASE, sql_token_dup(X));
}
cmd ::= ROLLBACK trans_opt TO savepoint_opt nm(X). {
  sql_savepoint(context, TK_ROLLBACK, sql_token_dup(X));
}

%include {
  /* This routine constructs a binary expression node out of two ExprSpan
  ** objects and uses the result to populate a new ExprSpan object.
  */
  static sql_expr_t* spanBinaryExpr(
    int op,             /* The binary operation */
    sql_expr_t* pLeft,    /* The left operand*/
    sql_expr_t *pRight    /* The right operand */
  ){
    sql_expr_t* the_op = sql_expr_new(op, 0);
    sql_expr_attach_subtrees(the_op, pLeft, pRight);
    return the_op; // output
  }

  /* If doNot is true, then add a TK_NOT Expr-node wrapper around the
  ** outside of *ppExpr. -- NOT BETWEEN/LIKE/IN --
  */
  static sql_expr_t* exprNot(int doNot, sql_expr_t *expr){
    if (doNot) {
      sql_expr_t* not_op = sql_expr_new(TK_NOT, 0);
      expr->flags |= EP_NOT;
      sql_expr_attach_subtrees(not_op, expr, NULL); // not->start <- expr->start
      not_op->end = expr->end;
      return not_op; // output
    }
    return expr;
  }

#define IS_PARSING_SELECT() (context->parsing_place >= SELECT_OPTION \
               && context->parsing_place < SELECT_DONE)

}

// Define operator precedence early so that this is the first occurrence
// of the operator tokens in the grammer.  Keeping the operators together
// causes them to be assigned integer values that are close together,
// which keeps parser tables smaller.
//
// The token values assigned to these symbols is determined by the order
// in which lemon first sees them.  It must be the case that ISNULL/NOTNULL,
// NE/EQ, GT/LE, and GE/LT are separated by only a single value.  See
// the sqlite3sql_expr_tIfFalse() routine for additional information on this
// constraint.
//
%left OR.
%left AND.
%right NOT.
%left IS MATCH LIKE_KW BETWEEN IN NE EQ.
%left GT LE LT GE.
%right ESCAPE.
%left BITAND BITOR LSHIFT RSHIFT.
%left PLUS MINUS.
%left STAR SLASH REM.
%left CONCAT.
%left COLLATE.
%right BITNOT.


// The following directive causes tokens ABORT, AFTER, ASC, etc. to
// fallback to ID if they will not parse as their original value.
// This obviates the need for the "id" nonterminal.
//
%fallback ID
  ABORT ACTION AFTER ANALYZE ASC ATTACH BEFORE BY CASCADE CAST COLUMNKW
  CONFLICT DESC DETACH EACH END FAIL FOR
  INITIALLY INSTEAD LIKE_KW MATCH NO PLAN
  QUERY KEY OF OFFSET PRAGMA RAISE RECURSIVE RELEASE REPLACE RESTRICT ROW
  ROLLBACK SAVEPOINT START TEMP TRIGGER VACUUM VIEW VIRTUAL WITH WITHOUT

  RENAME IF
  EXPLAIN DESCRIBE SHOW NAMES USE CALL
  CHARACTER CHARSET SESSION GLOBAL KILL ONLY
  DUPLICATE INTERVAL TIME_UNIT JOIN_KW SHARD_EXPLAIN
  COLUMNS FIELDS COMMENT_KW SHARE MODE TABLES LOCAL
  ISOLATION LEVEL COMMITTED UNCOMMITTED SERIALIZABLE REPEATABLE
  XA RECOVER WARNINGS
// HACK fallback
  CONSTRAINT CHECK AUTO_INCREMENT FOREIGN
  TRIM POSITION TRUNCATE SIGNED NCHAR
  .
%wildcard ANY.

// The name of a column or table can be any of the following:
%type nm {sql_token_t}
nm(A) ::= ID(A).
nm(A) ::= JOIN_KW(A).

// A typetoken is really zero or more tokens that form a type name such
// as can be found after the column name in a CREATE TABLE statement.
// Multiple tokens are concatenated to form the value of the typetoken.
//

%token_class number INTEGER|FLOAT.

// "carglist" is a list of additional constraints that come after the
// column name and column type in a CREATE TABLE statement.
//

////////////////////////// The DROP TABLE /////////////////////////////////////
//
cmd_head ::= ddl_cmd_head. {
    context->stmt_type = STMT_COMMON_DDL;
    context->rc = PARSE_HEAD;
    context->rw_flag |= CF_WRITE|CF_DDL;
}

ddl_cmd_head ::= DROP INDEX.
ddl_cmd_head ::= CREATE opt_unique INDEX.

ddl_cmd_head ::= CREATE VIEW.
ddl_cmd_head ::= ALTER VIEW.
ddl_cmd_head ::= DROP VIEW.

%token_class db_schema DATABASE|SCHEMA.
ddl_cmd_head ::= CREATE db_schema.
ddl_cmd_head ::= ALTER db_schema.

ddl_cmd_head ::= CREATE TABLE.
ddl_cmd_head ::= DROP TABLE.
ddl_cmd_head ::= TRUNCATE. /* optional TABLE */
ddl_cmd_head ::= ALTER TABLE.

opt_unique ::= UNIQUE.
opt_unique ::= .

////////////////////////// The DROP DATABASE /////////////////////////////////////
//
drop_database_stmt ::= DROP db_schema ifexists(A) nm(B). {
    sql_drop_database_t *p = sql_drop_database_new();
    sql_drop_database(context, p);
    p->schema_name = sql_token_dup(B);
    p->ifexists = A;
}

%type ifexists {int}
ifexists(A) ::= IF EXISTS.   {A = 1;}
ifexists(A) ::= .            {A = 0;}

//////////////////////// The SELECT statement /////////////////////////////////
//
select_stmt ::= select(X).  {
    context->rw_flag |= CF_READ;
    sql_select(context, X);
}

%type select { sql_select_t* }
%destructor select { sql_select_free($$); }
%type oneselect { sql_select_t* }
%destructor oneselect { sql_select_free($$); }

select(A) ::= oneselect(A).

select(A) ::= select(A) multiselect_op(Y) oneselect(Z). {
    sql_select_t* rhs = Z;
    sql_select_t* lhs = A;
    if (rhs) {
        rhs->op = Y;
        rhs->prior = lhs;//single list
    }
    A = rhs;
}

%type multiselect_op {int}
multiselect_op(A) ::= UNION. {A = TK_UNION;}
multiselect_op(A) ::= UNION ALL. {A = TK_UNION;}
multiselect_op(A) ::= UNION DISTINCT. {A = TK_UNION;}

oneselect(A) ::= SELECT select_options(D) selcollist(C) from(F) where_opt(W)
                 groupby_opt(G) having_opt(H) orderby_opt(O) limit_opt(L) lock_read(R). {
    A = sql_select_new();
    A->flags |= D;
    A->columns = C;
    A->from_src = F;
    A->where_clause = W;
    A->groupby_clause = G;
    A->having_clause = H;
    A->orderby_clause = O;
    A->limit = L.pLimit;
    A->offset = L.pOffset;
    A->lock_read = R;
    context->parsing_place = SELECT_DONE;
    context->is_parsing_subquery = 0; //since there are no nested subquery, good to set 0
}

%type values {sql_select_t*}
%destructor values { sql_select_free($$); }
values(A) ::= VALUES LP nexprlist(X) RP. {
  A = sql_select_new();
  A->columns = X;
}
values(A) ::= values(A) COMMA LP exprlist(Y) RP. {
  sql_select_t *right, *left = A;
  right = sql_select_new();
  if (right) {
    right->columns = Y;
    right->flags |= SF_MULTI_VALUE;
    right->prior = left;
    A = right;
  } else {
    A = left;
  }
}

// The "distinct" nonterminal is true (1) if the DISTINCT keyword is
// present and false (0) if it is not.
//
%type select_options {int}
%type select_option {int}
select_options(A) ::= . {
    A = 0;
    if (!context->allow_subquery_nesting && context->is_parsing_subquery) {
      sql_context_set_error(context, PARSE_NOT_SUPPORT,
           "(cetus) subquery nesting level too deep (1-level most)");
    }
    if (IS_PARSING_SELECT()) {
      context->is_parsing_subquery = 1;
    }
    context->parsing_place = SELECT_COLUMN;
}
select_options(A) ::= select_options(X) select_option(Y). {
    A = X|Y;
}
select_option(A) ::= DISTINCT.   {A = SF_DISTINCT;}
select_option(A) ::= ALL. {A = SF_ALL;}
select_option(A) ::= SQL_CALC_FOUND_ROWS.   {A = SF_CALC_FOUND_ROWS;}

%type distinct {int}
distinct(A) ::= DISTINCT. {A=1;}
distinct(A) ::= . {A=0;}

%type lock_read {int}
lock_read(A) ::= FOR UPDATE. {A=1;}
lock_read(A) ::= LOCK IN SHARE MODE. {A=1;}
lock_read(A) ::=. {A=0;}

// selcollist is a list of expressions that are to become the return
// values of the SELECT statement.  The "*" in statements like
// "SELECT * FROM ..." is encoded as a special expression with an
// opcode of TK_ASTERISK.
//
%type selcollist { sql_expr_list_t* }
%destructor selcollist { sql_expr_list_free($$); $$ = 0; }
%type sclp { sql_expr_list_t* }
%destructor sclp { sql_expr_list_free($$); $$ = 0; }
sclp(A) ::= selcollist(A) COMMA.
sclp(A) ::= .                    { A = 0; }
selcollist(A) ::= sclp(A) expr(B) as(C). {
    B->alias = sql_token_dup(C);
    A = sql_expr_list_append(A, B);
    context->parsing_place = SELECT_FROM;
}
selcollist(A) ::= sclp(A) STAR(B). {
    sql_expr_t* p = sql_expr_new(@B, &B);
    A = sql_expr_list_append(A, p);
    context->parsing_place = SELECT_FROM;
}
selcollist(A) ::= sclp(A) nm(B) DOT STAR(C). {
    sql_expr_t* left = sql_expr_new(TK_ID, &B);
    sql_expr_t* right = sql_expr_new(@C, &C);
    sql_expr_t* dot = sql_expr_new(TK_DOT, 0);
    sql_expr_attach_subtrees(dot, left, right);
    A = sql_expr_list_append(A, dot);
    context->parsing_place = SELECT_FROM;
}

// An option "AS <id>" phrase that can follow one of the expressions that
// define the result set, or one of the tables in the FROM clause.
//
%type as {sql_token_t}
as(X) ::= AS nm(Y).    {X = Y;}
as(X) ::= AS STRING(Y). {X = Y;}
as(X) ::= ID(X).
as(X) ::= .            {X.z = 0; X.n = 0;}


%type seltablist {sql_src_list_t*}
%destructor seltablist { sql_src_list_free($$); }
%type stl_prefix {sql_src_list_t*}
%destructor stl_prefix { sql_src_list_free($$); }
%type from {sql_src_list_t*}
%destructor from { sql_src_list_free($$); }

// A complete FROM clause.
//
from(A) ::= . {
  A = 0;
  context->parsing_place = SELECT_WHERE;
}
from(A) ::= FROM seltablist(X). {
  A = X;
  context->parsing_place = SELECT_WHERE;
}

// "seltablist" is a "Select Table List" - the content of the FROM clause
// in a SELECT statement.  "stl_prefix" is a prefix of this list.
//
stl_prefix(A) ::= seltablist(A) joinop(Y).    {
   if (A && A->len > 0) {
    sql_src_item_t* item = g_ptr_array_index(A, A->len-1);
    item->jointype = Y;
   }
}
stl_prefix(A) ::= .                           {A = 0;}
seltablist(A) ::= stl_prefix(A) nm(Y) dotnm(D) as(Z) index_hint(I) on_opt(N) using_opt(U). {
  if (D.n)
    A = sql_src_list_append(A,&D,&Y,I,&Z,0,N,U);
  else
    A = sql_src_list_append(A,&Y,0,I,&Z,0,N,U);
}
seltablist(A) ::= stl_prefix(A) nm(Y) dotnm(D) LP exprlist(E) RP as(Z)
                  on_opt(N) using_opt(U). {
  if (D.n)
    A = sql_src_list_append(A,&D,&Y,0,&Z,0,N,U);
  else
    A = sql_src_list_append(A,&Y,0,0,&Z,0,N,U);
  sql_src_item_t* item = g_ptr_array_index(A, A->len-1);
  item->func_arg = E;
}

seltablist(A) ::= stl_prefix(A) LP select(S) RP
                  as(Z) on_opt(N) using_opt(U). {
    context->clause_flags |= CF_SUBQUERY;
    A = sql_src_list_append(A,0,0,0,&Z,S,N,U);
}
seltablist(A) ::= stl_prefix(A) LP seltablist(F) RP
                  as(Z) on_opt(N) using_opt(U). {
    if (A==0 && Z.n==0 && N==0 && U==0) {
        A = F;
    } else if (F->len == 1) {
        A = sql_src_list_append(A,0,0,0,&Z,0,N,U);
        if (A) {
            printf("not implemented");//TODO;
        }
        sql_src_list_free(F);
    } else {
        printf("not implemented");//TODO
    }
}

%type dotnm {sql_token_t}
dotnm(A) ::= .          {A.z = 0; A.n = 0;}
dotnm(A) ::= DOT nm(X). {A = X;}

%type fullname {sql_src_list_t*}
%destructor fullname { sql_src_list_free($$);}
fullname(A) ::= nm(B) dotnm(C). {
  if (C.n)
    A = sql_src_list_append(0,&C,&B,0,0,0,0,0);
  else
    A = sql_src_list_append(0,&B,0,0,0,0,0,0);
}

%type joinop {int}
joinop(A) ::= COMMA|JOIN.              { A = JT_INNER; } //TODO: JOIN TYPE 
joinop(A) ::= JOIN_KW(X) JOIN. {
    A = sql_join_type(X);
}
joinop(A) ::= JOIN_KW(X) nm(Y) JOIN.{
    A = sql_join_type(X)|sql_join_type(Y);
}
joinop(A) ::= JOIN_KW(X) nm(Y) nm(Z) JOIN.{
    A = sql_join_type(X)|sql_join_type(Y)|sql_join_type(Z);
}

%type on_opt {sql_expr_t*}
%destructor on_opt {sql_expr_free($$);}
on_opt(N) ::= ON expr(E).   {N = E;}
on_opt(N) ::= .             {N = 0;}


%type using_opt {sql_id_list_t*}
%destructor using_opt {sql_id_list_free($$);}
using_opt(U) ::= USING LP idlist(L) RP.  {U = L;}
using_opt(U) ::= .                        {U = 0;}


%type orderby_opt {sql_column_list_t*}
%destructor orderby_opt {sql_column_list_free($$);}

// the sortlist non-terminal stores a list of expression where each
// expression is optionally followed by ASC or DESC to indicate the
// sort order.
//
%type sortlist {sql_column_list_t*}
%destructor sortlist {sql_column_list_free($$);}

orderby_opt(A) ::= .                          {A = 0;}
orderby_opt(A) ::= ORDER BY sortlist(X).      {A = X;}
sortlist(A) ::= sortlist(A) COMMA expr(Y) sortorder(Z). {
    sql_column_t* col = sql_column_new();
    col->expr = Y;
    col->sort_order = Z;
    A = sql_column_list_append(A, col);
}
sortlist(A) ::= expr(Y) sortorder(Z). {
    sql_column_t* col = sql_column_new();
    col->expr = Y;
    col->sort_order = Z;
    A = sql_column_list_append(0, col);
}

%type sortorder {int}

sortorder(A) ::= ASC.           {A = SQL_SO_ASC;}
sortorder(A) ::= DESC.          {A = SQL_SO_DESC;}
sortorder(A) ::= .              {A = SQL_SO_ASC;/*default to asc*/}

%type groupby_opt {sql_expr_list_t*}
%destructor groupby_opt {sql_expr_list_free($$);}
groupby_opt(A) ::= .                      {A = 0;}
groupby_opt(A) ::= GROUP BY nexprlist(X). {A = X;}

%type having_opt {sql_expr_t*}
%destructor having_opt {sql_expr_free($$);}
having_opt(A) ::= .                {A = 0;}
having_opt(A) ::= HAVING expr(X).  {A = X;}

%type limit_opt {struct LimitVal}

// The destructor for limit_opt will never fire in the current grammar.
// The limit_opt non-terminal only occurs at the end of a single production
// rule for SELECT statements.  As soon as the rule that create the 
// limit_opt non-terminal reduces, the SELECT statement rule will also
// reduce.  So there is never a limit_opt non-terminal on the stack 
// except as a transient.  So there is never anything to destroy.
//
//%destructor limit_opt {
//  sqlite3ExprDelete(pParse->db, $$.pLimit);
//  sqlite3ExprDelete(pParse->db, $$.pOffset);
//}
limit_opt(A) ::= .                    {A.pLimit = 0; A.pOffset = 0;}
limit_opt(A) ::= LIMIT expr(X).       {A.pLimit = X; A.pOffset = 0;}
limit_opt(A) ::= LIMIT expr(X) OFFSET expr(Y). 
                                      {A.pLimit = X; A.pOffset = Y;}
limit_opt(A) ::= LIMIT expr(X) COMMA expr(Y). 
                                      {A.pOffset = X; A.pLimit = Y;}

/////////////////////////// The DELETE statement /////////////////////////////
//
delete_stmt ::= DELETE FROM fullname(X) where_opt(W) orderby_opt(O) limit_opt(L). {
  sql_delete_t* del = sql_delete_new();
  del->from_src = X;
  del->where_clause = W;
  del->orderby_clause = O;
  del->limit = L.pLimit;
  del->offset = L.pOffset;
  sql_delete(context, del);
}

%type where_opt {sql_expr_t*}
%destructor where_opt {sql_expr_free($$);}

where_opt(A) ::= .                    {A = 0;}
where_opt(A) ::= where_sym expr(X). {
    A = X;
    if (context->where_flags & EP_LAST_INSERT_ID) {
        sql_context_set_error(context, PARSE_NOT_SUPPORT,
             "(proxy unsupported) last_insert_id in WHERE clause");
    }
}

where_sym ::= WHERE. {
    context->where_flags = 0;
}


////////////////////////// The UPDATE command ////////////////////////////////
//
update_stmt ::= UPDATE table_reference(X) SET update_list(Y) where_opt(W) orderby_opt(O) limit_opt(L).  {
  sql_update_t* p = sql_update_new();
  p->table_reference = X;
  p->set_list = Y;
  p->where_clause = W;
  p->orderby_clause = O;
  p->limit = L.pLimit;
  p->offset = L.pOffset;
  sql_update(context, p);
}


%type update_list {sql_expr_list_t*}
%destructor update_list {sql_expr_list_free($$);}
update_list(A) ::= update_list(A) COMMA update_elem(Y). {
  A = sql_expr_list_append(A, Y);
}
update_list(A) ::= update_elem(Y). {
  A = sql_expr_list_append(0, Y);
}


%type update_elem {sql_expr_t*}
%destructor update_elem { sql_expr_free($$); }
update_elem(A) ::= simple_ident_nospvar(X) EQ expr(Y). {
  A = spanBinaryExpr(TK_EQ, X, Y);
}


%type simple_ident_nospvar {sql_expr_t*} // not stored program variable
%destructor simple_ident_nospvar { sql_expr_free($$); }
simple_ident_nospvar(A) ::= ID(X). {
  A = sql_expr_new(@X, &X);
}
simple_ident_nospvar(A) ::= simple_ident_q(A).


%type simple_ident_q {sql_expr_t*} // qualified id
%destructor simple_ident_q { sql_expr_free($$); }
simple_ident_q(A) ::= ID(X) DOT ID(Y). {
    sql_expr_t* p1 = sql_expr_new(TK_ID, &X);
    sql_expr_t* p2 = sql_expr_new(TK_ID, &Y);
    A = sql_expr_new(TK_DOT, 0);
    sql_expr_attach_subtrees(A, p1, p2);
}
simple_ident_q(A) ::= DOT ID(X) DOT ID(Y). {
    sql_expr_t* p1 = sql_expr_new(TK_ID, &X);
    sql_expr_t* p2 = sql_expr_new(@Y, &Y);
    A = sql_expr_new(TK_DOT, 0);
    sql_expr_attach_subtrees(A, p1, p2);
}
simple_ident_q(A) ::= ID(X) DOT ID(Y) DOT ID(Z). {
    sql_expr_t* p1 = sql_expr_new(TK_ID, &X);
    sql_expr_t* p2 = sql_expr_new(TK_ID, &Y);
    sql_expr_t* p3 = sql_expr_new(TK_ID, &Z);
    sql_expr_t* p4 = sql_expr_new(TK_DOT, 0);
    sql_expr_attach_subtrees(p4, p2, p3);
    A = sql_expr_new(TK_DOT, 0);
    sql_expr_attach_subtrees(A, p1, p4);
}


%type table_reference {sql_table_reference_t*}
%destructor table_reference {sql_table_reference_free($$);}
table_reference(A) ::= fullname(F) as index_hint(I). {
    sql_table_reference_t * tr = sql_table_reference_new();
    tr->table_list = F;
    tr->index_hint = I;
    A = tr;
}

%type index_hint { sql_index_hint_t* }
%destructor index_hint { sql_index_hint_free($$); }
index_hint(I) ::= . {
    I = 0;
}
index_hint(I) ::= USE INDEX index_for LP index_list(L) RP. {
    sql_index_hint_t *ih = sql_index_hint_new();
    ih->type = IH_USE_INDEX;
    ih->names = L;
    I = ih;
}
index_hint(I) ::= USE KEY index_for LP index_list(L) RP. {
    sql_index_hint_t *ih = sql_index_hint_new();
    ih->type = IH_USE_KEY;
    ih->names = L;
    I = ih;
}
index_hint(I) ::= IGNORE INDEX index_for LP index_list(L) RP. {
    sql_index_hint_t *ih = sql_index_hint_new();
    ih->type = IH_IGNORE_INDEX;
    ih->names = L;
    I = ih;
}
index_hint(I) ::= IGNORE KEY index_for LP index_list(L) RP. {
    sql_index_hint_t *ih = sql_index_hint_new();
    ih->type = IH_IGNORE_KEY;
    ih->names = L;
    I = ih;
}
index_hint(I) ::= FORCE INDEX index_for LP index_list(L) RP. {
    sql_index_hint_t *ih = sql_index_hint_new();
    ih->type = IH_FORCE_INDEX;
    ih->names = L;
    I = ih;
}
index_hint(I) ::= FORCE KEY index_for LP index_list(L) RP. {
    sql_index_hint_t *ih = sql_index_hint_new();
    ih->type = IH_FORCE_KEY;
    ih->names = L;
    I = ih;
}
index_for ::= .
index_for ::= FOR JOIN.
index_for ::= FOR ORDER BY.
index_for ::= FOR GROUP BY.

%type index_list { sql_id_list_t* }
%destructor index_list { sql_id_list_free($$); }
index_list(L) ::= index_list(L) COMMA index_name(N). {
    L = sql_id_list_append(L, &N);
}
index_list(L) ::= index_name(N). {
    L = sql_id_list_append(0, &N);
}

%type index_name {sql_token_t}
index_name(N) ::= ID(N).
index_name(N) ::= PRIMARY(N).

////////////////////////// The INSERT command /////////////////////////////////
//
insert_stmt ::= insert_cmd(R) INTO fullname(X) idlist_opt(F) values(S) opt_insert_update_list(U). {
  sql_insert_t* p = sql_insert_new();
  p->is_replace = R;
  p->table = X;
  p->columns = F.list;
  p->columns_start = F.span_start;
  p->columns_end = F.span_end;
  p->sel_val = S;
  p->update_list = U;
  sql_insert(context, p);
}
insert_stmt ::= insert_cmd(R) INTO fullname(X) idlist_opt(F) select(S). {
  sql_insert_t* p = sql_insert_new();
  p->is_replace = R;
  p->table = X;
  p->columns = F.list;
  p->columns_start = F.span_start;
  p->columns_end = F.span_end;
  p->sel_val = S;
  sql_insert(context, p);
}
insert_stmt ::= insert_cmd(R) INTO fullname(X) idlist_opt(F) DEFAULT VALUES. {
  sql_insert_t* p = sql_insert_new();
  p->is_replace = R;
  p->table = X;
  p->columns = F.list;
  p->columns_start = F.span_start;
  p->columns_end = F.span_end;
  p->sel_val = 0;
  sql_insert(context, p);
}
%type opt_insert_update_list {sql_expr_list_t*}
%destructor opt_insert_update_list {sql_expr_list_free($$);}
opt_insert_update_list(A) ::= . { A = NULL; }
opt_insert_update_list(A) ::= ON DUPLICATE KEY UPDATE update_list(X). { A = X; }

%type insert_cmd {int}
insert_cmd(A) ::= INSERT. {A=0;}
insert_cmd(A) ::= REPLACE.  {A = 1;}
insert_cmd(A) ::= INSERT IGNORE. {A=1;}

%type idlist_opt {struct idlist_opt_t}
%destructor idlist_opt {sql_id_list_free($$.list);}
%type idlist {sql_id_list_t*}
%destructor idlist {sql_id_list_free($$);}

idlist_opt(A) ::= . {
  A.list = 0;
  A.span_start = A.span_end = 0;
}
idlist_opt(A) ::= LP(U) idlist(X) RP(V).    {
  A.list = X;
  A.span_start = U.z;
  A.span_end = V.z + V.n;
}
idlist(A) ::= idlist(A) COMMA nm(Y).
    {A = sql_id_list_append(A,&Y);}
idlist(A) ::= nm(Y).
    {A = sql_id_list_append(0,&Y); /*A-overwrites-Y*/}

%type concat_str {sql_token_t}
concat_str(A) ::= STRING(A).
concat_str(A) ::= concat_str(A) STRING. // TODO: collect all

%type text_literal {sql_token_t}
text_literal(A) ::= UNDERSCORE_CHARSET STRING(X). { A = X; }

/////////////////////////// sql_expression Processing /////////////////////////////
//
%include {
  static void spanSet(sql_expr_t *pOut, sql_token_t *pStart, sql_token_t *pEnd){
    pOut->start = pStart->z;
    pOut->end = &pEnd->z[pEnd->n];
  }

  static sql_expr_t* function_expr_new(sql_token_t* name,
                                       sql_expr_list_t* args, sql_token_t* rparenth)
  {
      sql_expr_t* func_expr = sql_expr_new(TK_FUNCTION, name);
      func_expr->flags |= EP_FUNCTION;
      func_expr->list = args;
      if (rparenth) {
          func_expr->end = &rparenth->z[rparenth->n];
      }
      return func_expr;
  }
}

%type expr {sql_expr_t*}
%destructor expr { sql_expr_free($$); }
%type term {sql_expr_t*}
%destructor term { sql_expr_free($$); }

expr(A) ::= term(A).
expr(A) ::= LP(B) expr(X) RP(E). {
    A = X;
    spanSet(A, &B, &E);
}
term(A) ::= NULL(X).   {A = sql_expr_new(@X, &X);}
expr(A) ::= ID(X).     {A = sql_expr_new(@X, &X);}
expr(A) ::= JOIN_KW(X).      {A = sql_expr_new(@X, &X);}
expr(A) ::= nm(X) DOT nm(Y). {
    sql_expr_t* p1 = sql_expr_new(TK_ID, &X);
    sql_expr_t* p2 = sql_expr_new(TK_ID, &Y);
    A = sql_expr_new(TK_DOT, 0);
    sql_expr_attach_subtrees(A, p1, p2);
}

expr(A) ::= nm(X) DOT ANY(Y). {// allow x.reserved-keyword
    sql_expr_t* p1 = sql_expr_new(TK_ID, &X);
    sql_expr_t* p2 = sql_expr_new(@Y, &Y);
    A = sql_expr_new(TK_DOT, 0);
    sql_expr_attach_subtrees(A, p1, p2);
}

expr(A) ::= nm(X) DOT nm(Y) DOT nm(Z). {
    sql_expr_t* p1 = sql_expr_new(TK_ID, &X);
    sql_expr_t* p2 = sql_expr_new(TK_ID, &Y);
    sql_expr_t* p3 = sql_expr_new(TK_ID, &Z);
    sql_expr_t* p4 = sql_expr_new(TK_DOT, 0);
    sql_expr_attach_subtrees(p4, p2, p3);
    A = sql_expr_new(TK_DOT, 0);
    sql_expr_attach_subtrees(A, p1, p4);
}
term(A) ::= INTEGER|FLOAT|BIN_NUM|HEX_NUM|BLOB(X). {A = sql_expr_new(@X, &X);}
term(A) ::= concat_str(X).         {A = sql_expr_new(TK_STRING, &X);} //TODO: span error
term(A) ::= text_literal(X).    {A = sql_expr_new(TK_NOT_HANDLED, &X);}
expr(A) ::= VARIABLE(X).          {A = sql_expr_new(@X, &X);}
expr(A) ::= expr(A) COLLATE ID|STRING(X).  {
    sql_expr_t* coll = sql_expr_new(@X, &X);
    sql_expr_attach_subtrees(A, coll, NULL);
    A->end = &X.z[X.n];
}

%type func_expr {sql_expr_t*}
%destructor func_expr { sql_expr_free($$); }

expr(A) ::= func_expr(X). {
    A = X;
    context->where_flags |= EP_FUNCTION;
}

func_expr(A) ::= ID(X) LP distinct(D) exprlist(Y) RP(R). {
    A = function_expr_new(&X, Y, &R);
    if (strncasecmp(X.z, "last_insert_id", X.n) == 0) {
        sql_context_set_error(context, PARSE_NOT_SUPPORT,
            "(proxy)LAST_INSERT_ID() not supported");
        //TODO: parse interupted, func_expr free?
    }
    if (sql_aggregate_type(X.z) != FT_UNKNOWN) {
        A->flags |= EP_AGGREGATE;
        if (context->parsing_place == SELECT_COLUMN) {
            context->clause_flags |= CF_AGGREGATE;
        }
    }
    if (D) {
        A->flags |= EP_DISTINCT;
        context->clause_flags |= CF_DISTINCT_AGGR;
    }
}
func_expr(A) ::= ID(X) LP STAR RP(R). {
    A = function_expr_new(&X, 0, &R);
    if (sql_aggregate_type(X.z) != FT_UNKNOWN) {
        A->flags |= EP_AGGREGATE;
        if (context->parsing_place == SELECT_COLUMN) {
            context->clause_flags |= CF_AGGREGATE;
        }
    }
}
func_expr(A) ::= JOIN_KW(N) LP expr(X) COMMA expr(Y) RP(R). {
    sql_expr_list_t *args = sql_expr_list_append(0, X);
    sql_expr_list_append(args, Y);
    A = function_expr_new(&N, args, &R);
}
func_expr(A) ::= VALUES(N) LP simple_ident_nospvar(X) RP(R). {
    sql_expr_list_t *args = sql_expr_list_append(0, X);
    A = function_expr_new(&N, args, &R);
}
func_expr(A) ::= INSERT(N) LP expr(X) COMMA expr(Y) COMMA expr(Z) COMMA expr(W) RP(R). {
    sql_expr_list_t *args = sql_expr_list_append(0, X);
    sql_expr_list_append(args, Y);
    sql_expr_list_append(args, Z);
    sql_expr_list_append(args, W);
    A = function_expr_new(&N, args, &R);
}
// TRIM, POSITION includes LP in itself, hack for special function
func_expr(A) ::= TRIM(N) expr RP(R). {
    A = function_expr_new(&N, 0, &R);
}
func_expr(A) ::= TRIM(N) expr FROM expr RP(R). {
    A = function_expr_new(&N, 0, &R);
}
func_expr(A) ::= TRIM(N) TRIM_SPEC expr FROM expr RP(R). {
    A = function_expr_new(&N, 0, &R);
}
func_expr(A) ::= TRIM(N) TRIM_SPEC FROM expr RP(R). {
    A = function_expr_new(&N, 0, &R);
}
func_expr(A) ::= POSITION(N) STRING IN expr RP(R). {
    A = function_expr_new(&N, 0, &R);
}
func_expr(A) ::= CURRENT_DATE(N) opt_parentheses. {
    A = function_expr_new(&N, 0, NULL);
    if (context->parsing_place == SELECT_COLUMN) {
        context->clause_flags |= CF_LOCAL_QUERY;
    }
}
func_expr(A) ::= CETUS_SEQUENCE(N) opt_parentheses. {
    A = function_expr_new(&N, 0, NULL);
    if (context->parsing_place == SELECT_COLUMN) {
        context->clause_flags |= CF_LOCAL_QUERY;
    }
}
func_expr(A) ::= CETUS_VERSION(N) opt_parentheses. {
    A = function_expr_new(&N, 0, NULL);
    if (context->parsing_place == SELECT_COLUMN) {
        context->clause_flags |= CF_LOCAL_QUERY;
    }
}
func_expr(A) ::= CAST(N) LP expr AS cast_type RP(R). {
    A = function_expr_new(&N, 0, &R);
}
func_expr(A) ::= DATABASE(N) LP RP(R). {
    A = function_expr_new(&N, 0, &R);
}

opt_parentheses ::= LP RP.
opt_parentheses ::= .

expr(A) ::= expr(A) AND expr(Y).    {A=spanBinaryExpr(TK_AND, A, Y);}
expr(A) ::= expr(A) OR expr(Y).     {A=spanBinaryExpr(TK_OR, A, Y);}
expr(A) ::= expr(A) LT|GT|GE|LE(OP) expr(Y).
                                        {A=spanBinaryExpr(@OP, A, Y);}
expr(A) ::= expr(A) EQ|NE(OP) expr(Y).  {A=spanBinaryExpr(@OP, A, Y);}
expr(A) ::= expr(A) BITAND|BITOR|LSHIFT|RSHIFT(OP) expr(Y).
                                        {A=spanBinaryExpr(@OP, A, Y);}
expr(A) ::= expr(A) PLUS|MINUS(OP) expr(Y).
                                        {A=spanBinaryExpr(@OP, A, Y);}
expr(A) ::= expr(A) STAR|SLASH|REM(OP) expr(Y).
                                        {A=spanBinaryExpr(@OP, A, Y);}
expr(A) ::= expr(A) CONCAT(OP) expr(Y). {A=spanBinaryExpr(@OP, A, Y);}

expr(A) ::= variable(A).

%type variable {sql_expr_t*}
%destructor variable {sql_expr_free($$);}

variable(A) ::= opt_var_ident_type(S) ID(X). { // sys var
    A = sql_expr_new(@X, &X); //TODO: span error
    A->var_scope = S;
    if (strcasecmp(X.z, "last_insert_id") == 0) {
        context->where_flags |= EP_LAST_INSERT_ID;
    }
}
variable(A) ::= AT_SIGN ID(X). {A = sql_expr_new(@X, &X);} // user var

expr(A) ::= predicate(A).

%type likeop {struct LikeOp}
likeop(A) ::= LIKE_KW|MATCH(X). {A.eOperator = X; A.bNot = 0;/*A-overwrites-X*/}
likeop(A) ::= NOT LIKE_KW|MATCH(X). {A.eOperator = X; A.bNot = 1;}

%type predicate {sql_expr_t*}
%destructor predicate {sql_expr_free($$);}
predicate(A) ::= expr(X) likeop(OP) expr(Y). [LIKE_KW]  {
  sql_expr_list_t *args = sql_expr_list_append(0, X);
  args = sql_expr_list_append(args, Y);
  sql_expr_t* like_expr = sql_expr_new(TK_LIKE_KW, 0);
  if (like_expr) {
      like_expr->list = args;
      like_expr->start = X->start;
      like_expr->end = Y->end;
  } else {
      sql_expr_list_free(args);
  }
  A = exprNot(OP.bNot, like_expr);
}
predicate(A) ::= expr(X) likeop(OP) expr(Y) ESCAPE expr(E).  [LIKE_KW]  {
  sql_expr_list_t *args = sql_expr_list_append(0, X);
  args = sql_expr_list_append(args, Y);
  args = sql_expr_list_append(args, E);
  sql_expr_t* like_expr = sql_expr_new(TK_LIKE_KW, 0);
  if (like_expr) {
      like_expr->list = args;
      like_expr->start = X->start;
      like_expr->end = E->end;
  } else {
      sql_expr_list_free(args);
  }
  A = exprNot(OP.bNot, like_expr);
}

expr(A) ::= expr(A) IS expr(Y).     {
  A=spanBinaryExpr(TK_IS,A,Y);
}
expr(A) ::= expr(A) IS NOT expr(Y). {
  A=spanBinaryExpr(TK_ISNOT,A,Y);
}

%include {
  /* Construct an expression node for a unary prefix operator
  */
  static sql_expr_t* spanUnaryPrefix(
    int op,                /* The operator */
    sql_expr_t* pOperand,    /* The operand */
    sql_token_t* token
  ){
    sql_expr_t* the_op = sql_expr_new(op, 0);
    sql_expr_attach_subtrees(the_op, pOperand, NULL);
    the_op->start = token->z;
    the_op->end = pOperand->end;
    return the_op;
  }
}


expr(A) ::= NOT(B) expr(X).
              {A=spanUnaryPrefix(TK_NOT, X, &B);/*A-overwrites-B*/}
expr(A) ::= BITNOT(B) expr(X).
              {A=spanUnaryPrefix(TK_BITNOT, X, &B);/*A-overwrites-B*/}
expr(A) ::= MINUS(B) expr(X). [BITNOT]
              {A=spanUnaryPrefix(TK_UMINUS, X, &B);/*A-overwrites-B*/}
expr(A) ::= PLUS(B) expr(X). [BITNOT]
              {A=spanUnaryPrefix(TK_UPLUS, X, &B);/*A-overwrites-B*/}

%type between_op {int}
between_op(A) ::= BETWEEN.     {A = 0;}
between_op(A) ::= NOT BETWEEN. {A = 1;}
expr(A) ::= expr(A) between_op(N) expr(X) AND expr(Y). [BETWEEN] {
  sql_expr_list_t* pList = sql_expr_list_append(0, X);
  pList = sql_expr_list_append(pList, Y);
  sql_expr_t* betw_op = sql_expr_new(TK_BETWEEN, 0);
  if (betw_op) {
      sql_expr_attach_subtrees(betw_op, A, NULL);
      betw_op->list = pList;
      betw_op->end = Y->end;
      A = betw_op;
  } else {
      sql_expr_list_free(pList);
  } 
  A = exprNot(N, A);
}

%type in_op {int}
in_op(A) ::= IN.      {A = 0;}
in_op(A) ::= NOT IN.  {A = 1;}
expr(A) ::= expr(B) in_op(N) LP exprlist(Y) RP(R). [IN] {
    A = sql_expr_new(TK_IN, 0);
    if (A) {
        sql_expr_attach_subtrees(A, B, NULL);
        A->list = Y;
        A->end = &R.z[R.n];
    } else {
        sql_expr_list_free(Y);
    }
    A = exprNot(N, A);
}
expr(A) ::= LP(L) select(X) RP(R). {
  context->clause_flags |= CF_SUBQUERY;
  A = sql_expr_new(TK_SELECT, 0);
  if (A) {
    spanSet(A, &L, &R);
    A->select = X;
  } else {
    sql_expr_free(X);
  };
}
/* example: (id, name) IN (select ..) */
expr(A) ::= LP(L) expr(B) COMMA nexprlist RP in_op(N) LP select(Y) RP(R). [IN] {
  A = sql_expr_new(TK_IN, 0);
  if (A) {
      A->select = Y;
      sql_expr_attach_subtrees(A, B, NULL);
      spanSet(A, &L, &R);
  } else {
      sql_select_free(Y);
      sql_expr_free(B);
  }
  A = exprNot(N, A);
}
expr(A) ::= expr(B) in_op(N) LP select(Y) RP(R).  [IN] {
  A = sql_expr_new(TK_IN, 0);
  if (A) {
      sql_expr_attach_subtrees(A, B, NULL);
      A->end = &R.z[R.n];
      A->select = Y;
  } else {
      sql_select_free(Y);
      sql_expr_free(B);
  }
  A = exprNot(N, A);
}
expr(A) ::= EXISTS(E) LP select(Y) RP(R). {
  A = sql_expr_new(TK_EXISTS, 0);
  A->select = Y;
  spanSet(A, &E, &R);
}

expr(A) ::= INTERVAL(I) expr TIME_UNIT(T). {
    context->where_flags |= EP_INTERVAL;
    A = sql_expr_new(TK_INTERVAL, 0); //TODO: interval?
    spanSet(A, &I, &T);
}

/* CASE expressions */
expr(A) ::= CASE(C) case_operand(X) case_exprlist(Y) case_else(Z) END(E). {
  A = sql_expr_new(TK_CASE, 0);
  if (A) {
      sql_expr_attach_subtrees(A, X, NULL);
      A->list = Z ? sql_expr_list_append(Y, Z) : Y;
      spanSet(A, &C, &E);
  } else {
      sql_expr_list_free(Y);
      sql_expr_free(Z);
  }
  context->where_flags |= EP_CASE_WHEN;
}

%type case_exprlist {sql_expr_list_t*}
%destructor case_exprlist {sql_expr_list_free($$);}
case_exprlist(A) ::= case_exprlist(A) WHEN expr(Y) THEN expr(Z). {
  A = sql_expr_list_append(A, Y);
  A = sql_expr_list_append(A, Z);
}
case_exprlist(A) ::= WHEN expr(Y) THEN expr(Z). {
  A = sql_expr_list_append(0, Y);
  A = sql_expr_list_append(A, Z);
}

%type case_else {sql_expr_t*}
%destructor case_else {sql_expr_free($$);}
case_else(A) ::=  ELSE expr(X).         {A = X;} //TODO: span
case_else(A) ::=  .                     {A = 0;} 

%type case_operand {sql_expr_t*}
%destructor case_operand {sql_expr_free($$);}
case_operand(A) ::= expr(X).            {A = X; /*A-overwrites-X*/} 
case_operand(A) ::= .                   {A = 0;} 

%type exprlist {sql_expr_list_t*}
%destructor exprlist {sql_expr_list_free($$);}
%type nexprlist {sql_expr_list_t*}
%destructor nexprlist {sql_expr_list_free($$);}

exprlist(A) ::= nexprlist(A).
exprlist(A) ::= .  {A = 0;}
nexprlist(A) ::= nexprlist(A) COMMA expr(Y).
    {A = sql_expr_list_append(A,Y);}
nexprlist(A) ::= expr(Y).
    {A = sql_expr_list_append(0,Y); /*A-overwrites-Y*/}


// The eidlist non-terminal (sql_expr_tession Id List) generates an sql_expr_list_t
// from a list of identifiers.  The identifier names are in sql_expr_list_t.a[].zName.
// This list is stored in an sql_expr_list_t rather than an sql_id_list_t so that it
// can be easily sent to sqlite3Columnssql_expr_list_t().
//
// eidlist is grouped with CREATE INDEX because it used to be the non-terminal
// used for the arguments to an index.  That is just an historical accident.
//
// IMPORTANT COMPATIBILITY NOTE:  Some prior versions of SQLite accepted
// COLLATE clauses and ASC or DESC keywords on ID lists in inappropriate
// places - places that might have been stored in the sqlite_master schema.
// Those extra features were ignored.  But because they might be in some
// (busted) old databases, we need to continue parsing them when loading
// historical schemas.
//
%type eidlist {sql_expr_list_t*}
%destructor eidlist {sql_expr_list_free($$);}
%type eidlist_opt {sql_expr_list_t*}
%destructor eidlist_opt {sql_expr_list_free($$);}


///////////////////////LOCK TABLES///////////////////////////
cmd ::= LOCK TABLES lock_tables. {
    sql_context_set_error(context, PARSE_NOT_SUPPORT,
                          "(cetus) LOCK TABLES not supported");
}
lock_tables ::= fullname as lock_type.
lock_tables ::= lock_tables COMMA fullname as lock_type.
lock_type ::= READ opt_local.
lock_type ::= opt_priority WRITE.
opt_local ::= LOCAL.
opt_local ::= .
opt_priority ::= LOW_PRIORITY.
opt_priority ::= .
cmd ::= UNLOCK TABLES. {
    sql_context_set_error(context, PARSE_NOT_SUPPORT,       
                          "(cetus) UNLOCK TABLES not supported");
}

cast_type ::= SIGNED.
cast_type ::= UNSIGNED.
cast_type ::= SIGNED INT_SYM.
cast_type ::= UNSIGNED INT_SYM.
cast_type ::= BINARY opt_field_length.
cast_type ::= NCHAR opt_field_length.
cast_type ::= DECIMAL float_options.

float_options ::= .
float_options ::= field_length.
float_options ::= precision.

precision ::= LP INTEGER COMMA INTEGER RP.

field_length ::= LP INTEGER RP.
opt_field_length ::= .
opt_field_length ::= field_length.

///////////////////////FLUSH TABLES///////////////////////////
cmd ::=FLUSH flush_tables. {
    sql_context_set_error(context, PARSE_NOT_SUPPORT,
                        "(cetus) FLUSH TABLES not supported");
}
flush_tables ::= tables_option.
flush_tables ::= LOCAL tables_option.
flush_tables ::= NO_WRITE_TO_BINLOG tables_option.
tables_option ::= TABLES WITH READ LOCK.
tables_option ::= TABLES tbl_list WITH READ LOCK.
tables_option ::= TABLE WITH READ LOCK.
tables_option ::= TABLE tbl_list WITH READ LOCK.
tbl_list ::= fullname.
tbl_list ::= tbl_list COMMA fullname.
