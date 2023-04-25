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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_SYS_FILIO_H
#include <sys/filio.h>
#endif

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include <errno.h>

#include <glib.h>

#ifndef HAVE_SOCKLEN_T
typedef int socklen_t;
#endif

#include <mysqld_error.h> /** for ER_UNKNOWN_ERROR */

#include "network-mysqld.h"
#include "network-mysqld-proto.h"
#include "network-mysqld-packet.h"

#include "network-conn-pool.h"
#include "network-conn-pool-wrap.h"

#include "sys-pedantic.h"
#include "network-injection.h"
#include "network-backend.h"
#include "sql-context.h"
#include "sql-filter-variables.h"
#include "glib-ext.h"
#include "chassis-timings.h"
#include "chassis-event.h"
#include "character-set.h"
#include "cetus-util.h"
#include "cetus-users.h"
#include "plugin-common.h"
#include "chassis-options.h"
#include "chassis-options-utils.h"
#include "chassis-sql-log.h"
#include "cetus-acl.h"

#ifndef PLUGIN_VERSION
#ifdef CHASSIS_BUILD_TAG
#define PLUGIN_VERSION CHASSIS_BUILD_TAG
#else
#define PLUGIN_VERSION PACKAGE_VERSION
#endif
#endif

typedef enum {
    PROXY_QUEUE_ADD_PREPEND,
    PROXY_QUEUE_ADD_APPEND
} proxy_queue_add_t;

typedef enum {
    INJ_ID_COM_DEFAULT = 1,
    INJ_ID_CHANGE_DB,
    INJ_ID_COM_QUERY,
    INJ_ID_COM_STMT_PREPARE,
    INJ_ID_CHAR_SET_CLT,
    INJ_ID_CHAR_SET_CONN,
    INJ_ID_CHAR_SET_RESULTS,
    INJ_ID_SET_NAMES,
    INJ_ID_CHANGE_MULTI_STMT,
    INJ_ID_CHANGE_SQL_MODE,
    INJ_ID_CHANGE_USER,
    INJ_ID_RESET_CONNECTION,
} proxy_inj_id_t;

struct chassis_plugin_config {
    /**< listening address of the proxy */
    gchar *address;

    /**< read-write backends */
    gchar **backend_addresses;

    /**< read-only  backends */
    gchar **read_only_backend_addresses;

    network_mysqld_con *listen_con;

    /* exposed in the config as double */
    gdouble connect_timeout_dbl;
    /* exposed in the config as double */
    gdouble read_timeout_dbl;
    /* exposed in the config as double */
    gdouble write_timeout_dbl;

    gchar *allow_ip;

    gchar *deny_ip;

    int read_master_percentage;
};

static gboolean proxy_get_backend_ndx(network_mysqld_con *con, int type, gboolean force_slave);

void
g_fast_stream_hexdump(const char *msg, const void *_s, size_t len)
{
    GString *hex;
    size_t i;
    const unsigned char *s = _s;

    hex = g_string_new(NULL);

    for (i = 0; i < len; i++) {
        if (i % 16 == 0) {
            g_string_append_printf(hex, "[%04" G_GSIZE_MODIFIER "x]  ", i);
        }
        g_string_append_printf(hex, "%02x", s[i]);

        if ((i + 1) % 16 == 0) {
            size_t j;
            g_string_append_len(hex, C("  "));
            for (j = i - 15; j <= i; j++) {
                g_string_append_c(hex, g_ascii_isprint(s[j]) ? s[j] : '.');
            }
            g_string_append_len(hex, C("\n  "));
        } else {
            g_string_append_c(hex, ' ');
        }
    }

    if (i % 16 != 0) {
        /* fill up the line */
        size_t j;

        for (j = 0; j < 16 - (i % 16); j++) {
            g_string_append_len(hex, C("   "));
        }

        g_string_append_len(hex, C(" "));
        for (j = i - (len % 16); j < i; j++) {
            g_string_append_c(hex, g_ascii_isprint(s[j]) ? s[j] : '.');
        }
    }

    g_warning("(%s) %" G_GSIZE_FORMAT " bytes:\n  %s", msg, len, hex->str);

    g_string_free(hex, TRUE);
}

/**
 * handle event-timeouts on the different states
 *
 * @note con->state points to the current state
 *
 */
NETWORK_MYSQLD_PLUGIN_PROTO(proxy_timeout)
{
    proxy_plugin_con_t *st = con->plugin_con_state;
    if (st == NULL)
        return NETWORK_SOCKET_ERROR;

    int diff = con->srv->current_time - con->client->update_time + 1;
    int idle_timeout = con->srv->client_idle_timeout;

    if (con->is_in_transaction) {
        idle_timeout = con->srv->incomplete_tran_idle_timeout;
    }

    if (con->srv->maintain_close_mode) {
        idle_timeout = con->srv->maintained_client_idle_timeout;
    }

    g_debug("%s, con:%p:call proxy_timeout, idle timeout:%d, diff:%d", 
            G_STRLOC, con, idle_timeout, diff);

    switch (con->state) {
    case ST_READ_QUERY:
        if (diff < idle_timeout) {
            if (con->server && !con->client->is_server_conn_reserved) {
                if (network_pool_add_conn(con, 0) != 0) {
                    g_debug("%s, con:%p:conn to pool failed", G_STRLOC, con);
                } else {
                    g_debug("%s, con:%p:conn returned to pool", G_STRLOC, con);
                }
            }
        } else {
            con->prev_state = con->state;
            con->state = ST_ERROR;
            g_debug("%s, con:%p:state is set ST_ERROR", G_STRLOC, con);
        }
        break;
    case ST_READ_QUERY_RESULT:
        if (con->server && !con->client->is_server_conn_reserved) {
            con->server_to_be_closed = 1;
            if (!con->resultset_is_needed && con->candidate_fast_streamed) {
                g_critical("%s, fast_stream_last_exec_index:%d, need more:%d", G_STRLOC, 
                        con->fast_stream_last_exec_index, con->fast_stream_need_more);
                g_critical("%s, eof_last_met:%d, eof_met_cnt:%d", G_STRLOC, 
                        con->eof_last_met, con->eof_met_cnt);
                g_critical("%s, partically_record_left_cnt:%d, analysis_next_pos:%d, cur_resp_len:%d", 
                        G_STRLOC, con->partically_record_left_cnt, (int) con->analysis_next_pos, (int) con->cur_resp_len);
                g_critical("%s, last_payload_len:%d, last_record_payload_len:%d", G_STRLOC, 
                        con->last_payload_len, con->last_record_payload_len);
                g_fast_stream_hexdump(G_STRLOC, con->record_last_payload, con->last_record_payload_len);
            }
            g_critical("%s, con:%p read query result timeout, sql:%s", G_STRLOC, con, con->orig_sql->str);

            network_mysqld_con_send_error_full(con->client,
                    C("Read query result timeout"), ER_ABORTING_CONNECTION, "29001");
            con->prev_state = con->state;
            con->state = ST_SEND_ERROR;
            break;
        }
    default:
        if (diff >= idle_timeout) {
            con->prev_state = con->state;
            con->state = ST_SEND_ERROR;
        }
        break;
    }
    return NETWORK_SOCKET_SUCCESS;
}

static int
store_server_ndx_in_prepared_resp(proxy_resultset_t *res, int index)
{
    network_packet packet;
    GString s;
    int err = 0;
    GString *tmp;
    tmp = res->result_queue->head->data;
    s.str = tmp->str + 4;       /* skip the network-header */
    s.len = tmp->len - 4;
    packet.data = &s;
    packet.offset = 0;

    err = network_mysqld_proto_change_stmt_id_from_ok_packet(&packet, index);
    if (err) {
        return -1;
    }

    return 0;
}

static network_mysqld_stmt_ret
proxy_c_read_query_result(network_mysqld_con *con)
{
    network_socket *send_sock = con->client;
    network_socket *recv_sock = con->server;
    injection *inj = NULL;
    proxy_plugin_con_t *st = con->plugin_con_state;
    network_mysqld_stmt_ret ret = PROXY_NO_DECISION;

    if (0 == st->injected.queries->length)
        return PROXY_NO_DECISION;

    inj = g_queue_pop_head(st->injected.queries);

    inj->result_queue = con->server->recv_queue->chunks;

    /* fields, rows */
    proxy_resultset_t *res;

    res = proxy_resultset_new();

    if (inj->resultset_is_needed && !inj->qstat.binary_encoded) {
        res->result_queue = inj->result_queue;
    }
    res->qstat = inj->qstat;
    res->rows = inj->rows;
    res->bytes = inj->bytes;

    gboolean is_continue = FALSE;

    g_debug("%s: check inj id:%d", G_STRLOC, inj->id);

    switch (inj->id) {
    case INJ_ID_COM_DEFAULT:
    case INJ_ID_COM_QUERY:
    case INJ_ID_COM_STMT_PREPARE:
        is_continue = TRUE;
        break;
    case INJ_ID_RESET_CONNECTION:
        ret = PROXY_IGNORE_RESULT;
        break;
    case INJ_ID_CHANGE_USER:
        if (con->is_changed_user_failed) {
            g_warning("%s: change user failed for user '%s'@'%s'", G_STRLOC,
                      con->client->response->username->str, con->client->src->name->str);
            network_mysqld_con_send_error_full(con->client,
                                               C("Access denied for serving requests"), ER_ACCESS_DENIED_ERROR,
                                               "29001");

            network_queue_clear(recv_sock->recv_queue);
            network_queue_clear(con->client->recv_queue);
            network_mysqld_queue_reset(con->client);
            ret = PROXY_NO_DECISION;
        } else {
            g_string_assign_len(con->server->response->username, S(con->client->response->username));
            ret = PROXY_IGNORE_RESULT;
        }
        break;
    case INJ_ID_CHANGE_DB: {
        network_mysqld_com_query_result_t *query = con->parse.data;
        if (query && query->query_status == MYSQLD_PACKET_OK) {
            g_string_truncate(con->server->default_db, 0);
            g_string_append_len(con->server->default_db, S(con->client->default_db));
            g_debug("%s: set server db to client db for con:%p", G_STRLOC, con);
        }
        ret = PROXY_IGNORE_RESULT;
        break;
    }
    default:
        ret = PROXY_IGNORE_RESULT;
        break;
    }

    if (inj->id > INJ_ID_COM_STMT_PREPARE && inj->id < INJ_ID_RESET_CONNECTION) {
        if (res->qstat.query_status == MYSQLD_PACKET_ERR) {
            con->resp_err_met = 1;
        }
    }

    if (is_continue) {
        if (res->qstat.query_status) {
            if (con->is_in_transaction) {
                con->is_changed_user_when_quit = 0;
            }
        } else {
            g_debug("%s: check is_in_transaction here:%p", G_STRLOC, con);
            if (inj->id != INJ_ID_COM_STMT_PREPARE) {
                if (res->qstat.server_status & SERVER_STATUS_IN_TRANS) {
                    if (recv_sock->is_read_only) {
                        g_message("%s: SERVER_STATUS_IN_TRANS true from read server", G_STRLOC);
                    } else {
                        con->is_in_transaction = 1;
                        g_debug("%s: set is_in_transaction true for con:%p", G_STRLOC, con);
                    }
                } else {
                    con->is_in_transaction = 0;
                    g_debug("%s: set is_in_transaction false for con:%p", G_STRLOC, con);
                }

                if (!con->is_in_transaction) {
                    if (!con->is_auto_commit) {
                        con->is_in_transaction = 1;
                        con->client->is_server_conn_reserved = 1;
                        g_debug("%s: set is_in_transaction true:%p", G_STRLOC, con);
                    } else {
                        if (con->is_calc_found_rows) {
                            con->client->is_server_conn_reserved = 1;
                            g_debug("%s: set is_server_conn_reserved true for con:%p", G_STRLOC, con);
                        } else {
                            if (!con->is_prepared && !con->is_in_sess_context && !con->last_warning_met) {
                                con->client->is_server_conn_reserved = 0;
                                g_debug("%s: set is_server_conn_reserved false", G_STRLOC);
                            } else {
                                con->client->is_server_conn_reserved = 1;
                                g_debug("%s: set is_server_conn_reserved true", G_STRLOC);
                            }
                        }
                    }
                } else {
                    con->client->is_server_conn_reserved = 1;
                    g_debug("%s: is_in_transaction true:%p", G_STRLOC, con);
                }
            }
        }

        g_debug("%s: con multiple_server_mode:%d", G_STRLOC, con->multiple_server_mode);
        if (con->multiple_server_mode) {
            if (inj->id == INJ_ID_COM_STMT_PREPARE) {
                if (st->backend_ndx >= 0 && st->backend_ndx_array != NULL) {
                    int index = st->backend_ndx_array[st->backend_ndx] - 1;
                    store_server_ndx_in_prepared_resp(res, index);
                }
            }
        }
    }

    switch (ret) {
    case PROXY_NO_DECISION:
        g_debug("%s: PROXY_NO_DECISION here", G_STRLOC);
        if (!st->injected.sent_resultset) {
                /**
                 * make sure we send only one result-set per client-query
                 */
            if (!con->is_changed_user_failed) {
                if (g_queue_is_empty(send_sock->send_queue->chunks)) {
                    g_debug("%s: exchange queue", G_STRLOC);
                    network_queue *queue = con->client->send_queue;
                    con->client->send_queue = con->server->recv_queue;
                    con->server->recv_queue = queue;
                    GString *packet = g_queue_peek_tail(con->client->send_queue->chunks);
                    if (packet) {
                        con->client->last_packet_id = network_mysqld_proto_get_packet_id(packet);
                    } else {
                        g_message("%s: packet is nil", G_STRLOC);
                    }
                } else {
                    g_debug("%s: client send queue is not empty", G_STRLOC);
                    GString *packet;
                    while ((packet = g_queue_pop_head(con->server->recv_queue->chunks)) != NULL) {
                        network_mysqld_queue_append_raw(con->client, con->client->send_queue, packet);
                    }
                }
            }
            st->injected.sent_resultset++;
            break;
        }

        st->injected.sent_resultset++;

        /* fall through */
    case PROXY_IGNORE_RESULT:
        /* trash the packets for the injection query */

        if (!con->resultset_is_needed) {
            break;
        }
        network_queue_clear(recv_sock->recv_queue);
        break;
    default:
        network_queue_clear(send_sock->send_queue);
        break;
    }

    proxy_resultset_free(res);
    injection_free(inj);

    return ret;
}

