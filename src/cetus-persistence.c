#include "cetus-persistence.h"
#include "chassis-options.h"
#include "chassis-options-utils.h"
#include "cetus-users.h"
#include "sharding-config.h"
#include "chassis-plugin.h"
#include <string.h>

gboolean rm_config_json_local(gchar *filename) {
    if(!filename) return FALSE;
    if(g_file_test(filename, G_FILE_TEST_EXISTS)) {
        gint ret = g_unlink(filename);
        if(ret == 0) {
            return TRUE;
        } else {
            g_critical("unlink file: %s failed", filename);
            return FALSE;
        }
    }
    return TRUE;
}

gboolean read_config_json_from_local(gchar *filename, gchar **str) {
    if(!filename) return FALSE;
    gchar *buffer = NULL;
    GError *err = NULL;
    if (!g_file_get_contents(filename, &buffer, NULL, &err)) {
        if (!g_error_matches(err, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
            g_critical(G_STRLOC "read config file failed:  %s", err->message);
        }
        g_clear_error(&err);
        return FALSE;
    }
    *str = buffer;
    return TRUE;
}

gboolean write_config_json_to_local(gchar *filename, gchar *str) {
    if(!filename) return FALSE;
    FILE *fp = fopen(filename, "w");
    if (!fp) {
        g_critical(G_STRLOC "can't open file: %s for write", filename);
        return FALSE;
    }
    if(str) {
        fwrite(str, 1, strlen(str), fp);
    }
    fclose(fp);
    return  TRUE;
}

gboolean get_config_from_json_by_type(gchar *json, config_type_t type, gchar **str) {
    if(!json) return TRUE;
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        g_critical(G_STRLOC ":json syntax error in get_config_from_json_by_type()");
        return FALSE;
    }
    switch(type) {
    case CONFIG_TYPE:{
        cJSON *config = cJSON_GetObjectItem(root, "config");
        *str = cJSON_Print(config);
        break;
    }
    case USERS_TYPE:{
        cJSON *users = cJSON_GetObjectItem(root, "users");
        *str = cJSON_Print(users);
        break;
    }
    case VARIABLES_TYPE:{
        cJSON *variables = cJSON_GetObjectItem(root, "variables");
        *str = cJSON_Print(variables);
        break;
    }
    case VDB_TYPE:{
        cJSON *vdb = cJSON_GetObjectItem(root, "vdb");
        *str = cJSON_Print(vdb);
        break;
    }
    case TABLES_TYPE:{
        cJSON *tables = cJSON_GetObjectItem(root, "table");
        *str = cJSON_Print(tables);
        break;
    }
    case SINGLE_TABLES_TYPE:{
        cJSON * single= cJSON_GetObjectItem(root, "single_tables");
        *str = cJSON_Print(single);
        break;
    }
    default:
        g_critical(G_STRLOC ":type unrecognized in get_config_from_json_by_type()");
        return FALSE;
    }
    cJSON_Delete(root);
    return TRUE;
}

gboolean parse_config_to_json(chassis *chas, gchar **str) {
    cJSON *config_node = cJSON_CreateArray();
    if(!config_node) {
        g_warning(G_STRLOC ":cJSON_CreateArray failed in parse_config_to_json()");
        return FALSE;
    }
    GList* list = chas->options->options;
    if(!list) {
        return FALSE;
    }
    GList *l = NULL;
    for(l = list; l; l = l->next) {
        chassis_option_t *opt = l->data;
        struct external_param param = {0};
        param.chas = chas;
        param.opt_type = SAVE_OPTS_PROPERTY;
        gchar *value = opt->show_hook != NULL? opt->show_hook(&param) : NULL;
        if(value) {
            cJSON *node = cJSON_CreateObject();
            cJSON_AddStringToObject(node, "key", opt->long_name);
            cJSON_AddStringToObject(node, "value", value);
            cJSON_AddItemToArray(config_node, node);
        }
    }
    cJSON *root = cJSON_CreateObject();
    if(!root) {
        g_warning(G_STRLOC ":cJSON_CreateObject failed in parse_config_to_json()");
        return FALSE;
    }
    cJSON_AddItemToObject(root, "config", config_node);
    *str = cJSON_Print(root);
    cJSON_Delete(root);
    return TRUE;
}

