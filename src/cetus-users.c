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

#include "cetus-users.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "cetus-util.h"
#include "chassis-config.h"

struct pwd_pair_t {
    char *client;
    char *server;
};

static struct pwd_pair_t *
pwd_pair_new(const char *c, const char *s)
{
    struct pwd_pair_t *pwd = g_new0(struct pwd_pair_t, 1);
    pwd->client = g_strdup(c);
    pwd->server = g_strdup(s);
    return pwd;
}

static void
pwd_pair_set_pwd(struct pwd_pair_t *pwd, const char *new_pass, enum cetus_pwd_type t)
{
    switch (t) {
    case CETUS_CLIENT_PWD:
        g_free(pwd->client);
        pwd->client = g_strdup(new_pass);
        break;
    case CETUS_SERVER_PWD:
        g_free(pwd->server);
        pwd->server = g_strdup(new_pass);
        break;
    default:
        g_assert(0);
    }
}

static gboolean
pwd_pair_same_pwd(struct pwd_pair_t *pwd, const char *new_pass, enum cetus_pwd_type t)
{
    switch (t) {
    case CETUS_CLIENT_PWD:
        return strcmp(pwd->client, new_pass) == 0;
    case CETUS_SERVER_PWD:
        return strcmp(pwd->server, new_pass) == 0;
    default:
        return FALSE;
    }
}

static void
pwd_pair_free(struct pwd_pair_t *pwd)
{
    if (pwd) {
        g_free(pwd->client);
        g_free(pwd->server);
        g_free(pwd);
    }
}

cetus_users_t *
cetus_users_new()
{
    cetus_users_t *users = g_new0(cetus_users_t, 1);
    users->records = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify) pwd_pair_free);
    return users;
}

void
cetus_users_free(cetus_users_t *users)
{
    if (users) {
        if (users->records)
            g_hash_table_destroy(users->records);
        g_free(users);
    }
}

static void
cetus_users_set_records(cetus_users_t *users, GHashTable *new_records)
{
    if (users->records) {
        g_hash_table_destroy(users->records);
    }
    users->records = new_records;
}

gboolean
cetus_users_parse_json(cetus_users_t *users, char *buffer)
{
    cJSON *root = cJSON_Parse(buffer);
    if (!root) {
        g_critical(G_STRLOC ":json syntax error");
        return FALSE;
    }

    gboolean success = FALSE;

    GHashTable *user_records = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                     g_free, (GDestroyNotify) pwd_pair_free);

    cJSON *users_node = cJSON_GetObjectItem(root, "users");
    cJSON *user_node = users_node ? users_node->child : NULL;
    for (; user_node; user_node = user_node->next) {
        cJSON *username = cJSON_GetObjectItem(user_node, "user");
        if (!username) {
            g_critical(G_STRLOC ":user conf error, no username");
            goto out;
        }
        cJSON *client_pwd = cJSON_GetObjectItem(user_node, "client_pwd");
        cJSON *server_pwd = cJSON_GetObjectItem(user_node, "server_pwd");
        if (!client_pwd && !server_pwd) {
            g_critical(G_STRLOC ":user conf error, at least one of client/server is needed");
            goto out;
        }
        if (client_pwd && !server_pwd)
            server_pwd = client_pwd;
        if (!client_pwd && server_pwd)
            client_pwd = server_pwd;
        g_hash_table_insert(user_records, g_strdup(username->valuestring),
                            pwd_pair_new(client_pwd->valuestring, server_pwd->valuestring));
    }

    success = TRUE;
  out:
    cJSON_Delete(root);
    if (success) {
        cetus_users_set_records(users, user_records);
    } else {
        g_hash_table_destroy(user_records);
    }
    return success;
}

gboolean
cetus_users_read_json(cetus_users_t *users, chassis_config_t *conf, int refresh)
{
    users->conf_manager = conf;
    char *buffer = NULL;
    if (!chassis_config_query_object(conf, "users", &buffer, refresh)) {
        return FALSE;
    }

    if (!buffer)
        return FALSE;

    gboolean success = cetus_users_parse_json(users, buffer);
    if (success) {
        g_message("read %d users", g_hash_table_size(users->records));
    }
    g_free(buffer);

    return success;
}

