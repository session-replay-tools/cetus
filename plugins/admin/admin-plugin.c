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
#include "cetus-channel.h"
#include "cetus-process-cycle.h"
#include "resultset_merge.h"
#include "cetus-acl.h"

#include "admin-lexer.l.h"
#include "admin-parser.y.h"
#include "admin-commands.h"
#include "admin-stats.h"

chassis_plugin_config *admin_config = NULL;

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
            if (network_ssl_create_connection(con->client, NETWORK_SSL_SERVER) == FALSE) {
                network_mysqld_con_send_error_full(con->client, C("SSL server failed"), 1045, "28000");
                network_mysqld_auth_response_free(auth);
                return NETWORK_SOCKET_ERROR;
            } else {
                g_string_free(g_queue_pop_tail(con->client->recv_queue->chunks), TRUE);
                con->state = ST_FRONT_SSL_HANDSHAKE;
                return NETWORK_SOCKET_SUCCESS;
            }
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
        gsize auth_data_len = packet.data->len - NET_HEADER_SIZE;
        GString *auth_data = g_string_sized_new(calculate_alloc_len(auth_data_len));
        network_mysqld_proto_get_gstr_len(&packet, auth_data_len, auth_data);
        g_string_assign_len(con->client->response->auth_plugin_data, S(auth_data));
        g_string_free(auth_data, TRUE);
        auth = con->client->response;
    }

    char **client_addr_arr = g_strsplit(con->client->src->name->str, ":", -1);
    char *client_ip = client_addr_arr[0];
    char *client_username = con->client->response->username->str;

    gboolean can_pass = cetus_acl_verify(con->srv->priv->acl, client_username, client_ip);
    if (!can_pass) {
        char *ip_err_msg = g_strdup_printf("Access denied for user '%s@%s'",
                                           client_username, client_ip);
        network_mysqld_con_send_error_full(recv_sock, L(ip_err_msg), 1045, "28000");
        g_free(ip_err_msg);
        g_strfreev(client_addr_arr);
        con->state = ST_SEND_ERROR;
        return NETWORK_SOCKET_SUCCESS;
    }

    g_strfreev(client_addr_arr);

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

    g_string_free(g_queue_pop_tail(recv_sock->recv_queue->chunks), TRUE);

    if (recv_sock->recv_queue->chunks->length > 0) {
        g_warning("%s: client-recv-queue-len = %d", G_STRLOC, recv_sock->recv_queue->chunks->length);
    }

    return NETWORK_SOCKET_SUCCESS;
}

void *adminParserAlloc(void *(*mallocProc)(size_t));
void adminParserFree(void*, void (*freeProc)(void*));
void adminParser(void*, int yymajor, token_t, void*);
void adminParserTrace(FILE*, char*);

