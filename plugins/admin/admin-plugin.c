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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <sys/stat.h>

#include "cetus-users.h"
#include "cetus-util.h"
#include "cetus-variable.h"
#include "character-set.h"
#include "chassis-event.h"
#include "chassis-options.h"
#include "cetus-monitor.h"
#include "glib-ext.h"
#include "network-mysqld-packet.h"
#include "network-mysqld-proto.h"
#include "network-mysqld.h"
#include "server-session.h"
#include "sys-pedantic.h"
#include "network-ssl.h"
#include "chassis-options-utils.h"

#ifndef PLUGIN_VERSION
#ifdef CHASSIS_BUILD_TAG
#define PLUGIN_VERSION CHASSIS_BUILD_TAG
#else
#define PLUGIN_VERSION PACKAGE_VERSION
#endif
#endif

struct chassis_plugin_config {
    gchar *address;               /**< listening address of the admin interface */

    gchar *admin_username;        /**< login username */
    gchar *admin_password;        /**< login password */

    gchar *allow_ip;                  /**< allow ip addr list */
    GHashTable *allow_ip_table;

    gchar *deny_ip;                  /**< deny ip addr list */
    GHashTable *deny_ip_table;

    gboolean has_shard_plugin;       /**< another plugin name is shard or proxy, TRUE is shard, FALSE is proxy */

    network_mysqld_con *listen_con;
};

static struct event *g_sampling_timer = NULL;

/*
 * tokenize input, alloc and return nth token
 * n -> [0,..)
 */
static char *
str_nth_token(const char *input, int n)
{
    char *t = NULL;
    char **tokens = g_strsplit(input, " ", -1);
    if (g_strv_length(tokens) > n) {
        t = g_strdup(tokens[n]);
    }
    g_strfreev(tokens);
    return t;
}

/* get config->has_shard_plugin */
static gboolean
has_shard_plugin(GPtrArray *modules)
{
    int i;
    for (i = 0; i < modules->len; i++) {
        chassis_plugin *plugin = modules->pdata[i];
        if (strcmp(plugin->name, "shard") == 0) {
            return TRUE;
        }
    }
    return FALSE;
}

/* judge if client_ip is in allowed or denied ip range*/
static gboolean
ip_range_lookup(GHashTable *ip_table, char *client_ip)
{
    char ip_range[128] = { 0 };
    char wildcard[128] = { 0 };
    GList *ip_range_table = g_hash_table_get_keys(ip_table);
    GList *l;
    for (l = ip_range_table; l; l = l->next) {
        sscanf(l->data, "%64[0-9.].%s", ip_range, wildcard);
        gchar *pos = NULL;
        if ((pos = strcasestr(client_ip, ip_range))) {
            if(pos == client_ip) {
                return TRUE;
            }
        }
    }
    return FALSE;
}

NETWORK_MYSQLD_PLUGIN_PROTO(server_con_init)
{
    network_mysqld_auth_challenge *challenge;
    GString *packet;

    challenge = network_mysqld_auth_challenge_new();
    challenge->server_version_str = g_strdup_printf("5.7 admin");
    challenge->server_version = 50700;
    challenge->charset = charset_get_number("utf8");
    challenge->capabilities = CETUS_DEFAULT_FLAGS & (~CLIENT_TRANSACTIONS);
#ifdef HAVE_OPENSSL
    if (chas->ssl) {
        challenge->capabilities |= CLIENT_SSL;
    }
#endif
    challenge->server_status = SERVER_STATUS_AUTOCOMMIT;
    challenge->thread_id = 1;

    /* generate a random challenge */
    network_mysqld_auth_challenge_set_challenge(challenge);

    packet = g_string_new(NULL);
    network_mysqld_proto_append_auth_challenge(packet, challenge);
    con->client->challenge = challenge;

    network_mysqld_queue_append(con->client, con->client->send_queue, S(packet));

    g_string_free(packet, TRUE);

    con->state = ST_SEND_HANDSHAKE;

    g_assert(con->plugin_con_state == NULL);

    return NETWORK_SOCKET_SUCCESS;
}

NETWORK_MYSQLD_PLUGIN_PROTO(server_read_auth)
{
    network_packet packet;
    network_socket *recv_sock, *send_sock;
    network_mysqld_auth_response *auth;
    GString *excepted_response;
    GString *hashed_pwd;

    recv_sock = con->client;
    send_sock = con->client;

    packet.data = g_queue_peek_head(recv_sock->recv_queue->chunks);
    packet.offset = 0;

    /* decode the packet */
    network_mysqld_proto_skip_network_header(&packet);

    if (con->client->response == NULL) {
        auth = network_mysqld_auth_response_new(con->client->challenge->capabilities);
        if (network_mysqld_proto_get_auth_response(&packet, auth)) {
            network_mysqld_auth_response_free(auth);
            return NETWORK_SOCKET_ERROR;
        }
        if (!(auth->client_capabilities & CLIENT_PROTOCOL_41)) {
            /* should use packet-id 0 */
            network_mysqld_queue_append(con->client, con->client->send_queue,
                                        C("\xff\xd7\x07" "4.0 protocol is not supported"));
            network_mysqld_auth_response_free(auth);
            return NETWORK_SOCKET_ERROR;
        }

#ifdef HAVE_OPENSSL
        if (auth->ssl_request) {
            network_ssl_create_connection(con->client, NETWORK_SSL_SERVER);
            g_string_free(g_queue_pop_tail(con->client->recv_queue->chunks), TRUE);
            con->state = ST_FRONT_SSL_HANDSHAKE;
            return NETWORK_SOCKET_SUCCESS;
        }
#endif
        con->client->response = auth;

        if ((auth->client_capabilities & CLIENT_PLUGIN_AUTH)
            && (g_strcmp0(auth->auth_plugin_name->str, "mysql_native_password") != 0))
        {
            GString *packet = g_string_new(0);
            network_mysqld_proto_append_auth_switch(packet, "mysql_native_password",
                con->client->challenge->auth_plugin_data);
            network_mysqld_queue_append(con->client, con->client->send_queue, S(packet));
            con->state = ST_SEND_AUTH_RESULT;
            con->auth_result_state = AUTH_SWITCH;
            g_string_free(packet, TRUE);
            g_string_free(g_queue_pop_tail(recv_sock->recv_queue->chunks), TRUE);
            return NETWORK_SOCKET_SUCCESS;
        }
    } else {
        /* auth switch response */
        gsize auth_data_len = packet.data->len - 4;
        GString *auth_data = g_string_sized_new(auth_data_len);
        network_mysqld_proto_get_gstr_len(&packet, auth_data_len, auth_data);
        g_string_append_len(con->client->response->auth_plugin_data, S(auth_data));
        g_string_free(auth_data, TRUE);
        auth = con->client->response;
    }
    /* Check client addr in admin-allow-ip and admin-deny-ip */
    gboolean check_ip;
    char *ip_err_msg = NULL;
    if (con->config->allow_ip_table || con->config->deny_ip_table) {
        char *client_addr = con->client->src->name->str;
        char **client_addr_arr = g_strsplit(client_addr, ":", -1);
        char *client_ip = client_addr_arr[0];
        if (g_hash_table_size(con->config->allow_ip_table) != 0 &&
            (g_hash_table_lookup(con->config->allow_ip_table, "*")
             || ip_range_lookup(con->config->allow_ip_table, client_ip)
             || g_hash_table_lookup(con->config->allow_ip_table, client_ip))) {
            check_ip = FALSE;
        } else if (g_hash_table_size(con->config->deny_ip_table) != 0 &&
                   (g_hash_table_lookup(con->config->deny_ip_table, "*")
                    || ip_range_lookup(con->config->deny_ip_table, client_ip)
                    || g_hash_table_lookup(con->config->deny_ip_table, client_ip))) {
            check_ip = TRUE;
            ip_err_msg = g_strdup_printf("Access denied for user '%s'@'%s'", con->config->admin_username, client_ip);
        } else {
            check_ip = FALSE;
        }
        g_strfreev(client_addr_arr);
    } else {
        check_ip = FALSE;
    }

    if (check_ip) {
        network_mysqld_con_send_error_full(send_sock, L(ip_err_msg), 1045, "28000");
        g_free(ip_err_msg);
        con->state = ST_SEND_ERROR;
    } else {
        /* check if the password matches */
        excepted_response = g_string_new(NULL);
        hashed_pwd = g_string_new(NULL);

        if (!strleq(S(con->client->response->username),
                    con->config->admin_username, strlen(con->config->admin_username))) {
            network_mysqld_con_send_error_full(send_sock, C("unknown user"), 1045, "28000");

            /* close the connection after we have sent this packet */
            con->state = ST_SEND_ERROR;
        } else if (network_mysqld_proto_password_hash(hashed_pwd,
                                                      con->config->admin_password,
                                                      strlen(con->config->admin_password))) {
        } else if (network_mysqld_proto_password_scramble(excepted_response,
                                                          S(recv_sock->challenge->auth_plugin_data), S(hashed_pwd))) {
            network_mysqld_con_send_error_full(send_sock, C("scrambling failed"), 1045, "28000");

            /* close the connection after we have sent this packet */
            con->state = ST_SEND_ERROR;
        } else if (!g_string_equal(excepted_response, auth->auth_plugin_data)) {
            network_mysqld_con_send_error_full(send_sock, C("password doesn't match"), 1045, "28000");

            /* close the connection after we have sent this packet */
            con->state = ST_SEND_ERROR;
        } else {
            network_mysqld_con_send_ok(send_sock);

            con->state = ST_SEND_AUTH_RESULT;
        }

        g_string_free(hashed_pwd, TRUE);
        g_string_free(excepted_response, TRUE);
    }

    g_string_free(g_queue_pop_tail(recv_sock->recv_queue->chunks), TRUE);

    if (recv_sock->recv_queue->chunks->length > 0) {
        g_warning("%s: client-recv-queue-len = %d", G_STRLOC, recv_sock->recv_queue->chunks->length);
    }

    return NETWORK_SOCKET_SUCCESS;
}

static const char *
get_conn_xa_state_name(network_mysqld_con_dist_tran_state_t state)
{
    switch (state) {
    case NEXT_ST_XA_START:
        return "XS";
    case NEXT_ST_XA_QUERY:
        return "XQ";
    case NEXT_ST_XA_END:
        return "XE";
    case NEXT_ST_XA_PREPARE:
        return "XP";
    case NEXT_ST_XA_COMMIT:
        return "XC";
    case NEXT_ST_XA_ROLLBACK:
        return "XR";
    case NEXT_ST_XA_CANDIDATE_OVER:
        return "XCO";
    case NEXT_ST_XA_OVER:
        return "XO";
    default:
        break;
    }

    return "NX";
}

static char *states[] = {
    "unknown",
    "up",
    "down",
    "maintaining",
    "deleted",
};

static char *types[] = {
    "unknown",
    "rw",
    "ro",
};

static int
admin_send_backends_info(network_mysqld_con *admin_con, const char *sql)
{
    chassis *chas = admin_con->srv;
    chassis_private *priv = chas->priv;

    chassis_plugin_config *config = admin_con->config;

    GPtrArray *fields = g_ptr_array_new_with_free_func((GDestroyNotify) network_mysqld_proto_fielddef_free);

    MYSQL_FIELD *field;
    field = network_mysqld_proto_fielddef_new();
    field->name = g_strdup("backend_ndx");
    field->type = MYSQL_TYPE_STRING;
    g_ptr_array_add(fields, field);

    field = network_mysqld_proto_fielddef_new();
    field->name = g_strdup("address");
    field->type = MYSQL_TYPE_STRING;
    g_ptr_array_add(fields, field);

    field = network_mysqld_proto_fielddef_new();
    field->name = g_strdup("state");
    field->type = MYSQL_TYPE_STRING;
    g_ptr_array_add(fields, field);

    field = network_mysqld_proto_fielddef_new();
    field->name = g_strdup("type");
    field->type = MYSQL_TYPE_STRING;
    g_ptr_array_add(fields, field);

    field = network_mysqld_proto_fielddef_new();
    field->name = g_strdup("slave delay");
    field->type = MYSQL_TYPE_STRING;
    g_ptr_array_add(fields, field);

    field = network_mysqld_proto_fielddef_new();
    field->name = g_strdup("uuid");
    field->type = MYSQL_TYPE_STRING;
    g_ptr_array_add(fields, field);

    field = network_mysqld_proto_fielddef_new();
    field->name = g_strdup("idle_conns");
    field->type = MYSQL_TYPE_STRING;
    g_ptr_array_add(fields, field);

    field = network_mysqld_proto_fielddef_new();
    field->name = g_strdup("used_conns");
    field->type = MYSQL_TYPE_STRING;
    g_ptr_array_add(fields, field);

    field = network_mysqld_proto_fielddef_new();
    field->name = g_strdup("total_conns");
    field->type = MYSQL_TYPE_STRING;
    g_ptr_array_add(fields, field);

    if (config->has_shard_plugin) {
        field = network_mysqld_proto_fielddef_new();
        field->name = g_strdup("group");
        field->type = MYSQL_TYPE_STRING;
        g_ptr_array_add(fields, field);
    }

    GPtrArray *rows = g_ptr_array_new_with_free_func((GDestroyNotify) network_mysqld_mysql_field_row_free);

    network_backends_t *bs = priv->backends;
    int len = bs->backends->len;
    int i;
    char buffer[32];
    for (i = 0; i < len; i++) {
        network_backend_t *backend = bs->backends->pdata[i];
        GPtrArray *row = g_ptr_array_new_with_free_func(g_free);

        snprintf(buffer, sizeof(buffer), "%d", i + 1);

        g_ptr_array_add(row, g_strdup(buffer));

        g_ptr_array_add(row, g_strdup(backend->addr->name->str));
        g_ptr_array_add(row, g_strdup(states[(int)(backend->state)]));
        g_ptr_array_add(row, g_strdup(types[(int)(backend->type)]));

        snprintf(buffer, sizeof(buffer), "%d", backend->slave_delay_msec);
        g_ptr_array_add(row, backend->type == BACKEND_TYPE_RO ? g_strdup(buffer) : NULL);

        g_ptr_array_add(row, backend->uuid->len ? g_strdup(backend->uuid->str) : NULL);

        snprintf(buffer, sizeof(buffer), "%d", backend->pool->cur_idle_connections);
        g_ptr_array_add(row, g_strdup(buffer));

        snprintf(buffer, sizeof(buffer), "%d", backend->connected_clients);
        g_ptr_array_add(row, g_strdup(buffer));

        snprintf(buffer, sizeof(buffer), "%d", backend->pool->cur_idle_connections + backend->connected_clients);
        g_ptr_array_add(row, g_strdup(buffer));

        g_ptr_array_add(row, backend->server_group->len ? g_strdup(backend->server_group->str) : NULL);

        g_ptr_array_add(rows, row);

    }

    network_mysqld_con_send_resultset(admin_con->client, fields, rows);

    /* Free data */
    g_ptr_array_free(rows, TRUE);
    g_ptr_array_free(fields, TRUE);
    return PROXY_SEND_RESULT;
}

