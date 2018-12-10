%{
#include <stdio.h>
#include <glib.h>
#include "myparser.y.h"
%}

%option reentrant
%option noyywrap
%option never-interactive
 /*yyin and yyout set to NULL*/
%option nostdinit
 /*%option outfile="mylexer.c" header-file="mylexer.h"*/
%option nodefault
%option warn
%option case-insensitive

%x PROPERTY
%x COMMENT
%x MYSQL_HINT

HEX_DIGIT [0-9a-fA-F]

%%

[ \t\r\n]+                    /* ignore whitespace */;
";"             return TK_SEMI;
"("             return TK_LP         ;
")"             return TK_RP         ;
","             return TK_COMMA      ;
"<>"            return TK_NE         ;
"!="            return TK_NE         ;
"="             return TK_EQ         ;
"=="            return TK_EQ         ;
">"             return TK_GT         ;
"<="            return TK_LE         ;
"<"             return TK_LT         ;
">="            return TK_GE         ;
"&"             return TK_BITAND     ;
"|"             return TK_BITOR      ;
"~"             return TK_BITNOT     ;
"<<"            return TK_LSHIFT     ;
">>"            return TK_RSHIFT     ;
"+"             return TK_PLUS       ;
"-"             return TK_MINUS      ;
"*"             return TK_STAR       ;
"/"             return TK_SLASH      ;
"%"             return TK_REM        ; /*REMAIN */
"."             return TK_DOT        ;
"?"             return TK_VARIABLE   ;

"TABLE"         return TK_TABLE      ;
"CREATE"        return TK_CREATE     ;
"IF"            return TK_IF         ;
"NOT"           return TK_NOT        ;
"!" return TK_NOT;
"EXISTS"        return TK_EXISTS     ;
"TEMP"          return TK_TEMP       ;
"AS"            return TK_AS         ;
"WITHOUT"       return TK_WITHOUT    ;
"OR"            return TK_OR         ;
"||" return TK_OR;
"AND"           return TK_AND        ;
"&&" return TK_AND;
"IS"            return TK_IS         ;
"MATCH"         return TK_MATCH      ;
"LIKE"          return TK_LIKE_KW    ;
"REGEXP"        return TK_LIKE_KW;
"BETWEEN"       return TK_BETWEEN    ;
"IN"            return TK_IN         ;
 /*"ISNULL"        return TK_ISNULL     ;*/
 /*"NOTNULL"       return TK_NOTNULL    ;*/
 /*"||"         return TK_CONCAT     ;*/ /*for ansi sql_mode*/
"COLLATE"       return TK_COLLATE    ;
"ABORT"         return TK_ABORT      ;
"ACTION"        return TK_ACTION     ;
"AFTER"         return TK_AFTER      ;
"ANALYZE"       return TK_ANALYZE    ;
"ASC"           return TK_ASC        ;
"ATTACH"        return TK_ATTACH     ;
"BEFORE"        return TK_BEFORE     ;
"BEGIN"         return TK_BEGIN;
"BY"            return TK_BY         ;
"CASCADE"       return TK_CASCADE    ;
"CAST"          return TK_CAST       ;
"COLUMN"        return TK_COLUMNKW   ;
"CONFLICT"      return TK_CONFLICT   ;
"DATABASE"      return TK_DATABASE   ;
"SCHEMA"        return TK_SCHEMA;
"DESC"          return TK_DESC       ;
"DETACH"        return TK_DETACH     ;
"EACH"          return TK_EACH       ;
"END"           return TK_END        ;
"FAIL"          return TK_FAIL       ;
"FOR"           return TK_FOR        ;
"IGNORE"        return TK_IGNORE     ;
"INITIALLY"     return TK_INITIALLY  ;
"INSTEAD"       return TK_INSTEAD    ;
"NO"            return TK_NO         ;
"PLAN"          return TK_PLAN       ;
"QUERY"         return TK_QUERY      ;
"KEY"           return TK_KEY        ;
"OF"            return TK_OF         ;
"TO"            return TK_TO;
"OFFSET"        return TK_OFFSET     ;
"PRAGMA"        return TK_PRAGMA     ;
"RAISE"         return TK_RAISE      ;
"RECURSIVE"     return TK_RECURSIVE  ;
"RELEASE"       return TK_RELEASE    ;
"REPLACE"       return TK_REPLACE    ;
"RESTRICT"      return TK_RESTRICT   ;
"ROW"           return TK_ROW        ;

"TRANSACTION"   return TK_TRANSACTION;
"START"         return TK_START;
"COMMIT"        return TK_COMMIT;
"ROLLBACK"      return TK_ROLLBACK   ;
"SAVEPOINT"     return TK_SAVEPOINT  ;

