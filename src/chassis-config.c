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

#include "chassis-config.h"
#include "chassis-timings.h"
#include "chassis-options.h"
#include "cetus-util.h"

#include <mysql.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <ctype.h>
#include <glib/gstdio.h>        /* for g_stat */

#define RF_MAX_NAME_LEN 128
struct config_object_t {
    char name[RF_MAX_NAME_LEN];
    char *cache;
    time_t mtime;
};

static void
config_object_free(struct config_object_t *ob)
{
    if (ob->cache)
        g_free(ob->cache);
    g_free(ob);
}

static gboolean
url_parse_user_pass(chassis_config_t *rconf, const char *userpass, int len)
{
    char *p = strndup(userpass, len);
    char *sep = strchr(p, ':');
    if (sep) {
        rconf->user = strndup(p, sep - p);
        rconf->password = strdup(sep + 1);
        g_free(p);
    } else {
        rconf->user = p;
    }
    return TRUE;
}

static gboolean
url_parse_host_port(chassis_config_t *rconf, const char *hostport, int len)
{
    char *p = strndup(hostport, len);
    char *sep = strchr(p, ':');
    if (sep) {
        rconf->host = strndup(p, sep - p);
        rconf->port = atoi(sep + 1);
        g_free(p);
    } else {
        rconf->host = p;
        rconf->port = 3306;
    }
    return TRUE;
}

static gboolean
url_parse_parameter(chassis_config_t *rconf, const char *param, int len)
{
    char **params = g_strsplit(param, "&", -1);
    int i = 0;
    GString *filter = g_string_new(0);
    for (i = 0; params[i]; ++i) {
        if (strncasecmp(params[i], "table=", 6) == 0) {
            rconf->options_table = strdup(params[i] + 6);
        } else {
            g_string_append(filter, params[i]);
            g_string_append(filter, " and ");
        }
    }
    if (filter->len >= 4) {     /* remove last 'and' */
        g_string_truncate(filter, filter->len - 4);
        rconf->options_filter = strdup(filter->str);
    }
    g_string_free(filter, TRUE);
    g_strfreev(params);
    return TRUE;
}

/* example mysql://user:pass@host:port/schema?table=xx&id=xx */
static gboolean
chassis_config_parse_mysql_url(chassis_config_t *rconf, const char *url, int len)
{
/* only "host" is required -> [dbuser[:[dbpassword]]@]host[:port][/schema] */
    char *param = strchr(url, '?');
    char *schema = strchr(url, '/');
    char *at = strchr(url, '@');

    if (schema) {
        if (param && param < schema)
            return FALSE;
        rconf->schema = param ? strndup(schema + 1, param - schema - 1) : strdup(schema + 1);
    }
    if (param)
        url_parse_parameter(rconf, param + 1, url + len - param);
    if (!rconf->options_table)
        rconf->options_table = g_strdup("settings");

    const char *hostend = schema ? schema : (param ? param : url + len);
    gboolean ok = FALSE;
    if (at) {
        ok = url_parse_user_pass(rconf, url, at - url);
        ok = ok && url_parse_host_port(rconf, at + 1, hostend - at - 1);
    } else {
        ok = url_parse_host_port(rconf, url, hostend - url);
    }
    return ok;
}