gchar* get_config_value_from_json(const gchar *key, gchar *json) {
    if(!json) {
        g_critical(G_STRLOC ":json content is nil in get_config_value_from_json()");
        return NULL;
    }
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        g_critical(G_STRLOC ":json syntax error in get_config_value_from_json()");
        return NULL;
    }
    cJSON *config_node = cJSON_GetObjectItem(root, "config");
    if(!config_node) {
        cJSON_Delete(root);
        return NULL;
    }
    cJSON *key_node = config_node->child;
    if(!key_node) {
        cJSON_Delete(root);
        return NULL;
    }
    for(;key_node; key_node = key_node->next) {
        cJSON *keyjson = cJSON_GetObjectItem(key_node, "key");
        if (!keyjson) {
            g_critical(G_STRLOC ": config error, no key, in get_config_value_from_json()");
            break;
        }
        if(strcasecmp(key, keyjson->valuestring) == 0) {
            cJSON *valuejson = cJSON_GetObjectItem(key_node, "value");
            gchar *value = g_strdup(valuejson->valuestring);
            cJSON_Delete(root);
            return value;
        }
    }
    cJSON_Delete(root);
    return NULL;
}

gboolean load_temporary_from_local(chassis *chas) {
    gchar *json = NULL;
    gboolean ret = read_config_json_from_local(chas->temporary_file, &json);
    if(!ret) {
        g_critical(G_STRLOC ": read config json from local failed");
        return ret;
    }
    if(chas->temporary_json) {
        g_free(chas->temporary_json);
        chas->temporary_json = NULL;
    }
    chas->temporary_json = json;
    return TRUE;
}

gboolean load_config_from_temporary_file(chassis *chas) {
    gboolean ret = load_temporary_from_local(chas);
    if(!ret) {
        return ret;
    }
    if(!chas->temporary_json) {
        return TRUE;
    }
    gchar *json = NULL;
    ret = get_config_from_json_by_type(chas->temporary_json, CONFIG_TYPE, &json);
    if(!ret) {
        return ret;
    }
    GList* list = chas->options->options;
    GList *l = NULL;
    for(l = list; l; l = l->next) {
        chassis_option_t *opt = l->data;
        gchar* value = get_config_value_from_json(opt->long_name, chas->temporary_json);
        if(value) {
            gint r = 0;
            struct external_param param = {0};
            param.chas = chas;
            param.opt_type = ASSIGN_OPTS_PROPERTY;
            r = opt->assign_hook != NULL? opt->assign_hook(value, &param) : ASSIGN_NOT_SUPPORT;
            if(r != 0) {
                g_critical(G_STRLOC ": load %s from temporary failed", opt->long_name);
            }
            g_free(value);
        }
    }
    return TRUE;
}

gboolean save_config_to_temporary_file(chassis *chas, gchar *key, gchar *value) {
    cJSON *root = NULL;
    if(chas->temporary_json) {
        root = cJSON_Parse(chas->temporary_json);
        if (!root) {
            g_critical(G_STRLOC ":json syntax error in save_config_to_temporary_file()");
            return FALSE;
        }
    } else {
        root = cJSON_CreateObject();
    }
    cJSON *config_node = cJSON_GetObjectItem(root, "config");
    if(!config_node) {
        config_node = cJSON_CreateArray();
        cJSON *node = cJSON_CreateObject();
        cJSON_AddStringToObject(node, "key", key);
        cJSON_AddStringToObject(node, "value", value);
        cJSON_AddItemToObject(root, "config", config_node);
        cJSON_AddItemToArray(config_node, node);
        goto save;
    }
    cJSON *key_node = config_node->child;
    if(!key_node) {
        cJSON_Delete(root);
        return FALSE;
    }
    for(;key_node; key_node = key_node->next) {
        cJSON *keyjson = cJSON_GetObjectItem(key_node, "key");
        if (!keyjson) {
            g_critical(G_STRLOC ": config error, no key");
            break;
        }
        if(strcasecmp(key, keyjson->valuestring) == 0) {
            cJSON *valuejson = cJSON_GetObjectItem(key_node, "value");
            if(strcasecmp(value, valuejson->valuestring) != 0) {
                cJSON_DeleteItemFromObject(key_node, "value");
                cJSON_AddItemToObject(key_node, "value", cJSON_CreateString(value));
                goto save;
            } else {
                cJSON_Delete(root);
                return TRUE;
            }
        }
    }

    cJSON *node = cJSON_CreateObject();
    cJSON_AddStringToObject(node, "key", key);
    cJSON_AddStringToObject(node, "value", value);
    cJSON_AddItemToArray(config_node, node);

save:
    if(chas->temporary_json) {
        g_free(chas->temporary_json);
    }
    chas->temporary_json = cJSON_Print(root);
    cJSON_Delete(root);
    return write_config_json_to_local(chas->temporary_file, chas->temporary_json);
}