NETWORK_MYSQLD_PLUGIN_PROTO(proxy_read_auth)
{
    return do_read_auth(con);
}

static int
network_mysqld_con_handle_insert_id_response(network_mysqld_con *con, char *name, int last_packet_id)
{
    char buffer[16];
    GPtrArray *fields;
    GPtrArray *rows;

    fields = network_mysqld_proto_fielddefs_new();

    MYSQL_FIELD *field;
    field = network_mysqld_proto_fielddef_new();
    field->name = name;
    field->type = MYSQL_TYPE_LONGLONG;
    g_ptr_array_add(fields, field);

    rows = g_ptr_array_new_with_free_func((void *)network_mysqld_mysql_field_row_free);

    GPtrArray *row = g_ptr_array_new();
    snprintf(buffer, sizeof(buffer), "%d", last_packet_id);
    g_ptr_array_add(row, buffer);
    g_ptr_array_add(rows, row);

    network_mysqld_con_send_resultset(con->client, fields, rows);

    network_mysqld_proto_fielddefs_free(fields);
    g_ptr_array_free(rows, TRUE);
    return 0;
}

static int
process_non_trans_prepare_stmt(network_mysqld_con *con)
{
  con->is_prepared = 1;
  proxy_plugin_con_t *st = con->plugin_con_state;
  sql_context_t *context = st->sql_context;

  gboolean is_orig_ro_server = FALSE;
  if (con->server != NULL) {
    if (st->backend && st->backend->type == BACKEND_TYPE_RO) {
      is_orig_ro_server = TRUE;
    }
  }

  g_debug("%s: call process_non_trans_prepare_stmt", G_STRLOC);
  if (!con->srv->master_preferred && context->stmt_type == STMT_SELECT) {
    if (con->prepare_stmt_count == 0 && !is_orig_ro_server) {
      /* use rw server */
      int type = BACKEND_TYPE_RW;
      if (!proxy_get_backend_ndx(con, type, FALSE)) {
        con->master_conn_shortaged = 1;
      }
    } else {
      if (con->server) {
        proxy_plugin_con_t *st = con->plugin_con_state;
        if (st->backend->state != BACKEND_STATE_UP &&
            st->backend->state != BACKEND_STATE_UNKNOWN) {
          g_debug("%s: slave down,move to master", G_STRLOC);
        }
      } else {
        g_warning("%s: original server null", G_STRLOC);
      }
    }
  }

  if (is_orig_ro_server) {
    int type = BACKEND_TYPE_RW;
    if (!proxy_get_backend_ndx(con, type, FALSE)) {
      con->master_conn_shortaged = 1;
      g_debug("%s:PROXY_NO_CONNECTION", G_STRLOC);
      /* no master connection */
      return PROXY_NO_CONNECTION;
    }
  }

  return PROXY_NO_DECISION;
}

static int
process_other_set_command(network_mysqld_con *con, const char *key, const char *s, mysqld_query_attr_t *query_attr)
{
    g_debug("%s: vist process_other_set_command", G_STRLOC);
    network_socket *sock = con->client;
    size_t s_len = strlen(s);

    if (strcasecmp(key, "character_set_client") == 0) {
        g_string_assign_len(sock->charset_client, s, s_len);
        query_attr->charset_client_set = 1;
    } else if (strcasecmp(key, "character_set_connection") == 0) {
        g_string_assign_len(sock->charset_connection, s, s_len);
        query_attr->charset_connection_set = 1;
    } else if (strcasecmp(key, "character_set_results") == 0) {
        g_string_assign_len(sock->charset_results, s, s_len);
        query_attr->charset_results_set = 1;
    } else if (strcasecmp(key, "sql_mode") == 0) {
        g_string_assign_len(sock->sql_mode, s, s_len);
        query_attr->sql_mode_set = 1;
    }
    return 0;
}

static int
process_set_names(network_mysqld_con *con, char *s, mysqld_query_attr_t *query_attr)
{
    network_socket *sock = con->client;
    size_t s_len = strlen(s);
    g_string_assign_len(sock->charset, s, s_len);
    g_string_assign_len(sock->charset_client, s, s_len);
    g_string_assign_len(sock->charset_connection, s, s_len);
    g_string_assign_len(sock->charset_results, s, s_len);

    query_attr->charset_client_set = 1;
    query_attr->charset_connection_set = 1;
    query_attr->charset_results_set = 1;
    query_attr->charset_set = 1;
    sock->charset_code = charset_get_number(s);
    if (s && strcasecmp(s, con->srv->default_charset) != 0 && sock->charset_code == DEFAULT_CHARSET) {
        g_warning("%s: charset code:%d, charset:%s", G_STRLOC, sock->charset_code, s == NULL ? "NULL":s);
    }
    return 0;
}

static int
process_trans_query(network_mysqld_con *con)
{
    proxy_plugin_con_t *st = con->plugin_con_state;
    sql_context_t *context = st->sql_context;
    g_debug("%s: visit process_trans_query here:%d", G_STRLOC, context->stmt_type);
    switch (context->stmt_type) {
    case STMT_SET:
        if (sql_context_is_autocommit_off(context)) {
            con->is_auto_commit = 0;
            g_debug("%s: autocommit off", G_STRLOC);
        } else if (sql_context_is_autocommit_on(context)) {
            if (con->is_in_transaction) {
                con->server_in_tran_and_auto_commit_received = 1;
                if (con->multiple_server_mode) {
                  if (!proxy_get_backend_ndx(con, BACKEND_TYPE_RW, FALSE)) {
                    g_critical("%s:serious error when change from slave to master", G_STRLOC);
                    return PROXY_NO_CONNECTION;
                  }
               }
            }
            con->is_auto_commit = 1;
            con->is_auto_commit_trans_buffered = 0;
            g_debug("%s: autocommit on", G_STRLOC);
        }
        break;
    default:
        break;
    }

    return PROXY_NO_DECISION;
}

static int
process_filter_for_trans_query(network_mysqld_con *con, sql_context_t *context, mysqld_query_attr_t *query_attr)
{
    gboolean is_orig_ro_server = FALSE;
    gboolean need_to_visit_master = FALSE;
    proxy_plugin_con_t *st = con->plugin_con_state;

    switch (context->stmt_type) {
    case STMT_SET_NAMES:{
        char *charset_name = (char *)context->sql_statement;
        process_set_names(con, charset_name, query_attr);
        break;
    }
    case STMT_SET:{
        sql_expr_list_t *set_list = context->sql_statement;
        if (set_list && set_list->len > 0) {
            sql_expr_t *expr = g_ptr_array_index(set_list, 0);
            if (expr && expr->op == TK_EQ) {
                sql_expr_t *left = expr->left;
                sql_expr_t *right = expr->right;
                if (!left || !right)
                    break;

                if (sql_filter_vars_is_silent(left->token_text, right->token_text)) {
                    network_mysqld_con_send_ok(con->client);
                    g_message("silent variable: %s", left->token_text);
                    return PROXY_SEND_RESULT;
                }
            }
        }
        break;
    }
    default:
        break;
    }

    return PROXY_NO_DECISION;
}


static int
process_non_trans_query(network_mysqld_con *con, sql_context_t *context, mysqld_query_attr_t *query_attr)
{
    gboolean is_orig_ro_server = FALSE;
    gboolean need_to_visit_master = FALSE;
    proxy_plugin_con_t *st = con->plugin_con_state;

    if (con->server != NULL) {
        if (st->backend && st->backend->type == BACKEND_TYPE_RO) {
            is_orig_ro_server = TRUE;
        }
    }

    switch (context->stmt_type) {
    case STMT_SELECT:{
        sql_select_t *select = (sql_select_t *)context->sql_statement;

        con->is_calc_found_rows = (select->flags & SF_CALC_FOUND_ROWS) ? 1 : 0;
        g_debug(G_STRLOC ": is_calc_found_rows: %d", con->is_calc_found_rows);

        char *last_insert_id_name = NULL;
        gboolean is_insert_id = FALSE;
        sql_expr_list_t *cols = select->columns;
        int i;
        for (i = 0; cols && i < cols->len; ++i) {
            sql_expr_t *col = g_ptr_array_index(cols, i);
            if (sql_expr_is_function(col, "LAST_INSERT_ID")) {
                is_insert_id = TRUE;
                last_insert_id_name = "LAST_INSERT_ID()";
            } else if (sql_expr_is_id(col, "LAST_INSERT_ID")) {
                is_insert_id = TRUE;
                last_insert_id_name = "@@LAST_INSERT_ID";
            }
        }
        if (is_insert_id == TRUE) {
            g_debug("%s: buffered last insert id:%d", G_STRLOC, (int)con->last_insert_id);
            network_mysqld_con_handle_insert_id_response(con, last_insert_id_name, con->last_insert_id);
            return PROXY_SEND_RESULT;
        }

        if (con->last_record_updated) {
            need_to_visit_master = TRUE;
        }

        break;
    }
    case STMT_SET_NAMES:{
        char *charset_name = (char *)context->sql_statement;
        process_set_names(con, charset_name, query_attr);
        break;
    }
    case STMT_SET:{
        sql_expr_list_t *set_list = context->sql_statement;
        if (set_list && set_list->len > 0) {
            sql_expr_t *expr = g_ptr_array_index(set_list, 0);
            if (expr && expr->op == TK_EQ) {
                sql_expr_t *left = expr->left;
                sql_expr_t *right = expr->right;
                if (!left || !right)
                    break;

                if (sql_filter_vars_is_silent(left->token_text, right->token_text)) {
                    network_mysqld_con_send_ok(con->client);
                    g_message("silent variable: %s", left->token_text);
                    return PROXY_SEND_RESULT;
                }

                /* set autocommit = x */
                if (sql_context_is_autocommit_off(context)) {
                    con->is_auto_commit = 0;
                    con->is_in_transaction = 1;
                    con->is_changed_user_when_quit = 0;
                    con->is_auto_commit_trans_buffered = 1;
                    g_debug("%s: autocommit off, now in transaction", G_STRLOC);
                    need_to_visit_master = TRUE;
                } else if (sql_context_is_autocommit_on(context)) {
                    if (con->is_in_transaction) {
                        con->server_in_tran_and_auto_commit_received = 1;
                    }
                    con->is_auto_commit = 1;
                    con->is_auto_commit_trans_buffered = 0;
                    need_to_visit_master = TRUE;
                    g_debug("%s: autocommit on", G_STRLOC);
                } else {
                    /* set charsetxxx = xxx */
                    if (left->op == TK_ID && right->token_text) {
                        process_other_set_command(con, left->token_text, right->token_text, query_attr);
                    }
                }
            }
        }
        break;
    }
    case STMT_UPDATE:
    case STMT_INSERT:
    case STMT_DELETE:
        break;
    default:
        if (con->is_auto_commit) {
            if (context->stmt_type == STMT_USE) {
                char *dbname = (char *)context->sql_statement;
                g_string_assign(con->client->default_db, dbname);
                g_debug("%s:set default db:%s for con:%p", G_STRLOC, con->client->default_db->str, con);
            }
        }
    }                           /* end switch */

    if (con->srv->master_preferred || context->rw_flag & CF_WRITE || need_to_visit_master) {
        g_debug("%s:rw here", G_STRLOC);
        /* rw operation */
        con->srv->query_stats.client_query.rw++;
        if (is_orig_ro_server) {
            gboolean success = proxy_get_backend_ndx(con, BACKEND_TYPE_RW, FALSE);
            if (!success) {
                con->master_conn_shortaged = 1;
                g_debug("%s:PROXY_NO_CONNECTION", G_STRLOC);
                return PROXY_NO_CONNECTION;
            }
        }
    } else {                    /* ro operation */
        g_debug("%s:ro here", G_STRLOC);
        con->srv->query_stats.client_query.ro++;
        con->is_read_ro_server_allowed = 1;
        if (con->srv->query_cache_enabled) {
            if (sql_context_is_cacheable(st->sql_context)) {
                if (try_to_get_resp_from_query_cache(con)) {
                    return PROXY_SEND_RESULT;
                }
            }
        }

        if (con->config->read_master_percentage != 100) {
            if (!is_orig_ro_server) {
                gboolean success = proxy_get_backend_ndx(con, BACKEND_TYPE_RO, FALSE);
                if (!success) {
                    con->slave_conn_shortaged = 1;
                    g_debug("%s:PROXY_NO_CONNECTION", G_STRLOC);
                }
            }
        } else {
            if (is_orig_ro_server) {
                gboolean success = proxy_get_backend_ndx(con, BACKEND_TYPE_RW, FALSE);
                if (!success) {
                    con->master_conn_shortaged = 1;
                    g_debug("%s:PROXY_NO_CONNECTION", G_STRLOC);
                    return PROXY_NO_CONNECTION;
                }
            }
        }
    }

    return PROXY_NO_DECISION;
}

