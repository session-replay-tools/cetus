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

#include "sql-context.h"

#include "mylexer.l.h"
#include "sql-property.h"

void sqlParser(void *yyp, int yymajor, sql_token_t yyminor, sql_context_t *);
void sqlParserFree(void *p, void (*freeProc) (void *));
void *sqlParserAlloc(void *(*mallocProc) (size_t));
void sqlParserTrace(FILE *TraceFILE, char *zTracePrompt);
void yylex_restore_buffer(void *);

void
sql_context_init(sql_context_t *p)
{
    p->rc = 0;
    p->message = 0;
    p->explain = 0;
    p->user_data = 0;
    p->sql_statement = 0;
    p->stmt_type = 0;
    p->stmt_count = 0;

    p->rw_flag = 0;
    p->clause_flags = 0;
    p->where_flags = 0;
    p->parsing_place = 0;

    p->property = 0;
    p->is_parsing_subquery = 0;
    /* allow_subquery_nesting; //keep unchanged */
}

void
sql_context_destroy(sql_context_t *p)
{
    if (p->sql_statement)
        sql_statement_free(p->sql_statement, p->stmt_type);
    if (p->message)
        g_free(p->message);
    if (p->property)
        sql_property_free(p->property);
}

void
sql_context_reset(sql_context_t *p)
{
    sql_context_destroy(p);
    sql_context_init(p);
}

void
sql_context_append_msg(sql_context_t *p, char *msg)
{
    int len = strlen(msg);
    int orig_len = 0;
    if (p->message == NULL) {
        p->message = g_malloc0(len + 1);
    } else {
        char *orig_msg = p->message;
        orig_len = strlen(orig_msg);
        p->message = g_malloc0(orig_len + len + 1);
        strcpy(p->message, orig_msg);
        g_free(orig_msg);
    }
    strcpy(p->message + orig_len, msg);
}

void
sql_context_set_error(sql_context_t *p, int err, char *msg)
{
    p->rc = err;
    sql_context_append_msg(p, msg);
}

void
sql_context_add_stmt(sql_context_t *p, enum sql_stmt_type_t type, void *clause)
{
    if (p->stmt_count == 0) {
        p->stmt_type = type;
        p->sql_statement = clause;
    } else {
        if (clause) {
            sql_statement_free(clause, type);
        }
    }
}

gboolean
sql_context_has_sharding_property(sql_context_t *p)
{
    return p && p->property && (p->property->table || p->property->group);
}

static void
parse_token(sql_context_t *context, int code, sql_token_t token, void *parser, sql_property_parser_t *prop_parser)
{
    if (code == TK_PROPERTY_START) {
        prop_parser->is_parsing = TRUE;
        if (context->property == NULL) {
            context->property = g_new0(sql_property_t, 1);
        }
        return;
    } else if (code == TK_PROPERTY_END) {
        prop_parser->is_parsing = FALSE;
        sql_property_t *prop = context->property;
        if (prop->mode == MODE_READWRITE) {
            context->rw_flag |= CF_FORCE_MASTER;
        } else if (prop->mode == MODE_READONLY) {
            context->rw_flag |= CF_FORCE_SLAVE;
        }
        if (!sql_property_is_valid(context->property)) {
            sql_property_free(context->property);
            context->property = NULL;
            g_message(G_STRLOC ":invalid comment");
            sql_context_set_error(context, PARSE_SYNTAX_ERR, "comment error");
        }
        return;
    } else if (code == TK_MYSQL_HINT) {
        context->rw_flag |= CF_WRITE;
        return;
    }

    if (prop_parser->is_parsing) {  /*# K = V */
        int rc = sql_property_parser_parse(prop_parser,
                                           token.z, token.n, context->property);
        if (!rc) {
            sql_property_free(context->property);
            context->property = NULL;
            sql_context_set_error(context, PARSE_SYNTAX_ERR, "comment error");
        }
    } else {
        sqlParser(parser, code, token, context);
    }
}

#define PARSER_TRACE 0

