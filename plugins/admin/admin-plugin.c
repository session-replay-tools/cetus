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

#include "admin-plugin.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>

#include "cetus-users.h"
#include "cetus-util.h"
#include "cetus-variable.h"
#include "character-set.h"
#include "chassis-event.h"
#include "chassis-options.h"
#include "cetus-monitor.h"
#include "network-mysqld-packet.h"
#include "network-mysqld-proto.h"
#include "server-session.h"
#include "sys-pedantic.h"
#include "network-ssl.h"
#include "chassis-options-utils.h"

#include "admin-lexer.l.h"
#include "admin-parser.y.h"
#include "admin-commands.h"
#include "admin-stats.h"

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
    challenge->server_version_str = g_strdup("5.7 admin");
    challenge->server_version     = 50700;
    challenge->charset            = charset_get_number("utf8");
    challenge->capabilities = CETUS_DEFAULT_FLAGS & (~CLIENT_TRANSACTIONS);
#ifdef HAVE_OPENSSL
    if (chas->ssl) {
        challenge->capabilities |= CLIENT_SSL;
    }
#endif
    challenge->server_status      = SERVER_STATUS_AUTOCOMMIT;
    challenge->thread_id          = 1;

    /* generate a random challenge */
    network_mysqld_auth_challenge_set_challenge(challenge);

    packet = g_string_new(NULL);
    network_mysqld_proto_append_auth_challenge(packet, challenge);
    con->client->challenge = challenge;

    network_mysqld_queue_append(con->client, con->client->send_queue, S(packet));

    g_string_free(packet, TRUE);

    con->state = ST_SEND_HANDSHAKE;

    /* status code of parser */
    con->plugin_con_state = g_new0(int, 1);

    return NETWORK_SOCKET_SUCCESS;
}

