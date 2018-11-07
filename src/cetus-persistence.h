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

//temporary file
gboolean rm_config_json_local(gchar *filename);
gboolean read_config_json_from_local(gchar *filename, gchar **str);
gboolean write_config_json_to_local(gchar *filename, gchar *str);
gboolean get_config_from_json_by_type(gchar *json, config_type_t type, gchar **str);

gboolean parse_config_to_json(chassis *chas, gchar **str);
gchar* get_config_value_from_json(const gchar *key, gchar *json);

gboolean load_temporary_from_local(chassis *chas);

gboolean load_config_from_temporary_file(chassis *chas);
gboolean save_config_to_temporary_file(chassis *chas, gchar *key, gchar *value);
gboolean sync_config_to_file(chassis *chas, gint *effected_rows);
gboolean config_set_local_option_by_key(chassis *chas, gchar *key);

gboolean load_users_from_temporary_file(chassis *chas);
gboolean save_users_to_temporary_file(chassis *chas);
gboolean sync_users_to_file(chassis *chas, gint *effected_rows);

gboolean load_variables_from_temporary_file(chassis *chas);
gboolean save_variables_to_temporary_file(chassis *chas);
gboolean sync_variables_to_file(chassis *chas, gint *effected_rows);

gboolean load_sharding_from_temporary_file(chassis *chas);
gboolean save_sharding_to_temporary_file(chassis *chas);
gboolean sync_sharding_to_file(chassis *chas, gint *effected_rows);


#endif