static void
g_table_free_all(gpointer q)
{
    GHashTable *table = q;
    g_hash_table_destroy(table);
}

struct used_conns_t {
    int num;
};

static int
admin_send_backend_detail_info(network_mysqld_con *admin_con, const char *sql)
{
    chassis *chas = admin_con->srv;
    chassis_private *priv = chas->priv;

    int i, j, len;

    char buffer[32];
    GPtrArray *fields;
    GPtrArray *rows;
    GPtrArray *row;
    MYSQL_FIELD *field;

    GHashTable *back_user_conn_hash_table = g_hash_table_new_full(g_str_hash,
                                                                  g_str_equal, g_free, g_table_free_all);

    network_backends_t *bs = priv->backends;
    len = bs->backends->len;

    for (i = 0; i < len; i++) {
        network_backend_t *backend = bs->backends->pdata[i];
        GHashTable *table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
        g_hash_table_insert(back_user_conn_hash_table, g_strdup(backend->addr->name->str), table);
    }

    len = priv->cons->len;

    for (i = 0; i < len; i++) {
        network_mysqld_con *con = priv->cons->pdata[i];

        if (!con->client || !con->client->response) {
            continue;
        }
#ifndef SIMPLE_PARSER
        if (con->servers == NULL) {
            continue;
        }

        for (j = 0; j < con->servers->len; j++) {
            server_session_t *ss = g_ptr_array_index(con->servers, j);

            GHashTable *table = g_hash_table_lookup(back_user_conn_hash_table,
                                                    ss->backend->addr->name->str);
            if (table == NULL) {
                g_warning("%s: table is null for backend:%s", G_STRLOC, ss->backend->addr->name->str);
                continue;
            }

            struct used_conns_t *total_used = g_hash_table_lookup(table,
                                                                  con->client->response->username->str);
            if (total_used == NULL) {
                total_used = g_new0(struct used_conns_t, 1);
                g_hash_table_insert(table, g_strdup(con->client->response->username->str), total_used);
            }
            total_used->num++;
        }

#else
        if (con->servers != NULL) {
            for (j = 0; j < con->servers->len; j++) {
                network_socket *sock = g_ptr_array_index(con->servers, j);
                GHashTable *table = g_hash_table_lookup(back_user_conn_hash_table,
                                                        sock->dst->name->str);
                if (table == NULL) {
                    g_warning("%s: table is null for backend:%s", G_STRLOC, sock->dst->name->str);
                    continue;
                }

                struct used_conns_t *total_used = g_hash_table_lookup(table,
                                                                      con->client->response->username->str);
                if (total_used == NULL) {
                    total_used = g_new0(struct used_conns_t, 1);
                    g_hash_table_insert(table, g_strdup(con->client->response->username->str), total_used);
                }
                total_used->num++;
            }
        } else {
            if (con->server == NULL) {
                continue;
            }

            GHashTable *table = g_hash_table_lookup(back_user_conn_hash_table,
                                                    con->server->dst->name->str);
            if (table == NULL) {
                g_warning("%s: table is null for backend:%s", G_STRLOC, con->server->dst->name->str);
                continue;
            }
            struct used_conns_t *total_used = g_hash_table_lookup(table,
                                                                  con->client->response->username->str);
            if (total_used == NULL) {
                total_used = g_new0(struct used_conns_t, 1);
                g_hash_table_insert(table, g_strdup(con->client->response->username->str), total_used);
            }

            total_used->num++;
        }
#endif
    }

    fields = g_ptr_array_new_with_free_func((void *)network_mysqld_proto_fielddef_free);

    field = network_mysqld_proto_fielddef_new();
    field->name = g_strdup("backend_ndx");
    field->type = MYSQL_TYPE_STRING;
    g_ptr_array_add(fields, field);

    field = network_mysqld_proto_fielddef_new();
    field->name = g_strdup("username");
    field->type = MYSQL_TYPE_STRING;
    g_ptr_array_add(fields, field);

    field = network_mysqld_proto_fielddef_new();
    field->name = g_strdup("idle_conns");
    field->type = MYSQL_TYPE_STRING;
    g_ptr_array_add(fields, field);

    field = network_mysqld_proto_fielddef_new();
    field->name = g_strdup("used_conns");
    field->type = MYSQL_TYPE_STRING;
    g_ptr_array_add(fields, field);

    field = network_mysqld_proto_fielddef_new();
    field->name = g_strdup("total_conns");
    field->type = MYSQL_TYPE_STRING;
    g_ptr_array_add(fields, field);

    rows = g_ptr_array_new_with_free_func((void *)network_mysqld_mysql_field_row_free);

    len = bs->backends->len;
    for (i = 0; i < len; i++) {
        network_backend_t *backend = bs->backends->pdata[i];

        GHashTable *table = g_hash_table_lookup(back_user_conn_hash_table,
                                                backend->addr->name->str);
        if (table == NULL) {
            g_warning("%s: table is null for backend:%s", G_STRLOC, backend->addr->name->str);
            continue;
        }

        GHashTable *users = backend->pool->users;
        if (users != NULL) {
            GHashTableIter iter;
            GString *key;
            GQueue *queue;
            g_hash_table_iter_init(&iter, users);
            /* count all users' pooled connections */
            while (g_hash_table_iter_next(&iter, (void **)&key, (void **)&queue)) {
                row = g_ptr_array_new_with_free_func(g_free);

                snprintf(buffer, sizeof(buffer), "%d", i + 1);
                g_ptr_array_add(row, g_strdup(buffer));
                g_ptr_array_add(row, g_strdup(key->str));
                snprintf(buffer, sizeof(buffer), "%d", queue->length);
                g_ptr_array_add(row, g_strdup(buffer));

                struct used_conns_t *total_used = g_hash_table_lookup(table, key->str);
                if (total_used) {
                    snprintf(buffer, sizeof(buffer), "%d", total_used->num);
                } else {
                    snprintf(buffer, sizeof(buffer), "%d", 0);
                }
                g_ptr_array_add(row, g_strdup(buffer));

                if (total_used) {
                    snprintf(buffer, sizeof(buffer), "%d", queue->length + total_used->num);
                } else {
                    snprintf(buffer, sizeof(buffer), "%d", queue->length);
                }
                g_ptr_array_add(row, g_strdup(buffer));

                g_ptr_array_add(rows, row);
            }
        }
    }

    network_mysqld_con_send_resultset(admin_con->client, fields, rows);

    /* Free data */
    g_ptr_array_free(rows, TRUE);
    g_ptr_array_free(fields, TRUE);

    g_hash_table_destroy(back_user_conn_hash_table);

    return PROXY_SEND_RESULT;
}

static int
admin_show_connectionlist(network_mysqld_con *admin_con, const char *sql)
{
    char *arg = str_nth_token(sql, 2);
    int number = 65536;
    if (arg) {
        number = atoi(arg);
        g_free(arg);
    }

    chassis *chas = admin_con->srv;
    chassis_private *priv = chas->priv;

    chassis_plugin_config *config = admin_con->config;

    int i, len;
    char buffer[32];
    GPtrArray *fields;
    GPtrArray *rows;
    GPtrArray *row;
    MYSQL_FIELD *field;

    fields = g_ptr_array_new_with_free_func((void *)network_mysqld_proto_fielddef_free);

    field = network_mysqld_proto_fielddef_new();
    field->name = g_strdup("User");
    field->type = MYSQL_TYPE_STRING;
    g_ptr_array_add(fields, field);

    field = network_mysqld_proto_fielddef_new();
    field->name = g_strdup("Host");
    field->type = MYSQL_TYPE_STRING;
    g_ptr_array_add(fields, field);

    field = network_mysqld_proto_fielddef_new();
    field->name = g_strdup("db");
    field->type = MYSQL_TYPE_STRING;
    g_ptr_array_add(fields, field);

    field = network_mysqld_proto_fielddef_new();
    field->name = g_strdup("Command");
    field->type = MYSQL_TYPE_STRING;
    g_ptr_array_add(fields, field);

    field = network_mysqld_proto_fielddef_new();
    field->name = g_strdup("ProcessTime");
    field->type = MYSQL_TYPE_STRING;
    g_ptr_array_add(fields, field);

    field = network_mysqld_proto_fielddef_new();
    field->name = g_strdup("Trans");
    field->type = MYSQL_TYPE_STRING;
    g_ptr_array_add(fields, field);

    field = network_mysqld_proto_fielddef_new();
    field->name = g_strdup("PS");
    field->type = MYSQL_TYPE_STRING;
    g_ptr_array_add(fields, field);

    field = network_mysqld_proto_fielddef_new();
    field->name = g_strdup("State");
    field->type = MYSQL_TYPE_STRING;
    g_ptr_array_add(fields, field);

    if (config->has_shard_plugin) {
        field = network_mysqld_proto_fielddef_new();
        field->name = g_strdup("Xa");
        field->type = MYSQL_TYPE_STRING;
        g_ptr_array_add(fields, field);

        field = network_mysqld_proto_fielddef_new();
        field->name = g_strdup("Xid");
        field->type = MYSQL_TYPE_STRING;
        g_ptr_array_add(fields, field);
    }

    field = network_mysqld_proto_fielddef_new();
    field->name = g_strdup("Server");
    field->type = MYSQL_TYPE_STRING;
    g_ptr_array_add(fields, field);

    field = network_mysqld_proto_fielddef_new();
    field->name = g_strdup("Info");
    field->type = MYSQL_TYPE_STRING;
    g_ptr_array_add(fields, field);

    rows = g_ptr_array_new_with_free_func((void *)network_mysqld_mysql_field_row_free);

    struct timeval now;
    gettimeofday(&(now), NULL);

    len = priv->cons->len;
    int count = 0;

    for (i = 0; i < len; i++) {
        network_mysqld_con *con = priv->cons->pdata[i];

        if (!con->client) {
            continue;
        }

        if (count >= number) {
            break;
        }

        count++;

        row = g_ptr_array_new_with_free_func(g_free);
        if (con->client->response != NULL) {
            g_ptr_array_add(row, g_strdup(con->client->response->username->str));
        } else {
            g_ptr_array_add(row, NULL);
        }
        g_ptr_array_add(row, g_strdup(con->client->src->name->str));

        g_ptr_array_add(row, g_strdup(con->client->default_db->str));

        if (con->state <= ST_READ_QUERY) {
            g_ptr_array_add(row, g_strdup("Sleep"));
            g_ptr_array_add(row, g_strdup("0"));
        } else {
            g_ptr_array_add(row, g_strdup("Query"));
            int diff = now.tv_sec - con->req_recv_time.tv_sec;
            if (diff > 7200) {
                g_critical("%s:too slow connection(%s) processing for con:%p",
                        G_STRLOC, con->client->src->name->str, con);
            }
            diff = diff * 1000;
            diff += (now.tv_usec - con->req_recv_time.tv_usec) / 1000;
            snprintf(buffer, sizeof(buffer), "%d", diff);
            g_ptr_array_add(row, g_strdup(buffer));
        }
        g_ptr_array_add(row, g_strdup(con->is_in_transaction ? "Y" : "N"));
        g_ptr_array_add(row, g_strdup(con->is_prepared ? "Y" : "N"));

        g_ptr_array_add(row, g_strdup(network_mysqld_con_st_name(con->state)));

        if (config->has_shard_plugin) {
            g_ptr_array_add(row, g_strdup(get_conn_xa_state_name(con->dist_tran_state)));
            if (con->dist_tran) {
                snprintf(buffer, sizeof(buffer), "%lld", con->xa_id);
                g_ptr_array_add(row, g_strdup(buffer));
            } else {
                g_ptr_array_add(row, NULL);
            }
        }

        if (con->servers != NULL) {
            int j;
            GString *servers = g_string_new(NULL);
            for (j = 0; j < con->servers->len; j++) {
                server_session_t *ss = g_ptr_array_index(con->servers, j);
                if (ss && ss->server) {
                    if (ss->server->src) {
                        g_string_append_len(servers, S(ss->server->src->name));
                        char *delim = "->";
                        g_string_append_len(servers, delim, strlen(delim));
                    }
                    g_string_append_len(servers, S(ss->server->dst->name));
                    g_string_append_c(servers, ' ');
                }
            }
            g_ptr_array_add(row, g_strdup(servers->str));
            g_string_free(servers, TRUE);

        } else {
            if (con->server != NULL) {
                GString *server = g_string_new(NULL);
                if (con->server->src) {
                    g_string_append_len(server, S(con->server->src->name));
                    char *delim = "->";
                    g_string_append_len(server, delim, strlen(delim));
                }
                g_string_append_len(server, S(con->server->dst->name));
                g_ptr_array_add(row, g_strdup(server->str));
                g_string_free(server, TRUE);
            } else {
                g_ptr_array_add(row, NULL);
            }
        }

        if (con->orig_sql->len) {
            if (con->state == ST_READ_QUERY) {
                g_ptr_array_add(row, NULL);
            } else {
                g_ptr_array_add(row, g_strdup(con->orig_sql->str));
            }
        } else {
            g_ptr_array_add(row, NULL);
        }

        g_ptr_array_add(rows, row);
    }

    network_mysqld_con_send_resultset(admin_con->client, fields, rows);

    g_ptr_array_free(rows, TRUE);
    g_ptr_array_free(fields, TRUE);
    return PROXY_SEND_RESULT;
}