NETWORK_MYSQLD_PLUGIN_PROTO(server_read_auth) {
    network_packet packet;
    network_socket *recv_sock, *send_sock;
    network_mysqld_auth_response *auth;
    GString *excepted_response;
    GString *hashed_pwd;

    recv_sock = con->client;
    send_sock = con->client;

    packet.data = g_queue_peek_head(recv_sock->recv_queue->chunks);
    if (packet.data == NULL) {
        g_critical("%s: packet.data is nil", G_STRLOC);
        return NETWORK_SOCKET_ERROR;
    }

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
        g_string_assign_len(con->client->response->auth_plugin_data, S(auth_data));
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
            (g_hash_table_lookup(con->config->allow_ip_table, client_ip)
             || g_hash_table_lookup(con->config->allow_ip_table, "*")
             || ip_range_lookup(con->config->allow_ip_table, client_ip))) {
            check_ip = FALSE;
        } else if (g_hash_table_size(con->config->deny_ip_table) != 0 &&
                   (g_hash_table_lookup(con->config->deny_ip_table, client_ip)
                    || g_hash_table_lookup(con->config->deny_ip_table, "*")
                    || ip_range_lookup(con->config->deny_ip_table, client_ip))) {
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

GList *network_mysqld_admin_plugin_allow_ip_get(chassis_plugin_config *config)
{
    if (config && config->allow_ip_table) {
        return g_hash_table_get_keys(config->allow_ip_table);
    }
    return NULL;
}

gboolean network_mysqld_admin_plugin_allow_ip_add(chassis_plugin_config *config, char *addr) {
    if (!config || !addr) return FALSE;
    if (!config->allow_ip_table) {
        config->allow_ip_table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    }
    gboolean success = FALSE;
    if (!g_hash_table_lookup(config->allow_ip_table, addr)) {
        g_hash_table_insert(config->allow_ip_table, g_strdup(addr), (void *) TRUE);
        success = TRUE;
    }
    return success;
}

gboolean network_mysqld_admin_plugin_allow_ip_del(chassis_plugin_config *config, char *addr) {
    if (!config || !addr || !config->allow_ip_table) return FALSE;
    return g_hash_table_remove(config->allow_ip_table, addr);
}

GList *network_mysqld_admin_plugin_deny_ip_get(chassis_plugin_config *config)
{
    if (config && config->deny_ip_table) {
        return g_hash_table_get_keys(config->deny_ip_table);
    }
    return NULL;
}

gboolean
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
network_mysqld_admin_plugin_deny_ip_del(chassis_plugin_config *config, char *addr)
{
    if (!config || !addr || !config->deny_ip_table)
        return FALSE;
    return g_hash_table_remove(config->deny_ip_table, addr);
}

void *adminParserAlloc(void *(*mallocProc)(size_t));
void adminParserFree(void*, void (*freeProc)(void*));
void adminParser(void*, int yymajor, token_t, void*);
void adminParserTrace(FILE*, char*);

static network_mysqld_stmt_ret admin_process_query(network_mysqld_con *con)
{
    network_socket *recv_sock = con->client;
    GList   *chunk  = recv_sock->recv_queue->chunks->head;
    GString *packet = chunk->data;

    if (packet->len < NET_HEADER_SIZE) {
        /* packet too short */
        return PROXY_SEND_QUERY;
    }

    char command = packet->str[NET_HEADER_SIZE + 0];

    if (COM_QUERY == command) {
        /* we need some more data after the COM_QUERY */
        if (packet->len < NET_HEADER_SIZE + 2) return PROXY_SEND_QUERY;
    }

    g_string_assign_len(con->orig_sql, packet->str + (NET_HEADER_SIZE + 1),
                        packet->len - (NET_HEADER_SIZE + 1));

    const char* sql = con->orig_sql->str;
    admin_clear_error(con);

    /* init lexer & parser */
    yyscan_t scanner;
    adminyylex_init(&scanner);
    YY_BUFFER_STATE buf_state = adminyy_scan_string(sql, scanner);
    void* parser = adminParserAlloc(malloc);
#if 0
    adminParserTrace(stdout, "---ParserTrace: ");
#endif
    int code;
    int last_parsed_token;
    token_t token;
    while ((code = adminyylex(scanner)) > 0) {
        token.z = adminyyget_text(scanner);
        token.n = adminyyget_leng(scanner);
        adminParser(parser, code, token, con);

        last_parsed_token = code;

        if (admin_get_error(con) != 0) {
            break;
        }
    }
    if (admin_get_error(con) == 0) {
        if (last_parsed_token != TK_SEMI) {
            adminParser(parser, TK_SEMI, token, con);
        }
        adminParser(parser, 0, token, con);
    }

    if (admin_get_error(con) != 0) {
        network_mysqld_con_send_error(con->client,
           C("syntax error, 'select help' for usage"));
    }

    /* free lexer & parser */
    adminParserFree(parser, free);
    adminyy_delete_buffer(buf_state, scanner);
    adminyylex_destroy(scanner);
    return PROXY_SEND_RESULT;
}

/**
 * gets called after a query has been read
 */
NETWORK_MYSQLD_PLUGIN_PROTO(server_read_query) {
    network_socket *recv_sock;
    network_mysqld_stmt_ret ret;

    gettimeofday(&(con->req_recv_time), NULL);

    con->is_admin_client = 1;
    recv_sock = con->client;

    if (recv_sock->recv_queue->chunks->length != 1) {
        g_message("%s: client-recv-queue-len = %d", G_STRLOC,
                  recv_sock->recv_queue->chunks->length);
    }

    ret = admin_process_query(con);

    switch (ret) {
    case PROXY_NO_DECISION:
        network_mysqld_con_send_error(con->client,
            C("request error, \"select * from help\" for usage"));
        con->state = ST_SEND_QUERY_RESULT;
        break;
    case PROXY_SEND_RESULT:
        con->state = ST_SEND_QUERY_RESULT;
        break;
    default:
        network_mysqld_con_send_error(con->client,
            C("network packet error, closing connection"));
        con->state = ST_SEND_ERROR;
        break;
    }

    g_string_free(g_queue_pop_tail(recv_sock->recv_queue->chunks), TRUE);

    return NETWORK_SOCKET_SUCCESS;
}

NETWORK_MYSQLD_PLUGIN_PROTO(server_timeout)
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
NETWORK_MYSQLD_PLUGIN_PROTO(admin_disconnect_client) {
    g_free(con->plugin_con_state);
    con->plugin_con_state = NULL;

    return NETWORK_SOCKET_SUCCESS;
}


static int network_mysqld_server_connection_init(network_mysqld_con *con) {
    con->plugins.con_init             = server_con_init;

    con->plugins.con_read_auth        = server_read_auth;

    con->plugins.con_read_query       = server_read_query;

    con->plugins.con_timeout          = server_timeout;

    con->plugins.con_cleanup          = admin_disconnect_client;

    return 0;
}

chassis_plugin_config *config;

static chassis_plugin_config *network_mysqld_admin_plugin_new(void)
{
    config = g_new0(chassis_plugin_config, 1);

    return config;
}

static void network_mysqld_admin_plugin_free(chassis *chas, chassis_plugin_config *config) {
    if (config->listen_con) {
        /* the socket will be freed by network_mysqld_free() */
    }

    if (config->address) {
        chassis_config_unregister_service(chas->config_manager, config->address);
        g_free(config->address);
    }

    g_free(config->admin_username);
    g_free(config->admin_password);
    g_free(config->allow_ip);
    if (config->allow_ip_table) g_hash_table_destroy(config->allow_ip_table);
    g_free(config->deny_ip);
    if (config->deny_ip_table) g_hash_table_destroy(config->deny_ip_table);

    if (config->admin_stats) admin_stats_free(config->admin_stats);

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


/**
 * init the plugin with the parsed config
 */
static int
network_mysqld_admin_plugin_apply_config(chassis *chas,
        chassis_plugin_config *config)
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
        g_critical("%s: --admin-username needs to be set",
                G_STRLOC);
        return -1;
    }
    if (!config->admin_password) {
        g_critical("%s: --admin-password needs to be set",
                G_STRLOC);
        return -1;
    }
    if (!g_strcmp0(config->admin_password, "")) {
        g_critical("%s: --admin-password cannot be empty",
                G_STRLOC);
        return -1;
    }
    GHashTable *allow_ip_table = NULL;
    if (config->allow_ip) {
        allow_ip_table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        char **ip_arr = g_strsplit(config->allow_ip, ",", -1);
        int i;
        for (i = 0; ip_arr[i]; i++) {
            g_hash_table_insert(allow_ip_table, g_strdup(ip_arr[i]), (void *) TRUE);
        }
        g_strfreev(ip_arr);
    }
    config->allow_ip_table = allow_ip_table;


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

    /* FIXME: network_socket_set_address() */
    if (0 != network_address_set_address(listen_sock->dst,
                config->address))
    {
        return -1;
    }

    /* FIXME: network_socket_bind() */
    if (0 != network_socket_bind(listen_sock)) {
        return -1;
    }
    g_message("admin-server listening on port %s", config->address);

    config->has_shard_plugin = has_shard_plugin(chas->modules);

    /**
     * call network_mysqld_con_accept() with this connection when we are done
     */
    event_set(&(listen_sock->event), listen_sock->fd,
            EV_READ|EV_PERSIST, network_mysqld_con_accept, con);
    chassis_event_add(chas, &(listen_sock->event));

    chassis_config_register_service(chas->config_manager, config->address, "admin");
    config->admin_stats = admin_stats_init(chas);
    return 0;
}

G_MODULE_EXPORT int plugin_init(chassis_plugin *p) {
    p->magic        = CHASSIS_PLUGIN_MAGIC;
    p->name         = g_strdup("admin");
    p->version		= g_strdup(PLUGIN_VERSION);

    p->init         = network_mysqld_admin_plugin_new;
    p->get_options  = network_mysqld_admin_plugin_get_options;
    p->apply_config = network_mysqld_admin_plugin_apply_config;
    p->destroy      = network_mysqld_admin_plugin_free;

    /* For allow_ip configs */
    p->allow_ip_get = network_mysqld_admin_plugin_allow_ip_get;
    p->allow_ip_add = network_mysqld_admin_plugin_allow_ip_add;
    p->allow_ip_del = network_mysqld_admin_plugin_allow_ip_del;

    return 0;
}