static void 
proxy_inject_packet(network_mysqld_con *con, int type, int resp_type, GString *payload,
        gboolean resultset_is_needed, gboolean is_fast_streamed)
{
    proxy_plugin_con_t *st = con->plugin_con_state;
    GQueue *q = st->injected.queries;
    injection *inj = injection_new(resp_type, payload);
    if (con->srv->sql_mgr && con->srv->sql_mgr->sql_log_switch == ON) {
        inj->ts_read_query = get_timer_microseconds();
    }
    inj->resultset_is_needed = resultset_is_needed;
    inj->is_fast_streamed = is_fast_streamed;

    switch (type) {
    case PROXY_QUEUE_ADD_APPEND:
        network_injection_queue_append(q, inj);
        break;
    case PROXY_QUEUE_ADD_PREPEND:
        network_injection_queue_prepend(q, inj);
        break;
    }
}

static int
change_stmt_id(network_mysqld_con *con, uint32_t stmt_id)
{
    proxy_plugin_con_t *st = con->plugin_con_state;
    int index = (stmt_id & 0xffff0000) >> 16;
    if (con->servers != NULL) {
        if (index >= con->servers->len) {
            g_warning("%s:index:%d, stmt id:%d is too big, servers len:%d",
                      G_STRLOC, index, (int)stmt_id, con->servers->len);
            return -1;
        }
        con->server = g_ptr_array_index(con->servers, index);
        int i;
        int value = index + 1;
        for (i = 0; i < MAX_SERVER_NUM_FOR_PREPARE; i++) {
            if (st->backend_ndx_array[i] == value) {
                st->backend_ndx = i;
                break;
            }
        }
        g_debug("change conn:%p, server:%p stmt_id:%d, fd:%d, new back ndx:%d",
                con, con->server, (int)stmt_id, con->server->fd, st->backend_ndx);

        if (index > 0) {
            network_packet packet;
            injection *inj;
            inj = g_queue_peek_head(st->injected.queries);
            if (inj != NULL) {
                packet.data = inj->query;
                packet.offset = 0;
                network_mysqld_proto_change_stmt_id_from_clt_stmt(&packet);
            }
        }
    }

    return 0;
}

static int
change_server_by_rw(network_mysqld_con *con, int backend_ndx)
{
    if (backend_ndx >= 0) {
        proxy_plugin_con_t *st = con->plugin_con_state;
        if (con->servers != NULL) {
            int index = st->backend_ndx_array[backend_ndx] - 1;
            g_debug("conn:%p, change_server_by_rw,ndx:%d, index:%d, st ndx:%d", con, backend_ndx, index, st->backend_ndx);
            con->server = g_ptr_array_index(con->servers, index);
            st->backend_ndx = backend_ndx;
        }
        return 0;
    } else {
        g_critical("%s: get backend ndx failed: %d", G_STRLOC, backend_ndx);
        return -1;
    }
}

static int
adjust_sql_mode(network_mysqld_con *con, mysqld_query_attr_t *query_attr)
{
    char *clt_sql_mode, *srv_sql_mode;

    if (con->client->sql_mode == NULL) {
        clt_sql_mode = "";
    } else {
        clt_sql_mode = con->client->sql_mode->str;
    }

    if (con->server->sql_mode == NULL) {
        srv_sql_mode = "";
    } else {
        srv_sql_mode = con->server->sql_mode->str;
    }

    if (!query_attr->sql_mode_set) {
        if (strcasecmp(clt_sql_mode, srv_sql_mode) != 0) {
            if (strcmp(clt_sql_mode, "") != 0) {
                GString *packet = g_string_new(NULL);
                g_string_append_c(packet, (char)COM_QUERY);
                g_string_append(packet, "SET sql_mode='");
                g_string_append_len(packet, con->client->sql_mode->str, con->client->sql_mode->len);
                g_string_append(packet, "'");
                proxy_inject_packet(con, PROXY_QUEUE_ADD_PREPEND, INJ_ID_CHANGE_SQL_MODE, packet, TRUE, FALSE);
            } else {
                GString *packet = g_string_new(NULL);
                g_string_append_c(packet, (char)COM_QUERY);
                g_string_append(packet, "SET sql_mode=''");
                proxy_inject_packet(con, PROXY_QUEUE_ADD_PREPEND, INJ_ID_CHANGE_SQL_MODE, packet, TRUE, FALSE);
            }

            g_string_assign_len(con->server->sql_mode, con->client->sql_mode->str, con->client->sql_mode->len);
        }
    } else {
        g_string_assign_len(con->server->sql_mode, con->client->sql_mode->str, con->client->sql_mode->len);
    }

    return 0;
}

static int
adjust_charset(network_mysqld_con *con, mysqld_query_attr_t *query_attr)
{
    char *charset_str = NULL;

    if (!query_attr->charset_set) {
        if (!g_string_equal(con->client->charset, con->server->charset)) {
            GString *charset = con->client->charset;
            if (charset->len > 0) {
                query_attr->charset_reset = 1;
                charset_str = charset->str;
                g_string_assign_len(con->server->charset_client, charset->str, charset->len);
                g_string_assign_len(con->server->charset_connection, charset->str, charset->len);
                g_string_assign_len(con->server->charset_results, charset->str, charset->len);
            }
            g_string_assign_len(con->server->charset, charset->str, charset->len);
        }
    } else {
        GString *charset = con->client->charset;
        g_string_assign_len(con->server->charset, charset->str, charset->len);
    }

    if (con->srv->charset_check) {
        if (strcmp(con->client->charset->str, con->srv->default_charset) != 0) {
            g_message("%s: client charset:%s, default charset:%s, client address:%s", G_STRLOC,
                    con->client->charset->str, con->srv->default_charset, con->client->src->name->str);
        }
    }
    
    if (!query_attr->charset_client_set) {
        if (!g_string_equal(con->client->charset_client, con->server->charset_client)) {
            if (con->client->charset_client->len > 0) {
                GString *packet = g_string_new(NULL);
                g_string_append_c(packet, (char)COM_QUERY);
                g_string_append(packet, "SET character_set_client = ");
                g_string_append(packet, con->client->charset_client->str);
                proxy_inject_packet(con, PROXY_QUEUE_ADD_PREPEND, INJ_ID_CHAR_SET_CLT, packet, TRUE, FALSE);
            }
            GString *charset_client = con->client->charset_client;
            g_string_assign_len(con->server->charset_client, charset_client->str, charset_client->len);
        }
    } else {
        GString *charset_client = con->client->charset_client;
        g_string_assign_len(con->server->charset_client, charset_client->str, charset_client->len);
    }

    if (!query_attr->charset_connection_set) {
        if (!g_string_equal(con->client->charset_connection, con->server->charset_connection)) {
            if (con->client->charset_connection->len > 0) {
                GString *packet = g_string_new(NULL);
                g_string_append_c(packet, (char)COM_QUERY);
                g_string_append(packet, "SET character_set_connection = ");
                g_string_append(packet, con->client->charset_connection->str);
                proxy_inject_packet(con, PROXY_QUEUE_ADD_PREPEND, INJ_ID_CHAR_SET_CONN, packet, TRUE, FALSE);
            }
            GString *charset_conn = con->client->charset_connection;
            g_string_assign_len(con->server->charset_connection, charset_conn->str, charset_conn->len);
        }
    } else {
        GString *charset_conn = con->client->charset_connection;
        g_string_assign_len(con->server->charset_connection, charset_conn->str, charset_conn->len);
    }

    if (!query_attr->charset_results_set) {
        if (!g_string_equal(con->client->charset_results, con->server->charset_results)) {
            if (con->client->charset_results->len > 0) {
                GString *packet = g_string_new(NULL);
                g_string_append_c(packet, (char)COM_QUERY);
                g_string_append(packet, "SET character_set_results = ");
                g_string_append(packet, con->client->charset_results->str);
                proxy_inject_packet(con, PROXY_QUEUE_ADD_PREPEND, INJ_ID_CHAR_SET_RESULTS, packet, TRUE, FALSE);
            } else {
                GString *packet = g_string_new(NULL);
                g_string_append_c(packet, (char)COM_QUERY);
                g_string_append(packet, "SET character_set_results = NULL");
                proxy_inject_packet(con, PROXY_QUEUE_ADD_PREPEND, INJ_ID_CHAR_SET_RESULTS, packet, TRUE, FALSE);
            }

            GString *charset_results = con->client->charset_results;
            g_string_assign_len(con->server->charset_results, charset_results->str, charset_results->len);
        }
    } else {
        GString *charset_results = con->client->charset_results;
        g_string_assign_len(con->server->charset_results, charset_results->str, charset_results->len);
    }

    if (query_attr->charset_reset) {
        GString *packet = g_string_new(NULL);
        g_string_append_c(packet, (char)COM_QUERY);
        g_string_append(packet, "SET NAMES ");

        if (strcmp(con->client->charset->str, "") == 0) {
            g_warning("%s: client charset is empty:%s", G_STRLOC, con->client->src->name->str);
            g_string_append(packet, "''");
        } else {
            g_string_append(packet, charset_str);
        }

        proxy_inject_packet(con, PROXY_QUEUE_ADD_PREPEND, INJ_ID_SET_NAMES, packet, TRUE, FALSE);
    }

    return 0;
}

static int
adjust_default_db(network_mysqld_con *con, enum enum_server_command cmd)
{
    GString *clt_default_db = con->client->default_db;
    GString *srv_default_db = con->server->default_db;

    g_debug(G_STRLOC " default client db:%s", clt_default_db ? clt_default_db->str : "null");
    g_debug(G_STRLOC " default server db:%s", srv_default_db ? srv_default_db->str : "null");

    if (clt_default_db->len > 0) {
        if (!g_string_equal(clt_default_db, srv_default_db)) {
            GString *packet = g_string_new(NULL);
            g_string_append_c(packet, (char)COM_INIT_DB);
            g_string_append_len(packet, clt_default_db->str, clt_default_db->len);
            proxy_inject_packet(con, PROXY_QUEUE_ADD_PREPEND, INJ_ID_CHANGE_DB, packet, TRUE, FALSE);
            g_debug("%s: adjust default db", G_STRLOC);
        }
    }
    return 0;
}

static int
reset_connection(network_mysqld_con *con)
{
    GString *packet = g_string_new(NULL);
    g_string_append_c(packet, (char)COM_RESET_CONNECTION);

    proxy_inject_packet(con, PROXY_QUEUE_ADD_PREPEND, INJ_ID_RESET_CONNECTION, packet, TRUE, FALSE);

    con->server->is_in_sess_context = 0;

    return 0;
}

static int
adjust_user(network_mysqld_con *con)
{
    g_debug("%s: user:%s try to robs conn from user:%s, server:%p for con:%p", G_STRLOC,
            con->client->response->username->str, con->server->response->username->str, con->server, con);

    GString *hashed_password = g_string_new(NULL);
    const char *user = con->client->response->username->str;
    cetus_users_get_hashed_server_pwd(con->srv->priv->users, user, hashed_password);
    if (hashed_password->len == 0) {
        g_warning("%s: user:%s  hashed password is null", G_STRLOC, user);
        g_string_free(hashed_password, TRUE);
        return -1;
    } else {
        mysqld_change_user_packet_t chuser = { 0 };
        chuser.username = con->client->response->username;
        chuser.auth_plugin_data = con->server->challenge->auth_plugin_data;
        chuser.hashed_pwd = hashed_password;

        if (strcmp(con->client->default_db->str, "") == 0) {
            if (con->srv->default_db != NULL) {
                g_string_assign(con->client->default_db, con->srv->default_db);
            }
        }
        chuser.database = con->client->default_db;
        chuser.charset = con->client->charset_code;
        g_debug("%s: charset:%d when change user", G_STRLOC, con->client->charset_code);

        GString *payload = g_string_new(NULL);
        mysqld_proto_append_change_user_packet(payload, &chuser);

        proxy_inject_packet(con, PROXY_QUEUE_ADD_PREPEND, INJ_ID_CHANGE_USER, payload, TRUE, FALSE);

        con->server->is_in_sess_context = 0;
        g_string_free(hashed_password, TRUE);
        return 0;
    }
}

