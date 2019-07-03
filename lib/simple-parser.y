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
    // since this is an incomplete parser, we don't know for sure whether a clause
    // has syntax error, just leave it to the backend
    context->rc = PARSE_UNRECOGNIZED;
    context->rw_flag |= CF_WRITE;// unrecognized sql direct to WRITE server
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

} // end %include

// Input is a single SQL command
input ::= cmdlist.
cmdlist ::= cmdlist ecmd.
cmdlist ::= ecmd.
ecmd ::= SEMI.
ecmd ::= cmdx SEMI. {
    context->stmt_count += 1;
}

cmdx ::= cmd.
cmdx ::= select_stmt.
cmdx ::= update_stmt.
cmdx ::= delete_stmt.
cmdx ::= insert_stmt.

///////////////////// EXPLAIN syntax ////////////////////////////
cmd ::= explain fullname opt_col_name.
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
}

///////////////////// Mysql Special //////////////////////////////
cmd_head ::= SHOW. {
    context->rw_flag |= CF_READ;
    sql_context_add_stmt(context, STMT_SHOW, NULL);
    context->rc = PARSE_HEAD;
}
cmd ::= SHOW WARNINGS. {
    context->rw_flag |= CF_READ;
    sql_context_add_stmt(context, STMT_SHOW_WARNINGS, NULL);
}
cmd ::= USE ID(X). {sql_use_database(context, sql_token_dup(X));}
cmd ::= CALL expr(X). {
    context->rw_flag |= CF_WRITE;
    sql_context_add_stmt(context, STMT_CALL, X);
}

anylist ::= ANY.
anylist ::= anylist ANY.

///////////////////// SET Command ///////////////////////////////
cmd ::= SET nexprlist(X). {
    sql_set_variable(context, X);
}
cmd ::= SET NAMES ID|STRING(X). {
    sql_set_names(context, sql_token_dup(X));
}

cmd ::= SET opt_var_scope(S) TRANSACTION transact_feature(T). {
    context->rc = PARSE_HEAD;
    sql_set_transaction(context, S, T.rw_feature, T.isolation_level);
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

%type var_scope {enum sql_var_scope_t}
var_scope(A) ::= GLOBAL. { A = SCOPE_GLOBAL; }
var_scope(A) ::= SESSION. { A = SCOPE_SESSION; }
var_scope(A) ::= AT_SIGN AT_SIGN GLOBAL DOT. { A = SCOPE_GLOBAL; }
var_scope(A) ::= AT_SIGN AT_SIGN SESSION DOT. { A = SCOPE_SESSION; }
var_scope(A) ::= AT_SIGN AT_SIGN. { A = SCOPE_SESSION; }
var_scope(A) ::= AT_SIGN. { A = SCOPE_USER; }

%type opt_var_scope {enum sql_var_scope_t}
opt_var_scope(A) ::= GLOBAL. { A = SCOPE_GLOBAL; }
opt_var_scope(A) ::= SESSION. { A = SCOPE_SESSION; }
opt_var_scope(A) ::= . { A = SCOPE_TRANSIENT; }


//////////////////////// KILL ! not supported ! ////////////////////////////
cmd ::= cmd_head ANY.
cmd_head ::= KILL. {
    sql_context_set_error(context, PARSE_NOT_SUPPORT,
                               "KILL not yet supported by proxy");
}

///////////////////// Begin and end transactions. ////////////////////////////
//

%token_class begin_trans BEGIN|START.
cmd ::= begin_trans trans_opt.  {sql_start_transaction(context);}
trans_opt ::= .
trans_opt ::= TRANSACTION.
trans_opt ::= TRANSACTION nm.

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

///////////////////// The CREATE TABLE statement ////////////////////////////
//
cmd ::= create_table create_table_args.
create_table ::= CREATE temp TABLE ifnotexists nm dotnm. {
    context->rw_flag |= CF_WRITE;
}

%type ifnotexists {int}
ifnotexists(A) ::= .              {A = 0;}
ifnotexists(A) ::= IF NOT EXISTS. {A = 1;}
%type temp {int}
temp(A) ::= TEMP.  {A = 1;}
temp(A) ::= .      {A = 0;}

create_table_args ::= LP columnlist conslist_opt RP table_options.
create_table_args ::= AS select.
//%type table_options {sql_expr_list_t*}
//%destructor table_options {sql_expr_list_free($$);}
table_options ::= .
table_options ::= table_optlist.

table_optlist ::= table_opt.
table_optlist ::= table_optlist COMMA table_opt.
table_optlist ::= table_optlist table_opt.

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
  ** outside of *ppExpr.
  */
  static sql_expr_t* exprNot(int doNot, sql_expr_t *pSpan){
    if (doNot) {
      sql_expr_t* not_op = sql_expr_new(TK_NOT, 0);
      sql_expr_attach_subtrees(not_op, pSpan, NULL);
      return not_op; // output
    }
    return pSpan;
  }
}