static void
network_read_sql_resp(int G_GNUC_UNUSED fd, short events, void *user_data)
{
    network_mysqld_con *con = user_data;

    g_debug("%s: network_read_sql_resp, fd:%d", G_STRLOC, fd);
    cetus_channel_t  ch;

    /* read header first */
    int ret = cetus_read_channel(fd, &ch, sizeof(cetus_channel_t));

    g_debug("%s: cetus_read_channel channel, fd:%d", G_STRLOC, fd);

    if (ret == NETWORK_SOCKET_ERROR) {
        con->num_read_pending--;
        if  (con->num_read_pending > 0) {
            return;
        } else {
            con->srv->socketpair_mutex = 0;
            network_mysqld_con_send_error(con->client, C("internal error"));
            con->state = ST_SEND_QUERY_RESULT;
            network_mysqld_queue_reset(con->client);
            network_queue_clear(con->client->recv_queue);
            network_mysqld_con_handle(-1, 0, con);
            return;
        }
    } else if (ret == NETWORK_SOCKET_WAIT_FOR_EVENT) {
        return;
    }

    g_debug("%s: channel command: %u, need to read servers:%d",
            G_STRLOC, ch.basics.command, con->num_read_pending);

    if (ch.basics.command == CETUS_CMD_ADMIN_RESP) {
        con->num_read_pending--;
        int  unread_len = ch.admin_sql_resp_len;
        GString *raw_packet = g_string_sized_new(calculate_alloc_len(unread_len));
        unsigned char *p = raw_packet->str;
        time_t start = time(0);
        int timeout = 0;
        do {
            int len = recv(fd, p, unread_len, 0);
            if (len > 0) {
                g_debug("%s: resp_len:%d, len:%d, fd:%d",
                        G_STRLOC, unread_len, len, fd);
                unread_len = unread_len - len;
                p = p + len;
            } else if (len == 0) {
                g_critical("%s: broken socketpair, resp_len:%d, len:%d, fd:%d",
                        G_STRLOC, unread_len, len, fd);
                break;
            } else {
              time_t end = time(0);
              if ((end - start) >= 6) {
                g_warning("%s:timeout, resp_len:%d, len:%d, fd:%d, "
                          "con->num_read_pending:%d",
                          G_STRLOC, unread_len, len, fd, con->num_read_pending);
                timeout = 1;
                break;
              }
              usleep(100000);
            }
        } while (unread_len > 0);

        if (timeout) {
          g_critical("%s: admin sql response timeout", G_STRLOC);
          con->srv->socketpair_mutex = 0;
          network_mysqld_con_send_error(con->client, C("internal error"));
          con->state = ST_SEND_QUERY_RESULT;
          network_mysqld_queue_reset(con->client);
          network_queue_clear(con->client->recv_queue);
          network_mysqld_con_handle(-1, 0, con);
          return;
        } else {
          network_socket *sock = network_socket_new();
          g_queue_push_tail(sock->recv_queue_raw->chunks, raw_packet);

          sock->recv_queue_raw->len += ch.admin_sql_resp_len;
          raw_packet->len = ch.admin_sql_resp_len;

          network_socket_retval_t ret =
              network_mysqld_con_get_packet(con->srv, sock);

          while (ret == NETWORK_SOCKET_SUCCESS) {
            network_packet packet;
            GList *chunk;

            chunk = sock->recv_queue->chunks->tail;
            packet.data = chunk->data;
            packet.offset = 0;

            int is_finished =
                network_mysqld_proto_get_query_result(&packet, con);
            if (is_finished == 1) {
              g_debug("%s: read finished", G_STRLOC);
              break;
            }

            ret = network_mysqld_con_get_packet(con->srv, sock);
          }

          if (con->servers == NULL) {
            con->servers = g_ptr_array_new();
          }

          g_ptr_array_add(con->servers, sock);
        }

    } else {
        g_critical("%s: not admin sql response command", G_STRLOC);
        con->srv->socketpair_mutex = 0;
        network_mysqld_con_send_error(con->client, C("internal error"));
        con->state = ST_SEND_QUERY_RESULT;
        network_mysqld_queue_reset(con->client);
        network_queue_clear(con->client->recv_queue);
        network_mysqld_con_handle(-1, 0, con);
        return;
    }

    if (con->num_read_pending == 0) {
        con->srv->socketpair_mutex = 0;
        int len = con->servers->len;
        GPtrArray *recv_queues = g_ptr_array_sized_new(len);

        /* get all participants' receive queues */
        int i;
        for (i = 0; i < len; i++) {
            network_socket *worker = g_ptr_array_index(con->servers, i);
            g_ptr_array_add(recv_queues, worker->recv_queue);
        }

        GPtrArray *servers = con->servers;
        con->servers = NULL;
        result_merge_t result;
        result.status = RM_SUCCESS;
        result.detail = NULL;
        g_debug("%s: call admin_resultset_merge", G_STRLOC);
        admin_resultset_merge(con, con->client->send_queue, recv_queues, &result);
        g_debug("%s: call admin_resultset_merge end", G_STRLOC);
        con->state = ST_SEND_QUERY_RESULT;
        network_mysqld_queue_reset(con->client);
        network_queue_clear(con->client->recv_queue);

        for (i = 0; i < len; i++) {
            network_socket *worker = g_ptr_array_index(servers, i);
            network_socket_free(worker);
        }
        g_ptr_array_free(servers, TRUE);

        if (!con->data) {
            g_ptr_array_free(recv_queues, TRUE);
        }
        network_mysqld_con_handle(-1, 0, con);
    }
}