static int
adjust_multi_stmt(network_mysqld_con *con, enum enum_server_command cmd)
{
    if (con->client->is_multi_stmt_set != con->server->is_multi_stmt_set) {
        GString *packet = g_string_new(NULL);
        g_string_append_c(packet, (char)COM_SET_OPTION);
        if (con->client->is_multi_stmt_set) {
            g_string_append_c(packet, (char)0);
        } else {
            g_string_append_c(packet, (char)1);
        }
        g_string_append_c(packet, (char)0);
        proxy_inject_packet(con, PROXY_QUEUE_ADD_PREPEND, INJ_ID_CHANGE_MULTI_STMT, packet, TRUE, FALSE);
        g_debug("%s: adjust multi stmt", G_STRLOC);
        con->server->is_multi_stmt_set = con->client->is_multi_stmt_set;
    }

    return 0;
}

gboolean
network_mysqld_con_is_trx_feature_changed(network_mysqld_con *con)
{
    proxy_plugin_con_t *st = con->plugin_con_state;
    if (!st) {
        return FALSE;
    }
    return st->trx_read_write != TF_READ_WRITE || st->trx_isolation_level != con->srv->internal_trx_isolation_level;
}

void
network_mysqld_con_reset_trx_feature(network_mysqld_con *con)
{
    proxy_plugin_con_t *st = con->plugin_con_state;
    if (st) {
        st->trx_read_write = TF_READ_WRITE;
        st->trx_isolation_level = con->srv->internal_trx_isolation_level;
    }
}

static int
proxy_handle_local_query(network_mysqld_con *con, sql_context_t *context)
{
    g_assert(context->stmt_type == STMT_SELECT);
    sql_select_t *select = context->sql_statement;
    sql_expr_t *col = g_ptr_array_index(select->columns, 0);
    if (sql_expr_is_function(col, "CURRENT_DATE")) {
        network_mysqld_con_send_current_date(con->client, "CURRENT_DATE");
    } else if (sql_expr_is_function(col, "CETUS_VERSION")) {
        network_mysqld_con_send_cetus_version(con->client);
    }
    return PROXY_SEND_RESULT;
}

static int
process_quit_cmd(network_mysqld_con *con, int backend_ndx, int *disp_flag)
{
    if (backend_ndx < 0 || (!con->is_in_transaction && !network_mysqld_con_is_trx_feature_changed(con))) {
        g_debug("%s: quit, backend ndx:%d", G_STRLOC, backend_ndx);
        *disp_flag = PROXY_SEND_NONE;
        return 0;
    }

    if (con->server == NULL) {
        g_critical("%s: server is null while ndx:%d", G_STRLOC, backend_ndx);
        *disp_flag = PROXY_SEND_NONE;
        return 0;
    }

    if (con->is_in_transaction || network_mysqld_con_is_trx_feature_changed(con)) {
        g_message("%s: change user when COM_QUIT:%d", G_STRLOC, backend_ndx);
        int result;
        if (con->server->is_reset_conn_supported) {
            g_debug("%s: reset conn when COM_QUIT:%d", G_STRLOC, backend_ndx);
            result = reset_connection(con);
        } else {
            result = adjust_user(con);
        }

        if (result != -1) {
            con->is_changed_user_when_quit = 1;
            network_mysqld_con_reset_trx_feature(con);
            *disp_flag = PROXY_SEND_INJECTION;
            return 0;
        }
    }

    g_message("%s: unbelievable for COM_QUIT:%d", G_STRLOC, backend_ndx);

    return 1;
}

static int
forced_visit(network_mysqld_con *con, proxy_plugin_con_t *st, sql_context_t *context, int *disp_flag)
{
    if (con->server && con->server->is_in_sess_context) {
        con->is_in_sess_context = 1;
        *disp_flag = PROXY_NO_DECISION;
        g_message("%s:use previous conn for forced_visit", G_STRLOC);
        return 1;
    }

    int type = (context->rw_flag & CF_FORCE_MASTER)
        ? BACKEND_TYPE_RW : BACKEND_TYPE_RO;

    if (type == BACKEND_TYPE_RO) {
        con->use_slave_forced = 1;
    }

    if (st->backend == NULL || (st->backend && st->backend->type != type)) {
        gboolean success = proxy_get_backend_ndx(con, type,
                                                 context->rw_flag & CF_FORCE_SLAVE);
        if (!success) {
            if (type == BACKEND_TYPE_RO) {
                con->slave_conn_shortaged = 1;
                g_debug("%s:slave_conn_shortaged is true", G_STRLOC);
                success = proxy_get_backend_ndx(con, BACKEND_TYPE_RW, FALSE);
            }

            if (!success) {
                con->master_conn_shortaged = 1;
                g_debug("%s:PROXY_NO_CONNECTION", G_STRLOC);
                *disp_flag = PROXY_NO_CONNECTION;
                return 0;
            }
        }
    }

    return 1;
}

static int
process_rw_split(network_mysqld_con *con, proxy_plugin_con_t *st,
                 sql_context_t *context, mysqld_query_attr_t *query_attr,
                 int is_under_sess_scope, int command, int *disp_flag)
{
    if (!con->is_in_transaction && !is_under_sess_scope && command == COM_QUERY) {
        /* send all non-transactional SELECTs to a slave */
        int ret = process_non_trans_query(con, context, query_attr);
        switch (ret) {
        case PROXY_NO_CONNECTION:
            *disp_flag = PROXY_NO_CONNECTION;
            return 0;
        case PROXY_SEND_RESULT:
            *disp_flag = PROXY_SEND_RESULT;
            return 0;
        default:
            break;
        }
    } else {
        if (command == COM_QUERY) {
            int ret = process_filter_for_trans_query(con, context, query_attr);
            if (ret == PROXY_SEND_RESULT) {
                *disp_flag = PROXY_SEND_RESULT;
                return 0;
            }
        }
        con->srv->query_stats.client_query.rw++;
        if (con->is_in_transaction) {
            query_attr->conn_reserved = 1;
            if (command == COM_QUERY) {
                process_trans_query(con);
            } else if (command == COM_STMT_PREPARE) {
                con->is_prepared = 1;
            }
        } else {
            if (command == COM_STMT_PREPARE) {
                query_attr->conn_reserved = 1;
                con->is_prepared = 1;
                if (process_non_trans_prepare_stmt(con) == PROXY_NO_CONNECTION) {
                    *disp_flag = PROXY_NO_CONNECTION;
                    return 0;
                }
            } else if (con->prepare_stmt_count > 0 || !con->is_auto_commit) {
                query_attr->conn_reserved = 1;
            } else if (con->is_in_sess_context) {
                query_attr->conn_reserved = 1;
            }
        }
    }

    return 1;
}

static int
process_query_or_stmt_prepare(network_mysqld_con *con, proxy_plugin_con_t *st,
                              network_packet *packet, mysqld_query_attr_t *query_attr, int command, int *disp_flag)
{
    gsize sql_len = packet->data->len - packet->offset;
    network_mysqld_proto_get_gstr_len(packet, sql_len, con->orig_sql);
    g_string_append_c(con->orig_sql, '\0'); /* 2 more NULL for lexer EOB */
    g_string_append_c(con->orig_sql, '\0');

    sql_context_t *context = st->sql_context;
    sql_context_parse_len(context, con->orig_sql);

    g_debug("%s process query:%s", G_STRLOC, con->orig_sql->str);

    if (context->rc == PARSE_SYNTAX_ERR) {
        char *msg = context->message;
        g_message("%s SQL syntax error: %s. while parsing: %s", G_STRLOC, msg, con->orig_sql->str);
        network_mysqld_con_send_error_full(con->client, msg, strlen(msg), ER_SYNTAX_ERROR, "42000");
        *disp_flag = PROXY_SEND_RESULT;
        return 0;
    } else if (context->rc == PARSE_NOT_SUPPORT) {
      if (con->srv->is_sql_special_processed) {
        network_mysqld_con_send_ok_full(con->client, 0, 0, 0, 0);
      } else {
        char *msg = context->message;
        g_message(
            "%s SQL unsupported: %s. while parsing: %s for con:%p, clt:%s",
            G_STRLOC, msg, con->orig_sql->str, con,
            con->client->src->name->str);
        network_mysqld_con_send_error_full(con->client, msg, strlen(msg),
                                           ER_NOT_SUPPORTED_YET, "42000");
      }
      *disp_flag = PROXY_SEND_RESULT;
      return 0;
    } else if (context->rc == PARSE_UNRECOGNIZED) {
        g_debug("%s SQL unrecognized: %s", G_STRLOC, con->orig_sql->str);
    }

    /* forbid force write on slave */
    if ((context->rw_flag & CF_FORCE_SLAVE) &&
            (((!con->srv->check_sql_loosely) && (context->rw_flag & CF_WRITE)) || con->is_in_transaction))
    {
        g_message("%s Comment usage error. SQL: %s", G_STRLOC, con->orig_sql->str);
        if (con->is_in_transaction) {
            network_mysqld_con_send_error(con->client, C("Force transaction on read-only slave"));
        } else {
            network_mysqld_con_send_error(con->client, C("Force write on read-only slave"));
        }
        *disp_flag = PROXY_SEND_RESULT;
        return 0;
    }

    if (context->clause_flags & CF_LOCAL_QUERY) {
        *disp_flag = proxy_handle_local_query(con, context);
        return 0;
    }

    /* query statistics */
    query_stats_t *stats = &(con->srv->query_stats);
    switch (context->stmt_type) {
    case STMT_SHOW_WARNINGS:
        if (con->last_warning_met) {
            g_debug("%s: show warnings is met", G_STRLOC);
            return 1;
        }
        break;
    case STMT_DROP_DATABASE: {
        sql_drop_database_t *drop_database = context->sql_statement;
        if (drop_database) {
            truncate_default_db_when_drop_database(con, drop_database->schema_name);
        }
        break;
    }
    default:
        break;
    }

    if (context->rw_flag & (CF_FORCE_MASTER | CF_FORCE_SLAVE)) {
        if (!forced_visit(con, st, context, disp_flag)) {
            return 0;
        }
    } else {
        int is_under_sess_scope = 0;
        if (context->stmt_type == STMT_SET_TRANSACTION) {
            is_under_sess_scope = 1;
            g_debug("%s:call set tran here", G_STRLOC);
            sql_set_transaction_t *feat;
            feat = (sql_set_transaction_t *)context->sql_statement;
            if (feat->scope == SCOPE_SESSION) {
                g_message("%s:set session transaction sql:%s for con:%p", G_STRLOC, con->orig_sql->str, con);
                if (feat->rw_feature) {
                    st->trx_read_write = feat->rw_feature;
                } else if (feat->level) {
                    st->trx_isolation_level = feat->level;
                } else {
                    g_warning("%s:unexpected transaction feature:%s", G_STRLOC, con->orig_sql->str);
                }
            }
        }

        if (network_mysqld_con_is_trx_feature_changed(con)) {
            g_debug("%s:transact feature changed for con:%p", G_STRLOC, con);
            if (st->backend && st->backend->type != BACKEND_TYPE_RW) {
                gboolean success = proxy_get_backend_ndx(con, BACKEND_TYPE_RW, FALSE);
                if (!success) {
                    con->master_conn_shortaged = 1;
                    g_debug("%s:PROXY_NO_CONNECTION", G_STRLOC);
                    *disp_flag = PROXY_NO_CONNECTION;
                    return 0;
                }
            }
            con->is_in_sess_context = 1;
            is_under_sess_scope = 1;
        }

        /* rw split */
        if (!process_rw_split(con, st, context, query_attr, is_under_sess_scope, command, disp_flag)) {
            return 0;
        }
    }

    return 1;
}