/* Parse user allocated sql string
  sql->str must be terminated with 2 NUL
  sql->len is length including the 2 NUL */
void
sql_context_parse_len(sql_context_t *context, GString *sql)
{
    yyscan_t scanner;
    yylex_init(&scanner);
    YY_BUFFER_STATE buf_state = yy_scan_buffer(sql->str, sql->len, scanner);
#if PARSER_TRACE
    sqlParserTrace(stdout, "---ParserTrace: ");
#endif

    void *parser = sqlParserAlloc(malloc);
    sql_context_reset(context);

    static sql_property_parser_t comment_parser;
    sql_property_parser_reset(&comment_parser);

    int code;
    int last_parsed_token = 0;
    sql_token_t token;
    while ((code = yylex(scanner)) > 0) {   /* 0 on EOF */
        token.z = yyget_text(scanner);
        token.n = yyget_leng(scanner);
#if PARSER_TRACE
        printf("***LexerTrace: code: %d, yytext: %.*s\n", code, token.n, token.z);
        printf("***LexerTrace: yytext addr: %p\n", token.z);
#endif
        parse_token(context, code, token, parser, &comment_parser);

        last_parsed_token = code;

        if (context->rc != PARSE_OK) {  /* break on PARSE_HEAD, other error */
            yylex_restore_buffer(scanner);  /* restore the input string */
            break;
        }
    }
    if (context->rc == PARSE_OK) {
        /* grammar require semicolon as ending token */
        if (last_parsed_token != TK_SEMI) {
            sqlParser(parser, TK_SEMI, token, context);
        }
        sqlParser(parser, 0, token, context);
    }
    sqlParserFree(parser, free);
    yy_delete_buffer(buf_state, scanner);
    yylex_destroy(scanner);
}

gboolean
sql_context_is_autocommit_on(sql_context_t *context)
{
    if (context->stmt_type == STMT_SET) {
        sql_expr_list_t *set_list = context->sql_statement;
        if (set_list && set_list->len > 0) {
            sql_expr_t *expr = g_ptr_array_index(set_list, 0);
            if (expr->op == TK_EQ && sql_expr_is_id(expr->left, "AUTOCOMMIT")) {
                gboolean on;
                if (sql_expr_is_boolean(expr->right, &on)) {
                    return on;
                } else {
                    sql_expr_t *p = expr->right;
                    if (p && p->op == TK_ON) {
                        if (strcasecmp(p->token_text, "on") == 0) {
                            return TRUE;
                        }
                    }
                }
            }
        }
    }
    return FALSE;
}

gboolean
sql_context_is_autocommit_off(sql_context_t *context)
{
    if (context->stmt_type == STMT_SET) {
        sql_expr_list_t *set_list = context->sql_statement;
        if (set_list && set_list->len > 0) {
            sql_expr_t *expr = g_ptr_array_index(set_list, 0);
            if (expr->op == TK_EQ && sql_expr_is_id(expr->left, "AUTOCOMMIT")) {
                gboolean on;
                if (sql_expr_is_boolean(expr->right, &on)) {
                    return on == FALSE;
                }
            }
        }
    }
    return FALSE;
}

gboolean
sql_context_is_single_node_trx(sql_context_t *context)
{
    return context && context->property && context->property->transaction == TRX_SINGLE_NODE;
}

gboolean
sql_context_is_cacheable(sql_context_t *context)
{
    if (context->stmt_type != STMT_SELECT)
        return FALSE;
    if (context->clause_flags & CF_SUBQUERY)
        return FALSE;
    sql_select_t *select = context->sql_statement;
    if (!select->from_src)
        return FALSE;
    if (select->lock_read)
        return FALSE;
    int i = 0;
    for (i = 0; i < select->columns->len; ++i) {    /* only allow aggreate functions */
        sql_expr_t *col = g_ptr_array_index(select->columns, i);
        if (col->flags & EP_FUNCTION && !(col->flags & EP_AGGREGATE))
            return FALSE;
    }
    return TRUE;
}