static 
int construct_channel_info(network_mysqld_con *con, char *sql)
{
    chassis *cycle = con->srv;
    g_debug("%s:call construct_channel_info, cetus_process_slot:%d", G_STRLOC, cetus_process_slot);
    cetus_channel_t  ch; 
    memset(&ch, 0, sizeof(cetus_channel_t));
    ch.basics.command = CETUS_CMD_ADMIN;
    ch.basics.pid = cetus_pid;
    ch.basics.slot = cetus_process_slot;

    con->num_read_pending = 0;

    int len = strlen(sql);
    if (len >= MAX_ADMIN_SQL_LEN) {
        g_message("%s:admin sql is too long:%d, sql:%s", G_STRLOC, len, sql);
        network_mysqld_con_send_error(con->client, C("admin sql is too long"));
        return -1;
    } else {
        strncpy(ch.admin_sql, sql, len);
        g_debug("%s:cetus_last_process:%d, ch admin sql:%s",
                G_STRLOC, cetus_last_process, ch.admin_sql);
        if (con->ask_the_given_worker) {
            int index = con->process_index;
            g_debug("%s: pass sql info to s:%i pid:%d to:%d", G_STRLOC,
                    ch.basics.slot, ch.basics.pid, cetus_processes[index].pid);
            ch.basics.fd = cetus_processes[index].parent_child_channel[0];
            /* TODO: AGAIN */
            cetus_write_channel(cetus_processes[index].parent_child_channel[0],
                    &ch, sizeof(cetus_channel_t));
            int fd = cetus_processes[index].parent_child_channel[0];
            g_debug("%s:fd:%d for network_read_sql_resp", G_STRLOC, fd);
            event_set(&(cetus_processes[index].event), fd, EV_READ, network_read_sql_resp, con);
            chassis_event_add_with_timeout(cycle, &(cetus_processes[index].event), NULL);
            con->num_read_pending++;
            g_debug("%s:con num_read_pending:%d", G_STRLOC, con->num_read_pending);
            return 0;
        }

        int num = cetus_last_process;
        int i;
        for (i = 0; i < num; i++) {
            g_debug("%s: pass sql info to s:%i pid:%d to:%d", G_STRLOC,
                    ch.basics.slot, ch.basics.pid, cetus_processes[i].pid);

            int fd = cetus_processes[i].parent_child_channel[0];
            if (fd > 0) {
                ch.basics.fd = cetus_processes[i].parent_child_channel[0];
                 /* TODO: AGAIN */
                cetus_write_channel(cetus_processes[i].parent_child_channel[0],
                        &ch, sizeof(cetus_channel_t));
                g_debug("%s:fd:%d for network_read_sql_resp", G_STRLOC, fd);
                event_set(&(cetus_processes[i].event), fd, EV_READ, network_read_sql_resp, con);
                chassis_event_add_with_timeout(cycle, &(cetus_processes[i].event), NULL);
                con->num_read_pending++;
                if (con->ask_one_worker) {
                    break;
                }
            } else {
                g_message("%s:fd is not valid:%d, num:%d, pending:%d", G_STRLOC, fd, num, con->num_read_pending);
            }
        }
        g_debug("%s:con num_read_pending:%d", G_STRLOC, con->num_read_pending);
        
        return 0;
    }
}

