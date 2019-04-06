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

#ifndef CHASSIS_CONFIG_H
#define CHASSIS_CONFIG_H

#include "glib-ext.h"
#include <mysql.h>

enum chassis_config_type_t {
    CHASSIS_CONF_SSH,
    CHASSIS_CONF_FTP,
    CHASSIS_CONF_MYSQL,
    CHASSIS_CONF_SQLITE,
    CHASSIS_CONF_LOCAL,         /* maybe unify local directory? */
};

struct chassis_config_t {
    unsigned int port:16;
    unsigned int retries:4;
    unsigned int options_success_flag:1;
    unsigned int options_update_flag:1;
    unsigned int options_index:1;
    unsigned int objects_index:1;
    unsigned int mysql_init_called:1;
    int   ms_timeout;
    enum  chassis_config_type_t type;
    char *user;
    char *password;
    char *host;
    char *schema;

/* this mysql conn might be used in 2 threads,
 * on startup, the main thread use it to load config
 * while running, the monitor thread use it periodically
 * for now, it is guaranteed not used simutaneously, so it's not locked
 */
    MYSQL *mysql_conn;

    char  *options_table;
    char  *options_filter;
    GHashTable *options_one;
    GHashTable *options_two;

    GList *objects_one;
    GList *objects_two;
    void *user_data;
    void *key;
    void *value;
    void *reserve1;
    void *reserve2;
};

/**
 * The config manager module, it manages `options` and `object`
 * currently support `mysql` and `local` media type
 */

typedef struct chassis_config_t chassis_config_t;

chassis_config_t *chassis_config_from_url(char *url);

chassis_config_t *chassis_config_from_local_dir(char *dir, char *conf_file);

void chassis_config_free(chassis_config_t *);

gint chassis_config_reload_options(chassis_config_t *conf);
gboolean chassis_config_load_options_mysql(chassis_config_t *conf);

gboolean chassis_config_set_remote_options(chassis_config_t *conf, gchar* key, gchar* value);

gboolean chassis_config_set_remote_backends(chassis_config_t *conf, gchar* key1, gchar* value1, gchar* key2, gchar* value2);

GHashTable *chassis_config_get_options(chassis_config_t *);

gboolean chassis_config_parse_options(chassis_config_t *, GList *entries);

gboolean chassis_config_query_object(chassis_config_t *, const char *name, char **json, int refresh);

struct config_object_t *chassis_config_get_object(chassis_config_t *conf, const char *name);

gboolean chassis_config_mysql_query_object(chassis_config_t *conf, struct config_object_t *object, const char *name);

gboolean chassis_config_write_object(chassis_config_t *, const char *name, const char *json);

gboolean chassis_config_is_object_outdated(chassis_config_t *, const char *name);

char *chassis_config_get_id(chassis_config_t *);

gboolean chassis_config_register_service(chassis_config_t *conf, char *id, char *data);

void chassis_config_unregister_service(chassis_config_t *conf, char *id);

gboolean chassis_config_reload_variables(chassis_config_t *conf, const char *name);

gboolean sql_filter_vars_reload_str_rules(const char *json_str);

gboolean chassis_config_mysql_write_object(chassis_config_t *conf, struct config_object_t *object, const char *name, const char *json);

#endif /* CHASSIS_CONFIG_H */