gboolean
cetus_users_update_record(cetus_users_t *users, const char *user, const char *pass, enum cetus_pwd_type type)
{
    struct pwd_pair_t *pwd = g_hash_table_lookup(users->records, user);
    if (pwd) {
        if (pwd_pair_same_pwd(pwd, pass, type)) {
            return FALSE;
        }
        pwd_pair_set_pwd(pwd, pass, type);
    } else {
        g_hash_table_insert(users->records, g_strdup(user), pwd_pair_new(pass, pass));
    }
    g_message("update user: %s", user);
    return TRUE;
}

gboolean
cetus_users_delete_record(cetus_users_t *users, const char *user)
{
    gboolean found = g_hash_table_remove(users->records, user);
    if (found)
        g_message("delete user: %s", user);
    return found;
}

gboolean
cetus_users_write_json(cetus_users_t *users)
{
    cJSON *users_node = cJSON_CreateArray();
    if (users_node == NULL) {
        g_warning(G_STRLOC ":users_node is nil");
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

    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "users", users_node);
    char *json_str = cJSON_Print(root);

    if (chassis_config_write_object(users->conf_manager, "users", json_str)) {
        cJSON_Delete(root);
        g_free(json_str);
        return TRUE;
    } else {
        cJSON_Delete(root);
        g_free(json_str);
        return FALSE;
    }
}

gboolean
cetus_users_authenticate_client(cetus_users_t *users,
                                network_mysqld_auth_challenge *challenge, network_mysqld_auth_response *response)
{
    char *user_name = response->username->str;

    struct pwd_pair_t *pwd = g_hash_table_lookup(users->records, user_name);
    if (pwd == NULL) {
        g_debug("pwd is null for user:%s", user_name);
        return FALSE;
    }

    /* term 2: user_name and password must in frontend users/passwords */
    if (pwd->client) {
        GString *sha1_pwd = g_string_new(NULL);
        network_mysqld_proto_password_hash(sha1_pwd, pwd->client, strlen(pwd->client));
        GString *expected_response = g_string_new(NULL);
        network_mysqld_proto_password_scramble(expected_response, S(challenge->auth_plugin_data), S(sha1_pwd));

        if (g_string_equal(response->auth_plugin_data, expected_response)) {
            g_string_free(expected_response, TRUE);
            g_string_free(sha1_pwd, TRUE);
            return TRUE;
        }
        g_string_free(expected_response, TRUE);
        g_string_free(sha1_pwd, TRUE);
    }
    return FALSE;
}

void
cetus_users_get_hashed_pwd(cetus_users_t *users, const char *user_name, enum cetus_pwd_type type, GString *sha1pwd)
{
    if (type == CETUS_CLIENT_PWD) {
        cetus_users_get_hashed_client_pwd(users, user_name, sha1pwd);
    } else if (type == CETUS_SERVER_PWD) {
        cetus_users_get_hashed_server_pwd(users, user_name, sha1pwd);
    }
}

void
cetus_users_get_hashed_client_pwd(cetus_users_t *users, const char *user_name, GString *sha1_pwd)
{
    struct pwd_pair_t *pwd = g_hash_table_lookup(users->records, user_name);
    if (pwd == NULL) {
        return;
    }
    if (pwd->client) {
        network_mysqld_proto_password_hash(sha1_pwd, pwd->client, strlen(pwd->client));
    }
}

void
cetus_users_get_hashed_server_pwd(cetus_users_t *users, const char *user_name, GString *sha1_pwd)
{
    struct pwd_pair_t *pwd = g_hash_table_lookup(users->records, user_name);
    if (pwd == NULL) {
        return;
    }
    if (pwd->server) {
        network_mysqld_proto_password_hash(sha1_pwd, pwd->server, strlen(pwd->server));
    }
}

void
cetus_users_get_server_pwd(cetus_users_t *users, const char *user_name, GString *res_pwd)
{
    struct pwd_pair_t *pwd = g_hash_table_lookup(users->records, user_name);
    if (pwd == NULL) {
        return;
    }
    if (pwd->server) {
        g_string_assign(res_pwd, pwd->server);
    }
}

gboolean
cetus_users_contains(cetus_users_t *users, const char *user_name)
{
    return g_hash_table_lookup(users->records, user_name) ? TRUE : FALSE;
}