table_opt ::= ID opt_equal expr.
table_opt ::= ct_opt_charset opt_equal expr.

ct_opt_charset ::= opt_default CHARACTER SET.
ct_opt_charset ::= opt_default CHARSET.
opt_default ::= .
opt_default ::= DEFAULT.

opt_equal ::= .
opt_equal ::= EQ.

columnlist ::= columnlist COMMA columnname carglist.
columnlist ::= columnname carglist.
columnname ::= nm typetoken.

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
  ABORT ACTION AFTER ANALYZE ASC ATTACH BEFORE BEGIN BY CASCADE CAST COLUMNKW
  CONFLICT DATABASE DESC DETACH EACH END FAIL FOR
  IGNORE INITIALLY INSTEAD LIKE_KW MATCH NO PLAN
  QUERY KEY OF OFFSET PRAGMA RAISE RECURSIVE RELEASE REPLACE RESTRICT ROW
  ROLLBACK SAVEPOINT START TEMP TRIGGER VACUUM VIEW VIRTUAL WITH WITHOUT

  RENAME IF
  EXPLAIN DESCRIBE SHOW NAMES USE CALL
  CHARACTER CHARSET SESSION GLOBAL KILL ONLY
  DUPLICATE INTERVAL TIME_UNIT SHARD_EXPLAIN
  COLUMNS FIELDS COMMENT_KW SHARE MODE TABLES LOCAL
  ISOLATION LEVEL COMMITTED UNCOMMITTED SERIALIZABLE REPEATABLE
  XA RECOVER WARNINGS UNDERSCORE_CHARSET
// all terminals in the full parser should be here, so that the
// generated header is compatible
// these keywords are recognized but not parsed as is
//  JOIN ON USING WHERE ORDER GROUP HAVING LIMIT
  INTO TRIM_SPEC TRIM POSITION TRUNCATE SIGNED UNSIGNED DECIMAL
  BINARY NCHAR INT_SYM CETUS_SEQUENCE SCHEMA
  .
%wildcard ANY.


// And "ids" is an identifer-or-string.
//
%token_class ids  ID.//|STRING.

// The name of a column or table can be any of the following:
%type nm {sql_token_t}
nm(A) ::= ID(A).
nm(A) ::= JOIN_KW(A).

// A typetoken is really zero or more tokens that form a type name such
// as can be found after the column name in a CREATE TABLE statement.
// Multiple tokens are concatenated to form the value of the typetoken.
//
%type typetoken {sql_token_t}
typetoken(A) ::= .   {A.z = 0; A.n = 0;}
typetoken(A) ::= typename(A).
typetoken(A) ::= typename(A) LP signed RP.
typetoken(A) ::= typename(A) LP signed COMMA signed RP.
%type typename {sql_token_t}
typename(A) ::= ids(A).
typename(A) ::= typename(A) ids.
signed ::= plus_num.
signed ::= minus_num.

%token_class number INTEGER|FLOAT.
plus_num(A) ::= PLUS number(X).       {A = X;}
plus_num(A) ::= number(A).
minus_num(A) ::= MINUS number(X).     {A = X;}

// "carglist" is a list of additional constraints that come after the
// column name and column type in a CREATE TABLE statement.
//
carglist ::= carglist ccons.
carglist ::= .
ccons ::= CONSTRAINT nm.
ccons ::= DEFAULT term.      
ccons ::= DEFAULT LP expr RP.
ccons ::= DEFAULT PLUS term. 
ccons ::= DEFAULT MINUS term.
ccons ::= DEFAULT ID.