static GList *
network_mysqld_admin_plugin_allow_ip_get(chassis_plugin_config *config)
{
    if (config && config->allow_ip_table) {
        return g_hash_table_get_keys(config->allow_ip_table);
    }
    return NULL;
}

static GList *
network_mysqld_admin_plugin_deny_ip_get(chassis_plugin_config *config)
{
    if (config && config->deny_ip_table) {
        return g_hash_table_get_keys(config->deny_ip_table);
    }
    return NULL;
}

static gboolean
network_mysqld_admin_plugin_allow_ip_add(chassis_plugin_config *config, char *addr)
{
    if (!config || !addr)
        return FALSE;
    if (!config->allow_ip_table) {
        config->allow_ip_table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    }
    gboolean success = FALSE;
    if (!g_hash_table_lookup(config->allow_ip_table, addr)) {
        g_hash_table_insert(config->allow_ip_table, g_strdup(addr), (void *)TRUE);
        success = TRUE;
    }
    return success;
}

static gboolean
network_mysqld_admin_plugin_deny_ip_add(chassis_plugin_config *config, char *addr)
{
    if (!config || !addr)
        return FALSE;
    if (!config->deny_ip_table) {
        config->deny_ip_table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    }
    gboolean success = FALSE;
    if (!g_hash_table_lookup(config->deny_ip_table, addr)) {
        g_hash_table_insert(config->deny_ip_table, g_strdup(addr), (void *)TRUE);
        success = TRUE;
    }
    return success;
}

static gboolean
network_mysqld_admin_plugin_allow_ip_del(chassis_plugin_config *config, char *addr)
{
    if (!config || !addr || !config->allow_ip_table)
        return FALSE;
    return g_hash_table_remove(config->allow_ip_table, addr);
}

static gboolean
network_mysqld_admin_plugin_deny_ip_del(chassis_plugin_config *config, char *addr)
{
    if (!config || !addr || !config->deny_ip_table)
        return FALSE;
    return g_hash_table_remove(config->deny_ip_table, addr);
}

static int
admin_show_allow_ip(network_mysqld_con *con, const char *sql)
{
    char *module_name = str_nth_token(sql, 2);
    if (!module_name) {
        return PROXY_NO_DECISION;
    }
    if (strcmp(module_name, "admin") != 0 && strcmp(module_name, "proxy") != 0 && strcmp(module_name, "shard") != 0) {
        g_free(module_name);
        return PROXY_NO_DECISION;
    }
    chassis *chas = con->srv;
    GPtrArray *fields = g_ptr_array_new_with_free_func((void *)network_mysqld_proto_fielddef_free);
    MYSQL_FIELD *field;
    GPtrArray *rows = g_ptr_array_new_with_free_func((void *)network_mysqld_mysql_field_row_free);
    GPtrArray *row;

    field = network_mysqld_proto_fielddef_new();
    field->name = g_strdup("Plugin");
    field->type = MYSQL_TYPE_STRING;
    g_ptr_array_add(fields, field);
    field = network_mysqld_proto_fielddef_new();
    field->name = g_strdup("Address");
    field->type = MYSQL_TYPE_STRING;
    g_ptr_array_add(fields, field);

    int i;
    for (i = 0; i < chas->modules->len; i++) {
        chassis_plugin *plugin = chas->modules->pdata[i];
        if (strcmp(plugin->name, module_name) == 0) {
            GList *allow_ip_list = plugin->allow_ip_get(plugin->config);
            if (allow_ip_list) {
                GList *cur_p = allow_ip_list;
                while (cur_p) {
                    row = g_ptr_array_new_with_free_func(g_free);
                    g_ptr_array_add(row, g_strdup(module_name));
                    g_ptr_array_add(row, g_strdup((char *)cur_p->data));
                    g_ptr_array_add(rows, row);
                    cur_p = cur_p->next;
                }
            }
            break;
        }
    }

    network_mysqld_con_send_resultset(con->client, fields, rows);

    /* Free data */
    g_ptr_array_free(rows, TRUE);
    g_ptr_array_free(fields, TRUE);
    g_free(module_name);
    return PROXY_SEND_RESULT;
}

static int
admin_show_deny_ip(network_mysqld_con *con, const char *sql)
{
    char *module_name = str_nth_token(sql, 2);
    if (!module_name) {
        return PROXY_NO_DECISION;
    }
    if (strcmp(module_name, "admin") != 0 && strcmp(module_name, "proxy") != 0 && strcmp(module_name, "shard") != 0) {
        g_free(module_name);
        return PROXY_NO_DECISION;
    }
    chassis *chas = con->srv;
    GPtrArray *fields = g_ptr_array_new_with_free_func((void *)network_mysqld_proto_fielddef_free);
    MYSQL_FIELD *field;
    GPtrArray *rows = g_ptr_array_new_with_free_func((void *)network_mysqld_mysql_field_row_free);
    GPtrArray *row;

    field = network_mysqld_proto_fielddef_new();
    field->name = g_strdup("Plugin");
    field->type = MYSQL_TYPE_STRING;
    g_ptr_array_add(fields, field);
    field = network_mysqld_proto_fielddef_new();
    field->name = g_strdup("Address");
    field->type = MYSQL_TYPE_STRING;
    g_ptr_array_add(fields, field);

    int i;
    for (i = 0; i < chas->modules->len; i++) {
        chassis_plugin *plugin = chas->modules->pdata[i];
        if (strcmp(plugin->name, module_name) == 0) {
            GList *deny_ip_list = plugin->deny_ip_get(plugin->config);
            if (deny_ip_list) {
                GList *cur_p = deny_ip_list;
                while (cur_p) {
                    row = g_ptr_array_new_with_free_func(g_free);
                    g_ptr_array_add(row, g_strdup(module_name));
                    g_ptr_array_add(row, g_strdup((char *)cur_p->data));
                    g_ptr_array_add(rows, row);
                    cur_p = cur_p->next;
                }
            }
            break;
        }
    }

    network_mysqld_con_send_resultset(con->client, fields, rows);

    /* Free data */
    g_ptr_array_free(rows, TRUE);
    g_ptr_array_free(fields, TRUE);
    g_free(module_name);
    return PROXY_SEND_RESULT;
}

static int
admin_add_allow_ip(network_mysqld_con *con, const char *sql)
{
    char *module_name = str_nth_token(sql, 2);
    if (!module_name)
        return PROXY_NO_DECISION;
    char *addr = str_nth_token(sql, 3);
    if (!addr)
        return PROXY_NO_DECISION;
    if (strcmp(module_name, "admin") != 0 && strcmp(module_name, "proxy") != 0 && strcmp(module_name, "shard") != 0) {
        g_free(module_name);
        g_free(addr);
        return PROXY_SEND_RESULT;
    }

    chassis *chas = con->srv;
    int i;
    gboolean success = FALSE;
    for (i = 0; i < chas->modules->len; i++) {
        chassis_plugin *plugin = chas->modules->pdata[i];
        if (strcmp(plugin->name, module_name) == 0) {
            success = plugin->allow_ip_add(plugin->config, addr);
            break;
        }
    }
    network_mysqld_con_send_ok_full(con->client, success, 0, SERVER_STATUS_AUTOCOMMIT, 0);
    g_free(module_name);
    g_free(addr);

    return PROXY_SEND_RESULT;
}

static int
admin_add_deny_ip(network_mysqld_con *con, const char *sql)
{
    char *module_name = str_nth_token(sql, 2);
    if (!module_name)
        return PROXY_NO_DECISION;
    char *addr = str_nth_token(sql, 3);
    if (!addr)
        return PROXY_NO_DECISION;
    if (strcmp(module_name, "admin") != 0 && strcmp(module_name, "proxy") != 0 && strcmp(module_name, "shard") != 0) {
        g_free(module_name);
        g_free(addr);
        return PROXY_SEND_RESULT;
    }

    chassis *chas = con->srv;
    int i;
    gboolean success = FALSE;
    for (i = 0; i < chas->modules->len; i++) {
        chassis_plugin *plugin = chas->modules->pdata[i];
        if (strcmp(plugin->name, module_name) == 0) {
            success = plugin->deny_ip_add(plugin->config, addr);
            break;
        }
    }
    network_mysqld_con_send_ok_full(con->client, success, 0, SERVER_STATUS_AUTOCOMMIT, 0);
    g_free(module_name);
    g_free(addr);

    return PROXY_SEND_RESULT;
}

static int
admin_delete_allow_ip(network_mysqld_con *con, const char *sql)
{
    char *module_name = str_nth_token(sql, 2);
    if (!module_name)
        return PROXY_NO_DECISION;
    char *addr = str_nth_token(sql, 3);
    if (!addr)
        return PROXY_NO_DECISION;
    if (strcmp(module_name, "admin") != 0 && strcmp(module_name, "proxy") != 0 && strcmp(module_name, "shard") != 0) {
        g_free(module_name);
        g_free(addr);
        return PROXY_SEND_RESULT;
    }
    chassis *chas = con->srv;
    int i;
    gboolean success = FALSE;
    for (i = 0; i < chas->modules->len; i++) {
        chassis_plugin *plugin = chas->modules->pdata[i];
        if (strcmp(plugin->name, module_name) == 0) {
            success = plugin->allow_ip_del(plugin->config, addr);
            break;
        }
    }
    network_mysqld_con_send_ok_full(con->client, success, 0, SERVER_STATUS_AUTOCOMMIT, 0);
    g_free(module_name);
    g_free(addr);

    return PROXY_SEND_RESULT;
}

static int
admin_delete_deny_ip(network_mysqld_con *con, const char *sql)
{
    char *module_name = str_nth_token(sql, 2);
    if (!module_name)
        return PROXY_NO_DECISION;
    char *addr = str_nth_token(sql, 3);
    if (!addr)
        return PROXY_NO_DECISION;
    if (strcmp(module_name, "admin") != 0 && strcmp(module_name, "proxy") != 0 && strcmp(module_name, "shard") != 0) {
        g_free(module_name);
        g_free(addr);
        return PROXY_SEND_RESULT;
    }
    chassis *chas = con->srv;
    int i;
    gboolean success = FALSE;
    for (i = 0; i < chas->modules->len; i++) {
        chassis_plugin *plugin = chas->modules->pdata[i];
        if (strcmp(plugin->name, module_name) == 0) {
            success = plugin->deny_ip_del(plugin->config, addr);
            break;
        }
    }
    network_mysqld_con_send_ok_full(con->client, success, 0, SERVER_STATUS_AUTOCOMMIT, 0);
    g_free(module_name);
    g_free(addr);

    return PROXY_SEND_RESULT;
}

#define MAKE_FIELD_DEF_1_COL(fields, col_name)  \
    do {\
    MYSQL_FIELD *field = network_mysqld_proto_fielddef_new();\
    field->name = g_strdup((col_name));                      \
    field->type = FIELD_TYPE_VAR_STRING;\
    g_ptr_array_add(fields, field);         \
    }while(0)

#define MAKE_FIELD_DEF_2_COL(fields, col1_name, col2_name) \
    do {\
    MYSQL_FIELD *field = network_mysqld_proto_fielddef_new();\
    field->name = g_strdup((col1_name));                      \
    field->type = FIELD_TYPE_VAR_STRING;\
    g_ptr_array_add(fields, field);         \
    field = network_mysqld_proto_fielddef_new();     \
    field->name = g_strdup((col2_name));                      \
    field->type = FIELD_TYPE_VAR_STRING;\
    g_ptr_array_add(fields, field);         \
    }while(0)

#define MAKE_FIELD_DEF_3_COL(fields, col1_name, col2_name, col3_name) \
    do {\
    MYSQL_FIELD *field = network_mysqld_proto_fielddef_new();\
    field->name = g_strdup((col1_name));                      \
    field->type = FIELD_TYPE_VAR_STRING;\
    g_ptr_array_add(fields, field);         \
    field = network_mysqld_proto_fielddef_new();     \
    field->name = g_strdup((col2_name));                      \
    field->type = FIELD_TYPE_VAR_STRING;\
    g_ptr_array_add(fields, field);         \
    field = network_mysqld_proto_fielddef_new();     \
    field->name = g_strdup((col3_name));                      \
    field->type = FIELD_TYPE_VAR_STRING;\
    g_ptr_array_add(fields, field);  \
    }while(0)

#define APPEND_ROW_1_COL(rows, row_data) \
    do {\
    GPtrArray *row = g_ptr_array_new();\
    g_ptr_array_add(row, (row_data));  \
    g_ptr_array_add(rows, row);\
    }while(0)

#define APPEND_ROW_2_COL(rows, col1, col2)           \
    do {\
    GPtrArray *row = g_ptr_array_new();\
    g_ptr_array_add(row, (col1));  \
    g_ptr_array_add(row, (col2));  \
    g_ptr_array_add(rows, row);\
    }while(0)

#define APPEND_ROW_3_COL(rows, col1, col2, col3)           \
    do {\
    GPtrArray *row = g_ptr_array_new();\
    g_ptr_array_add(row, (col1));  \
    g_ptr_array_add(row, (col2));  \
    g_ptr_array_add(row, (col3));  \
    g_ptr_array_add(rows, row);\
    }while(0)

static void
strip_extra_spaces(char *str)
{
    int i, x;
    for (i = x = 0; str[i]; ++i)
        if (!isspace(str[i]) || (i > 0 && !isspace(str[i - 1])))
            str[x++] = str[i];
    str[x] = '\0';
}

static void
str_replace(char *p, const char *x, char y)
{
    int i, j;
    for (i = j = 0; p[i]; ++i) {
        if (p[i] == x[0] && p[i + 1] == x[1]) {
            ++i;
            p[j++] = y;
        } else {
            p[j++] = p[i];
        }
    }
    p[j] = '\0';
}