static void
network_send_admin_sql_to_workers(int G_GNUC_UNUSED fd, short what, void *user_data)
{
    network_mysqld_con *con = user_data;

    if  (con->srv->socketpair_mutex) {
        event_set(&con->client->event, con->client->fd, EV_TIMEOUT, network_send_admin_sql_to_workers, con);
        struct timeval timeout = {0, 10000};
        chassis_event_add_with_timeout(con->srv, &con->client->event, &timeout);
        return;
    }

    con->srv->socketpair_mutex = 1;
    if (construct_channel_info(con, con->orig_sql->str) == -1) {
        con->srv->socketpair_mutex = 0;
        con->state = ST_SEND_QUERY_RESULT;
        network_mysqld_queue_reset(con->client);
        network_queue_clear(con->client->recv_queue);
        network_mysqld_con_handle(-1, 0, con);
    }

    g_debug("%s: network_send_admin_sql_to_workers, fd:%d", G_STRLOC, fd);

}


static void visit_parser(network_mysqld_con *con, const char *sql) 
{
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
    int last_parsed_token = 0;
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
        g_message("%s:syntax error", G_STRLOC);
        network_mysqld_con_send_error(con->client,
           C("syntax error, 'select help' for usage"));
        if (con->is_processed_by_subordinate) {
            con->direct_answer = 1;
        }
    }

    /* free lexer & parser */
    adminParserFree(parser, free);
    adminyy_delete_buffer(buf_state, scanner);
    adminyylex_destroy(scanner);

}

NETWORK_MYSQLD_PLUGIN_PROTO(execute_admin_query)
{
    if (con->config == NULL) {
        con->config = admin_config;
    }
    char *sql = con->orig_sql->str;

    g_debug("%s:call execute_admin_query:%s", G_STRLOC, sql);

    visit_parser(con, sql);

    return NETWORK_SOCKET_SUCCESS;
}

static network_mysqld_stmt_ret admin_process_query(network_mysqld_con *con)
{
    network_socket *recv_sock = con->client;
    GList   *chunk  = recv_sock->recv_queue->chunks->head;
    GString *packet = chunk->data;

    if (packet->len < NET_HEADER_SIZE) {
        /* packet too short */
        return PROXY_SEND_QUERY;
    }

    guchar command = packet->str[NET_HEADER_SIZE + 0];

    if (COM_QUIT == command) {
        return PROXY_CLIENT_QUIT;
    } else if (COM_QUERY == command) {
        /* we need some more data after the COM_QUERY */
        if (packet->len < NET_HEADER_SIZE + 2) return PROXY_SEND_QUERY;
    }

    g_string_assign_len(con->orig_sql, packet->str + (NET_HEADER_SIZE + 1),
                        packet->len - (NET_HEADER_SIZE + 1));
    g_debug("%s:admin sql:%s", G_STRLOC, con->orig_sql->str);
    
    con->direct_answer = 0;
    con->ask_one_worker = 0;
    con->ask_the_given_worker = 0;
    con->admin_read_merge = 0;
    con->srv->candidate_config_changed = 0;

    visit_parser(con, con->orig_sql->str);
    if (con->srv->worker_processes == 0) {
        con->direct_answer = 1;
    }

    if (con->direct_answer) {
        return PROXY_SEND_RESULT;
    } else {
        if  (con->srv->socketpair_mutex) {
            event_set(&con->client->event, con->client->fd, EV_TIMEOUT, network_send_admin_sql_to_workers, con);
            struct timeval timeout = {0, 10000};
            chassis_event_add_with_timeout(con->srv, &con->client->event, &timeout);
            return PROXY_WAIT_QUERY_RESULT;
        }

        con->srv->socketpair_mutex = 1;
        if (construct_channel_info(con, con->orig_sql->str) == -1) {
            con->srv->socketpair_mutex = 0;
            return PROXY_SEND_RESULT;
        } else {
            return PROXY_WAIT_QUERY_RESULT;
        }
    }
}

/**
 * gets called after a query has been read
 */
