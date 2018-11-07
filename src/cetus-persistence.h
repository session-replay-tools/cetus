#ifndef _CETUS_PERSISTENCE_H_
#define _CETUS_PERSISTENCY_H_

#include <glib.h>
#include <stdlib.h>
#include <stdio.h>
#include "cJSON.h"
#include "chassis-mainloop.h"

typedef enum {
    CONFIG_TYPE,
    USERS_TYPE,
    VARIABLES_TYPE,
    VDB_TYPE,
    TABLES_TYPE,
    SINGLE_TABLES_TYPE
}config_type_t;

typedef enum {
    CONFIG_OPERATOR_SUCCESS,
    CONFIG_OPERATOR_ARGS_ERROR,
    CONFIG_OPERATOR_READFILE_ERROR,
    CONFIG_OPERATOR_WRITEFILE_ERROR,
    CONFIG_OPERATOR_JSONSYNTAX_ERROR,
    CONFIG_OPERATOR_JSONROOT_ERROR,
    CONFIG_OPERATOR_RMFILE_ERROR,
    CONFIG_OPERATOR_CHMOD_ERROR,
    CONFIG_OPERATOR_SAVE_ERROR
}config_operator_result_t;
//temporary file
gint rm_config_json_local(gchar *filename);
gint read_config_json_from_local(gchar *filename, gchar **str);
gint write_config_json_to_local(gchar *filename, gchar *str);
gint get_config_from_json_by_type(gchar *json, config_type_t type, gchar **str);

gint parse_config_to_json(chassis *chas, gchar **str);
gchar* get_config_value_from_json(const gchar *key, gchar *json);

gint load_temporary_from_local(chassis *chas);

gint load_config_from_temporary_file(chassis *chas);
gint save_config_to_temporary_file(chassis *chas, gchar *key, gchar *value);
gint sync_config_to_file(chassis *chas, gint *effected_rows);
gboolean config_set_local_option_by_key(chassis *chas, gchar *key);

gint load_users_from_temporary_file(chassis *chas);
gint save_users_to_temporary_file(chassis *chas);
gint sync_users_to_file(chassis *chas, gint *effected_rows);

gint load_variables_from_temporary_file(chassis *chas);
gint save_variables_to_temporary_file(chassis *chas);
gint sync_variables_to_file(chassis *chas, gint *effected_rows);

gint load_sharding_from_temporary_file(chassis *chas);
gint save_sharding_to_temporary_file(chassis *chas);
gint sync_sharding_to_file(chassis *chas, gint *effected_rows);


#endif