static MYSQL *
chassis_config_get_mysql_connection(chassis_config_t *conf)
{
    g_debug("%s:call chassis_config_get_mysql_connection", G_STRLOC);
    /* first try the cached connection */
    if (conf->mysql_conn) {
        g_warning("%s:mysql conn was not closed", G_STRLOC);
        mysql_close(conf->mysql_conn);
        conf->mysql_conn = NULL;
    }

    g_debug("%s:call mysql_init", G_STRLOC);
    MYSQL *conn = mysql_init(NULL);
    conf->mysql_init_called = 1;

    if (!conn)
        return NULL;

    unsigned int timeout = 1 * SECONDS;
    mysql_options(conn, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
    mysql_options(conn, MYSQL_OPT_READ_TIMEOUT, &timeout);
    mysql_options(conn, MYSQL_OPT_WRITE_TIMEOUT, &timeout);

    g_debug("%s:call mysql_real_connect", G_STRLOC);
    if (mysql_real_connect(conn, conf->host, conf->user, conf->password, NULL, conf->port, NULL, 0) == NULL) {
        g_critical("%s:%s", G_STRLOC, mysql_error(conn));
        mysql_close(conn);
        return NULL;
    }
    conf->mysql_conn = conn;    /* cache the connection */
    return conn;
}

static gboolean
chassis_config_mysql_init_tables(chassis_config_t *conf)
{
    MYSQL *conn = chassis_config_get_mysql_connection(conf);
    if (!conn) {
        g_critical("%s: MySQL conn is nil", G_STRLOC);
        return FALSE;
    }
    gboolean status = FALSE;
    char sql[256] = { 0 };
    snprintf(sql, sizeof(sql), "CREATE DATABASE IF NOT EXISTS %s", conf->schema);
    if (mysql_query(conn, sql)) {
        g_critical("%s:%s", G_STRLOC, mysql_error(conn));
        goto recycle_mysql_resources;
    }
    snprintf(sql, sizeof(sql), "CREATE TABLE IF NOT EXISTS %s.objects("
             "object_name varchar(64) NOT NULL,"
             "object_value text NOT NULL," "mtime timestamp NOT NULL," "PRIMARY KEY(object_name))", conf->schema);
    if (mysql_query(conn, sql)) {
        g_critical("%s:%s", G_STRLOC, mysql_error(conn));
        goto recycle_mysql_resources;
    }
    snprintf(sql, sizeof(sql), "CREATE TABLE IF NOT EXISTS %s.%s("
             "option_key varchar(64) NOT NULL,"
             "option_value varchar(1024) NOT NULL," "PRIMARY KEY(option_key))", conf->schema, conf->options_table);
    if (mysql_query(conn, sql)) {
        g_critical("%s:%s", G_STRLOC, mysql_error(conn));
    } else {
        status = TRUE;
    }
  
recycle_mysql_resources:
    mysql_close(conf->mysql_conn);
    conf->mysql_conn = NULL;
    return status;
}

chassis_config_t *
chassis_config_from_url(char *url)
{
    chassis_config_t *rconf = g_new0(chassis_config_t, 1);
    gboolean ok = FALSE;
    if (strncasecmp(url, "mysql://", 8) == 0) {
        rconf->type = CHASSIS_CONF_MYSQL;
        ok = chassis_config_parse_mysql_url(rconf, url + 8, strlen(url) - 8);
    } else if (strncasecmp(url, "sqlite://", 9) == 0) {
        ok = chassis_config_parse_mysql_url(rconf, url + 8, strlen(url) - 8);
    } else {
        g_warning("url not supported: %s", url);
    }
    if (ok)
        ok = chassis_config_mysql_init_tables(rconf);

    if (!ok) {
        chassis_config_free(rconf);
        return NULL;
    }
    return rconf;
}

/* TODO: */
chassis_config_t *
chassis_config_from_local_dir(char *conf_dir, char *conf_file)
{
    chassis_config_t *conf = g_new0(chassis_config_t, 1);
    conf->type = CHASSIS_CONF_LOCAL;
    conf->schema = g_strdup(conf_dir);
    conf->options_table = g_strdup(conf_file);
    return conf;
}

void
chassis_config_free(chassis_config_t *p)
{
    if (!p)
        return;
    if (p->user)
        g_free(p->user);
    if (p->password)
        g_free(p->password);
    if (p->host)
        g_free(p->host);
    if (p->schema)
        g_free(p->schema);
    if (p->options_table)
        g_free(p->options_table);
    if (p->options_filter)
        g_free(p->options_filter);
    if (p->options_one)
        g_hash_table_destroy(p->options_one);
    if (p->options_two)
        g_hash_table_destroy(p->options_two);
    if (p->objects_one)
        g_list_free_full(p->objects_one, (GDestroyNotify) config_object_free);
    if (p->objects_two)
        g_list_free_full(p->objects_two, (GDestroyNotify) config_object_free);
    if (p->mysql_conn) {
        mysql_close(p->mysql_conn);
    }
    if (p->type == CHASSIS_CONF_MYSQL && p->mysql_init_called) {
        mysql_thread_end();
        g_message("%s:mysql_thread_end is called", G_STRLOC);
    }
    g_free(p);
}

gboolean
chassis_config_load_options_mysql(chassis_config_t *conf)
{
    MYSQL *conn = chassis_config_get_mysql_connection(conf);
    if (!conn) {
        g_warning("chassis_config can't get mysql conn");
        return FALSE;
    }
    gboolean status = FALSE;
    char sql[1024] = { 0 };
    snprintf(sql, sizeof(sql), "SELECT option_key,option_value FROM %s.%s", conf->schema, conf->options_table);
    if (mysql_query(conn, sql)) {
        g_warning("sql failed: %s", sql);
        goto recycle_mysql_resources;
    }
    MYSQL_RES *result = mysql_store_result(conn);
    if (!result)
        goto recycle_mysql_resources;

    GHashTable *options;
    if (conf->options_index == 0) {
        if (!conf->options_one)
            conf->options_one = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
        else
            g_hash_table_remove_all(conf->options_one);

        options = conf->options_one;

    } else {
        if (!conf->options_two)
            conf->options_two = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
        else
            g_hash_table_remove_all(conf->options_two);

        options = conf->options_two;
    }

    MYSQL_ROW row;
    while ((row = mysql_fetch_row(result))) {
        g_hash_table_insert(options, g_strdup(row[0]), g_strdup(row[1]));
    }
    mysql_free_result(result);

    status = TRUE;

recycle_mysql_resources:
    if (status) {
        conf->options_update_flag = 0;
        conf->options_success_flag = 1;

    } else {
        conf->options_success_flag = 0;
        conf->options_update_flag = 0;
    }
    mysql_close(conf->mysql_conn);
    conf->mysql_conn = NULL;
    return status;
}


gboolean
chassis_config_set_remote_options(chassis_config_t *conf, gchar* key, gchar* value)
{
    if(conf->type != CHASSIS_CONF_MYSQL){
        return TRUE;
    }

    MYSQL *conn = chassis_config_get_mysql_connection(conf);
    if (!conn) {
        g_warning("%s:Cannot connect to mysql server.", G_STRLOC);
        conf->options_update_flag = 0;
        conf->options_success_flag = 0;
        return FALSE;
    }
    gboolean status = FALSE;
    gchar sql[1024] = { 0 }, real_value[1024] = { 0 };
    mysql_real_escape_string(conn, real_value, value, strlen(value));
    snprintf(sql, sizeof(sql),
            "INSERT INTO %s.`settings`(option_key,option_value) VALUES ('%s', '%s') ON DUPLICATE KEY UPDATE option_value = '%s'", conf->schema, key, real_value, real_value);
    if (mysql_query(conn, sql)) {
        g_warning("sql failed: %s | error: %s", sql, mysql_error(conn));
    } else {
        status = TRUE;
    }

recycle_mysql_resources:
    if (status) {
        conf->options_update_flag = 0;
        conf->options_success_flag = 1;

    } else {
        conf->options_success_flag = 0;
        conf->options_update_flag = 0;
    }
    mysql_close(conf->mysql_conn);
    conf->mysql_conn = NULL;

    return status;
}

gboolean
chassis_config_set_remote_backends(chassis_config_t *conf, gchar* key1, gchar* value1, gchar* key2, gchar* value2)
{
    if(conf->type != CHASSIS_CONF_MYSQL){
        return TRUE;
    }

    MYSQL *conn = chassis_config_get_mysql_connection(conf);
    if (!conn) {
        g_warning("%s:Cannot connect to mysql server.", G_STRLOC);
        conf->options_update_flag = 0;
        conf->options_success_flag = 0;
        return FALSE;
    }
    gboolean status = FALSE, status1 = FALSE, status2 = FALSE;
    gchar sql1[1024] = { 0 }, real_value1[1024] = { 0 };
    gchar sql2[1024] = { 0 }, real_value2[1024] = { 0 };
    if (key1) {
        if (value1) {
            mysql_real_escape_string(conn, real_value1, value1, strlen(value1));
            snprintf(sql1, sizeof(sql1),
                "INSERT INTO %s.`settings`(option_key,option_value) VALUES ('%s', '%s') ON DUPLICATE KEY UPDATE option_value = '%s'", conf->schema, key1, real_value1, real_value1);
        } else {
            snprintf(sql1, sizeof(sql1),
                "DELETE FROM %s.`settings` where option_key = '%s'", conf->schema, key1);
        }
        if (mysql_query(conn, sql1)) {
            g_warning("sql failed: %s | error: %s", sql1, mysql_error(conn));
        } else {
            status1 = TRUE;
        }
    }
    if (status1 && key2) {
        if (value2) {
            mysql_real_escape_string(conn, real_value2, value2, strlen(value2));
            snprintf(sql2, sizeof(sql2),
                "INSERT INTO %s.`settings`(option_key,option_value) VALUES ('%s', '%s') ON DUPLICATE KEY UPDATE option_value = '%s'", conf->schema, key2, real_value2, real_value2);
        } else {
            snprintf(sql2, sizeof(sql2),
                "DELETE FROM %s.`settings` where option_key = '%s'", conf->schema, key2);
        }
        if (mysql_query(conn, sql2)) {
            g_warning("sql failed: %s | error: %s", sql1, mysql_error(conn));
        } else {
            status2 = TRUE;
        }
    } 
    status = status1 && (key2 == NULL? TRUE:status2);

recycle_mysql_resources:
    if (status) {
        conf->options_update_flag = 0;
        conf->options_success_flag = 1;

    } else {
        conf->options_success_flag = 0;
        conf->options_update_flag = 0;
    }
    mysql_close(conf->mysql_conn);
    conf->mysql_conn = NULL;
    return status;
}

gint chassis_config_reload_options(chassis_config_t *conf)
{
    switch (conf->type) {
    case CHASSIS_CONF_MYSQL:
        if(chassis_config_load_options_mysql(conf)) {
            return 0;
        } else {
            return -1;
        }
    default:
        /* TODO g_critical(G_STRLOC " not implemented"); */
        return -2;
    }
}

GHashTable *
chassis_config_get_options(chassis_config_t *conf)
{
    GHashTable *options;
    if (conf->options_index == 0) {
        options = conf->options_one;
    } else {
        options = conf->options_two;
    }
    return options;
}

static void
chassis_config_object_set_cache(struct config_object_t *ob, const char *str, time_t mt)
{
    if (ob->cache) {
        g_free(ob->cache);
    }
    if (str) {
        ob->cache = g_strdup(str);
    } else {
        ob->cache = NULL;
    }
    ob->mtime = mt;
}

struct config_object_t *
chassis_config_get_object(chassis_config_t *conf, const char *name)
{
    GList *l = NULL;
    GList *objects;

    if (conf->objects_index == 0) {
        objects = conf->objects_one;
    } else {
        objects = conf->objects_two;
    }

    for (l = objects; l; l = l->next) {
        struct config_object_t *ob = l->data;
        if (strcmp(ob->name, name) == 0)
            return ob;
    }
    return NULL;
}

gboolean
chassis_config_mysql_query_object(chassis_config_t *conf,
                                  struct config_object_t *object, const char *name)
{
    g_debug("%s:reach mysql_query", G_STRLOC);
    g_assert(conf->type == CHASSIS_CONF_MYSQL);

    MYSQL *conn = chassis_config_get_mysql_connection(conf);

    if (!conn) {
        g_warning("%s:Cannot connect to mysql server.", G_STRLOC);
        return FALSE;
    }
        
    g_debug("%s:reach mysql_query", G_STRLOC);
    char sql[256] = { 0 };
    gboolean status = FALSE;
    snprintf(sql, sizeof(sql), "SELECT object_value,mtime FROM %s.objects where object_name='%s'", conf->schema, name);
    if (mysql_query(conn, sql)) {
        g_warning("sql failed: %s", sql);
        goto recycle_mysql_resources;
    }
    MYSQL_RES *result = mysql_store_result(conn);
    if (!result) { 
        g_debug("%s:reach mysql_store_result, result:%s", G_STRLOC, sql);
        goto recycle_mysql_resources;
    }

    MYSQL_ROW row;
    row = mysql_fetch_row(result);
    if (!row) {
        g_debug("%s:reach mysql_fetch_row", G_STRLOC);
        mysql_free_result(result);
        goto recycle_mysql_resources;
    }

    time_t mt = chassis_epoch_from_string(row[1], NULL);

    chassis_config_object_set_cache(object, row[0], mt);
    mysql_free_result(result);
    status = TRUE;
recycle_mysql_resources:
    mysql_close(conf->mysql_conn);
    conf->mysql_conn = NULL;
    return status;
}

static gboolean
chassis_config_local_query_object(chassis_config_t *conf,
                                  struct config_object_t *object, const char *name, char **json_res)
{
    char basename[128] = { 0 };
    snprintf(basename, sizeof(basename), "%s.%s", name, "json");
    char *object_file = g_build_filename(conf->schema, basename, NULL);
    char *buffer = NULL;
    GError *err = NULL;
    if (!g_file_get_contents(object_file, &buffer, NULL, &err)) {
        if (!g_error_matches(err, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
            g_critical(G_STRLOC " %s", err->message);
        }
        g_clear_error(&err);
        g_free(object_file);
        if (buffer) {
            g_free(buffer);
        }
        return FALSE;
    }

    if (json_res) {
        *json_res = buffer;
    }

    GStatBuf sta;
    if (g_stat(object_file, &sta) == 0) {
        chassis_config_object_set_cache(object, buffer, sta.st_mtime);
    }

    g_free(object_file);

    return TRUE;
}

/* select a table, make it into a json */
gboolean
chassis_config_query_object(chassis_config_t *conf, const char *name, char **json_res, int refresh)
{
    struct config_object_t *object = chassis_config_get_object(conf, name);
    if (!object) {
        object = g_new0(struct config_object_t, 1);
        strncpy(object->name, name, RF_MAX_NAME_LEN - 1);

        if (conf->objects_index == 0) {
            conf->objects_one = g_list_append(conf->objects_one, object);
        } else {
            conf->objects_two = g_list_append(conf->objects_two, object);
        }
    } else {
        if (refresh) {
            time_t now = time(0);
            chassis_config_object_set_cache(object, NULL, now);
        } else {
            if (object->cache) {
                *json_res = g_strdup(object->cache);
                return TRUE;
            }
        }
    }

    g_debug(G_STRLOC ": config type:%d", conf->type);
    switch (conf->type) {
    case CHASSIS_CONF_MYSQL:
        if (refresh) {
            return FALSE;
        } else {
            if (chassis_config_mysql_query_object(conf, object, name)) {
                if (object->cache) {
                    *json_res = g_strdup(object->cache);
                    return TRUE;
                } else {
                    return FALSE;
                }
            }  else {
                return FALSE;
            }
        }
    case CHASSIS_CONF_LOCAL:
        return chassis_config_local_query_object(conf, object, name, json_res);
    default:
        return FALSE;
    }
}

gboolean
chassis_config_mysql_write_object(chassis_config_t *conf,
                                  struct config_object_t *object, const char *name, const char *json)
{
    g_assert(conf->type == CHASSIS_CONF_MYSQL);
    MYSQL *conn = chassis_config_get_mysql_connection(conf);
    if (!conn) {
        g_warning("%s:Cannot connect to mysql server.", G_STRLOC);
        return FALSE;
    }

    gboolean status = TRUE;
    time_t now = time(0);
    GString *sql = g_string_new(0);
    g_string_printf(sql, "INSERT INTO %s.objects(object_name,object_value,mtime)"
                    " VALUES('%s','%s', FROM_UNIXTIME(%ld)) ON DUPLICATE KEY UPDATE object_value = '%s', mtime = FROM_UNIXTIME(%ld)", conf->schema, name, json, now, json, now);

    if (mysql_query(conn, sql->str)) {
        g_warning("sql failed: %s", sql->str);
        status = FALSE;
    } else {
        chassis_config_object_set_cache(object, json, now);
    }

    mysql_close(conf->mysql_conn);
    conf->mysql_conn = NULL;
    g_string_free(sql, TRUE);

    conf->options_update_flag = 0;
    if (status) {
        conf->options_success_flag = 1;
    } else {
        conf->options_success_flag = 0;
    }

    return status;
}

static gboolean
chassis_config_local_write_object(chassis_config_t *conf,
                                  struct config_object_t *object, const char *name, const char *json_str)
{
    /* conf->schema is an absolute path for local config */
    char basename[128] = { 0 };
    snprintf(basename, sizeof(basename), "%s.%s", name, "json");
    char *object_file = g_build_filename(conf->schema, basename, NULL);
    FILE *fp = fopen(object_file, "w"); /* truncate and write */
    if (!fp) {
        g_critical(G_STRLOC "can't open file: %s for write", object_file);
        g_free(object_file);
        return FALSE;
    }
    fwrite(json_str, 1, strlen(json_str), fp);
    fclose(fp);

    GStatBuf sta;
    if (g_stat(object_file, &sta) == 0) {
        chassis_config_object_set_cache(object, json_str, sta.st_mtime);
    }
    g_free(object_file);
    return TRUE;
}

gboolean
chassis_config_write_object(chassis_config_t *conf, const char *name, const char *json)
{
    struct config_object_t *object = chassis_config_get_object(conf, name);
    if (!object) {
        object = g_new0(struct config_object_t, 1);
        strncpy(object->name, name, RF_MAX_NAME_LEN - 1);

        if (conf->objects_index == 0) {
            conf->objects_one = g_list_append(conf->objects_one, object);
        } else {
            conf->objects_two = g_list_append(conf->objects_two, object);
        }
    }
    switch (conf->type) {
    case CHASSIS_CONF_MYSQL:
        conf->user_data = strdup(json);
        return FALSE;
    case CHASSIS_CONF_LOCAL:
        return chassis_config_local_write_object(conf, object, name, json);
    default:
        return FALSE;
    }
}

gboolean
chassis_config_parse_options(chassis_config_t *conf, GList *entries)
{
    GHashTable *opts_table = chassis_config_get_options(conf);

    if (!opts_table) {
        chassis_config_reload_options(conf);
        if (conf->options_index == 0) {
            opts_table = conf->options_one;
        } else {
            opts_table = conf->options_two;
        }
    }

    if (!opts_table) {
        return FALSE;
    }

    GList *l;
    for (l = entries; l; l = l->next) {
        chassis_option_t *entry = l->data;
        /* already set by cmdline or config file */
        if ((entry->flags & OPTION_FLAG_CMDLINE) || (entry->flags & OPTION_FLAG_CONF_FILE))
            continue;

        char *entry_value = g_hash_table_lookup(opts_table, entry->long_name);
        if (entry_value) {
            switch (entry->arg) {
            case OPTION_ARG_NONE:
                if (entry->arg_data == NULL)
                    break;
                if (strcasecmp(entry_value, "false")==0 || strncmp(entry_value, "0", 1)==0) {
                    *(int *)(entry->arg_data) = 0;
                } else if (strcasecmp(entry_value, "true")==0 || isdigit(entry_value[0])) {
                    *(int *)(entry->arg_data) = 1;
                } else {
                    g_warning("error boolean value: %s", entry_value);
                }
                break;
            case OPTION_ARG_INT:
                if (entry->arg_data == NULL)
                    break;
                *(int *)(entry->arg_data) = atoi(entry_value);
                break;
            case OPTION_ARG_INT64:
                if (entry->arg_data == NULL)
                    break;
                *(gint64 *)(entry->arg_data) = g_ascii_strtoll(entry_value, NULL, 10);
                break;
            case OPTION_ARG_STRING:{
                if (entry->arg_data == NULL || *(char **)entry->arg_data != NULL)
                    break;
                char *value = g_strdup(entry_value);
                *(char **)(entry->arg_data) = value;
                break;
            }
            case OPTION_ARG_STRING_ARRAY:{
                if (entry->arg_data == NULL || *(char **)entry->arg_data != NULL)
                    break;
                char **values = g_strsplit(entry_value, ",", -1);
                *(char ***)(entry->arg_data) = values;
                break;
            }
            case OPTION_ARG_DOUBLE:
                *(double *)(entry->arg_data) = atof(entry_value);
                break;
            default:
                g_warning("Unhandled option arg type: %d", entry->arg);
                break;
            }
        }
    }
    return TRUE;
}

gboolean
chassis_config_mysql_is_object_outdated(chassis_config_t *conf, struct config_object_t *object, const char *name)
{
    MYSQL *conn = chassis_config_get_mysql_connection(conf);
    if (!conn)
        return FALSE;
    static char sql[128] = { 0 };
    snprintf(sql, sizeof(sql), "SELECT mtime FROM %s.objects where object_name='%s'", conf->schema, name);
    gboolean status = FALSE;
    if (mysql_query(conn, sql)) {
        g_warning("sql failed: %s", sql);
        goto recycle_mysql_resources;
    }
    MYSQL_RES *result = mysql_store_result(conn);
    if (!result)
        goto recycle_mysql_resources;
    MYSQL_ROW row;
    row = mysql_fetch_row(result);
    if (!row)
        goto recycle_mysql_resources;
    time_t mt = chassis_epoch_from_string(row[0], NULL);
    mysql_free_result(result);
    status = TRUE;
recycle_mysql_resources:
    mysql_close(conf->mysql_conn);
    conf->mysql_conn = NULL;
    if (status) {
        return object->mtime < mt;
    } else {
        return FALSE;
    }
}

gboolean
chassis_config_local_is_object_outdated(chassis_config_t *conf, struct config_object_t *object, const char *name)
{
    GStatBuf sta;
    char basename[128] = { 0 };
    snprintf(basename, sizeof(basename), "%s.%s", name, "json");
    char *object_file = g_build_filename(conf->schema, basename, NULL);
    if (g_stat(object_file, &sta)) {
        g_free(object_file);
        return FALSE;
    }
    g_free(object_file);
    return object->mtime < sta.st_mtime;
}

gboolean
chassis_config_is_object_outdated(chassis_config_t *conf, const char *name)
{
    struct config_object_t *object = chassis_config_get_object(conf, name);
    if (!object) {
        return FALSE;
    }
    switch (conf->type) {
    case CHASSIS_CONF_MYSQL:
        return chassis_config_mysql_is_object_outdated(conf, object, name);
    case CHASSIS_CONF_LOCAL:
        return chassis_config_local_is_object_outdated(conf, object, name);
    default:
        return FALSE;
    }
}

#define MAX_ID_SIZE 127
char *
chassis_config_get_id(chassis_config_t *conf)
{
    GString *id = g_string_new(0);
    switch (conf->type) {
    case CHASSIS_CONF_MYSQL:
        g_string_printf(id, "%s_%d_%s", conf->host, conf->port, conf->schema);
        break;
    case CHASSIS_CONF_LOCAL:
        g_string_assign(id, conf->schema);
        break;
    default:
        g_string_assign(id, "error-config-id");
        g_assert(0);
        break;
    }
    g_string_append_printf(id, "_%u_%u", g_random_int(), g_random_int());
    if (id->len >= MAX_ID_SIZE) {
        g_string_erase(id, 0, id->len - MAX_ID_SIZE);
        g_warning("id truncated to: %s", id->str);
    }
    char *id_str = id->str;
    g_string_free(id, FALSE);
    return id_str;
}

gboolean
chassis_config_register_service(chassis_config_t *conf, char *id, char *data)
{
    if (conf->type != CHASSIS_CONF_MYSQL)
        return FALSE;

    MYSQL *conn = chassis_config_get_mysql_connection(conf);
    if (!conn) {
        g_critical("%s: MySQL conn is nil", G_STRLOC);
        return FALSE;
    }

    gboolean status = FALSE;
    char sql[512] = { 0 };
    snprintf(sql, sizeof(sql), "CREATE TABLE IF NOT EXISTS %s.services("
             "id varchar(64) NOT NULL,"
             "data varchar(64) NOT NULL," "start_time timestamp, PRIMARY KEY(id))", conf->schema);
    if (mysql_query(conn, sql)) {
        g_critical("%s:%s", G_STRLOC, mysql_error(conn));
        goto recycle_mysql_resources;
    }
    time_t now = time(0);
    snprintf(sql, sizeof(sql), "INSERT INTO %s.services(id, data, start_time)"
             " VALUES('%s','%s',FROM_UNIXTIME(%ld)) ON DUPLICATE KEY UPDATE"
             " start_time=FROM_UNIXTIME(%ld)", conf->schema, id, data, now, now);
    if (mysql_query(conn, sql)) {
        g_critical("%s:%s", G_STRLOC, mysql_error(conn));
        goto recycle_mysql_resources;
    }

    status = TRUE;
recycle_mysql_resources:
    mysql_close(conf->mysql_conn);
    conf->mysql_conn = NULL;
    return status;
}

void
chassis_config_unregister_service(chassis_config_t *conf, char *id)
{
    if (conf == NULL || conf->type != CHASSIS_CONF_MYSQL)
        return;

    MYSQL *conn = chassis_config_get_mysql_connection(conf);
    if (!conn) {
        g_critical("%s: MySQL conn is nil", G_STRLOC);
        return;
    }

    char sql[512] = { 0 };
    snprintf(sql, sizeof(sql), "DELETE FROM %s.services WHERE id='%s'", conf->schema, id);
    if (mysql_query(conn, sql)) {
        g_critical("%s:%s", G_STRLOC, mysql_error(conn));
    }

    mysql_close(conf->mysql_conn);
    conf->mysql_conn = NULL;
}

gboolean
chassis_config_reload_variables(chassis_config_t *conf, const char *name)
{
    g_debug("call chassis_config_reload_variables");
    gboolean status = FALSE;
    MYSQL *conn = chassis_config_get_mysql_connection(conf);

    if (!conn) {
        g_warning("%s:Cannot connect to mysql server when reload variables.", G_STRLOC);
        return FALSE;
    }
    char sql[256] = { 0 };
    snprintf(sql, sizeof(sql), "SELECT object_value,mtime FROM %s.objects where object_name='%s'", conf->schema, name);
    if (mysql_query(conn, sql)) {
        g_warning("sql failed: %s, when reload variables, mysql_errno: %d", sql, mysql_errno(conn));
        goto recycle_mysql_resources;
    }
    MYSQL_RES *result = mysql_store_result(conn);
    if (!result) {
        g_debug("%s:call mysql_store_result nil", G_STRLOC);
        goto recycle_mysql_resources;
    }

    MYSQL_ROW row;
    row = mysql_fetch_row(result);
    if (!row) {
        g_debug("%s:call mysql_fetch_row nil", G_STRLOC);
        mysql_free_result(result);
        goto recycle_mysql_resources;
    }

    time_t mt = chassis_epoch_from_string(row[1], NULL);

    struct config_object_t *object = chassis_config_get_object(conf, name);
    if (!object) {
        mysql_free_result(result);
        g_debug("call chassis_config_get_object nil");
        goto recycle_mysql_resources;
    }
    chassis_config_object_set_cache(object, row[0], mt);
    mysql_free_result(result);
    status = TRUE;

recycle_mysql_resources:
    if (status) {
        conf->options_update_flag = 0;
        conf->options_success_flag = 1;
    } else {
        conf->options_update_flag = 0;
        conf->options_success_flag = 0;
    }
    mysql_close(conf->mysql_conn);
    conf->mysql_conn = NULL;
    return status;
}