"TRIGGER"       return TK_TRIGGER    ;
"VACUUM"        return TK_VACUUM     ;
"VIEW"          return TK_VIEW       ;
"VIRTUAL"       return TK_VIRTUAL    ;
"WITH"          return TK_WITH       ;
"INDEX"         return TK_INDEX    ;
"RENAME"        return TK_RENAME     ;
 /*
"DATE"          return TK_CTIME_KW   ;
"TIME"          return TK_CTIME_KW   ;
"NOW"           return TK_CTIME_KW   ;
"TIMESTAMP"         return TK_CTIME_KW   ;
"TIMEDIFF"      return TK_CTIME_KW   ;
 */
"LEFT"          return TK_JOIN_KW;
"RIGHT"         return TK_JOIN_KW;
"INNER"         return TK_JOIN_KW;
"OUTER"         return TK_JOIN_KW;
"FULL"          return TK_JOIN_KW;

"ANY"           return TK_ANY        ;
"CONSTRAINT"    return TK_CONSTRAINT ;
"DEFAULT"       return TK_DEFAULT    ;
"CHECK"         return TK_CHECK      ;
"AUTO_INCREMENT" return TK_AUTO_INCREMENT  ;
"PRIMARY"       return TK_PRIMARY    ;
"UNIQUE"        return TK_UNIQUE     ;
"FOREIGN"       return TK_FOREIGN    ;
"DROP"          return TK_DROP       ;
"SELECT"        return TK_SELECT     ;
"DISTINCT"      return TK_DISTINCT   ;
"FROM"          return TK_FROM       ;
"JOIN"          return TK_JOIN       ;
"ON"            return TK_ON         ;
"USING"         return TK_USING      ;
"ORDER"         return TK_ORDER      ;
"GROUP"         return TK_GROUP      ;
"HAVING"        return TK_HAVING     ;
"LIMIT"         return TK_LIMIT      ;
"DELETE"        return TK_DELETE     ;
"WHERE"         return TK_WHERE      ;
"UPDATE"        return TK_UPDATE     ;
"SET"           return TK_SET        ;
"INTO"          return TK_INTO       ;
"VALUES"        return TK_VALUES     ;
"INSERT"        return TK_INSERT     ;
"NULL"          return TK_NULL       ;
"BLOB"          return TK_BLOB       ;
"CASE"          return TK_CASE       ;
"WHEN"          return TK_WHEN       ;
"THEN"          return TK_THEN       ;
"ELSE"          return TK_ELSE       ;
"EXPLAIN"       return TK_EXPLAIN;
"DESCRIBE"      return TK_DESCRIBE;
"SHOW"          return TK_SHOW;
"SQL_CALC_FOUND_ROWS"  return TK_SQL_CALC_FOUND_ROWS;
"NAMES"                return TK_NAMES;
"USE"                  return TK_USE;
"CALL"                 return TK_CALL;
"CHARACTER"            return TK_CHARACTER;
"CHARSET"              return TK_CHARSET;
"SESSION"              return TK_SESSION;
"GLOBAL"               return TK_GLOBAL;
"KILL"                 return TK_KILL;
"READ"                 return TK_READ;
"WRITE"                return TK_WRITE;
"ONLY"                 return TK_ONLY;
"UNION"                return TK_UNION;
"ALL"                  return TK_ALL;
"DUPLICATE"            return TK_DUPLICATE;
"INTERVAL"             return TK_INTERVAL;
"DAY"                  return TK_TIME_UNIT;
"MONTH"                return TK_TIME_UNIT;
"YEAR"                 return TK_TIME_UNIT;
"HOUR"                 return TK_TIME_UNIT;
"MINUTE"               return TK_TIME_UNIT;
"SECOND"               return TK_TIME_UNIT;
"SHARD_EXPLAIN"        return TK_SHARD_EXPLAIN;
"ALTER"                return TK_ALTER;
"FORCE"               return TK_FORCE;
"COLUMNS"             return TK_COLUMNS;
"FIELDS"              return TK_FIELDS;
"COMMENT"             return TK_COMMENT_KW;
"LOCK"                return TK_LOCK;/*R*/
"SHARE"               return TK_SHARE;
"MODE"                return TK_MODE;
"UNLOCK"              return TK_UNLOCK;/*R*/
"TABLES"              return TK_TABLES;
"LOCAL"               return TK_LOCAL;
"LOW_PRIORITY"        return TK_LOW_PRIORITY;/*R*/
"ISOLATION"           return TK_ISOLATION;
"LEVEL"               return TK_LEVEL;
"COMMITTED"           return TK_COMMITTED;
"UNCOMMITTED"         return TK_UNCOMMITTED;
"SERIALIZABLE"        return TK_SERIALIZABLE;
"REPEATABLE"          return TK_REPEATABLE;
"TRIM("                return TK_TRIM; //special function
"POSITION("            return TK_POSITION; //special function
"BOTH"                return TK_TRIM_SPEC;
"LEADING"             return TK_TRIM_SPEC;
"TRAILING"            return TK_TRIM_SPEC;
"TRUNCATE"            return TK_TRUNCATE;
"XA"                  return TK_XA;
"RECOVER"             return TK_RECOVER;
"CURRENT_DATE"        return TK_CURRENT_DATE;
"SIGNED"              return TK_SIGNED;
"UNSIGNED"            return TK_UNSIGNED;
"DECIMAL"             return TK_DECIMAL;
"BINARY"              return TK_BINARY;
"FLUSH"               return TK_FLUSH;
"NO_WRITE_TO_BINLOG"  return TK_NO_WRITE_TO_BINLOG;
"NCHAR"               return TK_NCHAR;
"INT"                 return TK_INT_SYM;
"ESCAPE"              return TK_ESCAPE;
"CETUS_SEQUENCE"      return TK_CETUS_SEQUENCE;
"CETUS_VERSION"       return TK_CETUS_VERSION;
"WARNINGS"            return TK_WARNINGS;
"_binary"             return TK_UNDERSCORE_CHARSET;
"_latin1"             return TK_UNDERSCORE_CHARSET;
"_utf8"               return TK_UNDERSCORE_CHARSET;

