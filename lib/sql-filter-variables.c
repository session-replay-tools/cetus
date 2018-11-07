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

#include "sql-filter-variables.h"
#include "cetus-util.h"

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include "cJSON.h"

enum _value_type_t {
    VAL_UNKNOWN = -1,
    VAL_INT = 1,
    VAL_STRING,
    VAL_STRING_CSV,             /* comma seperated string value */
};

static enum _value_type_t
value_type(const char *str)
{
    if (strcasecmp(str, "int") == 0)
        return VAL_INT;
    else if (strcasecmp(str, "string") == 0)
        return VAL_STRING;
    else if (strcasecmp(str, "string-csv") == 0)
        return VAL_STRING_CSV;
    else
        return VAL_UNKNOWN;
}

static gchar *
value_type_name(enum _value_type_t type) {
    switch(type) {
    case VAL_INT: {
        return "int";
        break;
    }
    case VAL_STRING: {
        return "string";
        break;
    }
    case VAL_STRING_CSV: {
        return "string-csv";
        break;
    }
    default:
        return "unknown";
        break;
    }
}

struct sql_variable_t {
    char *name;
    enum _value_type_t type;
    GList *silent_values;       /* GList<char *> */
    GList *allowed_values;      /* GList<char *> */
    gboolean allow_all;
    gboolean silence_all;
};

static void
sql_variable_free(struct sql_variable_t *p)
{
    if (p->name)
        g_free(p->name);
    if (p->silent_values)
        g_list_free_full(p->silent_values, g_free);
    if (p->allowed_values)
        g_list_free_full(p->allowed_values, g_free);
    g_free(p);
}

static gboolean
sql_variable_is_silent_value(struct sql_variable_t *p, const char *value)
{
    GList *l;
    for (l = p->silent_values; l; l = l->next) {
        if (strcasecmp(l->data, value) == 0) {
            return TRUE;
        }
    }
    return FALSE;
}

static gboolean
sql_variable_is_allowed_value(struct sql_variable_t *p, const char *value)
{
    GList *l;
    for (l = p->allowed_values; l; l = l->next) {
        if (strcasecmp(l->data, value) == 0) {
            return TRUE;
        }
    }
    return FALSE;
}

static GHashTable *cetus_variables = NULL;

void
sql_filter_vars_destroy()
{
    if (cetus_variables) {
        g_hash_table_destroy(cetus_variables);
    }
}

gboolean
str_case_equal(gconstpointer v1, gconstpointer v2)
{
    if (!v1 || !v2)
        return FALSE;
    return strcasecmp((const char *)v1, (const char *)v2) == 0;
}

guint
str_case_hash(gconstpointer v)
{
    char *lower = g_ascii_strdown((const char *)v, -1);
    guint hash = g_str_hash(lower);
    g_free(lower);
    return hash;
}

gboolean
sql_filter_vars_load_str_rules(const char *json_str)
{
    if (!cetus_variables) {
        cetus_variables = g_hash_table_new_full(str_case_hash, str_case_equal,
                                                NULL, (GDestroyNotify) sql_variable_free);
    }
    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        g_warning(G_STRLOC ":rule file parse error");
        return FALSE;
    }
    cJSON *var_node = cJSON_GetObjectItem(root, "variables");
    if (!var_node) {
        g_warning("cannot find \"variables\" json node");
        return FALSE;
    }
    cJSON *cur = var_node->child;
    for (; cur; cur = cur->next) {
        cJSON *name = cJSON_GetObjectItem(cur, "name");
        cJSON *type = cJSON_GetObjectItem(cur, "type");
        if (!name || !type) {
            return FALSE;
        }
        struct sql_variable_t *var = g_new0(struct sql_variable_t, 1);
        var->name = g_strdup(name->valuestring);
        var->type = value_type(type->valuestring);
        cJSON *silent_array = cJSON_GetObjectItem(cur, "silent_values");
        if (silent_array) {
            cJSON *silent = silent_array->child;
            for (; silent; silent = silent->next) {
                if (strcmp(silent->valuestring, "*") == 0) {
                    var->silence_all = TRUE;
                    break;
                }
                var->silent_values = g_list_append(var->silent_values, g_strdup(silent->valuestring));
            }
        }
        cJSON *allowed_array = cJSON_GetObjectItem(cur, "allowed_values");
        if (allowed_array) {
            cJSON *allowed = allowed_array->child;
            for (; allowed; allowed = allowed->next) {
                if (strcmp(allowed->valuestring, "*") == 0) {
                    var->allow_all = TRUE;
                    break;
                }
                var->allowed_values = g_list_append(var->allowed_values, g_strdup(allowed->valuestring));
            }
        }
        /* if duplicated, replace and free the old (key & value) */
        g_hash_table_replace(cetus_variables, var->name, var);
    }
    cJSON_Delete(root);
    return TRUE;
}

gboolean
sql_filter_vars_is_silent(const char *name, const char *val)
{
    if (!name) {
        return FALSE;
    }
    struct sql_variable_t *var = g_hash_table_lookup(cetus_variables, name);
    if (!var) {
        return FALSE;
    }
    if (var->silence_all) {
        return TRUE;
    }
    if (!val) {
        return FALSE;
    }
    switch (var->type) {
    case VAL_STRING:
        return sql_variable_is_silent_value(var, val);
    case VAL_STRING_CSV:{
        gchar **values = g_strsplit_set(val, ", ", -1);
        int i = 0;
        for (i = 0; values[i] != NULL; ++i) {
            if (!sql_variable_is_silent_value(var, values[i])) {
                g_strfreev(values);
                return FALSE;
            }
        }
        g_strfreev(values);
        return TRUE;
    }
    default:
        g_warning(G_STRLOC ":not implemented");
        break;
    }
    return FALSE;
}