static void
normalize_equal_sign(char *p)
{
    str_replace(p, "= ", '=');
    str_replace(p, " =", '=');
}

/*
 * tolower, but leave "quoted strings" unmodified
 */
static void
lower_identifiers(char *str)
{
    gboolean in_string = FALSE;
    int i;
    for (i = 0; str[i]; ++i) {
        if (in_string)
            continue;
        if (str[i] == '\'' || str[i] == '"')
            in_string = !in_string;
        if (isalpha(str[i]))
            str[i] = tolower(str[i]);
    }
}

/* only match % wildcard, case insensitive */
static gboolean
sql_pattern_like(const char *pattern, const char *string)
{
    if (!pattern || pattern[0] == '\0')
        return TRUE;
    char *glob = g_strdup(pattern);
    int i;
    for (i = 0; glob[i]; ++i) {
        if (glob[i] == '%')
            glob[i] = '*';
        glob[i] = tolower(glob[i]);
    }
    char *lower_str = g_ascii_strdown(string, -1);
    gboolean rc = g_pattern_match_simple(glob, lower_str);
    g_free(glob);
    g_free(lower_str);
    return rc;
}

/* returned list must be freed */
static GList *
admin_get_all_options(chassis *chas)
{
    GList *options = g_list_copy(chas->options->options);   /* shallow copy */
    return options;
}

static int
admin_show_variables(network_mysqld_con *con, const char *sql)
{
    char **tokens = g_strsplit(sql, " ", -1);
    int token_count = g_strv_length(tokens);
    char *pattern = NULL;
    if (token_count == 2) {
        pattern = "%";
    } else if (token_count == 4) {
        pattern = tokens[3];
        cetus_string_dequote(pattern);
    } else {
        network_mysqld_con_send_error(con->client, C("error syntax"));
        g_strfreev(tokens);
        return PROXY_SEND_RESULT;
    }
    GList *options = admin_get_all_options(con->srv);

    GPtrArray *fields = network_mysqld_proto_fielddefs_new();
    MAKE_FIELD_DEF_3_COL(fields, "Variable_name", "Value", "Property");

    GPtrArray *rows = g_ptr_array_new_with_free_func((void *)network_mysqld_mysql_field_row_free);

    GList *freelist = NULL;
    GList *l = NULL;
    for (l = options; l; l = l->next) {
        chassis_option_t *opt = l->data;
        /* just support these for now */
        if (sql_pattern_like(pattern, opt->long_name)) {
            struct external_param param = {0};
            param.chas = con->srv;
            param.opt_type = opt->opt_property;
            char *value = opt->show_hook != NULL? opt->show_hook(&param) : NULL;
            if(NULL == value) {
                continue;
            }
            freelist = g_list_append(freelist, value);
            APPEND_ROW_3_COL(rows, (char *)opt->long_name, value, (CAN_ASSIGN_OPTS_PROPERTY(opt->opt_property)? "Dynamic" : "Static"));
        }
    }
    network_mysqld_con_send_resultset(con->client, fields, rows);

    network_mysqld_proto_fielddefs_free(fields);
    g_ptr_array_free(rows, TRUE);
    g_list_free_full(freelist, g_free);
    g_list_free(options);
    g_strfreev(tokens);
    return PROXY_SEND_RESULT;
}

static int
admin_show_status(network_mysqld_con *con, const char *sql)
{
    char **tokens = g_strsplit(sql, " ", -1);
    int token_count = g_strv_length(tokens);
    char *pattern = NULL;
    if (token_count == 2) {
        pattern = "%";
    } else if (token_count == 4) {
        pattern = tokens[3];
        cetus_string_dequote(pattern);
    } else {
        network_mysqld_con_send_error(con->client, C("error syntax"));
        g_strfreev(tokens);
        return PROXY_SEND_RESULT;
    }
    cetus_variable_t *variables = con->srv->priv->stats_variables;

    GPtrArray *fields = network_mysqld_proto_fielddefs_new();
    MAKE_FIELD_DEF_2_COL(fields, "Variable_name", "Value");

    GPtrArray *rows = g_ptr_array_new_with_free_func((void *)network_mysqld_mysql_field_row_free);

    GList *freelist = NULL;
    int i = 0;
    for (i = 0; variables[i].name; ++i) {
        if (sql_pattern_like(pattern, variables[i].name)) {
            char *value = cetus_variable_get_value_str(&variables[i]);
            freelist = g_list_append(freelist, value);
            APPEND_ROW_2_COL(rows, variables[i].name, value);
        }
    }
    network_mysqld_con_send_resultset(con->client, fields, rows);

    network_mysqld_proto_fielddefs_free(fields);
    g_ptr_array_free(rows, TRUE);
    g_list_free_full(freelist, g_free);
    g_strfreev(tokens);
    return PROXY_SEND_RESULT;
}

static int
admin_set_reduce_conns(network_mysqld_con *con, const char *sql)
{
    char *mode = str_nth_token(sql, 2);
    if (mode) {
        if (strcasecmp(mode, "true") == 0) {
            con->srv->is_reduce_conns = 1;
        } else if (strcasecmp(mode, "false") == 0) {
            con->srv->is_reduce_conns = 0;
        }
        g_free(mode);
    }
    network_mysqld_con_send_ok(con->client);
    return PROXY_SEND_RESULT;
}

static int
admin_reduce_memory(network_mysqld_con *con, const char *sql)
{
    struct mallinfo m;

    m = mallinfo();

    g_message("%s:Total allocated space (bytes): %d", G_STRLOC, m.uordblks);
    g_message("%s:Total free space (bytes): %d", G_STRLOC, m.fordblks);
    g_message("%s:Top-most, releasable space (bytes): %d", G_STRLOC, m.keepcost);

    if (m.fordblks > m.uordblks) {
        malloc_trim(0);
        m = mallinfo();
        g_message("%s:After trim, total allocated space (bytes): %d", G_STRLOC, m.uordblks);
        g_message("%s:After trim, total free space (bytes): %d", G_STRLOC, m.fordblks);
        g_message("%s:After trim, top-most, releasable space (bytes): %d", G_STRLOC, m.keepcost);
    }

    network_mysqld_con_send_ok_full(con->client, 1, 0, SERVER_STATUS_AUTOCOMMIT, 0);
    return PROXY_SEND_RESULT;
}

static int
admin_set_maintain(network_mysqld_con *con, const char *sql)
{
    gboolean error = FALSE;
    gint effected_rows = 0;
    char *mode = str_nth_token(sql, 2);
    if (mode) {
        if (strcasecmp(mode, "true") == 0) {
            if(con->srv->maintain_close_mode != 1) {
                con->srv->maintain_close_mode = 1;
                effected_rows++;
            }
        } else if (strcasecmp(mode, "false") == 0) {
            if(con->srv->maintain_close_mode != 0) {
                con->srv->maintain_close_mode = 0;
                effected_rows++;
            }
        } else {
            error = TRUE;
        }
        g_free(mode);
    } else {
        error = TRUE;
    }
    if (error)
        return PROXY_NO_DECISION;
    network_mysqld_con_send_ok_full(con->client, effected_rows, 0, SERVER_STATUS_AUTOCOMMIT, 0);
    return PROXY_SEND_RESULT;
}

static int
admin_show_maintain(network_mysqld_con *con, const char *sql)
{
    GPtrArray *fields = network_mysqld_proto_fielddefs_new();
    GPtrArray *rows = g_ptr_array_new_with_free_func((void *)network_mysqld_mysql_field_row_free);
    MAKE_FIELD_DEF_1_COL(fields, "Cetus maintain status");
    if(con->srv->maintain_close_mode == 1) {
        APPEND_ROW_1_COL(rows, "true");
    } else {
        APPEND_ROW_1_COL(rows, "false");
    }
    network_mysqld_con_send_resultset(con->client, fields, rows);
    network_mysqld_proto_fielddefs_free(fields);
    g_ptr_array_free(rows, TRUE);
    return PROXY_SEND_RESULT;
}

static int
admin_reload_shard(network_mysqld_con *con, const char *sql)
{
    network_mysqld_con_send_ok_full(con->client, 1, 0, SERVER_STATUS_AUTOCOMMIT, 0);
    return PROXY_SEND_RESULT;
}

static int
admin_send_version(network_mysqld_con *con, const char *sql)
{
    GPtrArray *fields = network_mysqld_proto_fielddefs_new();

    MAKE_FIELD_DEF_1_COL(fields, "cetus version");

    GPtrArray *rows = g_ptr_array_new_with_free_func((void *)network_mysqld_mysql_field_row_free);

    APPEND_ROW_1_COL(rows, PLUGIN_VERSION);

    network_mysqld_con_send_resultset(con->client, fields, rows);

    network_mysqld_proto_fielddefs_free(fields);
    g_ptr_array_free(rows, TRUE);
    return PROXY_SEND_RESULT;
}

static int
admin_send_connection_stat(network_mysqld_con *con, const char *sql)
{
    int backend_ndx = -1;
    char user[128] = { 0 };
    sscanf(sql,
           "select conn_num from backends where backend_ndx=%d and user=%*['\"]%64[^'\"]%*['\"]", &backend_ndx, user);
    if (backend_ndx == -1 || user[0] == '\0')
        return PROXY_NO_DECISION;

    GPtrArray *fields = network_mysqld_proto_fielddefs_new();

    MAKE_FIELD_DEF_1_COL(fields, "connection_num");

    GPtrArray *rows = g_ptr_array_new_with_free_func((void *)network_mysqld_mysql_field_row_free);

    char *numstr = NULL;
    chassis_private *g = con->srv->priv;
    backend_ndx -= 1;           /* index in sql start from 1, not 0 */
    if (backend_ndx >= 0 && backend_ndx < network_backends_count(g->backends)) {
        network_backend_t *backend = network_backends_get(g->backends, backend_ndx);
        GString *user_name = g_string_new(user);
        GQueue *conns = NULL;

        if(backend && backend->pool && backend->pool->users && user_name) {
            conns = g_hash_table_lookup(backend->pool->users, user_name);
        }

        if (conns) {
            numstr = g_strdup_printf("%d", conns->length);
        }
        g_string_free(user_name, TRUE);
    }
    if (numstr) {
        APPEND_ROW_1_COL(rows, numstr);
    } else {
        APPEND_ROW_1_COL(rows, "0");
    }

    network_mysqld_con_send_resultset(con->client, fields, rows);

    network_mysqld_proto_fielddefs_free(fields);
    g_ptr_array_free(rows, TRUE);
    if (numstr)
        g_free(numstr);

    return PROXY_SEND_RESULT;
}

static void
bytes_to_hex_str(char *pin, int len, char *pout)
{
    const char *hex = "0123456789ABCDEF";
    int i = 0;
    for (; i < len; ++i) {
        *pout++ = hex[(*pin >> 4) & 0xF];
        *pout++ = hex[(*pin++) & 0xF];
    }
    *pout = 0;
}

static int
admin_send_user_password(network_mysqld_con *con, const char *sql)
{
    char from_table[128] = { 0 };
    char user[128] = { 0 };
    char *where_start = strcasestr(sql, "where");
    if (where_start) {
        sscanf(sql, "select * from %s where user=%*['\"]%64[^'\"]%*['\"]", from_table, user);
        if (from_table[0] == '\0' || user[0] == '\0')
            return PROXY_NO_DECISION;
    } else {
        sscanf(sql, "select * from %s", from_table);
        if (from_table[0] == '\0')
            return PROXY_NO_DECISION;
    }

    chassis_private *g = con->srv->priv;
    enum cetus_pwd_type pwd_type;
    if (strcmp(from_table, "user_pwd") == 0) {
        pwd_type = CETUS_SERVER_PWD;
    } else if (strcmp(from_table, "app_user_pwd") == 0) {
        pwd_type = CETUS_CLIENT_PWD;
    } else {
        g_critical("error target db");
        return PROXY_NO_DECISION;
    }

    GPtrArray *fields = network_mysqld_proto_fielddefs_new();

    MAKE_FIELD_DEF_2_COL(fields, "user", "password(sha1)");

    GPtrArray *rows = g_ptr_array_new_with_free_func((void *)network_mysqld_mysql_field_row_free);

    if (user[0]) {              /* one user */
        GString *hashpwd = g_string_new(0);
        cetus_users_get_hashed_pwd(g->users, user, pwd_type, hashpwd);
        char *pwdhex = NULL;
        if (hashpwd->len > 0) {
            pwdhex = g_malloc0(hashpwd->len * 2 + 10);
            bytes_to_hex_str(hashpwd->str, hashpwd->len, pwdhex);
            APPEND_ROW_2_COL(rows, user, pwdhex);
        }
        network_mysqld_con_send_resultset(con->client, fields, rows);
        network_mysqld_proto_fielddefs_free(fields);
        g_ptr_array_free(rows, TRUE);
        g_string_free(hashpwd, TRUE);
        if (pwdhex)
            g_free(pwdhex);
    } else {                    /* all users */
        GList *strings_to_free = NULL;
        GHashTableIter iter;
        char *username = NULL;
        GString *hashpwd = g_string_new(0);
        char *hack = NULL;      /* don't use value directly */
        g_hash_table_iter_init(&iter, g->users->records);
        while (g_hash_table_iter_next(&iter, (gpointer *) & username, (gpointer *) & hack)) {
            cetus_users_get_hashed_pwd(g->users, username, pwd_type, hashpwd);
            char *pwdhex = NULL;
            if (hashpwd->len > 0) {
                pwdhex = g_malloc0(hashpwd->len * 2 + 10);
                bytes_to_hex_str(hashpwd->str, hashpwd->len, pwdhex);
                strings_to_free = g_list_append(strings_to_free, pwdhex);
            }
            APPEND_ROW_2_COL(rows, username, pwdhex);
        }
        network_mysqld_con_send_resultset(con->client, fields, rows);
        network_mysqld_proto_fielddefs_free(fields);
        g_ptr_array_free(rows, TRUE);
        g_string_free(hashpwd, TRUE);
        if (strings_to_free)
            g_list_free_full(strings_to_free, g_free);
    }
    return PROXY_SEND_RESULT;
}