NETWORK_MYSQLD_PLUGIN_PROTO(server_read_query) {
    g_debug("%s:call server_read_query", G_STRLOC);
    network_socket *recv_sock;
    network_mysqld_stmt_ret ret;

    gettimeofday(&(con->req_recv_time), NULL);

    con->is_admin_client = 1;

    if (con->srv->worker_processes == 0) {
        con->is_processed_by_subordinate = 0;
    } else {
        con->is_processed_by_subordinate = 1;
    }
    recv_sock = con->client;

    if (recv_sock->recv_queue->chunks->length != 1) {
        g_message("%s: client-recv-queue-len = %d", G_STRLOC,
                  recv_sock->recv_queue->chunks->length);
    }

    ret = admin_process_query(con);

    switch (ret) {
    case PROXY_WAIT_QUERY_RESULT:
        return NETWORK_SOCKET_WAIT_FOR_EVENT;
    case PROXY_NO_DECISION:
        network_mysqld_con_send_error(con->client,
            C("request error, \"select * from help\" for usage"));
        con->state = ST_SEND_QUERY_RESULT;
        break;
    case PROXY_SEND_RESULT:
        con->state = ST_SEND_QUERY_RESULT;
        break;
    case PROXY_CLIENT_QUIT:
        con->state = ST_CLIENT_QUIT;
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

    g_debug("%s:call server_timeout", G_STRLOC);

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

    con->plugins.con_exectute_sql     = execute_admin_query;

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
    g_free(config->deny_ip);

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
        GString* hashed_pwd = g_string_new(0);
        network_mysqld_proto_password_hash(hashed_pwd, L(config->admin_password));
        char* pwdhex = g_malloc0(hashed_pwd->len * 2 + 10);
        bytes_to_hex_str(hashed_pwd->str, hashed_pwd->len, pwdhex);
        g_string_free(hashed_pwd, TRUE);
        return pwdhex;
    }
    if(CAN_SAVE_OPTS_PROPERTY(opt_type)) {
        return g_strdup(config->admin_password);
    }
    return NULL;
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
                        NULL, NULL, SAVE_OPTS_PROPERTY);
    chassis_options_add(&opts, "admin-deny-ip",
                        0, 0, OPTION_ARG_STRING, &(config->deny_ip),
                        "ip address denyed to connect to admin", "<string>",
                        NULL, NULL, SAVE_OPTS_PROPERTY);

    return opts.options;
}

#define MAX_CMD_OR_PATH_LEN 108
static void remove_unix_socket_if_stale(chassis *chas)
{
    char command[MAX_CMD_OR_PATH_LEN];

    memset(command, 0, MAX_CMD_OR_PATH_LEN);

    sprintf(command, "netstat -npl|grep '%s'", chas->proxy_address);

    FILE *p = popen(command, "r");

    if (p)  {
        char result[256];
        int count = fread(result, 1, sizeof(result), p);

        if (count == 0) {
            g_message("%s:call unlink", G_STRLOC);
            /* no matter if it does not exist */
            unlink(chas->unix_socket_name);
        }
        pclose(p);
    }

}

static int
check_allowed_running(chassis *chas)
{
    char buffer[MAX_CMD_OR_PATH_LEN];

    if (strlen(chas->proxy_address) > (MAX_CMD_OR_PATH_LEN / 2)) {
        g_message("%s:ip:port string is too long", G_STRLOC);
        return -1;
    }

    memset(buffer, 0 ,MAX_CMD_OR_PATH_LEN);

    sprintf(buffer, "/tmp/%s", chas->proxy_address);
    chas->unix_socket_name = g_strdup(buffer);

    remove_unix_socket_if_stale(chas);

    int fd;

    if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
        g_message("%s:create socket error", G_STRLOC);
        return -1;
    }


    struct sockaddr_un  un;
    memset(&un, 0, sizeof(un));
    un.sun_family = AF_UNIX;
    const char *name  = chas->unix_socket_name;
    if (strlen(name) >= sizeof(un.sun_path)) {
        strncpy(un.sun_path, name, sizeof(un.sun_path) - 1);
    } else {
        strncpy(un.sun_path, name, strlen(name));
    }
    int len = offsetof(struct sockaddr_un, sun_path) + strlen(name);

    if (bind(fd, (struct sockaddr *)&un, len) < 0) {
        g_message("%s:already running", G_STRLOC);
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}


/**
 * init the plugin with the parsed config
 */