gboolean sync_config_to_file(chassis *chas, gint *effected_rows) {
    GKeyFile *keyfile = g_key_file_new();
    g_key_file_set_list_separator(keyfile, ',');
    GString *free_path = g_string_new(NULL);
    if(chas->default_file == NULL) {
        free_path = g_string_append(free_path, chas->conf_dir);
        free_path = g_string_append(free_path, "/default.conf");
        chas->default_file = g_strdup(free_path->str);
    }
    if(!g_path_is_absolute(chas->default_file)) {
        gchar * current_dir =  g_get_current_dir();
        free_path = g_string_append(free_path, current_dir);
        free_path = g_string_append(free_path, "/");
        free_path = g_string_append(free_path, chas->default_file);
        g_free(chas->default_file);
        chas->default_file = g_strdup(free_path->str);
        g_free(current_dir);
    }
    if(free_path) {
        g_string_free(free_path, TRUE);
    }
    *effected_rows = chassis_options_save(keyfile, chas->options, chas);
    gsize file_size = 0;
    gchar *file_buf = g_key_file_to_data(keyfile, &file_size, NULL);
    GError *gerr = NULL;
    if (FALSE == g_file_set_contents(chas->default_file, file_buf, file_size, &gerr)) {
        g_clear_error(&gerr);
        return FALSE;
    } else {
        if(chmod(chas->default_file, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP)) {
            g_debug("chmod operate failed, filename:%s, errno:%d",
                    (chas->default_file == NULL? "":chas->default_file), errno);
            return FALSE;
        }
    }
    return TRUE;
}

gboolean save_users_to_temporary_file(chassis *chas) {
    cetus_users_t *users = chas->priv->users;

    cJSON *users_node = cJSON_CreateArray();
    if (!users_node) {
        g_warning(G_STRLOC ":users_node is nil in save_users_to_temporary_file()");
        return FALSE;
    }
    GHashTableIter iter;
    char *username = NULL;
    struct pwd_pair_t *pwd = NULL;
    g_hash_table_iter_init(&iter, users->records);
    while (g_hash_table_iter_next(&iter, (gpointer *) & username, (gpointer *) & pwd)) {
        cJSON *node = cJSON_CreateObject();
        cJSON_AddStringToObject(node, "user", username);
        cJSON_AddStringToObject(node, "client_pwd", pwd->client);
        cJSON_AddStringToObject(node, "server_pwd", pwd->server);
        cJSON_AddItemToArray(users_node, node);
    }

    cJSON *root = NULL;
    if(chas->temporary_json) {
        root = cJSON_Parse(chas->temporary_json);
        if (!root) {
            g_critical(G_STRLOC ":json syntax error in save_users_to_temporary_file()");
            return FALSE;
        }
    } else {
        root = cJSON_CreateObject();
    }
    cJSON *users_node_old = cJSON_GetObjectItem(root, "users");
    if(users_node_old) {
        cJSON_DeleteItemFromObject(root, "users");
    }
    cJSON_AddItemToObject(root, "users", users_node);

    if(chas->temporary_json) {
        g_free(chas->temporary_json);
    }
    chas->temporary_json = cJSON_Print(root);
    cJSON_Delete(root);
    return write_config_json_to_local(chas->temporary_file, chas->temporary_json);
}