/* update or insert */
static int
admin_update_user_password(network_mysqld_con *con, const char *sql)
{
    char from_table[128] = { 0 };
    char user[128] = { 0 };
    char new_pwd[128] = { 0 };
    sscanf(sql, "update %64s set password=%*['\"]%64[^'\"]%*['\"] where user=%*['\"]%64[^'\"]%*['\"]",
           from_table, new_pwd, user);
    if (new_pwd[0] == '\0' || from_table[0] == '\0' || user[0] == '\0')
        return PROXY_NO_DECISION;

    chassis_private *g = con->srv->priv;
    enum cetus_pwd_type pwd_type = CETUS_UNKNOWN_PWD;
    if (strcmp(from_table, "user_pwd") == 0) {
        pwd_type = CETUS_SERVER_PWD;
    } else if (strcmp(from_table, "app_user_pwd") == 0) {
        pwd_type = CETUS_CLIENT_PWD;
    }
    gboolean affected = cetus_users_update_record(g->users, user, new_pwd, pwd_type);
    if (affected)
        cetus_users_write_json(g->users);
    network_mysqld_con_send_ok_full(con->client, affected ? 1 : 0, 0, SERVER_STATUS_AUTOCOMMIT, 0);
    return PROXY_SEND_RESULT;
}

static int
admin_delete_user_password(network_mysqld_con *con, const char *sql)
{
    char from_table[128] = { 0 };
    char user[128] = { 0 };
    sscanf(sql, "delete from %64s where user=%*['\"]%64[^'\"]%*['\"]", from_table, user);
    if (from_table[0] == '\0' || user[0] == '\0')
        return PROXY_NO_DECISION;

    chassis_private *g = con->srv->priv;
    gboolean affected = cetus_users_delete_record(g->users, user);
    if (affected)
        cetus_users_write_json(g->users);
    network_mysqld_con_send_ok_full(con->client, affected ? 1 : 0, 0, SERVER_STATUS_AUTOCOMMIT, 0);
    return PROXY_SEND_RESULT;
}

static backend_type_t
backend_type(const char *str)
{
    backend_type_t type = BACKEND_TYPE_UNKNOWN;
    if (strcasecmp(str, "ro") == 0)
        type = BACKEND_TYPE_RO;
    else if (strcasecmp(str, "rw") == 0)
        type = BACKEND_TYPE_RW;
    return type;
}

static backend_state_t
backend_state(const char *str)
{
    backend_state_t state = BACKEND_STATE_END;
    if (strcasecmp(str, "up") == 0)
        state = BACKEND_STATE_UP;
    else if (strcasecmp(str, "down") == 0)
        state = BACKEND_STATE_DOWN;
    else if (strcasecmp(str, "maintaining") == 0)
        state = BACKEND_STATE_MAINTAINING;
    else if (strcasecmp(str, "unknown") == 0)
        state = BACKEND_STATE_UNKNOWN;
    return state;
}

static int
admin_insert_backend(network_mysqld_con *con, const char *sql)
{
    /* TODO: to which group */
    char address[128] = { 0 };
    char type_str[64] = { 0 };
    char state_str[64] = { 0 };
    sscanf(sql,
           "insert into backends values (%*['\"]%64[0-9:.@a-zA-Z]%*['\"], %*['\"]%32[rowRWO]%*['\"], %*['\"]%32[a-zA-Z]%*['\"])",
           address, type_str, state_str);
    if (address[0] == '\0' || type_str[0] == '\0' || state_str[0] == '\0')
        return PROXY_NO_DECISION;

    chassis_private *g = con->srv->priv;
    backend_state_t state = backend_state(state_str);
    if (state == BACKEND_STATE_DOWN) {
        con->srv->is_manual_down = 1;
    } else {
        con->srv->is_manual_down = 0;
    }

    int affected = network_backends_add(g->backends, address,
                                        backend_type(type_str),
                                        state,
                                        con->srv);
    network_mysqld_con_send_ok_full(con->client, affected == 0 ? 1 : 0, 0, SERVER_STATUS_AUTOCOMMIT, 0);
    return PROXY_SEND_RESULT;
}

static int
admin_add_backend(network_mysqld_con *con, const char *sql)
{
    char address[128] = { 0 };
    backend_type_t type = BACKEND_TYPE_UNKNOWN;
    if (strncasecmp(sql, "add master", 10) == 0) {
        sscanf(sql, "add master %*['\"]%64[0-9:.@a-zA-Z]%*['\"]", address);
        type = BACKEND_TYPE_RW;
    } else if (strncasecmp(sql, "add slave", 9) == 0) {
        sscanf(sql, "add slave %*['\"]%64[0-9:.@a-zA-Z]%*['\"]", address);
        type = BACKEND_TYPE_RO;
    }
    if (address[0] == '\0' || type == BACKEND_TYPE_UNKNOWN)
        return PROXY_NO_DECISION;

    chassis_private *g = con->srv->priv;
    int affected = network_backends_add(g->backends, address, type, BACKEND_STATE_UNKNOWN, con->srv);
    network_mysqld_con_send_ok_full(con->client, affected == 0 ? 1 : 0, 0, SERVER_STATUS_AUTOCOMMIT, 0);
    return PROXY_SEND_RESULT;
}

static int
admin_update_backend(network_mysqld_con *con, const char *sql)
{
    gint ret = 0;
    char *set_start = strcasestr(sql, "set");
    char *where_start = strcasestr(sql, "where");

    char *set_cols_str = NULL;
    if (where_start) {
        set_cols_str = g_strndup(set_start + 4, where_start - set_start - 4);
    } else {
        set_cols_str = g_strdup(set_start + 4);
    }
    char **set_cols_array = g_strsplit(set_cols_str, ",", -1);
    int i = 0;
    char type_str[64] = { 0 };
    char state_str[64] = { 0 };
    char *col;
    for (; (col = set_cols_array[i]) != NULL; ++i) {
        g_strstrip(col);
        if (strncasecmp(col, "type", 4) == 0) {
            sscanf(col, "type=%*['\"]%32[a-zA-Z]%*['\"]", type_str);
        } else if (strncasecmp(col, "state", 5) == 0) {
            sscanf(col, "state=%*['\"]%32[a-zA-Z]%*['\"]", state_str);
        }
    }
    g_free(set_cols_str);
    g_strfreev(set_cols_array);

    if (type_str[0] == '\0' && state_str[0] == '\0')
        return PROXY_NO_DECISION;

    chassis_private *g = con->srv->priv;
    int affected_rows = 0;
    if (where_start) {
        int backend_ndx = -1;
        sscanf(where_start, "where backend_ndx=%d", &backend_ndx);
        backend_ndx -= 1;       /* index in sql start from 1, not 0 */
        if (backend_ndx < 0) {
            char address[128] = { 0 };
            sscanf(where_start, "where address=%*['\"]%64[0-9:.]%*['\"]", address);
            backend_ndx = network_backends_find_address(g->backends, address);
        }
        network_backend_t *bk = network_backends_get(g->backends, backend_ndx);
        if (!bk) {
            network_mysqld_con_send_error(con->client, C("no such backend"));
            return PROXY_SEND_RESULT;
        }
        int type = type_str[0] != '\0' ? backend_type(type_str) : bk->type;
        int state = state_str[0] != '\0' ? backend_state(state_str) : bk->state;
        ret = network_backends_modify(g->backends, backend_ndx, type, state, NO_PREVIOUS_STATE);
        affected_rows = 1;
    } else {
        for (i = 0; i < network_backends_count(g->backends); ++i) {
            network_backend_t *bk = network_backends_get(g->backends, i);
            if (bk) {
                int type = type_str[0] != '\0' ? backend_type(type_str) : bk->type;
                int state = state_str[0] != '\0' ? backend_state(state_str) : bk->state;
                ret = network_backends_modify(g->backends, i, type, state, NO_PREVIOUS_STATE);
                affected_rows += 1;
            }
        }
    }
    if(ret == 0) {
        network_mysqld_con_send_ok_full(con->client, affected_rows, 0, SERVER_STATUS_AUTOCOMMIT, 0);
    } else {
        network_mysqld_con_send_error_full(con->client, C("Command or parameter is incorrect."), 1045, "28000");
    }
    return PROXY_SEND_RESULT;
}

static int
admin_delete_backend(network_mysqld_con *con, const char *sql)
{
    char *where_start = strcasestr(sql, "where");

    chassis_private *g = con->srv->priv;
    int backend_ndx = -1;
    if (where_start) {          /* delete from backends where xx=xx */
        sscanf(where_start, "where backend_ndx=%d", &backend_ndx);
        backend_ndx -= 1;       /* index in sql start from 1, not 0 */
        if (backend_ndx < 0) {
            char address[128] = { 0 };
            sscanf(where_start, "where address=%*['\"]%64[0-9:.]%*['\"]", address);
            backend_ndx = network_backends_find_address(g->backends, address);
        }
    } else {                    /* remove backend xxx */
        sscanf(sql, "remove backend %d", &backend_ndx);
        backend_ndx -= 1;       /* index in sql start from 1, not 0 */
    }
    if (backend_ndx >= 0 && backend_ndx < network_backends_count(g->backends)) {
        gint ret = network_backends_remove(g->backends, backend_ndx);  /* TODO: just change state? */
        if(ret == 0) {
            network_mysqld_con_send_ok_full(con->client, 1, 0, SERVER_STATUS_AUTOCOMMIT, 0);
        } else {
            network_mysqld_con_send_error_full(con->client, C("delete failed."), 1045, "28000");
        }
        return PROXY_SEND_RESULT;
    } else {
        network_mysqld_con_send_ok_full(con->client, 0, 0, SERVER_STATUS_AUTOCOMMIT, 0);
        return PROXY_SEND_RESULT;
    }
}

static int
admin_supported_stats(network_mysqld_con *con)
{
    GPtrArray *fields = network_mysqld_proto_fielddefs_new();
    MAKE_FIELD_DEF_1_COL(fields, "name");
    GPtrArray *rows = g_ptr_array_new_with_free_func((void *)network_mysqld_mysql_field_row_free);
    APPEND_ROW_1_COL(rows, "client_query");
    APPEND_ROW_1_COL(rows, "proxyed_query");
    APPEND_ROW_1_COL(rows, "reset");
    APPEND_ROW_1_COL(rows, "query_time_table");
    APPEND_ROW_1_COL(rows, "server_query_details");
    APPEND_ROW_1_COL(rows, "query_wait_table");
    network_mysqld_con_send_resultset(con->client, fields, rows);
    network_mysqld_proto_fielddefs_free(fields);
    g_ptr_array_free(rows, TRUE);
    return PROXY_SEND_RESULT;
}

static int
admin_get_stats(network_mysqld_con *con, const char *sql)
{
    const char *p = sql + 9;
    if (*p == '\0') {           /* just "stats get", no argument */
        return admin_supported_stats(con);
    } else {
        ++p;                    /* stats get [xxx], point to xxx */
    }

    GPtrArray *fields = network_mysqld_proto_fielddefs_new();
    MAKE_FIELD_DEF_2_COL(fields, "name", "value");
    GPtrArray *rows = g_ptr_array_new_with_free_func((void *)network_mysqld_mysql_field_row_free);
    chassis *chas = con->srv;
    query_stats_t *stats = &(chas->query_stats);
    char buf1[32] = { 0 };
    char buf2[32] = { 0 };
    int i;
    if (strcasecmp(p, "client_query") == 0) {
        snprintf(buf1, 32, "%lu", stats->client_query.ro);
        snprintf(buf2, 32, "%lu", stats->client_query.rw);
        APPEND_ROW_2_COL(rows, "client_query.ro", buf1);
        APPEND_ROW_2_COL(rows, "client_query.rw", buf2);
    } else if (strcasecmp(p, "proxyed_query") == 0) {
        snprintf(buf1, 32, "%lu", stats->proxyed_query.ro);
        snprintf(buf2, 32, "%lu", stats->proxyed_query.rw);
        APPEND_ROW_2_COL(rows, "proxyed_query.ro", buf1);
        APPEND_ROW_2_COL(rows, "proxyed_query.rw", buf2);
    } else if (strcasecmp(p, "query_time_table") == 0) {
        for (i = 0; i < MAX_QUERY_TIME && stats->query_time_table[i]; ++i) {
            GPtrArray *row = g_ptr_array_new_with_free_func(g_free);
            g_ptr_array_add(row, g_strdup_printf("query_time_table.%d", i + 1));
            g_ptr_array_add(row, g_strdup_printf("%lu", stats->query_time_table[i]));
            g_ptr_array_add(rows, row);
        }
    } else if (strcasecmp(p, "query_wait_table") == 0) {
        for (i = 0; i < MAX_QUERY_TIME && stats->query_wait_table[i]; ++i) {
            GPtrArray *row = g_ptr_array_new_with_free_func(g_free);
            g_ptr_array_add(row, g_strdup_printf("query_wait_table.%d", i + 1));
            g_ptr_array_add(row, g_strdup_printf("%lu", stats->query_wait_table[i]));
            g_ptr_array_add(rows, row);
        }
    } else if (strcasecmp(p, "server_query_details") == 0) {
        for (i = 0; i < MAX_SERVER_NUM && i < network_backends_count(chas->priv->backends); ++i) {
            GPtrArray *row = g_ptr_array_new_with_free_func(g_free);
            g_ptr_array_add(row, g_strdup_printf("server_query_details.%d.ro", i + 1));
            g_ptr_array_add(row, g_strdup_printf("%lu", stats->server_query_details[i].ro));
            g_ptr_array_add(rows, row);
            row = g_ptr_array_new_with_free_func(g_free);
            g_ptr_array_add(row, g_strdup_printf("server_query_details.%d.rw", i + 1));
            g_ptr_array_add(row, g_strdup_printf("%lu", stats->server_query_details[i].rw));
            g_ptr_array_add(rows, row);
        }
    } else if (strcasecmp(p, "reset") == 0) {
        APPEND_ROW_2_COL(rows, "reset", "0");
    } else {
        APPEND_ROW_2_COL(rows, (char *)p, (char *)p);
    }
    network_mysqld_con_send_resultset(con->client, fields, rows);

    network_mysqld_proto_fielddefs_free(fields);
    g_ptr_array_free(rows, TRUE);
    return PROXY_SEND_RESULT;
}