// In addition to the type name, we also care about the primary key and
// UNIQUE constraints.
//
ccons ::= NULL.
ccons ::= NOT NULL autoinc.
ccons ::= PRIMARY KEY sortorder autoinc.
ccons ::= UNIQUE.
ccons ::= CHECK LP expr RP.

ccons ::= COLLATE ids.
ccons ::= COMMENT_KW concat_str.

// The optional AUTOINCREMENT keyword
%type autoinc {int}
autoinc(X) ::= .          {X = 0;}
autoinc(X) ::= AUTO_INCREMENT.  {X = 1;}

conslist_opt(A) ::= .   {A.z = 0; A.n = 0;}
conslist_opt(A) ::= COMMA(A) conslist.
conslist ::= conslist tconscomma tcons.
conslist ::= tcons.
tconscomma ::= COMMA.
tconscomma ::= .
tcons ::= CONSTRAINT nm.
tcons ::= PRIMARY KEY LP sortlist autoinc RP.
tcons ::= UNIQUE LP sortlist RP.
tcons ::= UNIQUE KEY LP sortlist RP.
tcons ::= CHECK LP expr RP.
tcons ::= FOREIGN KEY LP eidlist RP.

////////////////////////// The DROP TABLE /////////////////////////////////////
//
cmd ::= DROP TABLE ifexists nm. {
    context->rw_flag |= CF_WRITE;
    sql_context_add_stmt(context, STMT_COMMON_DDL, NULL);
}
%type ifexists {int}
ifexists(A) ::= IF EXISTS.   {A = 1;}
ifexists(A) ::= .            {A = 0;}

////////////////////////// The DROP DATABASE /////////////////////////////////////
//
%token_class db_schema DATABASE|SCHEMA.

cmd ::= DROP db_schema ifexists(A) nm(B). {
    sql_drop_database_t *p = sql_drop_database_new();
    sql_drop_database(context, p);
    p->schema_name = sql_token_dup(B);
    p->ifexists = A;
}

//////////////////////// ALTER TABLE //////////////////////////////////
cmd ::= ALTER TABLE. {
    context->rw_flag |= CF_WRITE;
    sql_context_add_stmt(context, STMT_COMMON_DDL, NULL);
    context->rc = PARSE_HEAD;
}

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

oneselect(A) ::= SELECT select_options(D) selcollist(C) from where_opt
                 groupby_opt having_opt orderby_opt limit_opt lock_read(R). {
    A = sql_select_new();
    A->flags |= D;
    A->columns = C;
    A->lock_read = R;
    if (R) {
        context->rw_flag |= CF_WRITE;
    }
}

oneselect(A) ::= values(A).

%type values {sql_select_t*}
%destructor values { sql_select_free($$); }
values(A) ::= VALUES LP nexprlist(X) RP. {
  A = sql_select_new();
  A->columns = X;
}
values(A) ::= values(A) COMMA LP exprlist(Y) RP. {
  sql_select_t *pRight, *pLeft = A;
  pRight = sql_select_new();
  //if (pLeft)pLeft->selFlags &= ~SF_MultiValue;
  if (pRight) {
    pRight->columns = Y;
    //pRight->op = TK_ALL;
    pRight->prior = pLeft;
    A = pRight;
  } else {
    A = pLeft;
  }
}

// The "distinct" nonterminal is true (1) if the DISTINCT keyword is
// present and false (0) if it is not.
//
%type select_options {int}
%type select_option {int}
select_options(A) ::= . {A = 0;}
select_options(A) ::= select_options(X) select_option(Y).   { A = X|Y; }
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
}
selcollist(A) ::= sclp(A) STAR(B). {
    sql_expr_t* p = sql_expr_new(@B, &B);
    A = sql_expr_list_append(A, p);
}
selcollist(A) ::= sclp(A) nm(B) DOT STAR(C). {
    sql_expr_t* left = sql_expr_new(TK_ID, &B);
    sql_expr_t* right = sql_expr_new(@C, &C);
    sql_expr_t* dot = sql_expr_new(TK_DOT, 0);
    sql_expr_attach_subtrees(dot, left, right);
    A = sql_expr_list_append(A, dot);
}

