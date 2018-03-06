#ifndef _SQL_FILTER_VARIABLES_H_
#define _SQL_FILTER_VARIABLES_H_

#include <glib.h>

gboolean sql_filter_vars_load_rules(char *filename);

gboolean sql_filter_vars_load_str_rules(const char *json_str);

void sql_filter_vars_load_default_rules();

void sql_filter_vars_shard_load_default_rules();

void sql_filter_vars_destroy();

gboolean sql_filter_vars_is_silent(const char *name, const char *val);

gboolean sql_filter_vars_is_allowed(const char *name, const char *val);

#endif /*_SQL_FILTER_VARIABLES_H_*/