static network_mysqld_stmt_ret
network_read_query(network_mysqld_con *con, proxy_plugin_con_t *st)
{
    network_packet packet;
    GQueue *recv_queue = con->client->recv_queue->chunks;
    packet.data = g_queue_peek_head(recv_queue);

    if (packet.data == NULL) {
        g_critical("%s: chunk is null", G_STRLOC);
        network_mysqld_con_send_error(con->client, C("(proxy) unable to retrieve command"));
        return PROXY_SEND_RESULT;
    }

    if (con->client->default_db->len == 0) {
        if (con->srv->default_db != NULL) {
            g_string_assign(con->client->default_db, con->srv->default_db);
            g_debug("%s:set default db:%s for con:%p", G_STRLOC, con->client->default_db->str, con);
        }
    }

    packet.offset = 0;

    mysqld_query_attr_t query_attr = { 0 };

    con->master_conn_shortaged = 0;
    con->slave_conn_shortaged = 0;
    con->use_slave_forced = 0;
    con->candidate_fast_streamed = 0;
    con->candidate_tcp_streamed = 1;

    network_injection_queue_reset(st->injected.queries);

    int backend_ndx = st->backend_ndx;

    /* check if it is a read request */
    guint8 command;
    network_mysqld_proto_skip_network_header(&packet);
    if (network_mysqld_proto_get_int8(&packet, &command) != 0) {
        network_mysqld_con_send_error(con->client, C("(proxy) unable to retrieve command"));
        return PROXY_SEND_RESULT;
    }

    con->parse.command = command;
    con->is_in_sess_context = 0;

    g_debug("%s: command:%d, backend ndx:%d, con:%p, orig sql:%s",
            G_STRLOC, command, backend_ndx, con, con->orig_sql->str);

    if (con->is_in_transaction) {
        g_debug("%s: still in tran, backend ndx:%d", G_STRLOC, backend_ndx);
    }

    int disp_flag = 0;

    switch (con->parse.command) {
    case COM_QUIT:
        if (!process_quit_cmd(con, backend_ndx, &disp_flag)) {
            return disp_flag;
        }
        break;
    case COM_BINLOG_DUMP:
        network_mysqld_con_send_error(con->client, C("(proxy) unable to process binlog dump"));
        return PROXY_SEND_RESULT;
    case COM_STMT_PREPARE:
        con->candidate_tcp_streamed = 0;
    case COM_QUERY:
        if (!process_query_or_stmt_prepare(con, st, &packet, &query_attr, command, &disp_flag)) {
            return disp_flag;
        }

        break;
    case COM_CHANGE_USER:
        network_mysqld_con_send_error(con->client, C("(proxy) unable to process change user"));
        return PROXY_SEND_RESULT;
    default:
        break;
    }                           /* end switch */

    gboolean last_resort = FALSE;
    log_sql_client(con);
    if (con->server == NULL) {
        last_resort = TRUE;
    } else {
        if (!con->is_prepared && st->backend && st->backend->type == BACKEND_TYPE_RO) {
            if (st->backend->state != BACKEND_STATE_UP && st->backend->state != BACKEND_STATE_UNKNOWN) {
                last_resort = TRUE;
            }
        }
    }

    if (last_resort) {
        g_debug("%s: con server is null", G_STRLOC);
        /* we try to get a connection */
        if (!proxy_get_backend_ndx(con, BACKEND_TYPE_RW, FALSE)) {
            con->master_conn_shortaged = 1;
            g_debug("%s:PROXY_NO_CONNECTION", G_STRLOC);
            return PROXY_NO_CONNECTION;
        }
    }

    if (con->is_in_sess_context) {
        con->server->is_in_sess_context = 1;
        g_debug("%s: set is_in_sess_context true for con server:%p", G_STRLOC, con->server);
    } else {
        con->server->is_in_sess_context = 0;
        g_debug("%s: set is_in_sess_context false for con server:%p", G_STRLOC, con->server);
    }

    network_backend_t *backend = st->backend;

    if (backend == NULL) {
        con->master_conn_shortaged = 1;
        g_warning("%s:backend is null", G_STRLOC);
        return PROXY_NO_CONNECTION;
    }

    con->server->is_read_only = 0;

    if (backend->state != BACKEND_STATE_UP && backend->state != BACKEND_STATE_UNKNOWN) {
        switch (command) {
        case COM_STMT_PREPARE:
        case COM_STMT_EXECUTE:
        case COM_QUERY:{
            network_mysqld_con_send_error_full(con->client,
                                               C("proxy stops serving requests now"), ER_ABORTING_CONNECTION, "29001");
            g_message(G_STRLOC ": ER_ABORTING_CONNECTION, proxy stops serving requests");
            return PROXY_SEND_RESULT;
        }
        default:
            break;
        }
    } else {
        if (backend->type == BACKEND_TYPE_RW) {
            con->srv->query_stats.proxyed_query.rw++;
            con->srv->query_stats.server_query_details[st->backend_ndx].rw++;
        } else {
            con->srv->query_stats.proxyed_query.ro++;
            con->srv->query_stats.server_query_details[st->backend_ndx].ro++;
            con->server->is_read_only = 1;
        }
    }

    con->last_record_updated = 0;

    /* ! Normal packets also sent out through "injection" interface */
    int payload_len = packet.data->len - NET_HEADER_SIZE;
    GString *payload = g_string_sized_new(calculate_alloc_len(payload_len));
    g_string_append_len(payload, packet.data->str + NET_HEADER_SIZE, payload_len);
    sql_context_t *context = st->sql_context;
    switch (command) {
    case COM_QUERY:
        if (context->stmt_type == STMT_SELECT && con->is_read_ro_server_allowed) {
            if (con->srv->is_fast_stream_enabled) {
                if ((!con->srv->sql_mgr) ||
                        (con->srv->sql_mgr->sql_log_switch != ON && con->srv->sql_mgr->sql_log_switch != REALTIME))
                {
                    proxy_inject_packet(con, PROXY_QUEUE_ADD_APPEND, INJ_ID_COM_QUERY, payload, FALSE, TRUE);
                } else {
                    proxy_inject_packet(con, PROXY_QUEUE_ADD_APPEND, INJ_ID_COM_QUERY, payload, FALSE, FALSE);
                }
            } else {
                proxy_inject_packet(con, PROXY_QUEUE_ADD_APPEND, INJ_ID_COM_QUERY, payload, FALSE, FALSE);
            }
        } else {
            proxy_inject_packet(con, PROXY_QUEUE_ADD_APPEND, INJ_ID_COM_QUERY, payload, TRUE, FALSE);
        }
        break;
    case COM_STMT_PREPARE:
        proxy_inject_packet(con, PROXY_QUEUE_ADD_APPEND, INJ_ID_COM_STMT_PREPARE, payload, TRUE, FALSE);
        break;
    default:
        proxy_inject_packet(con, PROXY_QUEUE_ADD_APPEND, INJ_ID_COM_DEFAULT, payload, TRUE, FALSE);
    }

    if (context->stmt_type == STMT_SHOW_WARNINGS && con->last_warning_met) {
        if (con->server == NULL) {
            network_injection_queue_reset(st->injected.queries);
            network_mysqld_con_send_ok_full(con->client, 0, 0, 0, 0);
            return PROXY_SEND_RESULT;
        } else {
            return PROXY_SEND_INJECTION;
        }
    }

    if (con->multiple_server_mode) {
        query_attr.conn_reserved = 1;
        if (command == COM_STMT_EXECUTE || command == COM_STMT_CLOSE) {
            uint32_t stmt_id;
            packet.offset = NET_HEADER_SIZE;

            if (network_mysqld_proto_get_stmt_id(&packet, &stmt_id) == 0) {
                if (change_stmt_id(con, stmt_id) == -1) {
                    network_mysqld_con_send_error_full(con->client,
                         C("change stmt id failed"), ER_ABORTING_CONNECTION, "29001");
                    return PROXY_SEND_RESULT;
                }
            }
        } else if (command == COM_QUERY) {
            change_server_by_rw(con, st->backend_ndx);
        }
    }

    if (query_attr.conn_reserved == 1) {
        con->client->is_server_conn_reserved = 1;
        g_debug("%s: set is_server_conn_reserved true:%p", G_STRLOC, con);
    }

    adjust_sql_mode(con, &query_attr);

    adjust_charset(con, &query_attr);

    if (command != COM_INIT_DB && con->rob_other_conn == 0) {
        adjust_default_db(con, command);
    }

    if (command != COM_SET_OPTION) {
        adjust_multi_stmt(con, command);
    }

    if (con->rob_other_conn) {
        con->rob_other_conn = 0;
        if (adjust_user(con) == -1) {
            network_injection_queue_reset(st->injected.queries);
            network_mysqld_con_send_error_full(con->client,
                                               C("proxy stops serving requests"), ER_NO_SUCH_USER, "29001");
            g_message("%s: ER_NO_SUCH_USER, proxy stops serving requests", G_STRLOC);
            return PROXY_SEND_RESULT;
        }
    }

    return PROXY_SEND_INJECTION;
}

/**
 * gets called after a query has been read
 *
 * @see network_mysqld_con_handle_proxy_stmt
 */
NETWORK_MYSQLD_PLUGIN_PROTO(proxy_read_query)
{
    GString *packet;
    network_socket *recv_sock, *send_sock;
    proxy_plugin_con_t *st = con->plugin_con_state;
    int proxy_query = 1;
    int quietly_quit = 0;
    network_mysqld_stmt_ret ret;

    con->resp_too_long = 0;
    con->last_warning_met = 0;

    network_mysqld_con_reset_query_state(con);

    if (st == NULL)
        return NETWORK_SOCKET_ERROR;

    send_sock = NULL;
    recv_sock = con->client;
    st->injected.sent_resultset = 0;

    int server_attr_changed = 0;
    con->is_client_to_be_closed = 0;
    con->server_in_tran_and_auto_commit_received = 0;
    if (con->server_to_be_closed) {
        g_warning("%s server_to_be_closed is true", G_STRLOC);
    }
    if (con->client->send_queue->chunks->length > 0) {
        g_warning("%s send queue is not empty", G_STRLOC);
    } else {
        con->client->send_queue->len = 0;
    }
    
    con->server_to_be_closed = 0;

    if (con->server != NULL) {
        if (con->last_backend_type != st->backend->type) {
            g_warning("%s server_attr_changed, last backend type:%d, now type:%d",
                    G_STRLOC, con->last_backend_type, st->backend->type);
            server_attr_changed = 1;
        } else {
            if (st->backend->state != BACKEND_STATE_UP && st->backend->state != BACKEND_STATE_UNKNOWN) {
                if (con->is_prepared) {
                    if (con->srv->is_manual_down) {
                        g_message("%s Could not continue to process prepare stmt", G_STRLOC);
                        server_attr_changed = 1;
                        con->is_client_to_be_closed = 1;
                    }
                } else {
                    server_attr_changed = 1;
                    g_message("%s backend state:%d", G_STRLOC, st->backend->state);
                }
            }
        }
    }

    if (server_attr_changed && con->client->is_server_conn_reserved) {
        g_message("%s server attr changed and conn_reserved true, stop process", G_STRLOC);
        network_mysqld_con_send_error(con->client, C("(proxy) unable to continue processing command"));
        ret = PROXY_SEND_RESULT;
        con->server_to_be_closed = 1;
    } else {
        if (server_attr_changed) {
            g_message("%s server_attr_changed and add to pool", G_STRLOC);
            if (network_pool_add_conn(con, 0) != 0) {
                g_message("%s, con:%p:conn to pool failed", G_STRLOC, con);
            }
        }

        ret = network_read_query(con, st);
        con->last_warning_met = 0;

        if (con->server != NULL) {
            con->last_backend_type = st->backend->type;
        } else {
            con->last_backend_type = BACKEND_TYPE_UNKNOWN;
        }
    }

    /**
     * if we disconnected in read_query_result() we have no connection open
     * when we try to execute the next query
     *
     * for PROXY_SEND_RESULT we don't need a server
     */
    if (ret != PROXY_SEND_NONE && ret != PROXY_SEND_RESULT) {
        if (con->server == NULL || ret == PROXY_NO_CONNECTION) {
            g_debug("%s: I have no server backend, con:%p for user:%s",
                    G_STRLOC, con, con->client->response->username->str);
            if (con->master_unavailable) {
                return NETWORK_SOCKET_ERROR;
            } else {
                return NETWORK_SOCKET_ERROR_RETRY;
            }
        }
    }

    GQueue *chunks;
    switch (ret) {
    case PROXY_NO_DECISION:
        if (st->injected.queries->length) {
            g_critical("%s: discarding %d elements from the queue.", G_STRLOC, st->injected.queries->length);
            network_injection_queue_reset(st->injected.queries);
        }
        /* fall through */
    case PROXY_SEND_QUERY:
        g_message("error: this assumes to dead path");
        send_sock = con->server;

        /* no injection, pass on the chunks as is */
        while ((packet = g_queue_pop_head(recv_sock->recv_queue->chunks))) {
            network_mysqld_queue_append_raw(send_sock, send_sock->send_queue, packet);
        }
        /* we don't want to buffer the result-set */
        con->resultset_is_needed = 0;

        break;
    case PROXY_SEND_RESULT:{
        gboolean is_first_packet = TRUE;
        proxy_query = 0;

        send_sock = con->client;

        chunks = recv_sock->recv_queue->chunks;
        /* flush the recv-queue and track the command-states */
        while ((packet = g_queue_pop_head(chunks))) {
            if (is_first_packet) {
                network_packet p;

                p.data = packet;
                p.offset = 0;

                network_mysqld_con_reset_command_response_state(con);

                g_debug("%s: call network_mysqld_con_command_states_init for con:%p", G_STRLOC, con);

                if (network_mysqld_con_command_states_init(con, &p)) {
                    g_message("%s: states init failure", G_STRLOC);
                }

                is_first_packet = FALSE;
            }

            g_string_free(packet, TRUE);
        }

        break;
    }
    case PROXY_SEND_INJECTION:{
        injection *inj;

        inj = g_queue_peek_head(st->injected.queries);
        con->resultset_is_needed = inj->resultset_is_needed;
        con->candidate_fast_streamed = inj->is_fast_streamed;

        send_sock = con->server;

        network_mysqld_queue_reset(send_sock);
        network_mysqld_queue_append(send_sock, send_sock->send_queue, S(inj->query));

        network_queue_clear(recv_sock->recv_queue);
        break;
    }
    case PROXY_SEND_NONE:{
        quietly_quit = 1;
        break;
    }
    default:
        g_error("%s:ret:%d ", G_STRLOC, ret);
    }

    if (proxy_query) {
        if (quietly_quit) {
            con->state = ST_CLIENT_QUIT;
        } else {
            con->state = ST_SEND_QUERY;
        }
    } else {
        GList *cur;

        /*
         * if we don't send the query to the backend,
         * it won't be tracked. So track it here instead
         * to get the packet tracking right (LOAD DATA LOCAL INFILE, ...)
         */

        for (cur = send_sock->send_queue->chunks->head; cur; cur = cur->next) {
            network_packet p;

            p.data = cur->data;
            p.offset = 0;

            network_mysqld_proto_get_query_result(&p, con);
        }

        con->state = ST_SEND_QUERY_RESULT;
    }

    return NETWORK_SOCKET_SUCCESS;
}