// An option "AS <id>" phrase that can follow one of the expressions that
// define the result set, or one of the tables in the FROM clause.
//
%type as {sql_token_t}
as(X) ::= AS nm(Y).    {X = Y;}
as(X) ::= AS STRING(Y). {X = Y;}
as(X) ::= ID(X).
as(X) ::= .            {X.z = 0; X.n = 0;}

// A complete FROM clause.
//
from ::= .
from ::= FROM seltablist.

// "seltablist" is a "Select Table List" - the content of the FROM clause
// in a SELECT statement.  "stl_prefix" is a prefix of this list.
//
stl_prefix ::= seltablist joinop.
stl_prefix ::= .
seltablist ::= stl_prefix nm dotnm as index_hint on_opt using_opt.
seltablist ::= stl_prefix nm dotnm LP exprlist RP as on_opt using_opt.
seltablist ::= stl_prefix LP select RP as on_opt using_opt.
seltablist ::= stl_prefix LP seltablist RP as on_opt using_opt.

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
joinop(X) ::= COMMA|JOIN.              { X = JT_LEFT; } //TODO: JOIN TYPE 
joinop(X) ::= JOIN_KW JOIN.            { X = JT_LEFT; }
joinop(X) ::= JOIN_KW nm JOIN.         { X = JT_LEFT; }
joinop(X) ::= JOIN_KW nm nm JOIN.      { X = JT_LEFT; }

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

orderby_opt(A) ::= . {A=0;}
orderby_opt(A) ::= ORDER BY sortlist. {A=0;}
sortlist(A) ::= sortlist COMMA expr sortorder. { A = 0;}
sortlist(A) ::= expr sortorder. { A = 0;}

%type sortorder {int}

sortorder(A) ::= ASC.           {A = 0;}
sortorder(A) ::= DESC.          {A = 1;}
sortorder(A) ::= .              {A = -1;}

//%type groupby_opt {sql_expr_list_t*}
//%destructor groupby_opt {sql_expr_list_free($$);}
groupby_opt ::= .
groupby_opt ::= GROUP BY nexprlist.

//%type having_opt {sql_expr_t*}
//%destructor having_opt {sql_expr_free($$);}
having_opt ::= .
having_opt ::= HAVING expr.

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
limit_opt ::= .
limit_opt ::= LIMIT expr.
limit_opt ::= LIMIT expr OFFSET expr.
limit_opt ::= LIMIT expr COMMA expr. 


/////////////////////////// The DELETE statement /////////////////////////////
//
delete_stmt ::= delete_kw FROM fullname where_opt orderby_opt limit_opt.
delete_kw ::= DELETE. {
    context->rw_flag |= CF_WRITE;
    sql_context_add_stmt(context, STMT_DELETE, NULL);
}