gboolean load_users_from_temporary_file(chassis *chas) {
    gboolean ret = load_temporary_from_local(chas);
    if(!ret) {
        return ret;
    }
    if(!chas->temporary_json) {
        return TRUE;
    }
    gchar *json = NULL;
    ret = get_config_from_json_by_type(chas->temporary_json, USERS_TYPE, &json);
    if(!ret) {
        return ret;
    }
    if(json) {
        cJSON *root = cJSON_CreateObject();
        cJSON *users_node = cJSON_Parse(json);
        cJSON_AddItemToObject(root, "users", users_node);
        gchar *users_json = cJSON_Print(root);
        cJSON_Delete(root);
        cetus_users_parse_json(chas->priv->users, users_json);
    }
    return TRUE;
}

gboolean sync_users_to_file(chassis *chas, gint *effected_rows) {
    if(!chas->temporary_json) {
        return TRUE;
    }
    gchar *json = NULL;
    gboolean ret = get_config_from_json_by_type(chas->temporary_json, USERS_TYPE, &json);
    if(!ret) {
        return ret;
    }
    if(json) {
        cJSON *root = cJSON_CreateObject();
        cJSON *users_node = cJSON_Parse(json);
        cJSON_AddItemToObject(root, "users", users_node);
        gchar *users_json = cJSON_Print(root);
        cJSON_Delete(root);
        chassis_config_write_object(chas->priv->users->conf_manager, "users", users_json);
    }
    return TRUE;
}

gboolean load_variables_from_temporary_file(chassis *chas) {
    gboolean ret = load_temporary_from_local(chas);
    if(!ret) {
        return ret;
    }
    if(!chas->temporary_json) {
        return TRUE;
    }
    gchar *json = NULL;
    ret = get_config_from_json_by_type(chas->temporary_json, VARIABLES_TYPE, &json);
    if(!ret) {
        return ret;
    }
    if(json) {
        cJSON *root = cJSON_CreateObject();
        cJSON *variables_node = cJSON_Parse(json);
        cJSON_AddItemToObject(root, "variables", variables_node);
        gchar *variables_json = cJSON_Print(root);
        cJSON_Delete(root);
        sql_filter_vars_load_str_rules(variables_json);
    }
    return TRUE;
}

gboolean save_variables_to_temporary_file(chassis *chas) {
    gchar *json = NULL;
    parse_variables_to_json(&json);
    cJSON *variables_root = cJSON_Parse(json);
    cJSON *variables_node = cJSON_GetObjectItem(variables_root, "variables");
    cJSON *variables_node_copy = cJSON_Duplicate(variables_node, 1);
    cJSON *root = NULL;
    if(chas->temporary_json) {
        root = cJSON_Parse(chas->temporary_json);
        if (!root) {
            g_critical(G_STRLOC ":json syntax error in save_variables_to_temporary_file()");
            return FALSE;
        }
    } else {
        root = cJSON_CreateObject();
        if(!root) {
            g_critical(G_STRLOC ":cJSON_CreateObject failed in save_variables_to_temporary_file()");
            return FALSE;
        }
    }
    cJSON *variables_node_old = cJSON_GetObjectItem(root, "variables");
    if(variables_node_old) {
        cJSON_DeleteItemFromObject(root, "variables");
    }
    cJSON_AddItemToObject(root, "variables", variables_node_copy);

    if(chas->temporary_json) {
        g_free(chas->temporary_json);
    }
    chas->temporary_json = cJSON_Print(root);
    cJSON_Delete(root);
    cJSON_Delete(variables_root);
    return write_config_json_to_local(chas->temporary_file, chas->temporary_json);
}