static gboolean
proxy_get_backend_ndx(network_mysqld_con *con, int type, gboolean force_slave)
{
    proxy_plugin_con_t *st = con->plugin_con_state;
    chassis_private *g = con->srv->priv;

    con->max_retry_serv_cnt = 72;
    con->master_unavailable = 0;

    int idx;
    if (type == BACKEND_TYPE_RO) {
        if (force_slave) {
            idx = network_backends_get_ro_ndx(g->backends);
        } else {
            int x = g_random_int_range(0, 100);
            if (x < con->config->read_master_percentage) {
                idx = network_backends_get_rw_ndx(g->backends);
            } else {
                idx = network_backends_get_ro_ndx(g->backends);
            }
            g_debug(G_STRLOC ": %d, read_master_percentage: %d, read: %d",
                    x, con->config->read_master_percentage, idx);
        }
    } else {                    /* type == BACKEND_TYPE_RW */
        idx = network_backends_get_rw_ndx(g->backends);
    }

    if (idx == -1) {
        if (type == BACKEND_TYPE_RW) {
            if (con->server) {
                g_message("%s: free server conn to pool:%p", G_STRLOC, con);
                if (network_pool_add_conn(con, 0) != 0) {
                    g_message("%s, con:%p:conn to pool failed", G_STRLOC, con);
                }
            }
            con->master_unavailable = 1;
            con->max_retry_serv_cnt = 1;
            return FALSE;
        }
    } else {
        if (idx == st->backend_ndx && con->server) {
            g_debug("%s: no need to change server:%d", G_STRLOC, st->backend_ndx);
            return TRUE;
        }

        network_socket *send_sock = network_connection_pool_swap(con, idx);
        if (!send_sock) {
            return FALSE;
        }
        con->server = send_sock;
        st->backend_ndx = idx;
    }
    return TRUE;
}

/**
 * decide about the next state after the result-set has been written
 * to the client
 *
 * if we still have data in the queue, back to proxy_send_query()
 * otherwise back to proxy_read_query() to pick up a new client query
 *
 * @note we should only send one result back to the client
 */
NETWORK_MYSQLD_PLUGIN_PROTO(proxy_send_query_result)
{
    network_socket *send_sock;
    injection *inj;
    proxy_plugin_con_t *st = con->plugin_con_state;


    con->server_in_tran_and_auto_commit_received = 0;

    if (con->server_to_be_closed) {
        if (con->servers != NULL) {
            if (con->is_client_to_be_closed) {
                con->state = ST_CLOSE_CLIENT;
                g_debug("%s:client needs to closed for con:%p", G_STRLOC, con);
            } else {
                con->state = ST_READ_QUERY;
            }
            return NETWORK_SOCKET_SUCCESS;
        } else if (con->server) {
            GString *packet;
            while ((packet = g_queue_pop_head(con->server->recv_queue->chunks))) {
                g_string_free(packet, TRUE);
            }

            st->backend->connected_clients--;
            network_socket_send_quit_and_free(con->server);
            g_debug("%s:server needs to closed for con:%p", G_STRLOC, con);
            con->server = NULL;
            st->backend_ndx = -1;
            st->backend = NULL;
            con->server_to_be_closed = 0;
            con->server_closed = 0;

            if (con->is_client_to_be_closed) {
                con->state = ST_CLOSE_CLIENT;
                return NETWORK_SOCKET_SUCCESS;
            }
        }
    }

    if (con->is_changed_user_failed) {
        con->is_changed_user_failed = 0;
        con->state = ST_ERROR;
        g_debug("%s, con:%p:state is set ST_ERROR", G_STRLOC, con);
        return NETWORK_SOCKET_SUCCESS;
    }

    send_sock = con->server;

    /*
     * if we don't have a backend, don't try to forward queries
     */
    if (!send_sock) {
        network_injection_queue_reset(st->injected.queries);
    }

    if (st->injected.queries->length == 0) {
        /* we have nothing more to send, let's see what the next state is */
        if (st->sql_context->stmt_type == STMT_DROP_DATABASE) {
            network_mysqld_com_query_result_t *com_query = con->parse.data;
            if (com_query && com_query->query_status == MYSQLD_PACKET_OK) {
                if (con->servers != NULL) {
                    con->server_to_be_closed = 1;
                } else if (con->server) {
                    g_string_truncate(con->server->default_db, 0);
                    g_message("%s:truncate server database for con:%p", G_STRLOC, con);
                }
            }
        }
        con->state = ST_READ_QUERY;

        return NETWORK_SOCKET_SUCCESS;
    }

    /* looks like we still have queries in the queue,
     * push the next one
     */
    inj = g_queue_peek_head(st->injected.queries);
    con->resultset_is_needed = inj->resultset_is_needed;

    if (!inj->resultset_is_needed && st->injected.sent_resultset > 0) {
        /*
         * we already sent a resultset to the client and the next query
         * wants to forward it's result-set too, that can't work
         */
        g_critical("%s: append() mul-queries without true rs set.", G_STRLOC);

        return NETWORK_SOCKET_ERROR;
    }

    g_assert(inj);
    g_assert(send_sock);

    network_mysqld_queue_reset(send_sock);
    network_mysqld_queue_append(send_sock, send_sock->send_queue, S(inj->query));

    g_debug("%s: call reset_command_response_state for con:%p", G_STRLOC, con);
    network_mysqld_con_reset_command_response_state(con);

    con->state = ST_SEND_QUERY;

    return NETWORK_SOCKET_SUCCESS;
}

/**
 * handle the query-result we received from the server
 *
 * - decode the result-set to track if we are finished already
 * - handles BUG#25371 if requested
 * - if the packet is finished,
 *
 * @see network_mysqld_con_handle_proxy_resultset
 */
NETWORK_MYSQLD_PLUGIN_PROTO(proxy_read_query_result)
{
    network_packet packet;
    network_socket *recv_sock, *send_sock;
    proxy_plugin_con_t *st = con->plugin_con_state;
    injection *inj = NULL;

    recv_sock = con->server;
    send_sock = con->client;

    /* check if the last packet is valid */
    packet.data = g_queue_peek_tail(recv_sock->recv_queue->chunks);
    packet.offset = 0;

    if (0 != st->injected.queries->length) {
        inj = g_queue_peek_head(st->injected.queries);
    }

    g_debug("%s: here we visit network_mysqld_proto_get_query_result for con:%p", G_STRLOC, con);

    if (!con->resp_too_long) {
        /* TODO if attribute adjustment fails, then the backend connection should not be put to pool */
        switch (con->parse.command) {
        case COM_QUERY:
        case COM_PROCESS_INFO:
        case COM_STMT_EXECUTE:{
            g_debug("%s: read finished: %p", G_STRLOC, con);
            network_mysqld_com_query_result_t *query = con->parse.data;
            if (query && query->query_status == MYSQLD_PACKET_ERR) {
                int offset = packet.offset;
                packet.offset = NET_HEADER_SIZE;
                network_mysqld_err_packet_t *err_packet;
                err_packet = network_mysqld_err_packet_new();
                if (!network_mysqld_proto_get_err_packet(&packet, err_packet)) {
                    g_message("%s:dst:%s,sql:%s,errmsg:%s",
                              G_STRLOC, con->server->dst->name->str, con->orig_sql->str, err_packet->errmsg->str);
                } else {
                    g_message("%s:dst:%s,sql:%s", G_STRLOC, con->server->dst->name->str, con->orig_sql->str);
                }
                network_mysqld_err_packet_free(err_packet);
                packet.offset = offset;
            }
            break;
        }
        case COM_STMT_PREPARE:{
            network_mysqld_com_stmt_prep_result_t *r = con->parse.data;
            if (r->status == MYSQLD_PACKET_OK) {
                con->prepare_stmt_count++;
            } else {
                g_warning("%s: prepare stmt not ok for con:%p", G_STRLOC, con);
            }
            break;
        }
        case COM_INIT_DB:
            break;
        case COM_CHANGE_USER:
            break;
        default:
            break;
        }
    }

    network_mysqld_stmt_ret ret;

    /**
     * the resultset handler might decide to trash the send-queue
     *
     */

    if (inj) {
        switch (con->parse.command) {
            case COM_QUERY:
            case COM_STMT_EXECUTE:
                {
                    network_mysqld_com_query_result_t *com_query = con->parse.data;

                    inj->bytes = com_query->bytes;
                    inj->rows = com_query->rows;
                    inj->qstat.was_resultset = com_query->was_resultset;
                    inj->qstat.binary_encoded = com_query->binary_encoded;

                    /* INSERTs have a affected_rows */
                    if (!com_query->was_resultset) {
                        if (com_query->affected_rows > 0) {
                            con->last_record_updated = 1;
                        }
                        inj->qstat.affected_rows = com_query->affected_rows;
                        inj->qstat.insert_id = com_query->insert_id;
                        if (inj->qstat.insert_id > 0) {
                            con->last_insert_id = inj->qstat.insert_id;
                            g_debug("%s: last insert id:%d for con:%p", G_STRLOC, (int)con->last_insert_id, con);
                        }
                    }
                    inj->qstat.server_status = com_query->server_status;
                    inj->qstat.warning_count = com_query->warning_count;
                    inj->qstat.query_status = com_query->query_status;
                    g_debug("%s: server status, got: %d, con:%p", G_STRLOC, com_query->server_status, con);
                    break;
                }
            case COM_INIT_DB:
                break;
            case COM_CHANGE_USER:
                break;
            default:
                g_debug("%s: no chance to get server status", G_STRLOC);
        }
        if (con->srv->sql_mgr && (con->srv->sql_mgr->sql_log_switch == ON || con->srv->sql_mgr->sql_log_switch == REALTIME)) {
            inj->ts_read_query_result_last = get_timer_microseconds();
            log_sql_backend(con, inj);
        }
    }

    /* reset the packet-id checks as the server-side is finished */
    network_mysqld_queue_reset(recv_sock);

    ret = proxy_c_read_query_result(con);

    g_debug("%s: after proxy_c_read_query_result,ret:%d", G_STRLOC, ret);

    if (PROXY_IGNORE_RESULT != ret) {
        /* reset the packet-id checks, if we sent something to the client */
        network_mysqld_queue_reset(send_sock);
    }

    /**
     * if the send-queue is empty, we have nothing to send
     * and can read the next query */
    if (send_sock->send_queue->chunks) {
        g_debug("%s: send queue is not empty:%d", G_STRLOC, send_sock->send_queue->chunks->length);
        con->state = ST_SEND_QUERY_RESULT;
    } else {
        /*
         * we already forwarded the resultset,
         * no way someone has flushed the resultset-queue
         */
        g_assert_cmpint(con->resultset_is_needed, ==, 1);

        con->state = ST_READ_QUERY;
    }

    return NETWORK_SOCKET_SUCCESS;
}

/**
 * connect to a backend
 *
 * @return
 *   NETWORK_SOCKET_SUCCESS        - connected successfully
 *   NETWORK_SOCKET_ERROR_RETRY    - connecting backend failed,
 *                                   call again to connect to another backend
 *   NETWORK_SOCKET_ERROR          - no backends available,
 *                                   adds a ERR packet to the client queue
 */