where_opt ::= .
where_opt ::= where_sym expr. {
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
update_stmt ::= update_kw table_reference SET setlist where_opt orderby_opt limit_opt.
update_kw ::= UPDATE. {
    context->rw_flag |= CF_WRITE;
    sql_context_add_stmt(context, STMT_UPDATE, NULL);
}

setlist ::= setlist COMMA expr.
setlist ::= expr.

table_reference ::= fullname as index_hint.
index_hint ::= .
index_hint ::= USE INDEX|KEY index_for LP index_list RP.
index_hint ::= IGNORE INDEX|KEY index_for LP index_list RP.
index_hint ::= FORCE INDEX|KEY index_for LP index_list RP.
index_for ::= .
index_for ::= FOR JOIN.
index_for ::= FOR ORDER BY.
index_for ::= FOR GROUP BY.
index_list ::= index_list COMMA ID.
index_list ::= ID|PRIMARY.

////////////////////////// The INSERT command /////////////////////////////////
//
insert_stmt ::= insert_cmd anylist.
insert_cmd ::= INSERT|REPLACE. {
    context->rw_flag |= CF_WRITE;
    sql_context_add_stmt(context, STMT_INSERT, NULL);
}

%type idlist_opt {sql_id_list_t*}
%destructor idlist_opt {sql_id_list_free($$);}
%type idlist {sql_id_list_t*}
%destructor idlist {sql_id_list_free($$);}

idlist(A) ::= idlist(A) COMMA nm(Y).
    {A = sql_id_list_append(A,&Y);}
idlist(A) ::= nm(Y).
    {A = sql_id_list_append(0,&Y); /*A-overwrites-Y*/}

%type concat_str {sql_token_t}
concat_str(A) ::= STRING(A).
concat_str(A) ::= concat_str(A) STRING. // TODO: collecet all

/////////////////////////// sql_expression Processing /////////////////////////////
//

%type expr {sql_expr_t*}
%destructor expr { sql_expr_free($$); }
%type term {sql_expr_t*}
%destructor term { sql_expr_free($$); }

expr(A) ::= term(A).
expr(A) ::= LP expr(X) RP. {A = X;}
term(A) ::= NULL(X).   {A = sql_expr_new(@X, &X);}
term(A) ::= ON(X). {A = sql_expr_new(@X, &X);} // set xx=on
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
term(A) ::= concat_str(X).         {A = sql_expr_new(TK_STRING, &X);}
expr(A) ::= VARIABLE(X).          {A = sql_expr_new(@X, &X);}
expr(A) ::= expr(A) COLLATE ID|STRING(X).  {
    sql_expr_t* coll = sql_expr_new(@X, &X);
    sql_expr_attach_subtrees(A, coll, NULL);
}

%include {
  static sql_expr_t* function_expr_new(sql_token_t* name, sql_expr_list_t* args)
  {
      sql_expr_t* func_expr = sql_expr_new(TK_FUNCTION, name);
      func_expr->list = args;
      return func_expr;
  }
}

expr(A) ::= ID(X) LP distinct exprlist(Y) RP. {
    A = function_expr_new(&X, Y);
    context->where_flags |= EP_FUNCTION;
    if (strncasecmp(X.z, "last_insert_id", X.n) == 0) {
        context->where_flags |= EP_LAST_INSERT_ID;
    }
    //A->distinct = D;
}
expr(A) ::= ID(X) LP STAR RP. {
    A = function_expr_new(&X, 0);
}
expr(A) ::= JOIN_KW(N) LP expr COMMA expr RP. {
    A = function_expr_new(&N, 0);
}
expr(A) ::= INSERT(N) LP expr COMMA expr COMMA expr COMMA expr RP. {
    A = function_expr_new(&N, 0);
}
expr(A) ::= TRIM(N) LP expr RP. {
    A = function_expr_new(&N, 0);
}
expr(A) ::= TRIM(N) LP expr FROM expr RP. {
    A = function_expr_new(&N, 0);
}
expr(A) ::= TRIM(N) LP TRIM_SPEC expr FROM expr RP. {
    A = function_expr_new(&N, 0);
}
expr(A) ::= TRIM(N) LP TRIM_SPEC FROM expr RP. {
    A = function_expr_new(&N, 0);
}
expr(A) ::= POSITION(N) LP STRING IN expr RP. {
    A = function_expr_new(&N, 0);
}
expr(A) ::= CURRENT_DATE(N) opt_parentheses. {
    A = function_expr_new(&N, 0);
    context->clause_flags |= CF_LOCAL_QUERY;
}
expr(A) ::= CETUS_VERSION(N) opt_parentheses. {
    A = function_expr_new(&N, 0);
    context->clause_flags |= CF_LOCAL_QUERY;
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

expr(A) ::= var_scope(S) ID(X). {
    A = sql_expr_new(@X, &X);
    A->var_scope = S;
    if (strcasecmp(X.z, "last_insert_id") == 0) {
        context->where_flags |= EP_LAST_INSERT_ID;
    }
}

%type likeop {struct LikeOp}
likeop(A) ::= LIKE_KW|MATCH(X). {A.eOperator = X; A.bNot = 0;/*A-overwrites-X*/}
likeop(A) ::= NOT LIKE_KW|MATCH(X). {A.eOperator = X; A.bNot = 1;}
expr(A) ::= expr(A) likeop(OP) expr(Y).  [LIKE_KW]  {
  sql_expr_list_t *pList = sql_expr_list_append(0, Y);
  pList = sql_expr_list_append(pList, A);
  A = function_expr_new(&OP.eOperator, pList);
  A = exprNot(OP.bNot, A);
}
expr(A) ::= expr(A) likeop(OP) expr(Y) ESCAPE expr(E).  [LIKE_KW]  {
  sql_expr_list_t *pList = sql_expr_list_append(0, Y);
  pList = sql_expr_list_append(pList, A);
  pList = sql_expr_list_append(pList, E);
  A = function_expr_new(&OP.eOperator, pList);
  A = exprNot(OP.bNot, A);
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
    sql_expr_t* pOperand    /* The operand */
  ){
    sql_expr_t* the_op = sql_expr_new(op, 0);
    sql_expr_attach_subtrees(the_op, pOperand, NULL);
    return the_op;
  }
}


expr(A) ::= NOT expr(X). 
              {A=spanUnaryPrefix(TK_NOT,X);/*A-overwrites-B*/}
expr(A) ::= BITNOT expr(X).
              {A=spanUnaryPrefix(TK_BITNOT,X);/*A-overwrites-B*/}
expr(A) ::= MINUS expr(X). [BITNOT]
              {A=spanUnaryPrefix(TK_UMINUS,X);/*A-overwrites-B*/}
expr(A) ::= PLUS expr(X). [BITNOT]
              {A=spanUnaryPrefix(TK_UPLUS,X);/*A-overwrites-B*/}

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
      A = betw_op;
  } else {
      sql_expr_list_free(pList);
  } 
  A = exprNot(N, A);
}