gboolean sync_variables_to_file(chassis *chas, gint *effected_rows) {
    if(!chas->temporary_json) {
        return TRUE;
    }
    gchar *json = NULL;
    gboolean ret = get_config_from_json_by_type(chas->temporary_json, VARIABLES_TYPE, &json);
    if(!ret) {
        return ret;
    }
    if(json) {
        cJSON *root = cJSON_CreateObject();
        cJSON *variables_node = cJSON_Parse(json);
        cJSON_AddItemToObject(root, "variables", variables_node);
        gchar *variables_json = cJSON_Print(root);
        cJSON_Delete(root);
        chassis_config_write_object(chas->priv->users->conf_manager, "variables", variables_json);
    }
    return TRUE;
}

gboolean save_sharding_to_temporary_file(chassis *chas) {
    gchar *vdb_json = NULL;
    gchar *tables_json = NULL;
    gchar *single_tables_json = NULL;
    parse_vdb_to_json(&vdb_json);
    parse_tables_to_json(&tables_json);
    parse_single_tables_to_json(&single_tables_json);

    cJSON *root = NULL;
    if(chas->temporary_json) {
        root = cJSON_Parse(chas->temporary_json);
        if (!root) {
            g_critical(G_STRLOC ":json syntax error in save_sharding_to_temporary_file()");
            return FALSE;
        }
    } else {
        root = cJSON_CreateObject();
        if(!root) {
            g_critical(G_STRLOC ":cJSON_CreateObject failed in save_sharding_to_temporary_file()");
            return FALSE;
        }
    }

    if(vdb_json) {
        cJSON *vdb_root = cJSON_Parse(vdb_json);
        cJSON *vdb_node = cJSON_GetObjectItem(vdb_root, "vdb");

        cJSON *vdb_node_old = cJSON_GetObjectItem(root, "vdb");
        if(vdb_node_old) {
            cJSON_DeleteItemFromObject(root, "vdb");
        }

        cJSON *vdb_node_copy = cJSON_Duplicate(vdb_node, 1);
        cJSON_AddItemToObject(root, "vdb", vdb_node_copy);
        cJSON_Delete(vdb_root);
    }

    if(tables_json) {
        cJSON *tables_root = cJSON_Parse(tables_json);
        cJSON *tables_node = cJSON_GetObjectItem(tables_root, "table");

        cJSON *tables_node_old = cJSON_GetObjectItem(root, "table");
        if(tables_node_old) {
            cJSON_DeleteItemFromObject(root, "table");
        }

        cJSON *tables_node_copy = cJSON_Duplicate(tables_node, 1);
        cJSON_AddItemToObject(root, "table", tables_node_copy);
        cJSON_Delete(tables_root);
    }

    if(single_tables_json) {
        cJSON *single_root = cJSON_Parse(single_tables_json);
        cJSON *single_node = cJSON_GetObjectItem(single_root, "single_tables");

        cJSON *single_node_old = cJSON_GetObjectItem(root, "single_tables");
        if(single_node_old) {
            cJSON_DeleteItemFromObject(root, "single_tables");
        }

        cJSON *single_node_copy = cJSON_Duplicate(single_node, 1);
        cJSON_AddItemToObject(root, "single_tables", single_node_copy);
        cJSON_Delete(single_root);
    }

    if(chas->temporary_json) {
        g_free(chas->temporary_json);
    }
    chas->temporary_json = cJSON_Print(root);
    cJSON_Delete(root);
    return write_config_json_to_local(chas->temporary_file, chas->temporary_json);
}