static int
network_mysqld_admin_plugin_apply_config(chassis *chas,
        chassis_plugin_config *config)
{
    if (config->listen_con) {
        g_message("%s:close listen socket", G_STRLOC);
        config->listen_con->server->event.ev_base = NULL;
        network_socket_free(config->listen_con->server);
        config->listen_con = NULL;
        return 0;
    }

    g_message("%s:call network_mysqld_admin_plugin_apply_config", G_STRLOC);
    network_mysqld_con *con;
    network_socket *listen_sock;

    if (!config->address) {
        config->address = g_strdup(":4041");
    }

    chas->proxy_address = config->address;
    g_message("set admin address for chassis:%s", config->address);

#if defined(SO_REUSEPORT)
    if (chas->enable_admin_listen) {
        if (check_allowed_running(chas) == -1) {
            return -1;
        }
    }
#endif

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

    if (config->allow_ip) {
        cetus_acl_add_rules(chas->priv->acl, ACL_WHITELIST, config->allow_ip);
    }

    if (config->deny_ip) {
        cetus_acl_add_rules(chas->priv->acl, ACL_BLACKLIST, config->deny_ip);
    }
    con = network_mysqld_con_new();
    con->config = config;
    network_mysqld_add_connection(chas, con, TRUE);

    /**
     * create a connection handle for the listen socket
     */
    if (chas->enable_admin_listen) {
        g_message("%s:enable_admin_listen true", G_STRLOC);
        config->listen_con = con;
        listen_sock = network_socket_new();
        con->server = listen_sock;
    }

    g_message("%s:before set hooks", G_STRLOC);

    /*
     * set the plugin hooks as we want to apply them to the new
     * connections too later
     */
    network_mysqld_server_connection_init(con);

    g_message("%s:after set hooks", G_STRLOC);

    if (chas->enable_admin_listen) {
        /* FIXME: network_socket_set_address() */
        if (0 != network_address_set_address(listen_sock->dst,
                    config->address))
        {
            return -1;
        }

        if (0 != network_socket_bind(listen_sock, 1)) {
            return -1;
        }

        /**
         * call network_mysqld_con_accept() with this connection when we are done
         */
        event_set(&(listen_sock->event), listen_sock->fd,
                EV_READ|EV_PERSIST, network_mysqld_con_accept, con);
        chassis_event_add(chas, &(listen_sock->event));
    }

    chas->admin_plugin = &(con->plugins);

    config->has_shard_plugin = has_shard_plugin(chas->modules);

    chassis_config_register_service(chas->config_manager, config->address, "admin");
    config->admin_stats = admin_stats_init(chas);

    admin_config = config;

    return 0;
}

static void 
network_mysqld_admin_plugin_stop_listening(chassis *chas,
        chassis_plugin_config *config)
{
    g_message("%s:call network_mysqld_admin_plugin_stop_listening", G_STRLOC);
    if (config->listen_con) {
        g_message("%s:close listen socket:%d", G_STRLOC, config->listen_con->server->fd);
        network_socket_free(config->listen_con->server);
        config->listen_con = NULL;

        int i;
        for (i = 0; i < chas->priv->cons->len; i++) {
            network_mysqld_con* con = g_ptr_array_index(chas->priv->cons, i);
            if (con->client) {
                g_message("%s:close socket:%d", G_STRLOC, con->client->fd);
                network_socket_free(con->client);
            }
        }
    }
}


G_MODULE_EXPORT int plugin_init(chassis_plugin *p) {
    p->magic        = CHASSIS_PLUGIN_MAGIC;
    p->name         = g_strdup("admin");
    p->version		= g_strdup(PLUGIN_VERSION);

    p->init         = network_mysqld_admin_plugin_new;
    p->get_options  = network_mysqld_admin_plugin_get_options;
    p->apply_config = network_mysqld_admin_plugin_apply_config;
    p->stop_listening = network_mysqld_admin_plugin_stop_listening;
    p->destroy      = network_mysqld_admin_plugin_free;

    return 0;
}