NETWORK_MYSQLD_PLUGIN_PROTO(proxy_connect_server)
{
    proxy_plugin_con_t *st = con->plugin_con_state;
    return do_connect_cetus(con, &st->backend, &st->backend_ndx);
}

static proxy_plugin_con_t *
proxy_plugin_con_new()
{
    proxy_plugin_con_t *st;

    st = g_new0(proxy_plugin_con_t, 1);

    st->injected.queries = network_injection_queue_new();
    return st;
}

NETWORK_MYSQLD_PLUGIN_PROTO(proxy_init)
{
    proxy_plugin_con_t *st = con->plugin_con_state;
    chassis_plugin_config *config = con->config;

    g_assert(con->plugin_con_state == NULL);

    st = proxy_plugin_con_new();

    /* TODO: this should inside "st"_new, but now "st" shared by many plugins */
    st->sql_context = g_new0(sql_context_t, 1);
    sql_context_init(st->sql_context);
    st->trx_read_write = TF_READ_WRITE;
    st->trx_isolation_level = con->srv->internal_trx_isolation_level;

    con->plugin_con_state = st;

    con->state = ST_CONNECT_SERVER;

    /* set the connection specific timeouts
     *
     * TODO: expose these settings at runtime
     */
    if (config->connect_timeout_dbl >= 0) {
        chassis_timeval_from_double(&con->connect_timeout, config->connect_timeout_dbl);
    }
    if (config->read_timeout_dbl >= 0) {
        chassis_timeval_from_double(&con->read_timeout, config->read_timeout_dbl);
    }
    if (config->write_timeout_dbl >= 0) {
        chassis_timeval_from_double(&con->write_timeout, config->write_timeout_dbl);
    }

    return NETWORK_SOCKET_SUCCESS;
}

static network_mysqld_stmt_ret
proxy_c_disconnect_client(network_mysqld_con *con)
{
    g_debug("%s: call proxy_c_disconnect_client: %p", G_STRLOC, con);
    gboolean client_abnormal_close = FALSE;
    if (con->state == ST_READ_QUERY_RESULT) {
        client_abnormal_close = TRUE;
        g_debug("%s: set client_abnormal_close true: %p", G_STRLOC, con);
    } else {
        if (con->prev_state > ST_READ_QUERY) {
            client_abnormal_close = TRUE;
            g_debug("%s: set client_abnormal_close true: %p", G_STRLOC, con);
        }
    }

    if (client_abnormal_close) {
        con->server_to_be_closed = 1;
    } else {
        if (con->is_changed_user_when_quit) {
            con->is_in_transaction = 0;
            con->is_auto_commit = 1;
            con->is_start_tran_command = 0;
            g_debug("%s: auto commit true", G_STRLOC);
        }

        if (con->is_in_transaction || !con->is_auto_commit || con->is_in_sess_context) {
            con->server_to_be_closed = 1;
        } else {

            if (con->server != NULL) {
                if (network_pool_add_conn(con, 0) != 0) {
                    g_debug("%s, con:%p:conn returned to pool failed", G_STRLOC, con);
                }
            }
        }
    }

    return PROXY_NO_DECISION;
}

static void
mysqld_con_reserved_connections_free(network_mysqld_con *con)
{
    proxy_plugin_con_t *st = con->plugin_con_state;
    chassis *srv = con->srv;
    chassis_private *g = srv->priv;
    if (st->backend_ndx_array) {
        int i, checked = 0;
        for (i = 0; i < MAX_SERVER_NUM_FOR_PREPARE; i++) {
            if (st->backend_ndx_array[i] <= 0) {
                continue;
            }
            /* rw-edition: after filtering, now [i] is a valid backend index */
            int index = st->backend_ndx_array[i] - 1;
            network_socket *server = g_ptr_array_index(con->servers, index);
            network_backend_t *backend = network_backends_get(g->backends, i);

            CHECK_PENDING_EVENT(&(server->event));

            network_socket_send_quit_and_free(server);
            backend->connected_clients--;
            g_debug("%s: connected_clients sub, con:%p, now clients:%d", G_STRLOC, con, backend->connected_clients);
            checked++;

            if (checked >= con->servers->len) {
                g_ptr_array_free(con->servers, TRUE);
                con->servers = NULL;
                break;
            }
        }

        if (st->backend_ndx_array) {
            g_free(st->backend_ndx_array);
            st->backend_ndx_array = NULL;
        }
    }
}

static void
proxy_plugin_con_free(network_mysqld_con *con, proxy_plugin_con_t *st)
{
    g_debug("%s: call proxy_plugin_con_free con:%p", G_STRLOC, con);

    if (!st)
        return;

    network_injection_queue_free(st->injected.queries);

    /* If con still has server list, then all are closed */
    if (con->servers != NULL) {
        mysqld_con_reserved_connections_free(con);
        con->server = NULL;
    } else {
        if (con->server) {
            st->backend->connected_clients--;
            g_debug("%s: connected_clients sub, con:%p, now clients:%d", G_STRLOC, con, st->backend->connected_clients);
        }
    }

    if (st->backend_ndx_array) {
        g_warning("%s: st backend_ndx_array is not nill for con:%p", G_STRLOC, con);
    }

    g_free(st);
}

/**
 * cleanup the proxy specific data on the current connection
 *
 * move the server connection into the connection pool in case it is a
 * good client-side close
 *
 * @return NETWORK_SOCKET_SUCCESS
 * @see plugin_call_cleanup
 */
NETWORK_MYSQLD_PLUGIN_PROTO(proxy_disconnect_client)
{
    proxy_plugin_con_t *st = con->plugin_con_state;

    if (st == NULL)
        return NETWORK_SOCKET_SUCCESS;

    network_mysqld_stmt_ret ret;
    ret = proxy_c_disconnect_client(con);
    switch (ret) {
    case PROXY_NO_DECISION:
        /* just go on */

        break;
    case PROXY_IGNORE_RESULT:
        break;
    default:
        g_error("%s: ... ", G_STRLOC);
        break;
    }

    if (con->server && !con->server_to_be_closed) {
        if (con->state == ST_CLOSE_CLIENT || con->prev_state <= ST_READ_QUERY) {
            /* move the connection to the connection pool
             *
             * this disconnects con->server and safes it
             * from getting free()ed later
             */

            if (network_pool_add_conn(con, 0) != 0) {
                g_message("%s, con:%p:server conn returned to pool failed", G_STRLOC, con);
            }
        }
    }

    if (con->servers != NULL) {
        g_critical("%s: conn server list is not freed:%p", G_STRLOC, con);
    }

    network_mysqld_con_reset_query_state(con);

    /* TODO: this should inside "st"_free, but now "st" shared by many plugins */
    if (st->sql_context) {
        sql_context_destroy(st->sql_context);
        g_free(st->sql_context);
        st->sql_context = NULL;
    }

    proxy_plugin_con_free(con, st);

    con->plugin_con_state = NULL;

    g_debug("%s: set plugin_con_state null:%p", G_STRLOC, con);

    /**
     * walk all pools and clean them up
     */

    return NETWORK_SOCKET_SUCCESS;
}

int
network_mysqld_proxy_connection_init(network_mysqld_con *con)
{
    con->plugins.con_init = proxy_init;
    con->plugins.con_connect_server = proxy_connect_server;
    con->plugins.con_read_handshake = NULL;
    con->plugins.con_read_auth = proxy_read_auth;
    con->plugins.con_read_auth_result = NULL;
    con->plugins.con_read_query = proxy_read_query;
    con->plugins.con_get_server_conn_list = NULL;
    con->plugins.con_read_query_result = proxy_read_query_result;
    con->plugins.con_send_query_result = proxy_send_query_result;
    con->plugins.con_cleanup = proxy_disconnect_client;
    con->plugins.con_timeout = proxy_timeout;

    return 0;
}

/**
 * free the global scope which is shared between all connections
 *
 * make sure that is called after all connections are closed
 */
void
network_mysqld_proxy_free(network_mysqld_con G_GNUC_UNUSED *con)
{
}

chassis_plugin_config *config;

chassis_plugin_config *
network_mysqld_proxy_plugin_new(void)
{
    config = g_new0(chassis_plugin_config, 1);

    /* use negative values as defaults to make them ignored */
    config->connect_timeout_dbl = -1.0;
    config->read_timeout_dbl = -1.0;
    config->write_timeout_dbl = -1.0;

    return config;
}

void
network_mysqld_proxy_plugin_free(chassis *chas, chassis_plugin_config *config)
{

    g_strfreev(config->backend_addresses);
    g_strfreev(config->read_only_backend_addresses);

    if (config->address) {
        /* free the global scope */
        network_mysqld_proxy_free(NULL);
        chassis_config_unregister_service(chas->config_manager, config->address);
        g_free(config->address);
    }
    sql_filter_vars_destroy();

    g_free(config);
}

static gchar*
show_proxy_address(gpointer param) {
    struct external_param *opt_param = (struct external_param *)param;
    gint opt_type = opt_param->opt_type;
    if(CAN_SHOW_OPTS_PROPERTY(opt_type)) {
        return g_strdup_printf("%s", config->address != NULL ? config->address: "NULL");
    }
    if(CAN_SAVE_OPTS_PROPERTY(opt_type)) {
        if(config->address) {
            return g_strdup_printf("%s", config->address);
        }
    }
    return NULL;
}

static gchar*
show_proxy_read_only_backend_address(gpointer param) {
    gchar *ret = NULL;
    struct external_param *opt_param = (struct external_param *)param;
    chassis *srv = opt_param->chas;
    gint opt_type = opt_param->opt_type;
    network_backends_t *bs = opt_param->chas->priv->backends;
    if(CAN_SAVE_OPTS_PROPERTY(opt_type)) {
        GString *free_str = g_string_new(NULL);
        guint i;
        for (i = 0; i < bs->backends->len; i++) {
            network_backend_t *old_backend = g_ptr_array_index(bs->backends, i);
            if(old_backend && old_backend->type == BACKEND_TYPE_RO
                        && old_backend->state != BACKEND_STATE_DELETED && old_backend->state != BACKEND_STATE_MAINTAINING) {
                free_str = g_string_append(free_str, old_backend->address->str);
                if (old_backend->server_weight) {
                  free_str = g_string_append(free_str, "#");
                  free_str = g_string_append_c(
                      free_str, '0' + old_backend->server_weight);
                }
                free_str = g_string_append(free_str, ",");
            }
        }
        if(free_str->len) {
            free_str->str[free_str->len -1] = '\0';
            ret = g_strdup(free_str->str);
        }
        g_string_free(free_str, TRUE);
    }
    return ret;
}

static gchar*
show_proxy_backend_addresses(gpointer param) {
    gchar *ret = NULL;
    struct external_param *opt_param = (struct external_param *)param;
    chassis *srv = opt_param->chas;
    gint opt_type = opt_param->opt_type;
    network_backends_t *bs = opt_param->chas->priv->backends;
    if(CAN_SAVE_OPTS_PROPERTY(opt_type)) {
        GString *free_str = g_string_new(NULL);
        guint i;
        for (i = 0; i < bs->backends->len; i++) {
            network_backend_t *old_backend = g_ptr_array_index(bs->backends, i);
            if(old_backend && old_backend->type == BACKEND_TYPE_RW
                        && old_backend->state != BACKEND_STATE_DELETED && old_backend->state != BACKEND_STATE_MAINTAINING) {
                free_str = g_string_append(free_str, old_backend->address->str);
                if (old_backend->server_weight) {
                  free_str = g_string_append(free_str, "#");
                  free_str = g_string_append_c(
                      free_str, '0' + old_backend->server_weight);
                }

                free_str = g_string_append(free_str, ",");
            }
        }
        if(free_str->len) {
            free_str->str[free_str->len -1] = '\0';
        }
        /* handle defaults */
        if(!strcasecmp(free_str->str, "127.0.0.1:3306")) {
            ret = NULL;
        } else {
            if(free_str->len) {
                ret = g_strdup(free_str->str);
            }
        }

        g_string_free(free_str, TRUE);
    }
    return ret;
}

static gchar*
show_proxy_connect_timeout(gpointer param) {
    struct external_param *opt_param = (struct external_param *)param;
    gint opt_type = opt_param->opt_type;
    if(CAN_SHOW_OPTS_PROPERTY(opt_type)) {
        return g_strdup_printf("%lf (s)", config->connect_timeout_dbl);
    }
    if(CAN_SAVE_OPTS_PROPERTY(opt_type)) {
        /* handle default */
        if(config->connect_timeout_dbl == -1) {
            return NULL;
        }
        return g_strdup_printf("%lf", config->connect_timeout_dbl);
    }
    return NULL;
}