gboolean load_sharding_from_temporary_file(chassis *chas) {
    gboolean ret = load_temporary_from_local(chas);
    if(!ret) {
        return ret;
    }
    if(!chas->temporary_json) {
        return TRUE;
    }
    gchar *vdb_json = NULL;
    gchar *tables_json = NULL;
    gchar *single_json = NULL;
    cJSON *sharding_root = cJSON_CreateObject();

    ret = get_config_from_json_by_type(chas->temporary_json, VDB_TYPE, &vdb_json);
    if(!ret) {
        cJSON_Delete(sharding_root);
        return ret;
    }
    if(vdb_json) {
        cJSON *vdb_node = cJSON_Parse(vdb_json);
        cJSON *vdb_node_copy = cJSON_Duplicate(vdb_node, 1);
        cJSON_AddItemToObject(sharding_root, "vdb", vdb_node_copy);
        cJSON_Delete(vdb_node);
    }

    ret = get_config_from_json_by_type(chas->temporary_json, TABLES_TYPE, &tables_json);
    if(!ret) {
        cJSON_Delete(sharding_root);
        return ret;
    }
    if(tables_json) {
        cJSON *tables_node = cJSON_Parse(tables_json);
        cJSON *table_node_copy = cJSON_Duplicate(tables_node, 1);
        cJSON_AddItemToObject(sharding_root, "table", table_node_copy);
        cJSON_Delete(tables_node);
    }

    ret = get_config_from_json_by_type(chas->temporary_json, SINGLE_TABLES_TYPE, &single_json);
    if(!ret) {
        cJSON_Delete(sharding_root);
        return ret;
    }
    if(single_json) {
        cJSON *single_node = cJSON_Parse(single_json);
        cJSON *single_node_copy = cJSON_Duplicate(single_node, 1);
        cJSON_AddItemToObject(sharding_root, "single_tables", single_node_copy);
        cJSON_Delete(single_node);
    }
    gchar *sharding_json = cJSON_Print(sharding_root);
    gint num_groups = chas->priv->backends->groups->len;
    if (shard_conf_load(sharding_json, num_groups)) {
        g_message("sharding config is updated");
    } else {
        g_warning("sharding config update failed");
    }
    cJSON_Delete(sharding_root);
    return TRUE;
}

gboolean sync_sharding_to_file(chassis *chas, gint *effected_rows) {
    if(!chas->temporary_json) {
        return TRUE;
    }
    gchar *vdb_json = NULL;
    gchar *tables_json = NULL;
    gchar *single_tables_json = NULL;
    gchar *sharding_json = NULL;
    cJSON *root = cJSON_CreateObject();

    gboolean ret = get_config_from_json_by_type(chas->temporary_json, VDB_TYPE, &vdb_json);
    if(!ret) {
        return ret;
    }
    if(vdb_json) {
        cJSON *vdb_node = cJSON_Parse(vdb_json);
        cJSON *vdb_node_copy = cJSON_Duplicate(vdb_node, 1);
        cJSON_AddItemToObject(root, "vdb", vdb_node_copy);
        cJSON_Delete(vdb_node);
    }

    ret = get_config_from_json_by_type(chas->temporary_json, TABLES_TYPE, &tables_json);
    if(!ret) {
        return ret;
    }
    if(tables_json) {
        cJSON *tables_node = cJSON_Parse(tables_json);
        cJSON *tables_node_copy = cJSON_Duplicate(tables_node, 1);
        cJSON_AddItemToObject(root, "table", tables_node_copy);
        cJSON_Delete(tables_node);
    }

    ret = get_config_from_json_by_type(chas->temporary_json, SINGLE_TABLES_TYPE, &single_tables_json);
    if(!ret) {
        return ret;
    }
    if(single_tables_json) {
        cJSON *single_tables_node = cJSON_Parse(single_tables_json);
        cJSON *single_tables_node_copy = cJSON_Duplicate(single_tables_node, 1);
        cJSON_AddItemToObject(root, "single_tables", single_tables_node_copy);
        cJSON_Delete(single_tables_node);
    }

    sharding_json = cJSON_Print(root);
    chassis_config_write_object(chas->priv->users->conf_manager, "sharding", sharding_json);
    cJSON_Delete(root);
    return TRUE;
}

gboolean config_set_local_option_by_key(chassis *chas, gchar *key) {
    if(!key) return ASSIGN_ERROR;
    GList *options = g_list_copy(chas->options->options);
    GList *l = NULL;
    for(l = options; l; l = l->next) {
        chassis_option_t *opt = l->data;
        if(strcasecmp(key, opt->long_name) == 0) {
            struct external_param param = {0};
            param.chas = chas;
            param.opt_type = SAVE_OPTS_PROPERTY;
            gchar *value = opt->show_hook != NULL? opt->show_hook(&param) : NULL;
            if(!value) {
                return TRUE;
            }
            gboolean ret = save_config_to_temporary_file(chas, key, value);
            g_free(value);
            return ret;
        }
    }
    return FALSE;
}