static int
admin_supported_config(network_mysqld_con *con)
{
    GPtrArray *fields = network_mysqld_proto_fielddefs_new();
    MAKE_FIELD_DEF_1_COL(fields, "name");
    GPtrArray *rows = g_ptr_array_new_with_free_func((void *)network_mysqld_mysql_field_row_free);
    APPEND_ROW_1_COL(rows, "common");
    APPEND_ROW_1_COL(rows, "pool");
    network_mysqld_con_send_resultset(con->client, fields, rows);
    network_mysqld_proto_fielddefs_free(fields);
    g_ptr_array_free(rows, TRUE);
    return PROXY_SEND_RESULT;
}

static int
admin_get_config(network_mysqld_con *con, const char *sql)
{
    const char *p = sql + 10;
    if (*p == '\0') {           /* just "config get", no argument */
        return admin_supported_config(con);
    } else {
        ++p;                    /* config get [xxx], point to xxx */
    }

    GPtrArray *fields = network_mysqld_proto_fielddefs_new();
    MAKE_FIELD_DEF_2_COL(fields, "name", "value");
    GPtrArray *rows = g_ptr_array_new_with_free_func((void *)network_mysqld_mysql_field_row_free);
    chassis *chas = con->srv;
    char buf1[32] = { 0 }, buf2[32] = {
    0};
    char buf3[32] = { 0 }, buf4[32] = {
    0};
    if (strcasecmp(p, "common") == 0) {
        snprintf(buf1, 32, "%d", chas->check_slave_delay);
        snprintf(buf2, 32, "%lf", chas->slave_delay_down_threshold_sec);
        snprintf(buf3, 32, "%lf", chas->slave_delay_recover_threshold_sec);
        snprintf(buf4, 32, "%d", chas->long_query_time);
        APPEND_ROW_2_COL(rows, "common.check_slave_delay", buf1);
        APPEND_ROW_2_COL(rows, "common.slave_delay_down_threshold_sec", buf2);
        APPEND_ROW_2_COL(rows, "common.slave_delay_recover_threshold_sec", buf3);
        APPEND_ROW_2_COL(rows, "common.long_query_time", buf4);
    } else if (strcasecmp(p, "pool") == 0) {
        snprintf(buf1, 32, "%d", chas->mid_idle_connections);
        snprintf(buf2, 32, "%d", chas->max_idle_connections);
        snprintf(buf3, 32, "%d", chas->max_resp_len);
        snprintf(buf4, 32, "%d", chas->master_preferred);
        APPEND_ROW_2_COL(rows, "pool.default_pool_size", buf1);
        APPEND_ROW_2_COL(rows, "pool.max_pool_size", buf2);
        APPEND_ROW_2_COL(rows, "pool.max_resp_len", buf3);
        APPEND_ROW_2_COL(rows, "pool.master_preferred", buf4);
    } else {
        APPEND_ROW_2_COL(rows, (char *)p, (char *)p);
    }
    network_mysqld_con_send_resultset(con->client, fields, rows);

    network_mysqld_proto_fielddefs_free(fields);
    g_ptr_array_free(rows, TRUE);
    return PROXY_SEND_RESULT;
}

static int
admin_set_config(network_mysqld_con *con, const char *sql)
{
    char key[128] = { 0 };
    char val[128] = { 0 };
    sscanf(sql, "config set %64[a-zA-Z0-9_.-]=%64[a-zA-Z0-9_.-]", key, val);
    if (key[0] == '\0') {
        network_mysqld_con_send_ok_full(con->client, 0, 0, SERVER_STATUS_AUTOCOMMIT, 0);
        return PROXY_SEND_RESULT;
    }

    if (val[0] == '\0') {
        network_mysqld_con_send_error_full(con->client, C("Value can only contain [a-zA-Z0-9_.-], and don't have to surround with quotes. "), 1065, "28000");
        return PROXY_SEND_RESULT;
    }

    int affected_rows = 0;
    int ret = 0;

    GList *options = admin_get_all_options(con->srv);

    GList *l = NULL;
    for (l = options; l; l = l->next) {
        chassis_option_t *opt = l->data;
        if (strcasecmp(key, opt->long_name) == 0) {
            struct external_param param = {0};
            param.chas = con->srv;
            param.opt_type = opt->opt_property;
            ret = opt->assign_hook != NULL? opt->assign_hook(val, &param) : ASSIGN_NOT_SUPPORT;
            affected_rows++;
            break;
        }
    }
    g_list_free(options);

    if(0 == ret) {
        network_mysqld_con_send_ok_full(con->client, affected_rows, 0, SERVER_STATUS_AUTOCOMMIT, 0);
    } else if(ASSIGN_NOT_SUPPORT == ret){
        network_mysqld_con_send_error_full(con->client, C("Variable cannot be set dynamically"), 1065, "28000");
    } else if(ASSIGN_VALUE_INVALID == ret){
        network_mysqld_con_send_error_full(con->client, C("Value is illegal"), 1065, "28000");
    } else {
        network_mysqld_con_send_error_full(con->client, C("You have an error in your SQL syntax"), 1065, "28000");
    }
    return PROXY_SEND_RESULT;
}

static int
admin_reset_stats(network_mysqld_con *con, const char *sql)
{
    query_stats_t *stats = &con->srv->query_stats;
    memset(stats, 0, sizeof(*stats));
    network_mysqld_con_send_ok_full(con->client, 1, 0, SERVER_STATUS_AUTOCOMMIT, 0);
    return PROXY_SEND_RESULT;
}

static int
admin_save_settings(network_mysqld_con *con, const char *sql)
{
    chassis *srv = con->srv;
    GKeyFile *keyfile = g_key_file_new();
    g_key_file_set_list_separator(keyfile, ',');
    gint ret = ASSIGN_OK;
    gint effected_rows = 0;
    GString *free_path = g_string_new(NULL);

    if(srv->default_file == NULL) {
        free_path = g_string_append(free_path, get_current_dir_name());
        free_path = g_string_append(free_path, "/default.conf");
        srv->default_file = g_strdup(free_path->str);
    }

    if(!g_path_is_absolute(srv->default_file)) {
        free_path = g_string_append(free_path, get_current_dir_name());
        free_path = g_string_append(free_path, "/");
        free_path = g_string_append(free_path, srv->default_file);
        if(srv->default_file) {
            g_free(srv->default_file);
        }
        srv->default_file = g_strdup(free_path->str);
    }
    if(free_path) {
        g_string_free(free_path, TRUE);
    }
    /* rename config file */
    if(srv->default_file) {
        GString *new_file = g_string_new(NULL);
        new_file = g_string_append(new_file, srv->default_file);
        new_file = g_string_append(new_file, ".old");

        if (remove(new_file->str)) {
            g_debug("remove operate, filename:%s, errno:%d", 
                    new_file->str == NULL? "":new_file->str, errno);
        }

        if(rename(srv->default_file, new_file->str)) {
            g_debug("rename operate failed, filename:%s, filename:%s, errno:%d",
                    (srv->default_file == NULL ? "":srv->default_file),
                    (new_file->str == NULL ? "":new_file->str), errno);
            ret = RENAME_ERROR;
        }
        g_string_free(new_file, TRUE);
    }

    if(ret == ASSIGN_OK) {
        /* save new config */
        effected_rows = chassis_options_save(keyfile, srv->options, srv);
        gsize file_size = 0;
        gchar *file_buf = g_key_file_to_data(keyfile, &file_size, NULL);
        GError *gerr = NULL;
        if (FALSE == g_file_set_contents(srv->default_file, file_buf, file_size, &gerr)) {
            ret = SAVE_ERROR;
        } else {
            if((ret = chmod(srv->default_file, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP))) {
                g_debug("remove operate failed, filename:%s, errno:%d", 
                        (srv->default_file == NULL? "":srv->default_file), errno);
                ret = CHMOD_ERROR;
            }
        }
    }

    if(ret == ASSIGN_OK) {
        network_mysqld_con_send_ok_full(con->client, effected_rows, 0, SERVER_STATUS_AUTOCOMMIT, 0);
    } else if(ret == RENAME_ERROR) {
        network_mysqld_con_send_error_full(con->client, C("rename file failed"), 1066, "28000");
    } else if(ret == SAVE_ERROR) {
        network_mysqld_con_send_error_full(con->client, C("save file failed"), 1066, "28000");
    } else if(ret == CHMOD_ERROR) {
        network_mysqld_con_send_error_full(con->client, C("chmod file failed"), 1066, "28000");
    }

    return PROXY_SEND_RESULT;
}

static void calc_qps_average(char *buf, int len);
static void calc_tps_average(char *buf, int len);

static void
get_module_names(chassis *chas, GString *plugin_names)
{
    int i;
    for (i = 0; i < chas->modules->len; ++i) {
        chassis_plugin *p = g_ptr_array_index(chas->modules, i);
        g_string_append(plugin_names, p->name);
        g_string_append_c(plugin_names, ' ');
    }
}

static int
admin_send_status(network_mysqld_con *con, const char *sql)
{
    chassis_private *g = con->srv->priv;
    chassis_plugin_config *config = con->config;

    GPtrArray *fields = network_mysqld_proto_fielddefs_new();
    MAKE_FIELD_DEF_2_COL(fields, "Status", "Value");

    GPtrArray *rows = g_ptr_array_new_with_free_func((void *)network_mysqld_mysql_field_row_free);
    APPEND_ROW_2_COL(rows, "Cetus version", PLUGIN_VERSION);
    char start_time[32];
    chassis_epoch_to_string(&con->srv->startup_time, C(start_time));
    APPEND_ROW_2_COL(rows, "Startup time", start_time);
    GString *plugin_names = g_string_new(0);
    get_module_names(con->srv, plugin_names);
    APPEND_ROW_2_COL(rows, "Loaded modules", plugin_names->str);
    const int bsize = 32;
    static char buf1[32], buf2[32], buf3[32];
    snprintf(buf1, bsize, "%d", network_backends_idle_conns(g->backends));
    APPEND_ROW_2_COL(rows, "Idle backend connections", buf1);
    snprintf(buf2, bsize, "%d", network_backends_used_conns(g->backends));
    APPEND_ROW_2_COL(rows, "Used backend connections", buf2);
    snprintf(buf3, bsize, "%d", g->cons->len);
    APPEND_ROW_2_COL(rows, "Client connections", buf3);

    query_stats_t *stats = &(con->srv->query_stats);
    char qcount[32];
    snprintf(qcount, 32, "%ld", stats->client_query.ro + stats->client_query.rw);
    APPEND_ROW_2_COL(rows, "Query count", qcount);

    if (config->has_shard_plugin) {
        char xacount[32];
        snprintf(xacount, 32, "%ld", stats->xa_count);
        APPEND_ROW_2_COL(rows, "XA count", xacount);
    }

    char qps[64];
    calc_qps_average(C(qps));
    APPEND_ROW_2_COL(rows, "QPS (1min, 5min, 15min)", qps);
    char tps[64];
    calc_tps_average(C(tps));
    APPEND_ROW_2_COL(rows, "TPS (1min, 5min, 15min)", tps);
    network_mysqld_con_send_resultset(con->client, fields, rows);

    network_mysqld_proto_fielddefs_free(fields);
    g_ptr_array_free(rows, TRUE);
    g_string_free(plugin_names, TRUE);
    return PROXY_SEND_RESULT;
}

static int
admin_send_group_info(network_mysqld_con *con, const char *sql)
{
    GPtrArray *fields = network_mysqld_proto_fielddefs_new();

    MYSQL_FIELD *field = network_mysqld_proto_fielddef_new();
    field->name = g_strdup("group");
    field->type = FIELD_TYPE_VAR_STRING;
    g_ptr_array_add(fields, field);
    field = network_mysqld_proto_fielddef_new();
    field->name = g_strdup(("master"));
    field->type = FIELD_TYPE_VAR_STRING;
    g_ptr_array_add(fields, field);
    field = network_mysqld_proto_fielddef_new();
    field->name = g_strdup(("slaves"));
    field->type = FIELD_TYPE_VAR_STRING;
    g_ptr_array_add(fields, field);

    GPtrArray *rows = g_ptr_array_new_with_free_func((void *)network_mysqld_mysql_field_row_free);

    GList *free_list = NULL;
    network_backends_t *bs = con->srv->priv->backends;
    int i;
    for (i = 0; i < bs->groups->len; ++i) {
        network_group_t *gp = g_ptr_array_index(bs->groups, i);
        GPtrArray *row = g_ptr_array_new();
        g_ptr_array_add(row, gp->name->str);
        if (gp->master) {
            g_ptr_array_add(row, gp->master->addr->name->str);
        } else {
            g_ptr_array_add(row, "");
        }
        GString *slaves = g_string_new(0);
        network_group_get_slave_names(gp, slaves);
        g_ptr_array_add(row, slaves->str);
        free_list = g_list_append(free_list, slaves);
        g_ptr_array_add(rows, row);
    }
    network_mysqld_con_send_resultset(con->client, fields, rows);

    network_mysqld_proto_fielddefs_free(fields);
    g_ptr_array_free(rows, TRUE);
    g_list_free_full(free_list, g_string_true_free);
    return PROXY_SEND_RESULT;

}

static int admin_help(network_mysqld_con *con, const char *sql);

typedef int (*sql_handler_func) (network_mysqld_con *, const char *);

struct sql_handler_entry_t {
    const char *prefix;
    sql_handler_func func;
    const char *pattern;
    const char *desc;
};