gboolean
sql_filter_vars_is_allowed(const char *name, const char *val)
{
    if (!name) {
        return FALSE;
    }
    struct sql_variable_t *var = g_hash_table_lookup(cetus_variables, name);
    if (!var) {
        return FALSE;
    }
    if (var->allow_all) {
        return TRUE;
    }
    if (!val) {
        return FALSE;
    }
    switch (var->type) {
    case VAL_STRING:
        return sql_variable_is_allowed_value(var, val);
    case VAL_STRING_CSV:{
        gchar **values = g_strsplit_set(val, ", ", -1);
        int i = 0;
        for (i = 0; values[i] != NULL; ++i) {
            if (!sql_variable_is_allowed_value(var, values[i])) {
                g_strfreev(values);
                return FALSE;
            }
        }
        g_strfreev(values);
        return TRUE;
    }
    default:
        g_warning(G_STRLOC "not implemented");
        break;
    }
    return FALSE;
}

void
sql_filter_vars_load_default_rules()
{
    static const char *default_var_rule = "{"
        "  \"variables\": ["
        "    {"
        "      \"name\": \"sql_mode\","
        "      \"type\": \"string-csv\","
        "      \"allowed_values\":  ["
        "          \"STRICT_TRANS_TABLES\","
        "          \"NO_AUTO_CREATE_USER\","
        "          \"NO_ENGINE_SUBSTITUTION\""
        "       ]"
        "    },"
        "    {"
        "      \"name\": \"autocommit\","
        "      \"type\": \"string\","
        "      \"allowed_values\": [\"*\"]"
        "    },"
        "    {"
        "      \"name\": \"character_set_client\","
        "      \"type\": \"string\","
        "      \"allowed_values\": [\"latin1\",\"ascii\",\"gb2312\",\"gbk\",\"utf8\",\"utf8mb4\",\"binary\",\"big5\"]"
        "    },"
        "    {"
        "      \"name\": \"character_set_connection\","
        "      \"type\": \"string\","
        "      \"allowed_values\": [\"latin1\",\"ascii\",\"gb2312\",\"gbk\",\"utf8\",\"utf8mb4\",\"binary\",\"big5\"]"
        "    },"
        "    {"
        "      \"name\": \"character_set_results\","
        "      \"type\": \"string\","
        "      \"allowed_values\": [\"latin1\",\"ascii\",\"gb2312\",\"gbk\",\"utf8\",\"utf8mb4\",\"binary\",\"big5\",\"NULL\"]"
        "    }" "  ]" "}";
    gboolean rc = sql_filter_vars_load_str_rules(default_var_rule);
    g_assert(rc);
}

void
sql_filter_vars_shard_load_default_rules()
{
    static const char *default_var_rule = "{"
        "  \"variables\": ["
        "    {"
        "      \"name\": \"autocommit\","
        "      \"type\": \"string\"," "      \"allowed_values\": [\"*\"]" "    }" "  ]" "}";
    gboolean rc = sql_filter_vars_load_str_rules(default_var_rule);
    g_assert(rc);
}

gboolean
sql_filter_vars_reload_str_rules(const char *json_str)
{
    if (!json_str) {
        return FALSE;
    }

    if (cetus_variables) {
        g_hash_table_remove_all(cetus_variables);
    }

    return sql_filter_vars_load_str_rules(json_str);
}

gboolean parse_variables_to_json(gchar **json) {
    cJSON *variables_node = cJSON_CreateArray();
    if (!variables_node) {
        g_warning(G_STRLOC ":variables_node is nil");
        return FALSE;
    }
    GHashTableIter iter;
    g_hash_table_iter_init(&iter, cetus_variables);
    gchar * name= NULL;
    struct sql_variable_t *var = NULL;
    cJSON *item = cJSON_CreateArray();
    while(g_hash_table_iter_next(&iter, (gpointer *) & name, (gpointer *) & var)) {
        cJSON *node = cJSON_CreateObject();
        cJSON_AddStringToObject(node,"name",var->name);
        cJSON_AddStringToObject(node,"type",value_type_name(var->type));
        cJSON *allow_node = NULL;
        if(var->allow_all) {
            allow_node = cJSON_CreateArray();
            cJSON_AddItemToArray(allow_node, cJSON_CreateString("*"));
        } else {
            if(var->allowed_values) {
                allow_node = cJSON_CreateArray();
                GList *it = NULL;
                for(it = var->allowed_values; it; it = it->next) {
                    cJSON_AddItemToArray(allow_node, cJSON_CreateString(it->data));
                }
            }
        }
        if(allow_node) {
            cJSON_AddItemToObject(node, "allowed_values", allow_node);
        }

        cJSON *silence_node = NULL;
        if(var->silence_all) {
            silence_node = cJSON_CreateArray();
            cJSON_AddItemToArray(silence_node, cJSON_CreateString("*"));
        } else {
            if(var->silent_values) {
                silence_node = cJSON_CreateArray();
                GList *it = NULL;
                for(it = var->silent_values; it; it = it->next) {
                    cJSON_AddItemToArray(silence_node, cJSON_CreateString(it->data));
                }
            }
        }
        if(silence_node) {
            cJSON_AddItemToObject(node, "silent_values", silence_node);
        }
        cJSON_AddItemToArray(variables_node, node);
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "variables", variables_node);
    *json = cJSON_Print(root);
    return TRUE;
}