[0-9]+                 return TK_INTEGER; /*sign symbol is handled in parser*/
0[xX]{HEX_DIGIT}+      return TK_INTEGER;
[xX]'({HEX_DIGIT}{HEX_DIGIT})+'     return TK_HEX_NUM; /* X'even num of digit' */
0b[01]+          return TK_BIN_NUM;
[bB]'[01]+'      return TK_BIN_NUM;

[0-9]*\.[0-9]+([eE][-+]?[0-9]+)? return TK_FLOAT;

[a-zA-Z][a-zA-Z0-9_]*  return TK_ID;
"@"                    return TK_AT_SIGN;

'([^'\\]|\\.|'')*'     return TK_STRING;
\"([^"\\]|\"\"|\\.)*\"   return TK_STRING;
`([^`]|``)*`   return TK_ID;


"/*#" { BEGIN(PROPERTY); return TK_PROPERTY_START;}
<PROPERTY>[a-zA-Z0-9_.-]+  return TK_ID;
<PROPERTY>"="  return TK_EQ;
<PROPERTY>\"([^"\\]|\"\"|\\.)*\"   return TK_STRING;
<PROPERTY>'([^'\\]|\\.|'')*'     return TK_STRING;
<PROPERTY>[ \t\n\r,;]+ /* ignore */
<PROPERTY>. { g_warning("### bad char in property comment: %02x", yytext[0]);}
<PROPERTY>"*"+"/" { BEGIN(INITIAL); return TK_PROPERTY_END;}


"/*" { BEGIN(COMMENT); }
<COMMENT>[^*]*
<COMMENT>"*"+[^*/]*
<COMMENT>"*"+"/" { BEGIN(INITIAL); }

"/*!" { BEGIN(MYSQL_HINT); }
<MYSQL_HINT>[^*]*
<MYSQL_HINT>"*"+[^*/]*
<MYSQL_HINT>"*"+"/" { BEGIN(INITIAL); return TK_MYSQL_HINT;}


"#".*  { /* useless comment */ }
"--"[ \t\n].*  { /* useless comment */ }

.   {g_warning("### flex: bad input character: %02x", yytext[0]);return 0;}

%%
/*hack function to restore user allocated string.
input string will be modified in the process of lexing
the string will be restored in the end of lexing, however,
if we break in the middle of lex process, the input
string will be left dirty, flex does not supply a
function to restore it. we use this function as a hack,
its declaration is in sql-expression.h
*/

void yylex_restore_buffer(void* yyscanner)
{
    struct yyguts_t * yyg = (struct yyguts_t*)yyscanner;

	if ( YY_CURRENT_BUFFER )
	{
		/* Flush out information for old buffer. */
		*yyg->yy_c_buf_p = yyg->yy_hold_char;
		YY_CURRENT_BUFFER_LVALUE->yy_buf_pos = yyg->yy_c_buf_p;
		YY_CURRENT_BUFFER_LVALUE->yy_n_chars = yyg->yy_n_chars;
	}
}