static struct sql_handler_entry_t sql_handler_shard_map[] = {
    {"select conn_details from backend", admin_send_backend_detail_info,
     "select conn_details from backend", "display the idle conns"},
    {"select * from backends", admin_send_backends_info,
     "select * from backends", "list the backends and their state"},
    {"select * from groups", admin_send_group_info,
     "select * from groups","list the backends and their groups"},
    {"show connectionlist", admin_show_connectionlist,
     "show connectionlist [<num>]", "show <num> connections"},
    {"show allow_ip ", admin_show_allow_ip,
     "show allow_ip <module>", "show allow_ip rules of module, currently admin|shard"},
    {"show deny_ip ", admin_show_deny_ip,
     "show deny_ip <module>", "show deny_ip rules of module, currently admin|shard"},
    {"add allow_ip ", admin_add_allow_ip,
     "add allow_ip <module> <address>", "add address to white list of module"},
    {"add deny_ip ", admin_add_deny_ip,
     "add deny_ip <module> <address>", "add address to black list of module"},
    {"delete allow_ip ", admin_delete_allow_ip,
     "delete allow_ip <module> <address>", "delete address from white list of module"},
    {"delete deny_ip ", admin_delete_deny_ip,
     "delete deny_ip <module> <address>", "delete address from black list of module"},
    {"set reduce_conns ", admin_set_reduce_conns,
     "set reduce_conns (true|false)", "reduce idle connections if set to true"},
    {"reduce memory", admin_reduce_memory,
     "reduce memory", "reduce memory occupied by system"},
    {"set maintain ", admin_set_maintain,
     "set maintain (true|false)", "close all client connections if set to true"},
    {"show maintain status", admin_show_maintain,
          "show maintain status", "query whether cetus' status is maintain"},
    {"reload shard", admin_reload_shard,
     "reload shard", "reload sharding config from remote db"},
    {"show status", admin_show_status,
     "show status [like '%<pattern>%']", "show select/update/insert/delete statistics"},
    {"show variables", admin_show_variables,
     "show variables [like '%<pattern>%']","show configuration variables"},
    {"select version", admin_send_version,
     "select version", "cetus version"},
    {"select conn_num from backends where", admin_send_connection_stat,
     "select conn_num from backends where backend_ndx=<index> and user='<name>')",
     "display selected backend and its connection number"},
    {"select * from user_pwd", admin_send_user_password,
     "select * from user_pwd [where user='<name>']","display server username and password"},
    {"select * from app_user_pwd", admin_send_user_password,
     "select * from app_user_pwd [where user='<name>']","display client username and password"},
    {"update user_pwd set password", admin_update_user_password,
     "update user_pwd set password='xx' where user='<name>'","update server username and password"},
    {"update app_user_pwd set password", admin_update_user_password,
     "update app_user_pwd set password='xx' where user='<name>'","update client username and password"},
    {"delete from user_pwd where", admin_delete_user_password,
     "delete from user_pwd where user='<name>'","delete server username and password"},
    {"delete from app_user_pwd where", admin_delete_user_password,
     "delete from app_user_pwd where user='<name>'","delete client username and password"},
    {"insert into backends values", admin_insert_backend,
     "insert into backends values ('<ip:port@group>', '(ro|rw)', '<state>')",
     "add mysql instance to backends list"},
    {"update backends set", admin_update_backend,
     "update backends set (type|state)='<value>' where (backend_ndx=<index>|address='<ip:port>')",
     "update mysql instance type or state"},
    {"delete from backends", admin_delete_backend,
     "delete from backends where (backend_ndx=<index>|address='<ip:port>')",
     "set state of mysql instance to deleted"},
    {"remove backend ", admin_delete_backend,   /* TODO: unify */
     "remove backend where (backend_ndx=<index>|address='<ip:port>')",
     "set state of mysql instance to deleted"},
    {"add master", admin_add_backend, "add master '<ip:port@group>'","add master"},
    {"add slave", admin_add_backend, "add slave '<ip:port@group>'","add slave"},
    {"stats get", admin_get_stats, "stats get [<item>]", "show query statistics"},
    {"config get", admin_get_config, "config get [<item>]", "show config"},
    {"config set ", admin_set_config, "config set <key>=<value>","set config"},
    {"stats reset", admin_reset_stats, "stats reset", "reset query statistics"},
    {"save settings", admin_save_settings, "save settings", "save config file"},
    {"select * from help", admin_help, "select * from help", "show this help"},
    {"select help", admin_help, "select help", "show this help"},
    {"cetus", admin_send_status, "cetus", "show overall status of Cetus"},
    {NULL, NULL, NULL, NULL}
};

static struct sql_handler_entry_t sql_handler_rw_map[] = {
    {"select conn_details from backend", admin_send_backend_detail_info,
     "select conn_details from backend", "display the idle conns"},
    {"select * from backends", admin_send_backends_info,
     "select * from backends", "list the backends and their state"},
    {"show connectionlist", admin_show_connectionlist,
     "show connectionlist [<num>]", "show <num> connections"},
    {"show allow_ip ", admin_show_allow_ip,
     "show allow_ip <module>", "show allow_ip rules of module, currently admin|proxy"},
    {"show deny_ip ", admin_show_deny_ip,
     "show deny_ip <module>", "show deny_ip rules of module, currently admin|proxy"},
    {"add allow_ip ", admin_add_allow_ip,
     "add allow_ip <module> <address>", "add address to white list of module"},
    {"add deny_ip ", admin_add_deny_ip,
     "add deny_ip <module> <address>", "add address to black list of module"},
    {"delete allow_ip ", admin_delete_allow_ip,
     "delete allow_ip <module> <address>", "delete address from white list of module"},
    {"delete deny_ip ", admin_delete_deny_ip,
     "delete deny_ip <module> <address>", "delete address from black list of module"},
    {"set reduce_conns ", admin_set_reduce_conns,
     "set reduce_conns (true|false)", "reduce idle connections if set to true"},
    {"reduce memory", admin_reduce_memory,
     "reduce memory", "reduce memory occupied by system"},
    {"set maintain ", admin_set_maintain,
     "set maintain (true|false)", "close all client connections if set to true"},
    {"show maintain status", admin_show_maintain,
           "show maintain status", "query whether cetus' status is maintain"},
    {"show status", admin_show_status,
     "show status [like '%<pattern>%']", "show select/update/insert/delete statistics"},
    {"show variables", admin_show_variables,
     "show variables [like '%<pattern>%']","show configuration variables"},
    {"select version", admin_send_version,
     "select version", "cetus version"},
    {"select conn_num from backends where", admin_send_connection_stat,
     "select conn_num from backends where backend_ndx=<index> and user='<name>')",
     "display selected backend and its connection number"},
    {"select * from user_pwd", admin_send_user_password,
     "select * from user_pwd [where user='<name>']","display server username and password"},
    {"select * from app_user_pwd", admin_send_user_password,
     "select * from app_user_pwd [where user='<name>']","display client username and password"},
    {"update user_pwd set password", admin_update_user_password,
     "update user_pwd set password='xx' where user='<name>'","update server username and password"},
    {"update app_user_pwd set password", admin_update_user_password,
     "update app_user_pwd set password='xx' where user='<name>'","update client username and password"},
    {"delete from user_pwd where", admin_delete_user_password,
     "delete from user_pwd where user='<name>'","delete server username and password"},
    {"delete from app_user_pwd where", admin_delete_user_password,
     "delete from app_user_pwd where user='<name>'","delete client username and password"},
    {"insert into backends values", admin_insert_backend,
     "insert into backends values ('<ip:port>', '(ro|rw)', '<state>')",
     "add mysql instance to backends list"},
    {"update backends set", admin_update_backend,
     "update backends set (type|state)='<value>' where (backend_ndx=<index>|address='<ip:port>')",
     "update mysql instance type or state"},
    {"delete from backends", admin_delete_backend,
     "delete from backends where (backend_ndx=<index>|address='<ip:port>')",
     "set state of mysql instance to deleted"},
    {"remove backend ", admin_delete_backend,   /* TODO: unify */
     "remove backend where (backend_ndx=<index>|address='<ip:port>')",
     "set state of mysql instance to deleted"},
    {"add master", admin_add_backend, "add master '<ip:port>'","add master"},
    {"add slave", admin_add_backend, "add slave '<ip:port>'","add slave"},
    {"stats get", admin_get_stats, "stats get [<item>]", "show query statistics"},
    {"config get", admin_get_config, "config get [<item>]", "show config"},
    {"config set ", admin_set_config, "config set <key>=<value>","set config"},
    {"stats reset", admin_reset_stats, "stats reset", "reset query statistics"},
    {"save settings", admin_save_settings, "save settings", "save config file"},
    {"select * from help", admin_help, "select * from help", "show this help"},
    {"select help", admin_help, "select help", "show this help"},
    {"cetus", admin_send_status, "cetus", "show overall status of Cetus"},
    {NULL, NULL, NULL, NULL}
};

static int
admin_help(network_mysqld_con *con, const char *sql)
{
    GPtrArray *fields = network_mysqld_proto_fielddefs_new();
    MAKE_FIELD_DEF_2_COL(fields, "Command", "Description");
    GPtrArray *rows = g_ptr_array_new_with_free_func((void *)network_mysqld_mysql_field_row_free);

    chassis_plugin_config *config = con->config;
    struct sql_handler_entry_t *sql_handler_map = config->has_shard_plugin ? sql_handler_shard_map : sql_handler_rw_map;
    int i;
    for (i = 0; sql_handler_map[i].prefix; ++i) {
        struct sql_handler_entry_t *e = &(sql_handler_map[i]);
        char *pattern = e->pattern ? (char *)e->pattern : "";
        char *desc = e->desc ? (char *)e->desc : "";
        APPEND_ROW_2_COL(rows, pattern, desc);
    }

    network_mysqld_con_send_resultset(con->client, fields, rows);
    network_mysqld_proto_fielddefs_free(fields);
    g_ptr_array_free(rows, TRUE);
    return PROXY_SEND_RESULT;
}

static network_mysqld_stmt_ret
admin_process_query(network_mysqld_con *con)
{
    char command = -1;
    network_socket *recv_sock = con->client;
    GList *chunk = recv_sock->recv_queue->chunks->head;
    GString *packet = chunk->data;

    if (packet->len < NET_HEADER_SIZE) {
        /* packet too short */
        return PROXY_SEND_QUERY;
    }

    command = packet->str[NET_HEADER_SIZE + 0];

    if (COM_QUERY == command) {
        /* we need some more data after the COM_QUERY */
        if (packet->len < NET_HEADER_SIZE + 2)
            return PROXY_SEND_QUERY;

        /* LOAD DATA INFILE is nasty */
        if (packet->len - NET_HEADER_SIZE - 1 >= sizeof("LOAD ") - 1 &&
            !g_ascii_strncasecmp(packet->str + NET_HEADER_SIZE + 1, C("LOAD "))) {
            return PROXY_SEND_QUERY;
        }
    }

    g_string_assign_len(con->orig_sql, packet->str + (NET_HEADER_SIZE + 1), packet->len - (NET_HEADER_SIZE + 1));

    g_strstrip(con->orig_sql->str); /* strip leading and trailing spaces */
    strip_extra_spaces(con->orig_sql->str); /* replace multiple spaces with one */
    normalize_equal_sign(con->orig_sql->str);   /* remove spaces on the side of = */
    lower_identifiers(con->orig_sql->str);
    con->orig_sql->len = strlen(con->orig_sql->str);

    const char *sql = con->orig_sql->str;

    chassis_plugin_config *config = con->config;
    struct sql_handler_entry_t *sql_handler_map = config->has_shard_plugin ? sql_handler_shard_map : sql_handler_rw_map;
    int i;
    for (i = 0; sql_handler_map[i].prefix; ++i) {
        gchar *pos = NULL;
        if ((pos = strcasestr(sql, sql_handler_map[i].prefix))) {
            if(pos == sql) {
                return sql_handler_map[i].func(con, sql);
            }
        }
    }

    network_mysqld_con_send_error(con->client, C("request error, \"select * from help\" for usage"));
    return PROXY_SEND_RESULT;
}

/**
 * gets called after a query has been read
 */
NETWORK_MYSQLD_PLUGIN_PROTO(server_read_query)
{
    network_socket *recv_sock;
    network_mysqld_stmt_ret ret;

    gettimeofday(&(con->req_recv_time), NULL);

    recv_sock = con->client;

    if (recv_sock->recv_queue->chunks->length != 1) {
        g_message("%s: client-recv-queue-len = %d", G_STRLOC, recv_sock->recv_queue->chunks->length);
    }

    ret = admin_process_query(con);

    switch (ret) {
    case PROXY_NO_DECISION:
        network_mysqld_con_send_error(con->client, C("request error, \"select * from help\" for usage"));
        con->state = ST_SEND_QUERY_RESULT;
        break;
    case PROXY_SEND_RESULT:
        con->state = ST_SEND_QUERY_RESULT;
        break;
    default:
        network_mysqld_con_send_error(con->client, C("need a resultset + proxy.PROXY_SEND_RESULT, got something else"));

        con->state = ST_SEND_ERROR;
        break;
    }

    g_string_free(g_queue_pop_tail(recv_sock->recv_queue->chunks), TRUE);

    return NETWORK_SOCKET_SUCCESS;
}

NETWORK_MYSQLD_PLUGIN_PROTO(proxy_timeout)
{
    con->prev_state = con->state;
    con->state = ST_SEND_ERROR;

    return NETWORK_SOCKET_SUCCESS;
}


/**
 * cleanup the admin specific data on the current connection
 *
 * @return NETWORK_SOCKET_SUCCESS
 */
NETWORK_MYSQLD_PLUGIN_PROTO(admin_disconnect_client)
{
    /* private state is not used in admin-plugin */
    con->plugin_con_state = NULL;

    return NETWORK_SOCKET_SUCCESS;
}

static int
network_mysqld_server_connection_init(network_mysqld_con *con)
{
    con->plugins.con_init = server_con_init;

    con->plugins.con_read_auth = server_read_auth;

    con->plugins.con_read_query = server_read_query;

    con->plugins.con_timeout = proxy_timeout;

    con->plugins.con_cleanup = admin_disconnect_client;

    return 0;
}

chassis_plugin_config *config;

static chassis_plugin_config *
network_mysqld_admin_plugin_new(void)
{
    config = g_new0(chassis_plugin_config, 1);

    return config;
}