%type in_op {int}
in_op(A) ::= IN.      {A = 0;}
in_op(A) ::= NOT IN.  {A = 1;}
expr(A) ::= expr(B) in_op(N) LP exprlist(Y) RP. [IN] {
    A = sql_expr_new(TK_IN, 0);
    if (A) {
        sql_expr_attach_subtrees(A, B, NULL);
        A->list = Y;
    } else {
        sql_expr_list_free(Y);
    }
    A = exprNot(N, A);
}
expr(A) ::= LP select(X) RP. {
  A = sql_expr_new(TK_SELECT, 0);
  if (A) {
    A->select = X;
  } else {
    sql_expr_free(X);
  };
}
expr(A) ::= expr(B) in_op(N) LP select(Y) RP.  [IN] {
  A = sql_expr_new(TK_IN, 0);
  if (A) {
      sql_expr_attach_subtrees(A, B, NULL);
      A->select = Y;
  } else {
      sql_expr_free(Y);
  }
  A = exprNot(N, A);
}
expr(A) ::= EXISTS LP select(Y) RP. {
  A = sql_expr_new(TK_EXISTS, 0);
  A->select = Y;
}

expr(A) ::= INTERVAL expr TIME_UNIT. { A = 0; }

/* CASE expressions */
expr(A) ::= CASE case_operand(X) case_exprlist(Y) case_else(Z) END. {
  A = sql_expr_new(TK_CASE, 0);
  if (A) {
      sql_expr_attach_subtrees(A, X, NULL);
      A->list = Z ? sql_expr_list_append(Y, Z) : Y;
  } else {
      sql_expr_list_free(Y);
      sql_expr_free(Z);
  }
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
case_else(A) ::=  ELSE expr(X).         {A = X;}
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

%include {
  /* Add a single new term to an sql_expr_list_t that is used to store a
  ** list of identifiers.  Report an error if the ID list contains
  ** a COLLATE clause or an ASC or DESC keyword, except ignore the
  ** error while parsing a legacy schema.
  */
  static sql_expr_list_t *parserAddExprIdListTerm(
    sql_expr_list_t *pPrior,
    sql_token_t *pIdToken,
    int hasCollate,
    int sortOrder
  ){
    sql_expr_list_t *p = sql_expr_list_append(pPrior, 0);
    if (hasCollate || sortOrder != -1) {
      printf("syntax error after column name \"%.*s\"",
             pIdToken->n, pIdToken->z);
    }
    return p;
  }
} // end %include

eidlist(A) ::= eidlist(A) COMMA nm(Y) collate(C) sortorder(Z).  {
  A = parserAddExprIdListTerm(A, &Y, C, Z);
}
eidlist(A) ::= nm(Y) collate(C) sortorder(Z). {
  A = parserAddExprIdListTerm(0, &Y, C, Z); /*A-overwrites-Y*/
}

%type collate {int}
collate(C) ::= .              {C = 0;}
collate(C) ::= COLLATE ids.   {C = 1;}

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