static gint
assign_proxy_connect_timeout(const gchar *newval, gpointer param) {
    gint ret = ASSIGN_ERROR;
    struct external_param *opt_param = (struct external_param *)param;
    gint opt_type = opt_param->opt_type;
    if(CAN_ASSIGN_OPTS_PROPERTY(opt_type)) {
        if(NULL != newval) {
            gdouble value = 0;
            if(try_get_double_value(newval, &value)) {
                config->connect_timeout_dbl = value;
                ret = ASSIGN_OK;
            } else {
                ret = ASSIGN_VALUE_INVALID;
            }
        } else {
            ret = ASSIGN_VALUE_INVALID;
        }
    }
    return ret;
}

static gchar*
show_proxy_read_timeout(gpointer param) {
    struct external_param *opt_param = (struct external_param *)param;
    gint opt_type = opt_param->opt_type;
    if(CAN_SHOW_OPTS_PROPERTY(opt_type)) {
        return g_strdup_printf("%lf (s)", config->read_timeout_dbl);
    }
    if(CAN_SAVE_OPTS_PROPERTY(opt_type)) {
        /* handle default */
        if(config->read_timeout_dbl == -1) {
            return NULL;
        }
        return g_strdup_printf("%lf", config->read_timeout_dbl);
    }
    return NULL;
}

static gint
assign_proxy_read_timeout(const gchar *newval, gpointer param) {
    gint ret = ASSIGN_ERROR;
    struct external_param *opt_param = (struct external_param *)param;
    gint opt_type = opt_param->opt_type;
    if(CAN_ASSIGN_OPTS_PROPERTY(opt_type)) {
        if(NULL != newval) {
            gdouble value = 0;
            if(try_get_double_value(newval, &value)) {
                config->read_timeout_dbl = value;
                ret = ASSIGN_OK;
            } else {
                ret = ASSIGN_VALUE_INVALID;
            }
        } else {
            ret = ASSIGN_VALUE_INVALID;
        }
    }
    return ret;
}

static gchar*
show_proxy_write_timeout(gpointer param) {
    struct external_param *opt_param = (struct external_param *)param;
    gint opt_type = opt_param->opt_type;
    if(CAN_SHOW_OPTS_PROPERTY(opt_type)) {
        return g_strdup_printf("%lf (s)", config->write_timeout_dbl);
    }
    if(CAN_SAVE_OPTS_PROPERTY(opt_type)) {
        if(config->write_timeout_dbl == -1) {
            return NULL;
        }
        return g_strdup_printf("%lf", config->write_timeout_dbl);
    }
    return NULL;
}

static gint
assign_proxy_write_timeout(const gchar *newval, gpointer param) {
    gint ret = ASSIGN_ERROR;
    struct external_param *opt_param = (struct external_param *)param;
    gint opt_type = opt_param->opt_type;
    if(CAN_ASSIGN_OPTS_PROPERTY(opt_type)) {
        if(NULL != newval) {
            gdouble value = 0;
            if(try_get_double_value(newval, &value)) {
                config->write_timeout_dbl = value;
                ret = ASSIGN_OK;
            } else {
                ret = ASSIGN_VALUE_INVALID;
            }
        } else {
            ret = ASSIGN_VALUE_INVALID;
        }
    }
    return ret;
}

static gchar*
show_read_master_percentage(gpointer param) {
    struct external_param *opt_param = (struct external_param *)param;
    gint opt_type = opt_param->opt_type;
    if(CAN_SHOW_OPTS_PROPERTY(opt_type)) {
        return g_strdup_printf("%d", config->read_master_percentage);
    }
    if(CAN_SAVE_OPTS_PROPERTY(opt_type)) {
        /* handle default */
        if(config->read_master_percentage == 0) {
            return NULL;
        }
        return g_strdup_printf("%d", config->read_master_percentage);
    }
    return NULL;
}

static gint
assign_read_master_percentage(const gchar *newval, gpointer param) {
    gint ret = ASSIGN_ERROR;
    struct external_param *opt_param = (struct external_param *)param;
    gint opt_type = opt_param->opt_type;
    if(CAN_ASSIGN_OPTS_PROPERTY(opt_type)) {
        if(NULL != newval) {
            gint value = 0;
            if(try_get_int_value(newval, &value)) {
                if(value >= 0 && value <= 100) {
                    config->read_master_percentage = value;
                    ret = ASSIGN_OK;
                } else {
                    ret = ASSIGN_VALUE_INVALID;
                }
            } else {
                ret = ASSIGN_VALUE_INVALID;
            }
        } else {
            ret = ASSIGN_VALUE_INVALID;
        }
    }
    return ret;
}

static gchar*
show_proxy_allow_ip(gpointer param) {
    gchar *ret = NULL;
    struct external_param *opt_param = (struct external_param *)param;
    gint opt_type = opt_param->opt_type;
    GList *list = opt_param->chas->priv->acl->whitelist;
    if(CAN_SAVE_OPTS_PROPERTY(opt_type)) {
        GString *free_str = g_string_new(NULL);
        GList *l = NULL;
        for (l = list; l; l = l->next) {
            struct cetus_acl_entry_t* entry = l->data;
            free_str = g_string_append(free_str, entry->username);
            free_str = g_string_append(free_str, "@");
            free_str = g_string_append(free_str, entry->host);
            free_str = g_string_append(free_str, ",");
        }
        if(free_str->len) {
            free_str->str[free_str->len -1] = '\0';
            ret = g_strdup(free_str->str);
        }
        g_string_free(free_str, TRUE);
    }
    return ret;
}

static gchar*
show_proxy_deny_ip(gpointer param) {
    gchar *ret = NULL;
    struct external_param *opt_param = (struct external_param *)param;
    gint opt_type = opt_param->opt_type;
    GList *list = opt_param->chas->priv->acl->blacklist;
    if(CAN_SAVE_OPTS_PROPERTY(opt_type)) {
        GString *free_str = g_string_new(NULL);
        GList *l = NULL;
        for (l = list; l; l = l->next) {
            struct cetus_acl_entry_t* entry = l->data;
            free_str = g_string_append(free_str, entry->username);
            free_str = g_string_append(free_str, "@");
            free_str = g_string_append(free_str, entry->host);
            free_str = g_string_append(free_str, ",");
        }
        if(free_str->len) {
            free_str->str[free_str->len -1] = '\0';
            ret = g_strdup(free_str->str);
        }
        g_string_free(free_str, TRUE);
    }
    return ret;
}

/**
 * plugin options
 */
static GList *
network_mysqld_proxy_plugin_get_options(chassis_plugin_config *config)
{
    chassis_options_t opts = { 0 };

    chassis_options_add(&opts, "proxy-address",
                        'P', 0, OPTION_ARG_STRING, &(config->address),
                        "listening address:port of the proxy-server (default: :4040)", "<host:port>",
                        NULL, show_proxy_address, SHOW_OPTS_PROPERTY|SAVE_OPTS_PROPERTY);

    chassis_options_add(&opts, "proxy-read-only-backend-addresses",
                        'r', 0, OPTION_ARG_STRING_ARRAY, &(config->read_only_backend_addresses),
                        "address:port of the remote slave-server (default: not set)", "<host:port>",
                        NULL, show_proxy_read_only_backend_address, SAVE_OPTS_PROPERTY);

    chassis_options_add(&opts, "proxy-backend-addresses",
                        'b', 0, OPTION_ARG_STRING_ARRAY, &(config->backend_addresses),
                        "address:port of the remote backend-servers (default: 127.0.0.1:3306)", "<host:port>",
                        NULL, show_proxy_backend_addresses, SAVE_OPTS_PROPERTY);

    chassis_options_add(&opts, "proxy-connect-timeout",
                        0, 0, OPTION_ARG_DOUBLE, &(config->connect_timeout_dbl),
                        "connect timeout in seconds (default: 2.0 seconds)", NULL,
                        assign_proxy_connect_timeout, show_proxy_connect_timeout, ALL_OPTS_PROPERTY);

    chassis_options_add(&opts, "proxy-read-timeout",
                        0, 0, OPTION_ARG_DOUBLE, &(config->read_timeout_dbl),
                        "read timeout in seconds (default: 600 seconds)", NULL,
                        assign_proxy_read_timeout, show_proxy_read_timeout, ALL_OPTS_PROPERTY);

    chassis_options_add(&opts, "proxy-write-timeout",
                        0, 0, OPTION_ARG_DOUBLE, &(config->write_timeout_dbl),
                        "write timeout in seconds (default: 600 seconds)", NULL,
                        assign_proxy_write_timeout, show_proxy_write_timeout, ALL_OPTS_PROPERTY);

    chassis_options_add(&opts, "proxy-allow-ip",
                        0, 0, OPTION_ARG_STRING, &(config->allow_ip), "allow user@IP for proxy permission", NULL,
                        NULL, show_proxy_allow_ip, SAVE_OPTS_PROPERTY);

    chassis_options_add(&opts, "proxy-deny-ip",
                        0, 0, OPTION_ARG_STRING, &(config->deny_ip), "deny user@IP for proxy permission", NULL,
                        NULL, show_proxy_deny_ip, SAVE_OPTS_PROPERTY);

    chassis_options_add(&opts, "read-master-percentage",
                        0, 0, OPTION_ARG_INT, &(config->read_master_percentage), "range [0, 100]", NULL,
                        assign_read_master_percentage, show_read_master_percentage, ALL_OPTS_PROPERTY);

    return opts.options;
}

/**
 * init the plugin with the parsed config
 */
int
network_mysqld_proxy_plugin_apply_config(chassis *chas, chassis_plugin_config *config)
{
    network_mysqld_con *con;
    network_socket *listen_sock;
    chassis_private *g = chas->priv;

    if (!config->address)
        config->address = g_strdup(":4040");
    if (!config->backend_addresses) {
        config->backend_addresses = g_new0(char *, 2);
        config->backend_addresses[0] = g_strdup("127.0.0.1:3306");
        config->backend_addresses[1] = NULL;
    }

    if (config->allow_ip) {
        cetus_acl_add_rules(g->acl, ACL_WHITELIST, config->allow_ip);
    }
    if (config->deny_ip) {
        cetus_acl_add_rules(g->acl, ACL_BLACKLIST, config->deny_ip);
    }

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
     * set the plugin hooks as we want to apply them
     * to the new connections too later
     */
    network_mysqld_proxy_connection_init(con);

    if (network_address_set_address(listen_sock->dst, config->address)) {
        return -1;
    }

    if (network_socket_bind(listen_sock, 1)) {
        return -1;
    }
    g_message("proxy listening on port %s, con:%p", config->address, con);

    plugin_add_backends(chas, config->backend_addresses, config->read_only_backend_addresses);

    /**
     * call network_mysqld_con_accept() with this connection when we are done
     */

    event_set(&(listen_sock->event), listen_sock->fd, EV_READ | EV_PERSIST, network_mysqld_con_accept, con);
    event_base_set(chas->event_base, &(listen_sock->event));
    event_add(&(listen_sock->event), NULL);
    g_debug("%s:listen sock, ev:%p", G_STRLOC, (&listen_sock->event));

    if (network_backends_load_config(g->backends, chas) != -1) {
        evtimer_set(&chas->update_timer_event, update_time_func, chas);
        struct timeval update_time_interval = {1, 0};
        chassis_event_add_with_timeout(chas, &chas->update_timer_event, &update_time_interval);

        network_connection_pool_create_conns(chas);
        evtimer_set(&chas->auto_create_conns_event, check_and_create_conns_func, chas);
        struct timeval check_interval = {30, 0};
        chassis_event_add_with_timeout(chas, &chas->auto_create_conns_event, &check_interval);
        g_debug("%s:set callback check_and_create_conns_func", G_STRLOC);
    }
    chassis_config_register_service(chas->config_manager, config->address, "proxy");

    sql_filter_vars_load_default_rules();
    char* var_json = NULL;
    if (chassis_config_query_object(chas->config_manager, "variables", &var_json, 0)) {
        g_message("reading variable rules");
        if (sql_filter_vars_load_str_rules(var_json) == FALSE) {
            g_warning("variable rule load error");
        }
        g_free(var_json);
    }
    return 0;
}

static void 
network_mysqld_proxy_plugin_stop_listening(chassis *chas,
        chassis_plugin_config *config)
{
    g_message("%s:call network_mysqld_proxy_plugin_stop_listening", G_STRLOC);
    if (config->listen_con) {
        g_message("%s:close listen socket", G_STRLOC);
        network_socket_free(config->listen_con->server);
        config->listen_con = NULL;
    }
}


G_MODULE_EXPORT int
plugin_init(chassis_plugin *p)
{
    p->magic = CHASSIS_PLUGIN_MAGIC;
    p->name = g_strdup("proxy");
    p->version = g_strdup(PLUGIN_VERSION);

    p->init = network_mysqld_proxy_plugin_new;
    p->get_options = network_mysqld_proxy_plugin_get_options;
    p->apply_config = network_mysqld_proxy_plugin_apply_config;
    p->stop_listening = network_mysqld_proxy_plugin_stop_listening;
    p->destroy = network_mysqld_proxy_plugin_free;

    return 0;
}