static void
network_mysqld_admin_plugin_free(chassis *chas, chassis_plugin_config *config)
{
    if (config->listen_con) {
        /* the socket will be freed by network_mysqld_free() */
    }

    if (config->address) {
        chassis_config_unregister_service(chas->config_manager, config->address);
        g_free(config->address);
    }
    if (g_sampling_timer) {
        evtimer_del(g_sampling_timer);
        g_free(g_sampling_timer);
        g_sampling_timer = NULL;
    }

    if (config->admin_username)
        g_free(config->admin_username);
    if (config->admin_password)
        g_free(config->admin_password);
    if (config->allow_ip)
        g_free(config->allow_ip);
    if (config->allow_ip_table)
        g_hash_table_destroy(config->allow_ip_table);
    if (config->deny_ip)
        g_free(config->deny_ip);
    if (config->deny_ip_table)
        g_hash_table_destroy(config->deny_ip_table);
    g_free(config);
}

gchar*
show_admin_address(gpointer param) {
    struct external_param *opt_param = (struct external_param *)param;
    gint opt_type = opt_param->opt_type;
    if(CAN_SHOW_OPTS_PROPERTY(opt_type)) {
        return g_strdup_printf("%s", config->address != NULL ? config->address: "NULL");
    }
    if(CAN_SAVE_OPTS_PROPERTY(opt_type)) {
        if(config->address != NULL) {
            return g_strdup_printf("%s", config->address);
        }
    }
    return NULL;
}

gchar*
show_admin_username(gpointer param) {
    struct external_param *opt_param = (struct external_param *)param;
    gint opt_type = opt_param->opt_type;
    if(CAN_SHOW_OPTS_PROPERTY(opt_type)) {
        return g_strdup_printf("%s", config->admin_username != NULL ? config->admin_username: "NULL");
    }
    if(CAN_SAVE_OPTS_PROPERTY(opt_type)) {
        if(config->admin_username != NULL) {
            return g_strdup_printf("%s", config->admin_username);
        }
    }
    return NULL;
}

gchar*
show_admin_password(gpointer param) {
    struct external_param *opt_param = (struct external_param *)param;
    gint opt_type = opt_param->opt_type;
    if(CAN_SHOW_OPTS_PROPERTY(opt_type)) {
        return g_strdup_printf("%s", config->admin_password != NULL ? config->admin_password: "NULL");
    }
    if(CAN_SAVE_OPTS_PROPERTY(opt_type)) {
        if(config->admin_password != NULL) {
            return g_strdup_printf("%s", config->admin_password);
        }
    }
    return NULL;
}

gchar*
show_admin_allow_ip(gpointer param) {
    struct external_param *opt_param = (struct external_param *)param;
    gchar *ret = NULL;
    gint opt_type = opt_param->opt_type;
    if(CAN_SAVE_OPTS_PROPERTY(opt_type)) {
        GString *free_str = g_string_new(NULL);
        GList *free_list = NULL;
        if(config && config->allow_ip_table && g_hash_table_size(config->allow_ip_table)) {
            free_list = g_hash_table_get_keys(config->allow_ip_table);
            GList *it = NULL;
            for(it = free_list; it; it=it->next) {
                free_str = g_string_append(free_str, it->data);
                free_str = g_string_append(free_str, ",");
            }
            if(free_str->len) {
                free_str->str[free_str->len - 1] = '\0';
                ret = g_strdup(free_str->str);
            }
        }
        if(free_str) {
            g_string_free(free_str, TRUE);
        }
        if(free_list) {
            g_list_free(free_list);
        }
    }
    return ret;
}

gchar*
show_admin_deny_ip(gpointer param) {
    struct external_param *opt_param = (struct external_param *)param;
    gchar *ret = NULL;
    gint opt_type = opt_param->opt_type;
    if(CAN_SAVE_OPTS_PROPERTY(opt_type)) {
        GString *free_str = g_string_new(NULL);
        GList *free_list = NULL;
        if(config && config->deny_ip_table && g_hash_table_size(config->deny_ip_table)) {
            free_list = g_hash_table_get_keys(config->deny_ip_table);
            GList *it = NULL;
            for(it = free_list; it; it=it->next) {
                free_str = g_string_append(free_str, it->data);
                free_str = g_string_append(free_str, ",");
            }
            if(free_str->len) {
                free_str->str[free_str->len - 1] = '\0';
                ret = g_strdup(free_str->str);
            }
        }
        if(free_str) {
            g_string_free(free_str, TRUE);
        }
        if(free_list) {
            g_list_free(free_list);
        }
    }
    return ret;
}

/**
 * add the proxy specific options to the cmdline interface
 */
static GList *
network_mysqld_admin_plugin_get_options(chassis_plugin_config *config)
{
    chassis_options_t opts = { 0 };

    chassis_options_add(&opts, "admin-address",
                        0, 0, OPTION_ARG_STRING, &(config->address),
                        "listening address:port of the admin-server (default: :4041)", "<host:port>",
                        NULL, show_admin_address, SHOW_OPTS_PROPERTY|SAVE_OPTS_PROPERTY);

    chassis_options_add(&opts, "admin-username",
                        0, 0, OPTION_ARG_STRING, &(config->admin_username), "username to allow to log in", "<string>",
                        NULL, show_admin_username, SHOW_OPTS_PROPERTY|SAVE_OPTS_PROPERTY);

    chassis_options_add(&opts, "admin-password",
                        0, 0, OPTION_ARG_STRING, &(config->admin_password), "password to allow to log in", "<string>",
                        NULL, show_admin_password, SHOW_OPTS_PROPERTY|SAVE_OPTS_PROPERTY);

    chassis_options_add(&opts, "admin-allow-ip",
                        0, 0, OPTION_ARG_STRING, &(config->allow_ip),
                        "ip address allowed to connect to admin", "<string>",
                        NULL, show_admin_allow_ip, SAVE_OPTS_PROPERTY);
    chassis_options_add(&opts, "admin-deny-ip",
                        0, 0, OPTION_ARG_STRING, &(config->deny_ip),
                        "ip address denyed to connect to admin", "<string>",
                        NULL, show_admin_deny_ip, SAVE_OPTS_PROPERTY);

    return opts.options;
}

/* ring buffer from: https://github.com/AndersKaloer/Ring-Buffer */
#define RING_BUFFER_SIZE 128    /* must be power of 2, !! index [0, 126] !! */
#define RING_BUFFER_MASK (RING_BUFFER_SIZE-1)
struct ring_buffer_t {
    int head;
    int tail;
    guint64 buffer[RING_BUFFER_SIZE];
};

static void
ring_buffer_add(struct ring_buffer_t *buffer, guint64 data)
{
    if (((buffer->head - buffer->tail) & RING_BUFFER_MASK) == RING_BUFFER_MASK)
        buffer->tail = ((buffer->tail + 1) & RING_BUFFER_MASK);
    buffer->buffer[buffer->head] = data;
    buffer->head = ((buffer->head + 1) & RING_BUFFER_MASK);
}

static guint64
ring_buffer_get(struct ring_buffer_t *buffer, int index)
{
    if (index >= ((buffer->head - buffer->tail) & RING_BUFFER_MASK))
        return 0;
    int data_index = ((buffer->tail + index) & RING_BUFFER_MASK);
    return buffer->buffer[data_index];
}

static struct ring_buffer_t g_sql_count = { 126, 0 };
static struct ring_buffer_t g_trx_count = { 126, 0 };

static void
calc_qps_average(char *buf, int len)
{
    const int MOST_RECENT = 126;
    guint64 c_now = ring_buffer_get(&g_sql_count, MOST_RECENT);
    guint64 c_1min = ring_buffer_get(&g_sql_count, MOST_RECENT - 6);
    guint64 c_5min = ring_buffer_get(&g_sql_count, MOST_RECENT - 6 * 5);
    guint64 c_15min = ring_buffer_get(&g_sql_count, MOST_RECENT - 6 * 15);
    snprintf(buf, len, "%.2f, %.2f, %.2f",
             (c_now - c_1min) / 60.0, (c_now - c_5min) / 300.0, (c_now - c_15min) / 900.0);
}

static void
calc_tps_average(char *buf, int len)
{
    const int MOST_RECENT = 126;
    guint64 c_now = ring_buffer_get(&g_trx_count, MOST_RECENT);
    guint64 c_1min = ring_buffer_get(&g_trx_count, MOST_RECENT - 6);
    guint64 c_5min = ring_buffer_get(&g_trx_count, MOST_RECENT - 6 * 5);
    guint64 c_15min = ring_buffer_get(&g_trx_count, MOST_RECENT - 6 * 15);
    snprintf(buf, len, "%.2f, %.2f, %.2f",
             (c_now - c_1min) / 60.0, (c_now - c_5min) / 300.0, (c_now - c_15min) / 900.0);
}

struct _timer_func_arg_t {
    chassis *chas;
    struct event *ev;
};

/* sample interval is 10-sec, 127 samples takes about 21-min */
static void
sql_stats_sampling_func(int fd, short what, void *arg)
{
    chassis *chas = arg;

    query_stats_t *stats = &(chas->query_stats);
    ring_buffer_add(&g_sql_count, stats->client_query.ro + stats->client_query.rw);
    ring_buffer_add(&g_trx_count, stats->xa_count);

    static struct timeval ten_sec = { 10, 0 };
    /* EV_PERSIST not work for libevent1.4, re-activate timer each time */
    chassis_event_add_with_timeout(chas, g_sampling_timer, &ten_sec);
}

/**
 * init the plugin with the parsed config
 */
static int
network_mysqld_admin_plugin_apply_config(chassis *chas, chassis_plugin_config *config)
{
    network_mysqld_con *con;
    network_socket *listen_sock;

    if (!config->address) {
        config->address = g_strdup(":4041");
    } else {
        chas->proxy_address = config->address;
        g_message("set proxy address for chassis:%s", config->address);
    }

    if (!config->admin_username) {
        g_critical("%s: --admin-username needs to be set", G_STRLOC);
        return -1;
    }
    if (!config->admin_password) {
        g_critical("%s: --admin-password needs to be set", G_STRLOC);
        return -1;
    }
    if (!g_strcmp0(config->admin_password, "")) {
        g_critical("%s: --admin-password cannot be empty", G_STRLOC);
        return -1;
    }
    g_message("%s:admin-server listening on port", G_STRLOC);
    GHashTable *allow_ip_table = NULL;
    if (config->allow_ip) {
        allow_ip_table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        char **ip_arr = g_strsplit(config->allow_ip, ",", -1);
        int i;
        for (i = 0; ip_arr[i]; i++) {
            g_hash_table_insert(allow_ip_table, g_strdup(ip_arr[i]), (void *)TRUE);
        }
        g_strfreev(ip_arr);
    }
    config->allow_ip_table = allow_ip_table;

    GHashTable *deny_ip_table = NULL;
    if (config->deny_ip) {
        deny_ip_table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        char **ip_arr = g_strsplit(config->deny_ip, ",", -1);
        int i;
        for (i = 0; ip_arr[i]; i++) {
            g_hash_table_insert(deny_ip_table, g_strdup(ip_arr[i]), (void *)TRUE);
        }
        g_strfreev(ip_arr);
    }
    config->deny_ip_table = deny_ip_table;

    g_message("%s:admin-server listening on port", G_STRLOC);

    /**
     * create a connection handle for the listen socket
     */
    con = network_mysqld_con_new();
    network_mysqld_add_connection(chas, con, TRUE);
    con->config = config;

    config->listen_con = con;

    listen_sock = network_socket_new();
    con->server = listen_sock;

    /*
     * set the plugin hooks as we want to apply them to the new
     * connections too later
     */
    network_mysqld_server_connection_init(con);

    g_message("%s:admin-server listening on port:%s", G_STRLOC, config->address);
    /* FIXME: network_socket_set_address() */
    if (0 != network_address_set_address(listen_sock->dst, config->address)) {
        return -1;
    }

    g_message("%s:admin-server listening on port", G_STRLOC);
    /* FIXME: network_socket_bind() */
    if (0 != network_socket_bind(listen_sock)) {
        return -1;
    }
    g_message("admin-server listening on port %s", config->address);

    /* set config->has_shard_plugin */
    config->has_shard_plugin = has_shard_plugin(chas->modules);

    /**
     * call network_mysqld_con_accept() with this connection when we are done
     */
    event_set(&(listen_sock->event), listen_sock->fd, EV_READ | EV_PERSIST, network_mysqld_con_accept, con);
    chassis_event_add(chas, &(listen_sock->event));

    chassis_config_register_service(chas->config_manager, config->address, "admin");

    /* EV_PERSIST not work for libevent 1.4 */
    g_sampling_timer = g_new0(struct event, 1);
    evtimer_set(g_sampling_timer, sql_stats_sampling_func, chas);
    struct timeval ten_sec = { 10, 0 };
    chassis_event_add_with_timeout(chas, g_sampling_timer, &ten_sec);
    return 0;
}

G_MODULE_EXPORT int
plugin_init(chassis_plugin *p)
{
    p->magic = CHASSIS_PLUGIN_MAGIC;
    p->name = g_strdup("admin");
    p->version = g_strdup(PLUGIN_VERSION);

    p->init = network_mysqld_admin_plugin_new;
    p->get_options = network_mysqld_admin_plugin_get_options;
    p->apply_config = network_mysqld_admin_plugin_apply_config;
    p->destroy = network_mysqld_admin_plugin_free;

    /* For allow_ip configs */
    p->allow_ip_get = network_mysqld_admin_plugin_allow_ip_get;
    p->allow_ip_add = network_mysqld_admin_plugin_allow_ip_add;
    p->allow_ip_del = network_mysqld_admin_plugin_allow_ip_del;

    /* For deny_ip configs */
    p->deny_ip_get = network_mysqld_admin_plugin_deny_ip_get;
    p->deny_ip_add = network_mysqld_admin_plugin_deny_ip_add;
    p->deny_ip_del = network_mysqld_admin_plugin_deny_ip_del;

    return 0;
}
