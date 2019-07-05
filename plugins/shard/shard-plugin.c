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

#include <string.h>
#include <stdlib.h>
#include <glib.h>
#include <mysqld_error.h> /** for ER_UNKNOWN_ERROR */

#include "cetus-users.h"
#include "cetus-util.h"
#include "cetus-acl.h"
#include "character-set.h"
#include "chassis-event.h"
#include "chassis-options.h"
#include "cetus-monitor.h"
#include "glib-ext.h"
#include "network-backend.h"
#include "network-conn-pool.h"
#include "network-conn-pool-wrap.h"
#include "plugin-common.h"
#include "network-mysqld-packet.h"
#include "network-mysqld-proto.h"
#include "network-mysqld.h"
#include "server-session.h"
#include "shard-plugin-con.h"
#include "sharding-config.h"
#include "sharding-parser.h"
#include "sharding-query-plan.h"
#include "sql-filter-variables.h"
#include "cetus-log.h"
#include "chassis-options-utils.h"
#include "chassis-sql-log.h"

#ifndef PLUGIN_VERSION
#ifdef CHASSIS_BUILD_TAG
#define PLUGIN_VERSION CHASSIS_BUILD_TAG
#else
#define PLUGIN_VERSION PACKAGE_VERSION
#endif
#endif

#define XA_LOG_BUF_LEN 2048

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

    gdouble dist_tran_decided_read_timeout_dbl;
    /* exposed in the config as double */
    gdouble write_timeout_dbl;

    gchar *allow_ip;

    gchar *deny_ip;

    int allow_nested_subquery;
};

/**
 * handle event-timeouts on the different states
 *
 * @note con->state points to the current state
 *
 */
NETWORK_MYSQLD_PLUGIN_PROTO(proxy_timeout)
{
    int diff;
    shard_plugin_con_t *st = con->plugin_con_state;

    if (st == NULL)
        return NETWORK_SOCKET_ERROR;

    int idle_timeout = con->srv->client_idle_timeout;

    if (con->is_in_transaction) {
        idle_timeout = con->srv->incomplete_tran_idle_timeout;
    }

    if (con->srv->maintain_close_mode) {
        idle_timeout = con->srv->maintained_client_idle_timeout;
    }

    diff = con->srv->current_time - con->client->update_time + 1;

    g_debug("%s, con:%p:call proxy_timeout, state:%d, idle timeout:%d, diff:%d",
            G_STRLOC, con, con->state, idle_timeout, diff);

    switch (con->state) {
    case ST_READ_M_QUERY_RESULT:
    case ST_READ_QUERY_RESULT:
        g_warning("%s:read query result timeout", G_STRLOC);
        if (con->dist_tran) {
            if (con->dist_tran_state > NEXT_ST_XA_CANDIDATE_OVER) {
                g_critical("%s:EV_TIMEOUT, phase two, not recv response:%p", G_STRLOC, con);
            } else {
                con->dist_tran_failed = 1;
                g_critical("%s:xa tran failed here:%p, xa state:%d, xid:%s",
                           G_STRLOC, con, con->dist_tran_state, con->xid_str);
            }
        }

        con->server_to_be_closed = 1;
        con->prev_state = con->state;
        con->state = ST_ERROR;
        break;
    default:
        if (diff < idle_timeout) {
            if (!con->client->is_server_conn_reserved) {
                g_debug("%s, is_server_conn_reserved is false", G_STRLOC);
                if (con->servers && con->servers->len > 0) {
                    g_debug("%s, server conns returned to pool", G_STRLOC);
                    proxy_put_shard_conn_to_pool(con);
                }
            }
        } else {
            g_message("%s, client timeout, closing, diff:%d, con:%p", G_STRLOC, diff, con);
            con->prev_state = con->state;
            con->state = ST_ERROR;
        }
        break;
    }
    return NETWORK_SOCKET_SUCCESS;
}

NETWORK_MYSQLD_PLUGIN_PROTO(proxy_read_auth)
{
    return do_read_auth(con);
}

static int
process_other_set_command(network_mysqld_con *con, const char *key, const char *s, mysqld_query_attr_t *query_attr)
{
    g_debug("%s: vist process_other_set_command", G_STRLOC);
    con->conn_attr_check_omit = 1;
    network_socket *sock = con->client;
    size_t s_len = strlen(s);

    if (strcasecmp(key, "sql_mode") == 0) {
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

    con->conn_attr_check_omit = 1;

    query_attr->charset_set = 1;
    sock->charset_code = charset_get_number(s);
    return 0;
}

static int proxy_parse_query(network_mysqld_con *con);
static int proxy_get_server_list(network_mysqld_con *con);

static int
check_backends_attr_changed(network_mysqld_con *con)
{
    int server_attr_changed = 0;
    size_t i;

    for (i = 0; i < con->servers->len; i++) {
        server_session_t *ss = g_ptr_array_index(con->servers, i);
        if (ss->backend->type != con->last_backends_type[i]) {
            g_message("%s backend type:%d, record type:%d",
                    G_STRLOC, ss->backend->type, con->last_backends_type[i]);
            server_attr_changed = 1;
            break;
        }

        if (ss->backend->state != BACKEND_STATE_UP && ss->backend->state != BACKEND_STATE_UNKNOWN) {
            server_attr_changed = 1;
            g_message("%s backend state:%d", G_STRLOC, ss->backend->state);
        }
    }

    return server_attr_changed;
}

static void
network_mysqld_con_purify_sharding_plan(struct sharding_plan_t *sharding_plan)
{
    sharding_plan->modified_sql = NULL;
    sharding_plan->is_modified = 0;
}

NETWORK_MYSQLD_PLUGIN_PROTO(proxy_read_query)
{
    GQueue *chunks = con->client->recv_queue->chunks;
    network_packet p;
    p.data = g_queue_peek_head(chunks);
    if (p.data == NULL) {
        g_critical("%s: packet data is nil", G_STRLOC);
        network_mysqld_con_send_error(con->client, C("(proxy) unable to process command"));
        con->state = ST_SEND_QUERY_RESULT;
        network_mysqld_queue_reset(con->client);
        return NETWORK_SOCKET_SUCCESS;
    }

    p.offset = 0;

    network_mysqld_con_reset_command_response_state(con);
    network_mysqld_con_reset_query_state(con);

    if (con->sharding_plan) {
        if (con->sharding_plan->is_modified) {
            g_critical("%s: sharding_plan's sql is modified for con:%p", G_STRLOC, con);
            network_mysqld_con_purify_sharding_plan(con->sharding_plan);
        }
    }

    g_debug("%s: call network_mysqld_con_command_states_init", G_STRLOC);
    if (network_mysqld_con_command_states_init(con, &p)) {
        g_warning("%s: tracking mysql proto states failed", G_STRLOC);
        con->prev_state = con->state;
        con->state = ST_ERROR;
        return NETWORK_SOCKET_SUCCESS;
    }
    int is_process_stopped = 0;
    int rc;

    if (con->servers != NULL) {
        is_process_stopped = check_backends_attr_changed(con);
        if (is_process_stopped) {
            if (!con->client->is_server_conn_reserved) {
                is_process_stopped = 0;
                proxy_put_shard_conn_to_pool(con);
                g_debug("%s server attr changed, but process continues", G_STRLOC);
            } else {
                network_mysqld_con_send_error(con->client, C("(proxy) unable to continue processing command"));
                rc = PROXY_SEND_RESULT;
                network_mysqld_con_clear_xa_env_when_not_expected(con);
                g_message("%s server attr changed", G_STRLOC);
            }
        }
    }

    if (!is_process_stopped) {
        rc = proxy_parse_query(con);
        log_sql_client(con);
    }

    switch (rc) {
    case PROXY_NO_DECISION:
        break;                  /* go on to get groups */
    case PROXY_SEND_RESULT:
        con->state = ST_SEND_QUERY_RESULT;
        network_queue_clear(con->client->recv_queue);
        network_mysqld_queue_reset(con->client);
        return NETWORK_SOCKET_SUCCESS;
    case PROXY_SEND_NONE:
        network_queue_clear(con->client->recv_queue);
        network_mysqld_queue_reset(con->client);
        return NETWORK_SOCKET_SUCCESS;
    default:
        g_assert(0);
        break;
    }

    if (con->srv->query_cache_enabled) {
        shard_plugin_con_t *st = con->plugin_con_state;
        if (sql_context_is_cacheable(st->sql_context)) {
            if (!con->is_in_transaction && !con->srv->master_preferred &&
                !(st->sql_context->rw_flag & CF_FORCE_MASTER) && !(st->sql_context->rw_flag & CF_FORCE_SLAVE)) {
                if (try_to_get_resp_from_query_cache(con)) {
                    return NETWORK_SOCKET_SUCCESS;
                }
            }
        }
    }

    con->master_conn_shortaged = 0;
    con->slave_conn_shortaged = 0;
    con->use_slave_forced = 0;

    rc = proxy_get_server_list(con);

    switch (rc) {
    case RET_SUCCESS:
        if (con->use_all_prev_servers || (con->sharding_plan && con->sharding_plan->groups->len > 0)) {
            con->state = ST_GET_SERVER_CONNECTION_LIST;
        } else {
            con->state = ST_SEND_QUERY_RESULT;
            if (con->buffer_and_send_fake_resp) {
                network_mysqld_con_send_ok_full(con->client, 0, 0, 0, 0);
                g_debug("%s: send faked resp to client", G_STRLOC);
            } else {
                network_mysqld_con_send_error_full(con->client, C("no group yet"), ER_NO_DB_ERROR, "3D000");
                g_debug("%s: no group yet for this query", G_STRLOC);
            }
            network_queue_clear(con->client->recv_queue);
            network_mysqld_queue_reset(con->client);
        }
        break;
    case PROXY_SEND_RESULT:
        con->state = ST_SEND_QUERY_RESULT;
        network_queue_clear(con->client->recv_queue);
        network_mysqld_queue_reset(con->client);
        break;
    case PROXY_NO_DECISION:
        con->state = ST_GET_SERVER_CONNECTION_LIST;
        break;

    default:
        g_critical("%s: plugin(GET_SERVER_LIST) failed", G_STRLOC);
        con->state = ST_ERROR;
        break;
    }
    return NETWORK_SOCKET_SUCCESS;
}

static void
mysqld_con_send_sequence(network_mysqld_con *con)
{
    char buffer[32];
    chassis *srv = con->srv;
    uint64_t uniq_id = incremental_guid_get_next(&(srv->guid_state));
    snprintf(buffer, sizeof(buffer), "%llu", (unsigned long long)uniq_id);

    GPtrArray *fields = network_mysqld_proto_fielddefs_new();

    MYSQL_FIELD *field = network_mysqld_proto_fielddef_new();
    field->name = "SEQUENCE";
    field->type = MYSQL_TYPE_LONGLONG;
    g_ptr_array_add(fields, field);

    GPtrArray *rows = g_ptr_array_new();
    GPtrArray *row = g_ptr_array_new();
    g_ptr_array_add(row, buffer);
    g_ptr_array_add(rows, row);

    network_mysqld_con_send_resultset(con->client, fields, rows);

    network_mysqld_proto_fielddefs_free(fields);
    g_ptr_array_free(row, TRUE);
    g_ptr_array_free(rows, TRUE);
}

static const GString *
sharding_get_sql(network_mysqld_con *con, GString *group)
{
    if (!con->srv->is_partition_mode || con->sharding_plan->is_sql_rewrite_completely) {
        return sharding_plan_get_sql(con->sharding_plan, group);
    } else {
        g_debug("%s: first group:%s, now group:%s for con:%p", G_STRLOC, con->first_group->str, group->str, con);
        if (g_string_equal(con->first_group, group)) {
            const GString *new_sql = sharding_plan_get_sql(con->sharding_plan, group);
            if (new_sql == NULL) {
                new_sql = con->orig_sql;
            }
            return new_sql;

        } else {
            shard_plugin_con_t *st = con->plugin_con_state;
            sql_context_t *context = st->sql_context;
            GString *new_sql = sharding_modify_sql(context, &(con->hav_condi),
                    con->srv->is_groupby_need_reconstruct, con->srv->is_partition_mode, con->sharding_plan->groups->len);
            if (new_sql) {
                sharding_plan_add_group_sql(con->sharding_plan, group, new_sql);
                g_debug("%s: new sql:%s for con:%p", G_STRLOC, new_sql->str, con);
            } else {
                new_sql = con->orig_sql;
            }
            return new_sql;
        }
    }
}

static int
explain_shard_sql(network_mysqld_con *con, sharding_plan_t *plan)
{
    int rv = 0;

    if (con->client->default_db->len == 0) {
        if (con->srv->default_db != NULL) {
            g_string_assign(con->client->default_db, con->srv->default_db);
            g_debug("%s:set client default db:%s for con:%p", G_STRLOC, con->client->default_db->str, con);
        }
    }

    shard_plugin_con_t *st = con->plugin_con_state;

    rv = sharding_parse_groups(con->client->default_db, st->sql_context, &(con->srv->query_stats),
            con->key, plan);

    con->modified_sql = sharding_modify_sql(st->sql_context, &(con->hav_condi),
            con->srv->is_groupby_need_reconstruct, con->srv->is_partition_mode, plan->groups->len);
    if (con->modified_sql) {
        sharding_plan_set_modified_sql(plan, con->modified_sql);
    }

    sharding_plan_sort_groups(plan);
    int abnormal = 0;
    if (rv == ERROR_UNPARSABLE) {
        const char *msg = st->sql_context->message ? : "sql parse error";
        network_mysqld_con_send_error_full(con->client, L(msg), ER_CETUS_PARSE_SHARDING, "HY000");
        g_message(G_STRLOC ": unparsable sql:%s", con->orig_sql->str);
        abnormal = 1;
    }
    return abnormal;
}

static void
proxy_generate_shard_explain_packet(network_mysqld_con *con)
{
    sharding_plan_t *plan = sharding_plan_new(con->orig_sql);
    plan->is_partition_mode = con->srv->is_partition_mode;
    if (explain_shard_sql(con, plan) != 0) {
        sharding_plan_free(plan);
        return;
    }

    GPtrArray *fields = network_mysqld_proto_fielddefs_new();

    MYSQL_FIELD *field1 = network_mysqld_proto_fielddef_new();
    field1->name = "groups";
    field1->type = MYSQL_TYPE_VAR_STRING;
    g_ptr_array_add(fields, field1);
    MYSQL_FIELD *field2 = network_mysqld_proto_fielddef_new();
    field2->name = "sql";
    field2->type = MYSQL_TYPE_VAR_STRING;
    g_ptr_array_add(fields, field2);

    GPtrArray *rows;
    rows = g_ptr_array_new_with_free_func((void *)network_mysqld_mysql_field_row_free);

    struct sharding_plan_t *sharding_plan = con->sharding_plan;
    con->sharding_plan = plan;

    int i;
    for (i = 0; i < plan->groups->len; i++) {
        GPtrArray *row = g_ptr_array_new();

        GString *group = g_ptr_array_index(plan->groups, i);
        if  (i == 0) {
            con->first_group = group;
        }
        g_ptr_array_add(row, group->str);
        const GString *sql = sharding_get_sql(con, group);
        g_ptr_array_add(row, sql->str);

        g_ptr_array_add(rows, row);
    }

    network_mysqld_con_send_resultset(con->client, fields, rows);

    network_mysqld_proto_fielddefs_free(fields);
    g_ptr_array_free(rows, TRUE);
    sharding_plan_free(plan);
    con->sharding_plan = sharding_plan;

}

static int
analysis_query(network_mysqld_con *con, mysqld_query_attr_t *query_attr)
{
    shard_plugin_con_t *st = con->plugin_con_state;
    sql_context_t *context = st->sql_context;

    switch (context->stmt_type) {
    case STMT_SELECT:{
        if (!con->dist_tran) {
            if (con->srv->is_tcp_stream_enabled) {
                g_debug("%s: con dist tran is false", G_STRLOC);
                con->could_be_tcp_streamed = 1;
            }
            if (con->srv->is_fast_stream_enabled) {
                con->could_be_fast_streamed = 1;
            }
        }
        sql_select_t *select = (sql_select_t *)context->sql_statement;

        if (con->could_be_tcp_streamed) {
            if (sql_expr_list_find_aggregate(select->columns, NULL) != -1) {
                con->could_be_tcp_streamed = 0;
                g_debug("%s: con tcp stream false", G_STRLOC);
            }
        }

        gboolean is_insert_id = FALSE;
        sql_expr_list_t *cols = select->columns;
        if (cols && cols->len > 0) {
            sql_expr_t *col = g_ptr_array_index(cols, 0);
            if (sql_expr_is_function(col, "LAST_INSERT_ID")) {
                is_insert_id = TRUE;
            } else if (sql_expr_is_id(col, "LAST_INSERT_ID")) {
                is_insert_id = TRUE;
            }
        }
        if (is_insert_id == TRUE) {
            g_debug("%s: return last insert id", G_STRLOC);
            /* TODO last insert id processing */
        }
        break;
    }
    case STMT_SET_NAMES:{
        char *charset_name = (char *)context->sql_statement;
        process_set_names(con, charset_name, query_attr);
        g_debug("%s: set names", G_STRLOC);
        break;
    }
    case STMT_SET_TRANSACTION:
        if (sql_filter_vars_is_silent("TRANSACTION", "*")) {
            network_mysqld_con_send_ok(con->client);
        } else {
            network_mysqld_con_send_error_full(con->client,
                                               L("(cetus) SET TRANSACTION not supported"),
                                               ER_CETUS_NOT_SUPPORTED, "HY000");
        }
        return PROXY_SEND_RESULT;
    case STMT_SET:{
        sql_expr_list_t *set_list = context->sql_statement;
        if (set_list && set_list->len > 0) {
            sql_expr_t *expr = g_ptr_array_index(set_list, 0);
            if (expr->op == TK_EQ) {
                const char *lhs = sql_expr_id(expr->left);
                const char *rhs = sql_expr_id(expr->right);

                if (sql_filter_vars_is_silent(lhs, rhs)) {
                    network_mysqld_con_send_ok(con->client);
                    g_string_free(g_queue_pop_tail(con->client->recv_queue->chunks), TRUE);
                    g_message("silent variable: %s", lhs);
                    return PROXY_SEND_RESULT;
                }

                /* set autocomit = x */
                if (sql_context_is_autocommit_on(context)) {
                    con->is_auto_commit = 1;
                    con->is_auto_commit_trans_buffered = 0;
                    g_debug("%s: autocommit on", G_STRLOC);
                } else if (sql_context_is_autocommit_off(context)) {
                    con->is_auto_commit = 0;
                    con->is_auto_commit_trans_buffered = 1;
                    g_debug("%s: autocommit off, now in transaction", G_STRLOC);
                } else {
                    if (lhs && rhs) {
                        process_other_set_command(con, lhs, rhs, query_attr);
                    }
                }
            }
        }

        break;
    }
    case STMT_COMMIT:
        con->is_commit_or_rollback = 1;
        break;
    case STMT_ROLLBACK:
        con->is_commit_or_rollback = 1;
        con->is_rollback = 1;
        break;
    case STMT_USE:{
        char *dbname = (char *)context->sql_statement;
        g_string_assign(con->client->default_db, dbname);
        g_debug("%s:set default db:%s for con:%p", G_STRLOC, con->client->default_db->str, con);
        break;
    }
    case STMT_START:
        if (con->is_auto_commit) {
            g_debug("%s: start transaction command here", G_STRLOC);
            con->is_start_trans_buffered = 1;
            con->is_start_tran_command = 1;
            con->is_auto_commit = 0;
        }
        break;
    default:
        break;
    }
    return PROXY_NO_DECISION;
}

static int
shard_handle_local_query(network_mysqld_con *con, sql_context_t *context)
{
    /* currently 3 kinds of local query */
    if (context->explain == TK_SHARD_EXPLAIN) {
        proxy_generate_shard_explain_packet(con);
        return PROXY_SEND_RESULT;
    }
    g_assert(context->stmt_type == STMT_SELECT);
    sql_select_t *select = context->sql_statement;
    sql_expr_t *col = g_ptr_array_index(select->columns, 0);
    if (sql_expr_is_function(col, "CURRENT_DATE")) {
        network_mysqld_con_send_current_date(con->client, "CURRENT_DATE");
    } else if (sql_expr_is_function(col, "CETUS_SEQUENCE")) {
        mysqld_con_send_sequence(con);
    } else if (sql_expr_is_function(col, "CETUS_VERSION")) {
        network_mysqld_con_send_cetus_version(con->client);
    }
    return PROXY_SEND_RESULT;
}

static int
proxy_parse_query(network_mysqld_con *con)
{
    shard_plugin_con_t *st = con->plugin_con_state;

    g_debug("%s: call proxy_parse_query:%p", G_STRLOC, con);

    if (con->is_commit_or_rollback) {   /* previous sql */
        if (!con->is_auto_commit) {
            con->is_auto_commit_trans_buffered = 1;
        }
        if (con->dist_tran_state >= NEXT_ST_XA_START && con->dist_tran_state != NEXT_ST_XA_OVER) {
            g_warning("%s: xa is not over yet:%p, xa state:%d", G_STRLOC, con, con->dist_tran_state);
            if (con->server && con->servers->len > 0) {
                con->server_to_be_closed = 1;
            }
        } else if (con->dist_tran_xa_start_generated && !con->dist_tran_decided) {
            if (con->servers && con->servers->len > 0) {
                con->server_to_be_closed = 1;
                g_message("%s: server conn should be closed:%p", G_STRLOC, con);
            }
        }
        con->client->is_server_conn_reserved = 0;
        g_debug("%s: set is_server_conn_reserved false:%p", G_STRLOC, con);
    } else {
        g_debug("%s: is_commit_or_rollback is false:%p", G_STRLOC, con);
    }

    con->conn_attr_check_omit = 0;
    con->is_commit_or_rollback = 0;
    con->is_rollback = 0;
    con->is_timeout = 0;
    con->is_xa_query_sent = 0;
    con->xa_query_status_error_and_abort = 0;
    con->could_be_tcp_streamed = 0;
    con->could_be_fast_streamed = 0;
    con->candidate_tcp_streamed = 0;
    con->candidate_fast_streamed = 0;
    con->process_through_special_tunnel = 0;

    network_packet packet;
    packet.data = g_queue_peek_head(con->client->recv_queue->chunks);
    packet.offset = 0;
    if (packet.data != NULL) {
        guint8 command;
        network_mysqld_proto_skip_network_header(&packet);
        if (network_mysqld_proto_get_int8(&packet, &command) != 0) {
            network_mysqld_con_send_error(con->client, C("(proxy) unable to retrieve command"));
            return PROXY_SEND_RESULT;
        }

        con->parse.command = command;
        g_debug("%s:command:%d", G_STRLOC, command);
        switch (command) {
        case COM_QUERY:{
            gsize sql_len = packet.data->len - packet.offset;
            network_mysqld_proto_get_gstr_len(&packet, sql_len, con->orig_sql);
            g_string_append_c(con->orig_sql, '\0'); /* 2 more NULL for lexer EOB */
            g_string_append_c(con->orig_sql, '\0');

            g_debug("%s: sql:%s", G_STRLOC, con->orig_sql->str);
            sql_context_t *context = st->sql_context;
            sql_context_parse_len(context, con->orig_sql);

            if (context->rc == PARSE_SYNTAX_ERR) {
                if (con->srv->is_sql_special_processed) {
                    if (check_property_has_groups(context)) {
                        con->process_through_special_tunnel = 1;
                        return PROXY_NO_DECISION;
                    }
                }
                char *msg = context->message;
                g_message("%s SQL syntax error: %s. while parsing: %s", G_STRLOC, msg, con->orig_sql->str);
                network_mysqld_con_send_error_full(con->client, msg, strlen(msg), ER_SYNTAX_ERROR, "42000");
                return PROXY_SEND_RESULT;
            } else if (context->rc == PARSE_NOT_SUPPORT) {
                char *msg = context->message;
                g_message("%s SQL unsupported: %s. while parsing: %s, clt:%s",
                          G_STRLOC, msg, con->orig_sql->str, con->client->src->name->str);
                network_mysqld_con_send_error_full(con->client, msg, strlen(msg), ER_CETUS_NOT_SUPPORTED, "HY000");
                return PROXY_SEND_RESULT;
            }
            /* forbid force write on slave */
            if ((context->rw_flag & CF_FORCE_SLAVE) && ((context->rw_flag & CF_WRITE) || con->is_in_transaction)) {
                g_message("%s Comment usage error. SQL: %s", G_STRLOC, con->orig_sql->str);
                if (con->is_in_transaction) {
                    network_mysqld_con_send_error(con->client, C("Force transaction on read-only slave"));
                } else {
                    network_mysqld_con_send_error(con->client, C("Force write on read-only slave"));
                }
                return PROXY_SEND_RESULT;
            }

            if (context->clause_flags & CF_LOCAL_QUERY) {
                return shard_handle_local_query(con, context);
            }
            memset(&(con->query_attr), 0, sizeof(mysqld_query_attr_t));
            return analysis_query(con, &(con->query_attr));
        }
        case COM_INIT_DB:
            break;
        case COM_QUIT:
            g_debug("%s: quit command:%d", G_STRLOC, command);
            con->state = ST_CLOSE_CLIENT;
            return PROXY_SEND_NONE;
        case COM_STMT_PREPARE:{
            network_mysqld_con_send_error_full(con->client,
                                               C("sharding proxy does not support prepare stmt"),
                                               ER_CETUS_NOT_SUPPORTED, "HY000");
            return PROXY_SEND_RESULT;
        }
        case COM_PING:
            network_mysqld_con_send_ok(con->client);
            return PROXY_SEND_RESULT;
        default:{
            GString *sql = g_string_new(NULL);
            GString *data = g_queue_peek_head(con->client->recv_queue->chunks);
            g_string_append_len(sql, data->str + (NET_HEADER_SIZE + 1), data->len - (NET_HEADER_SIZE + 1));
            network_mysqld_con_send_error_full(con->client,
                                               C("sharding proxy does not support this command now"),
                                               ER_CETUS_NOT_SUPPORTED, "HY000");
            g_warning("%s: unknown command:%d, sql:%s", G_STRLOC, command, sql->str);
            g_string_free(sql, TRUE);
            return PROXY_SEND_RESULT;
        }
        }
    } else {
        g_warning("%s: chunk is null", G_STRLOC);
    }

    return PROXY_NO_DECISION;
}

static int
wrap_check_sql(network_mysqld_con *con, struct sql_context_t *sql_context)
{
    if (con->srv->is_partition_mode && sql_context->stmt_type != STMT_SELECT &&
            con->sharding_plan->table_type == GLOBAL_TABLE)
    {
        g_debug("%s:don't change sql for: %s", G_STRLOC, con->orig_sql->str);
        return 0;
    }

    if (con->sharding_plan->is_sql_rewrite_completely) {
        g_debug("%s:don't change sql for: %s", G_STRLOC, con->orig_sql->str);
        return 0;
    }

    con->modified_sql = sharding_modify_sql(sql_context, &(con->hav_condi),
            con->srv->is_groupby_need_reconstruct, con->srv->is_partition_mode, con->sharding_plan->groups->len);
    if (con->modified_sql) {
        g_debug("orig_sql: %s", con->orig_sql->str);
        g_debug("modified:  %s", con->modified_sql->str);
    }
    if (con->modified_sql) {
        con->sql_modified = 1;
        sharding_plan_set_modified_sql(con->sharding_plan, con->modified_sql);
    }

    return con->sql_modified;
}

static void
record_last_backends_type(network_mysqld_con *con)
{
    size_t i;

    g_debug("%s record_last_backends_type", G_STRLOC);
    for (i = 0; i < con->servers->len; i++) {
        server_session_t *ss = g_ptr_array_index(con->servers, i);
        con->last_backends_type[i] = ss->backend->type;
    }
}

static void
generate_sql(network_mysqld_con *con)
{
    size_t i;

    for (i = 0; i < con->servers->len; i++) {
        server_session_t *ss = g_ptr_array_index(con->servers, i);
        if (!con->is_commit_or_rollback && !ss->participated) {
            continue;
        } 
        ss->sql = sharding_get_sql(con, ss->server->group);
    }
}


static void
remove_ro_servers(network_mysqld_con *con)
{
    int has_rw_server = 0;
    int has_ro_server = 0;
    size_t i;
    GPtrArray *new_servers = NULL;

    g_debug("%s: call remove_ro_servers:%p", G_STRLOC, con);

    for (i = 0; i < con->servers->len; i++) {
        server_session_t *ss = g_ptr_array_index(con->servers, i);
        if (!ss->server->is_read_only) {
            has_rw_server = 1;
            break;
        } else {
            has_ro_server = 1;
        }
    }

    if (!has_ro_server) {
        g_debug("%s: has no ro server:%p", G_STRLOC, con);
        return;
    }

    if (has_rw_server) {
        g_debug("%s: has rw server:%p", G_STRLOC, con);
        new_servers = g_ptr_array_new();
    }

    g_debug("%s: check servers:%p", G_STRLOC, con);
    for (i = 0; i < con->servers->len; i++) {
        server_session_t *ss = g_ptr_array_index(con->servers, i);
        if (ss->server->is_read_only) {
            network_connection_pool *pool = ss->backend->pool;
            network_socket *server = ss->server;

            CHECK_PENDING_EVENT(&(server->event));

            if (con->srv->server_conn_refresh_time <= server->create_time) {
                network_pool_add_idle_conn(pool, con->srv, server);
            } else {
                g_message("%s: old connection for con:%p", G_STRLOC, con);
                network_socket_send_quit_and_free(server);
                con->srv->complement_conn_flag = 1;
            }
            ss->backend->connected_clients--;
            g_debug("%s: conn clients sub, total len:%d, back:%p, value:%d con:%p, s:%p",
                    G_STRLOC, con->servers->len, ss->backend, ss->backend->connected_clients, con, server);

            ss->sql = NULL;
            g_free(ss);
        } else {
            ss->server->parse.qs_state = PARSE_COM_QUERY_INIT;
            g_ptr_array_add(new_servers, ss);
        }
    }

    gpointer *pdata = g_ptr_array_free(con->servers, FALSE);
    g_free(pdata);
    if (has_rw_server) {
        con->servers = new_servers;
    } else {
        con->servers = NULL;
    }
}

static void
network_mysqld_con_set_sharding_plan(network_mysqld_con *con, sharding_plan_t *plan)
{
    if (con->sharding_plan) {
        sharding_plan_free(con->sharding_plan);
    }
    con->sharding_plan = plan;
}

static int
process_init_db_when_get_server_list(network_mysqld_con *con, sharding_plan_t *plan, int *rv, int *disp_flag)
{
    GPtrArray *groups = g_ptr_array_new();

    network_packet packet;
    packet.data = g_queue_peek_head(con->client->recv_queue->chunks);
    packet.offset = NET_HEADER_SIZE + 1;
    int name_len = network_mysqld_proto_get_packet_len(packet.data);
    char *db_name = NULL;

    if (name_len > PACKET_LEN_MAX) {
        g_warning("%s: name len is too long:%d", G_STRLOC, name_len);
    } else {
        name_len = name_len - 1;
        network_mysqld_proto_get_str_len(&packet, &db_name, name_len);
        shard_conf_get_fixed_group(plan->is_partition_mode, groups, con->key);
    }

    if (groups->len > 0) {      /* has database */
        if (con->dist_tran) {
            *rv = USE_PREVIOUS_TRAN_CONNS;
        } else {
            g_string_assign_len(con->client->default_db, db_name, name_len);
            sharding_plan_add_groups(plan, groups);
            g_ptr_array_free(groups, TRUE);
            network_mysqld_con_set_sharding_plan(con, plan);
            *disp_flag = PROXY_NO_DECISION;
            if (db_name)
                g_free(db_name);

            return 0;
        }
    } else {
        network_mysqld_con_send_error(con->client, C("not a configured DB"));
        GString *data = g_queue_pop_head(con->client->recv_queue->chunks);
        g_string_free(data, TRUE);
        g_ptr_array_free(groups, TRUE);
        sharding_plan_free(plan);
        *disp_flag = PROXY_SEND_RESULT;
        if (db_name)
            g_free(db_name);

        return 0;
    }

    if (db_name)
        g_free(db_name);

    return 1;
}

static void
before_get_server_list(network_mysqld_con *con)
{

    shard_plugin_con_t *st = con->plugin_con_state;

    if (con->is_start_trans_buffered || con->is_auto_commit_trans_buffered) {
        if (con->last_warning_met) {
            con->last_warning_met = 0;
            if (con->is_in_transaction) {
                g_warning("%s: is_in_transaction true for con:%p", G_STRLOC, con);
            }
            con->server_closed = 1;
            con->client->is_server_conn_reserved = 0;
        }
        con->is_in_transaction = 1;
        con->dist_tran_xa_start_generated = 0;
        if (sql_context_is_single_node_trx(st->sql_context)) {
            con->is_tran_not_distributed_by_comment = 1;
            g_debug("%s: set is_tran_not_distributed_by_comment true:%p", G_STRLOC, con);
        }

        g_debug("%s: check is_server_conn_reserved:%p", G_STRLOC, con);
        if (!con->client->is_server_conn_reserved) {
            g_debug("%s: is_server_conn_reserved false:%p", G_STRLOC, con);
            if (con->servers && con->servers->len > 0) {
                g_debug("%s: call proxy_put_shard_conn_to_pool:%p", G_STRLOC, con);
                proxy_put_shard_conn_to_pool(con);
            }
        } else {
            g_message("%s: still hold conn when starting a new transaction:%p", G_STRLOC, con);
        }
    }

    if (con->dist_tran_decided) {
        if (con->servers && con->servers->len > 0) {
            g_debug("%s: call proxy_put_shard_conn_to_pool:%p", G_STRLOC, con);
            proxy_put_shard_conn_to_pool(con);
        }
        con->dist_tran_xa_start_generated = 0;
    }

    if (con->sharding_plan) {
        if (con->servers == NULL || con->servers->len == 0) {
            if (con->sharding_plan) {
                sharding_plan_free(con->sharding_plan);
                g_debug("%s: call sharding_plan_free here:%p", G_STRLOC, con);
                con->sharding_plan = NULL;
            }
        } else {
            sharding_plan_free_map(con->sharding_plan);
        }
    }
}

static void
process_rv_use_none(network_mysqld_con *con, sharding_plan_t *plan, int *disp_flag)
{
    /* SET AUTOCOMMIT = 0 || START TRANSACTION */
    con->delay_send_auto_commit = 1;
    g_debug("%s: delay send autocommit 0", G_STRLOC);
    network_mysqld_con_set_sharding_plan(con, plan);
    GString *packet = g_queue_pop_head(con->client->recv_queue->chunks);
    g_string_free(packet, TRUE);
    network_mysqld_con_send_ok_full(con->client, 0, 0, 0, 0);
    *disp_flag = PROXY_SEND_RESULT;
}

static int
process_rv_use_same(network_mysqld_con *con, sharding_plan_t *plan, int *disp_flag)
{
    /* SET AUTOCOMMIT = 1 */
    con->is_auto_commit = 1;
    if (con->dist_tran && con->servers && con->servers->len > 0 &&
            con->dist_tran_state < NEXT_ST_XA_END)
    {
        con->client->is_server_conn_reserved = 0;
        con->is_commit_or_rollback = 1;
        g_message("%s: no commit when set autocommit = 1:%p", G_STRLOC, con);
    } else {
        con->delay_send_auto_commit = 0;
        network_mysqld_con_set_sharding_plan(con, plan);
        g_debug("%s: no need to send autocommit true", G_STRLOC);
        GString *packet = g_queue_pop_head(con->client->recv_queue->chunks);
        g_string_free(packet, TRUE);
        con->is_in_transaction = 0;
        con->dist_tran = 0;
        con->client->is_server_conn_reserved = 0;
        network_mysqld_con_send_ok_full(con->client, 0, 0, 0, 0);
        *disp_flag = PROXY_SEND_RESULT;
        return 0;
    }

    return 1;
}

static int
process_rv_use_previous_tran_conns(network_mysqld_con *con, sharding_plan_t *plan, int *rv, int *disp_flag)
{
    /* COMMIT/ROLLBACK */
    g_debug("%s: use previous conn for con:%p", G_STRLOC, con);

    if (con->is_auto_commit || con->servers == NULL || con->servers->len == 0) {
        con->buffer_and_send_fake_resp = 1;
        con->delay_send_auto_commit = 0;
        con->is_auto_commit_trans_buffered = 0;
        con->is_start_trans_buffered = 0;
        g_debug("%s: buffer_and_send_fake_resp set true:%p", G_STRLOC, con);
    } else {
        if (con->servers->len > 1) {
            if (!con->dist_tran) {
                network_mysqld_con_send_ok_full(con->client, 0, 0, 0, 0);
                g_debug("%s: set ERROR_DUP_COMMIT_OR_ROLLBACK here", G_STRLOC);
                sharding_plan_free(plan);
                *disp_flag = PROXY_SEND_RESULT;
                return 0;
            } else {
                *rv = USE_DIS_TRAN;
                con->use_all_prev_servers = 1;
            }
        } else {
            if (!con->dist_tran) {
                if (!con->is_tran_not_distributed_by_comment) {
                    network_mysqld_con_send_ok_full(con->client, 0, 0, 0, 0);
                    g_debug("%s: set ERROR_DUP_COMMIT_OR_ROLLBACK here", G_STRLOC);
                    sharding_plan_free(plan);
                    *disp_flag = PROXY_SEND_RESULT;
                    return 0;
                }
            } else {
                *rv = USE_DIS_TRAN;
                con->use_all_prev_servers = 1;
            }
        }
    }

    if (con->servers != NULL && con->servers->len > 0) {
        sharding_plan_free(plan);
    } else {
        network_mysqld_con_set_sharding_plan(con, plan);
    }

    return 1;
}

static int
process_rv_default(network_mysqld_con *con, sharding_plan_t *plan, int *rv, int *disp_flag)
{
    g_debug("%s: process_rv_default is called", G_STRLOC);
    if (con->is_tran_not_distributed_by_comment) {
        g_debug("%s: default prcessing here for conn:%p", G_STRLOC, con);

        int valid_single_tran = 1;
        if (plan->groups->len != 1) {
            valid_single_tran = 0;
            g_debug("%s: group num:%d for con:%p", G_STRLOC, plan->groups->len, con);
        } else {
            if (con->sharding_plan) {
                if (con->sharding_plan->groups->len == 1) {
                    GString *prev_group = g_ptr_array_index(con->sharding_plan->groups, 0);
                    GString *cur_group = g_ptr_array_index(plan->groups, 0);
                    if (strcasecmp(prev_group->str, cur_group->str) != 0) {
                        valid_single_tran = 0;
                    }
                } else if (con->sharding_plan->groups->len > 1) {
                    valid_single_tran = 0;
                    g_debug("%s: orig group num:%d for con:%p", G_STRLOC, con->sharding_plan->groups->len, con);
                }
            }
        }

        if (!valid_single_tran) {
            sharding_plan_free(plan);
            g_message("%s: tran conflicted here for con:%p", G_STRLOC, con);
            network_mysqld_con_send_error_full(con->client,
                                               C("conflict with stand-alone tran comment"),
                                               ER_CETUS_SINGLE_NODE_FAIL, "HY000");
            *disp_flag = PROXY_SEND_RESULT;
            return 0;
        } else {
            network_mysqld_con_set_sharding_plan(con, plan);
        }
    } else {
        network_mysqld_con_set_sharding_plan(con, plan);
        if (plan->groups->len >= 2) {
            if (!con->is_auto_commit || con->is_start_tran_command) {   /* current sql START ? */
                *rv = USE_DIS_TRAN;
            } else if (*rv == USE_DIS_TRAN) {
                g_debug("%s: user distributed trans found for sql:%s", G_STRLOC, con->orig_sql->str);
                con->dist_tran_xa_start_generated = 0;
            } else {
                con->delay_send_auto_commit = 0;
                g_debug("%s: not in transaction:%s", G_STRLOC, con->orig_sql->str);
            }
        } else {
            if (con->dist_tran) {
                g_debug("%s: xa transaction", G_STRLOC);
                if (plan->groups->len == 0 && *rv != ERROR_UNPARSABLE) {
                    network_mysqld_con_send_error_full(con->client,
                                                       C("Cannot find backend groups"), ER_CETUS_NO_GROUP, "HY000");
                    sharding_plan_free(plan);
                    con->sharding_plan = NULL;  /* already ref by con, remove! */
                    *disp_flag = PROXY_SEND_RESULT;
                    return 0;
                }
            } else {
                g_debug("%s: check if it is a xa transaction", G_STRLOC);
                if (plan->groups->len == 1 && (!con->is_auto_commit)) {
                    con->delay_send_auto_commit = 0;
                    *rv = USE_DIS_TRAN;
                }
            }
        }
    }

    return 1;
}

static int
make_first_decision(network_mysqld_con *con, sharding_plan_t *plan, int *rv, int *disp_flag)
{
    switch (*rv) {
    case USE_NONE:
        process_rv_use_none(con, plan, disp_flag);
        return 0;

    case USE_PREVIOUS_WARNING_CONN:
        sharding_plan_free(plan);
        if (con->sharding_plan == NULL) {
            con->client->is_server_conn_reserved = 0;
            *disp_flag = PROXY_SEND_RESULT;
            network_mysqld_con_send_ok_full(con->client, 0, 0, 0, 0);
            g_debug("%s: origin has no sharding plan yet", G_STRLOC);
            return 0;
        }
        if (con->last_warning_met) {
            con->use_all_prev_servers = 1;
            if (con->servers == NULL) {
                con->client->is_server_conn_reserved = 0;
                *disp_flag = PROXY_SEND_RESULT;
                network_mysqld_con_send_ok_full(con->client, 0, 0, 0, 0);
                g_warning("%s: show warnings has no servers yet", G_STRLOC);
                return 0;
            }
        }
        break;
    case USE_SAME:
        if (!process_rv_use_same(con, plan, disp_flag)) {
            return 0;
        } else {
            if (!process_rv_use_previous_tran_conns(con, plan, rv, disp_flag)) {
                return 0;
            }
        }
        break;
    case USE_PREVIOUS_TRAN_CONNS:
        if (con->sharding_plan == NULL) {
            sharding_plan_free(plan);
            con->client->is_server_conn_reserved = 0;
            *disp_flag = PROXY_SEND_RESULT;
            network_mysqld_con_send_ok_full(con->client, 0, 0, 0, 0);
            g_debug("%s: origin has no sharding plan yet", G_STRLOC);
            return 0;
        }
        if (!process_rv_use_previous_tran_conns(con, plan, rv, disp_flag)) {
            return 0;
        }
        break;

    default:
        if (!process_rv_default(con, plan, rv, disp_flag)) {
            return 0;
        }
        break;

    }                           /* switch */

    return 1;
}

static int
make_decisions(network_mysqld_con *con, int rv, int *disp_flag)
{
    shard_plugin_con_t *st = con->plugin_con_state;

    query_stats_t *stats = &(con->srv->query_stats);

    switch (rv) {               /* TODO: move these inside to give specific reasons */
    case ERROR_UNPARSABLE:
    {
        const char *msg = st->sql_context->message ? : "sql parse error";
        int err_code = (st->sql_context->rc == PARSE_NOT_SUPPORT)
            ? ER_CETUS_NOT_SUPPORTED : ER_CETUS_PARSE_SHARDING;
        network_mysqld_con_send_error_full(con->client, L(msg), err_code, "HY000");
        g_message(G_STRLOC ": unparsable sql:%s", con->orig_sql->str);
        *disp_flag = PROXY_SEND_RESULT;
        return 0;
    }

    case USE_DIS_TRAN:
        if (!con->dist_tran) {
            con->dist_tran_state = NEXT_ST_XA_START;
            con->dist_tran_xa_start_generated = 0;
            stats->xa_count += 1;
            con->partition_dist_tran = 0;
        }
        con->dist_tran = 1;
        con->could_be_tcp_streamed = 0;
        con->could_be_fast_streamed = 0;
        con->dist_tran_failed = 0;
        con->delay_send_auto_commit = 0;
        g_debug("%s: xa transaction query:%s for con:%p", G_STRLOC, con->orig_sql->str, con);
        if (con->sharding_plan && con->sharding_plan->groups->len > 0) {
            wrap_check_sql(con, st->sql_context);
        }
        break;

    default:
        con->dist_tran_failed = 0;
        if (con->sharding_plan && con->sharding_plan->groups->len > 0) {
            wrap_check_sql(con, st->sql_context);
        }
        break;
    }

    return 1;
}

static int
proxy_get_server_list(network_mysqld_con *con)
{

    g_debug("%s: call proxy_get_server_list:%p for sql:%s, clt:%s, xa state:%d", G_STRLOC,
            con, con->orig_sql->str, con->client->src->name->str, con->dist_tran_state);

    before_get_server_list(con);

    if (con->client->default_db->len == 0) {
        if (con->srv->default_db != NULL) {
            g_string_assign(con->client->default_db, con->srv->default_db);
            g_debug("%s:set default db:%s for con:%p", G_STRLOC, con->client->default_db->str, con);
        }
    }

    con->write_flag = 0;
    con->use_all_prev_servers = 0;

    query_stats_t *stats = &(con->srv->query_stats);
    sharding_plan_t *plan = sharding_plan_new(con->orig_sql);
    plan->is_partition_mode = con->srv->is_partition_mode;
    int rv = 0, disp_flag = 0;

    shard_plugin_con_t *st = con->plugin_con_state;

    if (con->process_through_special_tunnel) {
        rv = sharding_parse_groups_by_property(con->client->default_db, st->sql_context, plan);
    } else {

        if (st->sql_context->rw_flag & CF_WRITE) {
            con->write_flag = 1;
        }

        switch (con->parse.command) {
            case COM_INIT_DB:
                if (!process_init_db_when_get_server_list(con, plan, &rv, &disp_flag)) {
                    return disp_flag;
                }
                break;
            default:
                rv = sharding_parse_groups(con->client->default_db, st->sql_context,
                        stats, con->key, plan);
                break;
        }
    }

    if (plan->groups->len > 1) {
        switch (st->sql_context->stmt_type) {
        case STMT_DROP_DATABASE: {
            sql_drop_database_t *drop_database = st->sql_context->sql_statement;
            if (drop_database) {
                truncate_default_db_when_drop_database(con, drop_database->schema_name);
            }
            break;
        }
        default:
            break;
        }
    }

    con->dist_tran_decided = 0;
    con->buffer_and_send_fake_resp = 0;
    con->server_to_be_closed = 0;
    con->server_closed = 0;
    con->resp_too_long = 0;

    if (con->last_record_updated || con->srv->master_preferred ||
        st->sql_context->rw_flag & CF_WRITE ||
        st->sql_context->rw_flag & CF_FORCE_MASTER || !con->is_auto_commit || rv == USE_SAME) {
        if (!con->client->is_server_conn_reserved) {
            if (con->servers) {
                remove_ro_servers(con);
            }
        }
        stats->client_query.rw++;
        stats->proxyed_query.rw++;
        if (st->sql_context->rw_flag & CF_FORCE_SLAVE) {
            con->is_read_ro_server_allowed = 1;
        }
    } else {
        con->is_read_ro_server_allowed = 1;
        stats->client_query.ro++;
        stats->proxyed_query.ro++;
    }

    if (rv != USE_PREVIOUS_WARNING_CONN) {
        con->last_warning_met = 0;
        if (!con->is_in_transaction) {
            if (con->client->is_server_conn_reserved) {
                con->client->is_server_conn_reserved = 0;
                g_debug("%s: is_server_conn_reserved is set false", G_STRLOC);
            }
        }
    }

    if (!make_first_decision(con, plan, &rv, &disp_flag)) {
        return disp_flag;
    }

    if (con->is_commit_or_rollback) {   /* current sql */
        if (con->is_tran_not_distributed_by_comment) {
            con->is_tran_not_distributed_by_comment = 0;
        }
    }

    if (!make_decisions(con, rv, &disp_flag)) {
        return disp_flag;
    }

    con->last_record_updated = 0;
    return RET_SUCCESS;
}

static gboolean
proxy_get_pooled_connection(network_mysqld_con *con,
                            shard_plugin_con_t *st,
                            GString *group, int type, network_socket **sock, int *is_robbed, int *server_unavailable)
{
    chassis_private *g = con->srv->priv;
    network_backend_t *backend = NULL;

    g_debug("%s:group:%s", G_STRLOC, group->str);
    network_group_t *backend_group = network_backends_get_group(g->backends, group);
    if (backend_group == NULL) {
        g_message("%s:backend_group is nil", G_STRLOC);
        *server_unavailable = 1;
        return FALSE;
    }

    if (type == BACKEND_TYPE_RO) {
        backend = network_group_pick_slave_backend(backend_group);
        if (backend == NULL) {  /* fallback to readwrite backend */
            type = BACKEND_TYPE_RW;
        }
    }

    if (type == BACKEND_TYPE_RW) {
        backend = backend_group->master;    /* may be NULL if master down */
        if (!backend || (backend->state != BACKEND_STATE_UP && backend->state != BACKEND_STATE_UNKNOWN)) {
            if (backend) {
                g_message("%s: backend->state:%d", G_STRLOC, backend->state);
            } else {
                g_message("%s: backend is nil", G_STRLOC);
            }
            *server_unavailable = 1;
            return FALSE;
        }
    }

    if (backend == NULL) {
        g_warning("%s: backend null, type:%d", G_STRLOC, type);
        *server_unavailable = 1;
        return FALSE;
    }

    *sock = network_connection_pool_get(backend->pool, con->client->response->username, is_robbed);
    if (*sock == NULL) {
        if (type == BACKEND_TYPE_RW) {
            con->master_conn_shortaged = 1;
        } else {
            con->slave_conn_shortaged = 1;
        }

        g_debug("%s: conn shortaged, type:%d", G_STRLOC, type);
        return FALSE;
    }

    (*sock)->is_read_only = (type == BACKEND_TYPE_RO) ? 1 : 0;
    st->backend = backend;

    st->backend->connected_clients++;

    g_debug("%s: connected_clients add, backend:%p, now:%d, con:%p, server:%p",
            G_STRLOC, backend, st->backend->connected_clients, con, *sock);

    return TRUE;
}

gboolean
proxy_add_server_connection(network_mysqld_con *con, GString *group, int *server_unavailable)
{
    server_session_t *ss;
    network_socket *server;

    if (con->servers != NULL) {
        size_t i;
        for (i = 0; i < con->servers->len; i++) {
            ss = (server_session_t *)(g_ptr_array_index(con->servers, i));
            if (ss != NULL) {
                if (g_string_equal(ss->server->group, group)) {
                    ss->participated = 1;
                    ss->state = NET_RW_STATE_NONE;
                    if (con->dist_tran) {
                        if (con->dist_tran_state == NEXT_ST_XA_START) {
                            ss->dist_tran_state = NEXT_ST_XA_START;
                            ss->xa_start_already_sent = 0;
                        }

                        if (ss->dist_tran_state == NEXT_ST_XA_OVER || ss->dist_tran_state == 0) {
                            g_message("%s: reset xa state:%d for ss ndx:%d, con:%p",
                                      G_STRLOC, ss->dist_tran_state, (int)i, con);
                            ss->dist_tran_state = NEXT_ST_XA_START;
                            ss->xa_start_already_sent = 0;
                        }
                        ss->dist_tran_participated = 1;
                    }
                    return TRUE;
                }
            }
        }
    } else {
        con->servers = g_ptr_array_new();
    }

    gboolean ok;
    int type = BACKEND_TYPE_RW;
    if (con->is_read_ro_server_allowed) {
        type = BACKEND_TYPE_RO;
    }
    shard_plugin_con_t *st = con->plugin_con_state;
    int is_robbed = 0;
    if ((ok = proxy_get_pooled_connection(con, st, group, type, &server, &is_robbed, server_unavailable))) {
        ss = g_new0(server_session_t, 1);

        con->is_new_server_added = 1;

        ss->con = con;
        ss->backend = st->backend;
        ss->server = server;
        server->group = group;
        ss->attr_consistent_checked = 0;
        ss->attr_consistent = 0;
        ss->server->last_packet_id = 0;
        ss->server->parse.qs_state = PARSE_COM_QUERY_INIT;
        ss->participated = 1;
        ss->has_xa_write = 0;
        ss->state = NET_RW_STATE_NONE;
        ss->fresh = 1;
        ss->is_xa_over = 0;

        if (con->dist_tran) {
            ss->is_in_xa = 1;
            ss->dist_tran_state = NEXT_ST_XA_START;
            ss->dist_tran_participated = 1;
            ss->xa_start_already_sent = 0;
        } else {
            ss->is_in_xa = 0;
        }
        ss->server->is_robbed = is_robbed;
        if (con->srv->sql_mgr && con->srv->sql_mgr->sql_log_switch == ON) {
            ss->ts_read_query = get_timer_microseconds();
        }

        g_ptr_array_add(con->servers, ss); /* TODO: CHANGE SQL */
    }

    return ok;
}

static gint
ss_comp(gconstpointer a1, gconstpointer a2)
{
    server_session_t *ss1 = *(server_session_t **)a1;
    server_session_t *ss2 = *(server_session_t **)a2;

    return strcmp(ss1->server->group->str, ss2->server->group->str);
}

static gboolean
proxy_add_server_connection_array(network_mysqld_con *con, int *server_unavailable)
{
    sharding_plan_t *plan = con->sharding_plan;
    size_t i;

    gint8 server_map[MAX_SERVER_NUM] = { 0 };

    if (con->dist_tran == 0 && con->servers != NULL && con->servers->len > 0) {
        int hit = 0;
        for (i = 0; i < con->servers->len; i++) {
            server_session_t *ss = g_ptr_array_index(con->servers, i);
            ss->dist_tran_participated = 0;
            const GString *group = ss->server->group;
            if (sharding_plan_has_group(plan, group)) {
                if (con->is_read_ro_server_allowed && !ss->server->is_read_only) {
                    g_debug("%s: use read server", G_STRLOC);
                } else if (!con->is_read_ro_server_allowed && ss->server->is_read_only) {
                    g_debug("%s: should release ro server to pool", G_STRLOC);
                } else {
                    if (hit == 0) {
                        con->first_group = group;
                    }
                    hit++;
                    server_map[i] = 1;
                    g_debug("%s: hit server", G_STRLOC);
                }
            }
        }

        if (hit == plan->groups->len && con->servers->len == hit) {
            return TRUE;
        } else {
            if (con->is_in_transaction) {
                g_warning("%s:in single tran, but visit multi servers for con:%p, sql:%s",
                        G_STRLOC, con, con->orig_sql->str);
                con->xa_tran_conflict = 1;
                return FALSE;
            }
            GPtrArray *new_servers = g_ptr_array_new();
            for (i = 0; i < con->servers->len; i++) {
                server_session_t *ss = g_ptr_array_index(con->servers, i);
                if (server_map[i] == 0) {
                    network_connection_pool *pool = ss->backend->pool;
                    network_socket *server = ss->server;

                    CHECK_PENDING_EVENT(&(server->event));

                    if (con->srv->server_conn_refresh_time <= server->create_time) {
                        network_pool_add_idle_conn(pool, con->srv, server);
                    } else {
                        g_message("%s: old connection for con:%p", G_STRLOC, con);
                        network_socket_send_quit_and_free(server);
                        con->srv->complement_conn_flag = 1;
                    }
                    ss->backend->connected_clients--;
                    g_debug("%s: conn clients sub, total len:%d, back:%p, value:%d con:%p, s:%p",
                            G_STRLOC, con->servers->len, ss->backend, ss->backend->connected_clients, con, server);

                    ss->sql = NULL;
                    g_free(ss);

                } else {
                    ss->server->parse.qs_state = PARSE_COM_QUERY_INIT;
                    g_ptr_array_add(new_servers, ss);
                }
            }

            gpointer *pdata = g_ptr_array_free(con->servers, FALSE);
            g_free(pdata);

            con->servers = new_servers;
        }
    } else {

        if (con->dist_tran) {
            int groups;
            GString *last_group;
            GString *super_group;
            if (con->srv->is_partition_mode) {
                shard_plugin_con_t *st = con->plugin_con_state;
                sql_context_t *context = st->sql_context;
                super_group = partition_get_super_group();
                last_group = NULL;
                groups = 0;
                if (plan->groups->len > 1) {
                    if (context->stmt_type != STMT_SELECT) {
                        con->partition_dist_tran = 1;
                        g_debug("%s: set partition_dist_tran true for con:%p, sql:%s", G_STRLOC, con, con->orig_sql->str);
                    }
                }
            }

            if (con->servers) {
                for (i = 0; i < con->servers->len; i++) {
                    server_session_t *ss = g_ptr_array_index(con->servers, i);
                    if (ss->server->is_read_only) {
                        g_critical("%s: crazy, dist tran use readonly server:%p", G_STRLOC, con);
                    }
                    g_debug("%s: group:%s, len:%d for con:%p", G_STRLOC, ss->server->group->str, con->servers->len, con);
                    ss->participated = 0;
                    if (con->srv->is_partition_mode) {
                        if (g_string_equal(ss->server->group, super_group)) {
                            continue;
                        }
                        if (last_group == NULL) {
                            last_group = ss->server->group;
                            groups = 1;
                        } else {
                            if (!g_string_equal(ss->server->group, last_group)) {
                                last_group = ss->server->group;
                                groups++;
                            }
                        }
                    }
                }

                g_debug("%s: groups:%d for con:%p", G_STRLOC, groups, con);
                if (con->srv->is_partition_mode) {
                    if (groups == 1) {
                        if (plan->groups->len == 1) {
                            GString *new_group = g_ptr_array_index(plan->groups, 0);
                            for (i = 0; i < con->servers->len; i++) {
                                server_session_t *ss = g_ptr_array_index(con->servers, i);
                                ss->server->group = new_group;
                            }
                            if (con->servers->len > 1) {
                                g_critical("%s: crazy, server num is not equal to 1 for con:%p", G_STRLOC, con);
                            }
                        } else {
                            for (i = 0; i < plan->groups->len; i++) {
                                GString *group = g_ptr_array_index(plan->groups, i);
                                if (g_string_equal(group, super_group)) {
                                    g_ptr_array_remove_fast(plan->groups, group);
                                    g_ptr_array_add(plan->groups, last_group);
                                }
                            }
                        }
                    } else {
                        if (con->servers->len > 0) {
                            if (groups == 0) {
                                GString *group = NULL;
                                group = g_ptr_array_index(plan->groups, 0);
                                if (group) {
                                    for (i = 0; i < con->servers->len; i++) {
                                        server_session_t *ss = g_ptr_array_index(con->servers, i);
                                        ss->server->group = group;
                                    }
                                }
                            } else {
                                if (plan->groups->len == 1) {
                                    GString *group = g_ptr_array_index(plan->groups, 0);
                                    if (g_string_equal(group, super_group)) {
                                        g_ptr_array_remove_fast(plan->groups, group);
                                        g_ptr_array_add(plan->groups, last_group);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    for (i = 0; i < plan->groups->len; i++) {
        GString *group = g_ptr_array_index(plan->groups, i);
        g_debug("%s: group:%s for con:%p, plan group len:%d", G_STRLOC, group->str, con, plan->groups->len);
        if (i == 0) {
            con->first_group = group;
        }

        if (!proxy_add_server_connection(con, group, server_unavailable)) {
            return FALSE;
        }
    }

    if (con->is_new_server_added && con->dist_tran && con->servers->len > 1) {
        g_ptr_array_sort(con->servers, ss_comp);
    }

    return TRUE;
}

static gboolean
check_and_set_attr_bitmap(network_mysqld_con *con)
{
    size_t i;
    gboolean result = TRUE;
    gboolean consistant;

    if (con->conn_attr_check_omit) {    /* current sql is a SET statement */
        mysqld_query_attr_t *query_attr = &(con->query_attr);
        if (query_attr->sql_mode_set) {
            return result;
        }
        g_debug("%s:conn_attr_check_omit true", G_STRLOC);
        for (i = 0; i < con->servers->len; i++) {
            server_session_t *ss = g_ptr_array_index(con->servers, i);
            if (query_attr->charset_set) {
                g_string_assign(ss->server->charset, con->client->charset->str);
            }
        }
        return result;
    }

    con->unmatched_attribute = 0;

    g_debug("%s:check conn attr, default db:%s", G_STRLOC, con->client->default_db->str);

    for (i = 0; i < con->servers->len; i++) {
        server_session_t *ss = g_ptr_array_index(con->servers, i);
        if (ss->attr_consistent_checked) {
            g_debug("%s:already checked for server:%p", G_STRLOC, ss->server);
            ss->attr_consistent = 1;
            continue;
        }

        g_debug("%s:server:%p, query state:%d", G_STRLOC, ss->server, ss->server->parse.qs_state);

        consistant = TRUE;
        ss->attr_diff = 0;

        if (ss->server->is_robbed) {
            ss->attr_diff = ATTR_DIF_CHANGE_USER;
            result = FALSE;
            con->unmatched_attribute |= ATTR_DIF_CHANGE_USER;
            consistant = FALSE;
        } else {
            if (con->parse.command != COM_INIT_DB) {
                /* check default db */
                if (con->client->default_db && con->client->default_db->len > 0) {
                    if (!g_string_equal(con->client->default_db, ss->server->default_db)) {
                        g_debug("%s:default db for client:%s", G_STRLOC, con->client->default_db->str);
                        ss->attr_diff = ATTR_DIF_DEFAULT_DB;
                        result = FALSE;
                        con->unmatched_attribute |= ATTR_DIF_DEFAULT_DB;
                        consistant = FALSE;
                        g_debug("%s: default db different", G_STRLOC);
                    }
                }
            }
        }

        if (!g_string_equal(con->client->sql_mode, ss->server->sql_mode)) {
            g_warning("%s: not support different sql modes", G_STRLOC);
        }

        if (con->srv->charset_check) {
            if (strcmp(con->client->charset->str, con->srv->default_charset) != 0) {
                g_message("%s: client charset:%s, default charset:%s, client address:%s", G_STRLOC,
                        con->client->charset->str, con->srv->default_charset, con->client->src->name->str);
            }
        }

        if (!g_string_equal(con->client->charset, ss->server->charset)) {
            ss->attr_diff |= ATTR_DIF_CHARSET;
            con->unmatched_attribute |= ATTR_DIF_CHARSET;
            result = FALSE;
            consistant = FALSE;
            g_debug("%s: charset different, clt:%s, srv:%s, server:%p",
                    G_STRLOC, con->client->charset->str, ss->server->charset->str, ss->server);
        }

        if (con->client->is_multi_stmt_set != ss->server->is_multi_stmt_set) {
            ss->attr_diff |= ATTR_DIF_SET_OPTION;
            con->unmatched_attribute |= ATTR_DIF_SET_OPTION;
            result = FALSE;
            consistant = FALSE;
            g_debug("%s:set option different", G_STRLOC);
        }

        if (con->is_start_trans_buffered || con->is_auto_commit_trans_buffered) {
            if (con->is_tran_not_distributed_by_comment) {
                ss->attr_diff |= ATTR_DIF_SET_AUTOCOMMIT;
                con->unmatched_attribute |= ATTR_DIF_SET_AUTOCOMMIT;
                result = FALSE;
                consistant = FALSE;
                g_debug("%s:need sending autocommit or start transaction", G_STRLOC);
            }
        }

        if (consistant) {
            ss->attr_consistent = 1;
        }
        g_debug("%s:set checked for server:%p, query state:%d", G_STRLOC, ss->server, ss->server->parse.qs_state);
        ss->attr_consistent_checked = 1;
    }

    return result;
}

static gboolean
check_user_consistant(network_mysqld_con *con)
{
    enum enum_server_command command = con->parse.command;

    if (command == COM_CHANGE_USER) {
        return TRUE;
    }

    size_t i;
    gboolean result = TRUE;

    for (i = 0; i < con->servers->len; i++) {
        server_session_t *ss = g_ptr_array_index(con->servers, i);
        if (!ss->participated || ss->attr_consistent) {
            continue;
        }

        ss->attr_adjusted_now = 0;

        if ((ss->attr_diff & ATTR_DIF_CHANGE_USER) == 0) {
            continue;
        }

        GString *hashed_password = g_string_new(NULL);
        const char *user = con->client->response->username->str;
        cetus_users_get_hashed_server_pwd(con->srv->priv->users, user, hashed_password);
        if (hashed_password->len == 0) {
            g_warning("%s: user:%s  hashed password is null", G_STRLOC, user);
            g_string_free(hashed_password, TRUE);
            result = FALSE;
            break;
        } else {
            g_debug("%s: COM_CHANGE_USER:%d for server:%p", G_STRLOC, COM_CHANGE_USER, ss->server);
            mysqld_change_user_packet_t chuser = { 0 };
            chuser.username = con->client->response->username;
            chuser.auth_plugin_data = ss->server->challenge->auth_plugin_data;
            chuser.hashed_pwd = hashed_password;

            if (strcmp(con->client->default_db->str, "") == 0) {
                if (con->srv->default_db != NULL) {
                    g_string_assign(con->client->default_db, con->srv->default_db);
                }
            }
            chuser.database = con->client->default_db;
            chuser.charset = con->client->charset_code;

            GString *payload = g_string_new(NULL);
            mysqld_proto_append_change_user_packet(payload, &chuser);

            network_mysqld_queue_reset(ss->server);
            network_mysqld_queue_append(ss->server, ss->server->send_queue, S(payload));
            g_string_free(payload, TRUE);

            ss->server->is_robbed = 0;
            ss->attr_adjusted_now = 1;
            ss->server->parse.qs_state = PARSE_COM_QUERY_INIT;
            g_debug("%s: change user for server", G_STRLOC);

            con->attr_adj_state = ATTR_DIF_CHANGE_USER;
            con->resp_expected_num++;
            g_string_free(hashed_password, TRUE);
        }
    }
    return result;
}

static void
build_xa_end_command(network_mysqld_con *con, server_session_t *ss, int first)
{
    char buffer[XA_CMD_BUF_LEN];

    char *xid_str = generate_or_retrieve_xid_str(con, ss->server, 0);
    snprintf(buffer, sizeof(buffer), "XA END %s", xid_str);

    if (con->dist_tran_failed || con->is_rollback) {
        ss->dist_tran_state = NEXT_ST_XA_ROLLBACK;
        if (first) {
            con->dist_tran_state = NEXT_ST_XA_ROLLBACK;
            con->state = ST_SEND_QUERY;
        }
    } else {
        ss->dist_tran_state = NEXT_ST_XA_PREPARE;
        if (first) {
            con->dist_tran_state = NEXT_ST_XA_PREPARE;
            con->state = ST_SEND_QUERY;
        }
    }

    if (ss->server->unavailable) {
        return;
    }

    g_debug("%s:XA END %s, server:%s", G_STRLOC, xid_str, ss->server->dst->name->str);

    ss->server->parse.qs_state = PARSE_COM_QUERY_INIT;

    GString *srv_packet;

    srv_packet = g_string_sized_new(64);
    srv_packet->len = NET_HEADER_SIZE;
    g_string_append_c(srv_packet, (char)COM_QUERY);
    g_string_append(srv_packet, buffer);
    network_mysqld_proto_set_packet_len(srv_packet, 1 + strlen(buffer));
    network_mysqld_proto_set_packet_id(srv_packet, 0);

    g_queue_push_tail(ss->server->send_queue->chunks, srv_packet);

    ss->state = NET_RW_STATE_NONE;
}

NETWORK_MYSQLD_PLUGIN_PROTO(proxy_get_server_conn_list)
{
    GList *chunk = con->client->recv_queue->chunks->head;
    GString *packet = (GString *)(chunk->data);
    gboolean do_query = FALSE;
    int is_xa_query = 0;

    con->is_new_server_added = 0;

    if (!con->use_all_prev_servers) {
        int server_unavailable = 0;
        if (!proxy_add_server_connection_array(con, &server_unavailable)) {
            record_last_backends_type(con);
            if (con->xa_tran_conflict) {
                return NETWORK_SOCKET_ERROR;
            } else {
                if (!server_unavailable) {
                    return NETWORK_SOCKET_WAIT_FOR_EVENT;
                } else {
                    return NETWORK_SOCKET_ERROR;
                }
            }
        } else {
            record_last_backends_type(con);
        }

        do_query = check_and_set_attr_bitmap(con);
        if (do_query == FALSE) {
            generate_sql(con);
            g_debug("%s: check_and_set_attr_bitmap is different", G_STRLOC);
            g_debug("%s: resp expect num:%d", G_STRLOC, con->resp_expected_num);
            con->resp_expected_num = 0;
            con->candidate_tcp_streamed = 0;
            con->candidate_fast_streamed = 0;
            con->is_attr_adjust = 1;
            if (con->unmatched_attribute & ATTR_DIF_CHANGE_USER) {
                check_user_consistant(con);
            } else if (con->unmatched_attribute & ATTR_DIF_DEFAULT_DB) {
                shard_set_default_db_consistant(con);
                con->attr_adj_state = ATTR_DIF_DEFAULT_DB;
            } else if (con->unmatched_attribute & ATTR_DIF_CHARSET) {
                shard_set_charset_consistant(con);
                con->attr_adj_state = ATTR_DIF_CHARSET;
            } else if (con->unmatched_attribute & ATTR_DIF_SET_OPTION) {
                shard_set_multi_stmt_consistant(con);
                con->attr_adj_state = ATTR_DIF_SET_OPTION;
            } else if (con->unmatched_attribute & ATTR_DIF_SET_AUTOCOMMIT) {
                g_debug("%s: autocommit adjust", G_STRLOC);
                shard_set_autocommit(con);
                con->attr_adj_state = ATTR_DIF_SET_AUTOCOMMIT;
            }

            return NETWORK_SOCKET_SUCCESS;
        }
    } else {
        do_query = TRUE;
    }

    if (do_query == TRUE) {
        if (con->attr_adj_state != ATTR_START) {
            g_critical("%s: con->attr_adj_state is not ATTR_START:%p", G_STRLOC, con);
        }
        con->is_attr_adjust = 0;
        con->attr_adj_state = ATTR_START;

        if (con->could_be_tcp_streamed) {
            con->candidate_tcp_streamed = 1;
        }

        if (con->could_be_fast_streamed) {
            con->candidate_fast_streamed = 1;
        }
        g_debug("%s: check_and_set_attr_bitmap is the same:%p", G_STRLOC, con);
        if (con->dist_tran && !con->dist_tran_xa_start_generated) {
            /* append xa query to send queue */
            con->dist_tran_state = NEXT_ST_XA_QUERY;
            char *xid_str = generate_or_retrieve_xid_str(con, NULL, 1);
            g_debug("%s:xa start:%s for con:%p", G_STRLOC, xid_str, con);
            con->dist_tran_xa_start_generated = 1;
            con->is_start_trans_buffered = 0;
            con->is_auto_commit_trans_buffered = 0;
        }

        size_t i;

        con->resp_expected_num = 0;
        g_debug("%s: server num:%d", G_STRLOC, con->servers->len);

        gboolean xa_start_phase = FALSE;
        if (con->dist_tran) {
            for (i = 0; i < con->servers->len; i++) {
                server_session_t *ss = g_ptr_array_index(con->servers, i);
                if (!ss->xa_start_already_sent) {
                    xa_start_phase = TRUE;
                    g_debug("%s: start phase is true:%d", G_STRLOC, (int)i);
                    break;
                }
            }
        }

        int is_first_xa_query = 0;
        char xa_log_buffer[XA_LOG_BUF_LEN] = { 0 };
        char *p_xa_log_buffer = xa_log_buffer;

        for (i = 0; i < con->servers->len; i++) {
            server_session_t *ss = g_ptr_array_index(con->servers, i);

            if (!con->is_commit_or_rollback && !ss->participated) {
                g_debug("%s: omit it for server:%p", G_STRLOC, ss->server);
                continue;
            }

            if (ss->server->unavailable) {
                continue;
            }

            g_debug("%s:packet id:%d when get server", G_STRLOC, ss->server->last_packet_id);

            ss->sql = sharding_get_sql(con, ss->server->group);
            ss->server->parse.qs_state = PARSE_COM_QUERY_INIT;

            if (con->dist_tran) {
                ss->xa_start_already_sent = 1;
                if (ss->dist_tran_state == NEXT_ST_XA_START) {
                    g_debug("%s:ss start phase:%d", G_STRLOC, (int)i);
                } else {
                    g_debug("%s:ss not start phase:%d", G_STRLOC, (int)i);
                }

                if (ss->dist_tran_state == NEXT_ST_XA_START) {
                    if (con->srv->is_partition_mode) {
                        generate_or_retrieve_xid_str(con, ss->server, 1);
                        con->dist_tran_xa_start_generated = 1;
                        network_mysqld_send_xa_start(ss->server, ss->server->xid_str);
                    } else {
                        network_mysqld_send_xa_start(ss->server, con->xid_str);
                    }
                    ss->dist_tran_state = NEXT_ST_XA_QUERY;
                    ss->xa_start_already_sent = 0;
                    con->xa_start_phase = 1;
                    g_debug("%s:ss start phase:%d", G_STRLOC, (int)i);
                } else if (ss->dist_tran_state == NEXT_ST_XA_OVER) {
                    g_debug("%s:omit here for server:%p", G_STRLOC, ss->server);
                    continue;
                } else {
                    if (con->is_commit_or_rollback  /* current sql */
                        || con->dist_tran_failed) {
                        ss->dist_tran_state = NEXT_ST_XA_END;
                        ss->participated = 1;
                        build_xa_end_command(con, ss, 1);
                        if (con->dist_tran_failed) {
                            network_queue_clear(con->client->recv_queue);
                            network_mysqld_queue_reset(con->client);
                            g_message("%s: clear recv queue", G_STRLOC);
                        }
                    } else {
                        if (!ss->participated) {
                            g_debug("%s:omit here for server:%p", G_STRLOC, ss->server);
                            continue;
                        }
                        if (xa_start_phase) {
                            g_debug("%s:omit here for server:%p", G_STRLOC, ss->server);
                            continue;
                        }
                        ss->dist_tran_state = NEXT_ST_XA_QUERY;
                        if (is_first_xa_query) {
                            p_xa_log_buffer[0] = ',';
                            p_xa_log_buffer++;
                        } else {
                            is_first_xa_query = 1;
                        }
                        snprintf(p_xa_log_buffer, XA_LOG_BUF_LEN - (p_xa_log_buffer - xa_log_buffer),
                                 "%s@%d", ss->server->dst->name->str, ss->server->challenge->thread_id);
                        p_xa_log_buffer = p_xa_log_buffer + strlen(p_xa_log_buffer);
                        if (shard_build_xa_query(con, ss) == -1) {
                            g_warning("%s:shard_build_xa_query failed for con:%p", G_STRLOC, con);
                            con->server_to_be_closed = 1;
                            con->dist_tran_state = NEXT_ST_XA_OVER;
                            return NETWORK_SOCKET_ERROR;
                        }
                        is_xa_query = 1;
                        if (con->is_auto_commit) {
                            ss->dist_tran_state = NEXT_ST_XA_END;
                            g_debug("%s:set dist_tran_state xa end for con:%p", G_STRLOC, con);
                        }
                    }
                }
            } else {
                if (con->parse.command == COM_QUERY) {
                    GString *payload = g_string_new(0);
                    network_mysqld_proto_append_query_packet(payload, ss->sql->str);
                    network_mysqld_queue_reset(ss->server);
                    network_mysqld_queue_append(ss->server, ss->server->send_queue, S(payload));
                    g_string_free(payload, TRUE);
                } else {
                    network_queue_append(ss->server->send_queue, g_string_new_len(packet->str, packet->len));
                }
            }

            if (!is_xa_query) {
                con->resp_expected_num++;
                ss->state = NET_RW_STATE_NONE;
            }
        }

        if (is_xa_query) {
            if (con->srv->xa_log_detailed) {
                tc_log_info(LOG_INFO, 0, "XA QUERY %s %s %s", con->xid_str, 0,
                        xa_log_buffer, con->orig_sql->str);
            }
            network_queue_clear(con->client->recv_queue);
        } else {
            if (!con->dist_tran) {
                network_queue_clear(con->client->recv_queue);
            }
        }
    }

    return NETWORK_SOCKET_SUCCESS;
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
    shard_plugin_con_t *st = con->plugin_con_state;
    sql_context_t *context = st->sql_context;

    if (context->stmt_type == STMT_DROP_DATABASE) {
        network_mysqld_com_query_result_t *com_query = con->parse.data;
        if (com_query && com_query->query_status == MYSQLD_PACKET_OK) {
            if (con->servers != NULL) {
                int i;
                for (i = 0; i < con->servers->len; i++) {
                    server_session_t *ss = g_ptr_array_index(con->servers, i);
                    g_string_truncate(ss->server->default_db, 0);
                    g_message("%s:truncate server database for con:%p", G_STRLOC, con);
                }
            }
        }
    }

    if (con->server_to_be_closed) {
        if (con->servers != NULL) {
            g_debug("%s:call proxy_put_shard_conn_to_pool for con:%p", G_STRLOC, con);
            proxy_put_shard_conn_to_pool(con);

            if (con->is_client_to_be_closed) {
                con->state = ST_CLOSE_CLIENT;
            } else {
                con->state = ST_READ_QUERY;
            }

            return NETWORK_SOCKET_SUCCESS;
        }
    }

    if (con->is_changed_user_failed) {
        con->is_changed_user_failed = 0;
        con->state = ST_ERROR;
        return NETWORK_SOCKET_SUCCESS;
    }

    con->state = ST_READ_QUERY;

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
    shard_plugin_con_t *st = con->plugin_con_state;
    return do_connect_cetus(con, &st->backend, &st->backend_ndx);
}

NETWORK_MYSQLD_PLUGIN_PROTO(proxy_init)
{
    chassis_plugin_config *config = con->config;

    g_assert(con->plugin_con_state == NULL);

    shard_plugin_con_t *st = shard_plugin_con_new();

    /* TODO: this should inside "st"_new, but now "st" shared by many plugins */
    st->sql_context = g_new0(sql_context_t, 1);
    sql_context_init(st->sql_context);
    st->sql_context->allow_subquery_nesting = config->allow_nested_subquery;
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
    if (config->dist_tran_decided_read_timeout_dbl >= 0) {
        chassis_timeval_from_double(&con->dist_tran_decided_read_timeout, config->dist_tran_decided_read_timeout_dbl);
    }
    if (config->write_timeout_dbl >= 0) {
        chassis_timeval_from_double(&con->write_timeout, config->write_timeout_dbl);
    }

    return NETWORK_SOCKET_SUCCESS;
}

static int
proxy_c_disconnect_shard_client(network_mysqld_con *con)
{
    if (con->is_in_transaction || con->is_auto_commit == 0) {
        if (con->is_in_transaction) {
            g_message("%s: con is still in trans for con:%p", G_STRLOC, con);
        }

        if (!con->server_to_be_closed) {
            if (con->dist_tran_state != NEXT_ST_XA_OVER) {
                con->server_to_be_closed = 1;
            }
        }
    }

    if (con->servers) {
        g_debug("%s:call proxy_put_shard_conn_to_pool for con:%p", G_STRLOC, con);
        proxy_put_shard_conn_to_pool(con);
    }

    return PROXY_NO_DECISION;
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
    shard_plugin_con_t *st = con->plugin_con_state;

    if (st == NULL)
        return NETWORK_SOCKET_SUCCESS;

    if (con->servers != NULL) {
        g_debug("%s: call proxy_c_disconnect_shard_client:%p", G_STRLOC, con);
        proxy_c_disconnect_shard_client(con);
    }

    if (con->sharding_plan != NULL) {
        sharding_plan_free(con->sharding_plan);
        con->sharding_plan = NULL;
    }

    network_mysqld_con_reset_query_state(con);

    /* TODO: this should inside "st"_free, but now "st" shared by many plugins */
    if (st->sql_context) {
        sql_context_destroy(st->sql_context);
        g_free(st->sql_context);
        st->sql_context = NULL;
    }

    shard_plugin_con_free(con, st);

    con->plugin_con_state = NULL;

    g_debug("%s: set plugin_con_state null:%p", G_STRLOC, con);

    return NETWORK_SOCKET_SUCCESS;
}

int
network_mysqld_shard_connection_init(network_mysqld_con *con)
{
    con->plugins.con_init = proxy_init;
    con->plugins.con_connect_server = proxy_connect_server;
    con->plugins.con_read_handshake = NULL;
    con->plugins.con_read_auth = proxy_read_auth;
    con->plugins.con_read_auth_result = NULL;
    con->plugins.con_read_query = proxy_read_query;
    con->plugins.con_get_server_conn_list = proxy_get_server_conn_list;
    con->plugins.con_read_query_result = NULL;
    con->plugins.con_send_query_result = proxy_send_query_result;
    con->plugins.con_cleanup = proxy_disconnect_client;
    con->plugins.con_timeout = proxy_timeout;

    return 0;
}

chassis_plugin_config *config;

static chassis_plugin_config *
network_mysqld_shard_plugin_new(void)
{
    config = g_new0(chassis_plugin_config, 1);

    /* use negative values as defaults to make them ignored */
    config->connect_timeout_dbl = -1.0;
    config->read_timeout_dbl = -1.0;
    config->dist_tran_decided_read_timeout_dbl = -1.0;
    config->write_timeout_dbl = -1.0;

    return config;
}

void
network_mysqld_proxy_free(network_mysqld_con G_GNUC_UNUSED *con)
{
}

void
network_mysqld_shard_plugin_free(chassis *chas, chassis_plugin_config *config)
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
    shard_conf_destroy();

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
                if(old_backend->server_group && old_backend->server_group->len) {
                    free_str = g_string_append(free_str, "@");
                    free_str = g_string_append(free_str, old_backend->server_group->str);
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
                if(old_backend->server_group && old_backend->server_group->len) {
                    free_str = g_string_append(free_str, "@");
                    free_str = g_string_append(free_str, old_backend->server_group->str);
                }
                free_str = g_string_append(free_str, ",");
            }
        }
        if(free_str->len) {
            free_str->str[free_str->len -1] = '\0';
        }
        if(!strcasecmp("127.0.0.1:3306", free_str->str)) {
            return NULL;
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
        //handle default
        if(config->connect_timeout_dbl == -1) {
            return NULL;
        }
        return g_strdup_printf("%lf", config->connect_timeout_dbl);
    }
    return NULL;
}

static gchar* show_allow_nested_subquery(gpointer param) {
    struct external_param *opt_param = (struct external_param *)param;
    gint opt_type = opt_param->opt_type;
    if(CAN_SHOW_OPTS_PROPERTY(opt_type) || CAN_SAVE_OPTS_PROPERTY(opt_type)) {
        return g_strdup_printf("%s", config->allow_nested_subquery ? "true": "false");
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
show_proxy_dist_tran_decided_read_timeout(gpointer param) {
    struct external_param *opt_param = (struct external_param *)param;
    gint opt_type = opt_param->opt_type;
    if(CAN_SHOW_OPTS_PROPERTY(opt_type)) {
        return g_strdup_printf("%lf (s)", config->dist_tran_decided_read_timeout_dbl);
    }
    if(CAN_SAVE_OPTS_PROPERTY(opt_type)) {
        if(config->dist_tran_decided_read_timeout_dbl == -1) {
            return NULL;
        }
        return g_strdup_printf("%lf", config->dist_tran_decided_read_timeout_dbl);
    }
    return NULL;
}

static gint
assign_proxy_dist_tran_decided_read_timeout(const gchar *newval, gpointer param) {
    gint ret = ASSIGN_ERROR;
    struct external_param *opt_param = (struct external_param *)param;
    gint opt_type = opt_param->opt_type;
    if(CAN_ASSIGN_OPTS_PROPERTY(opt_type)) {
        if(NULL != newval) {
            gdouble value = 0;
            if(try_get_double_value(newval, &value)) {
                config->dist_tran_decided_read_timeout_dbl = value;
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
network_mysqld_shard_plugin_get_options(chassis_plugin_config *config)
{
    chassis_options_t opts = { 0 };

    chassis_options_add(&opts, "proxy-address",
                        'P', 0, OPTION_ARG_STRING, &(config->address),
                        "listening address:port of the proxy-server (default: :4040)", "<host:port>",
                        NULL, show_proxy_address, SHOW_OPTS_PROPERTY|SAVE_OPTS_PROPERTY);

    chassis_options_add(&opts, "proxy-backend-addresses",
                        'b', 0, OPTION_ARG_STRING_ARRAY, &(config->backend_addresses),
                        "address:port of the remote backend-servers (default: 127.0.0.1:3306)", "<host:port>",
                        NULL, show_proxy_backend_addresses, SAVE_OPTS_PROPERTY);

    chassis_options_add(&opts, "proxy-read-only-backend-addresses",
                        'r', 0, OPTION_ARG_STRING_ARRAY, &(config->read_only_backend_addresses),
                        "address:port of the remote slave-server (default: not set)", "<host:port>",
                        NULL, show_proxy_read_only_backend_address, SAVE_OPTS_PROPERTY);

    chassis_options_add(&opts, "proxy-connect-timeout",
                        0, 0, OPTION_ARG_DOUBLE, &(config->connect_timeout_dbl),
                        "connect timeout in seconds (default: 2.0 seconds)", NULL,
                        assign_proxy_connect_timeout, show_proxy_connect_timeout, ALL_OPTS_PROPERTY);

    chassis_options_add(&opts, "allow-nested-subquery",
                        0, 0, OPTION_ARG_NONE, &(config->allow_nested_subquery),
                        "Use this on your own risk, data integrity is not guaranteed", NULL,
                        NULL, show_allow_nested_subquery, SHOW_OPTS_PROPERTY|SAVE_OPTS_PROPERTY);

    chassis_options_add(&opts, "proxy-read-timeout",
                        0, 0, OPTION_ARG_DOUBLE, &(config->read_timeout_dbl),
                        "read timeout in seconds (default: 600 seconds)", NULL,
                        assign_proxy_read_timeout, show_proxy_read_timeout, ALL_OPTS_PROPERTY);

    chassis_options_add(&opts, "proxy-xa-commit-or-rollback-read-timeout",
                        0, 0, OPTION_ARG_DOUBLE, &(config->dist_tran_decided_read_timeout_dbl),
                        "xa commit or rollback read timeout in seconds (default: 30 seconds)", NULL,
                        assign_proxy_dist_tran_decided_read_timeout,
                        show_proxy_dist_tran_decided_read_timeout, ALL_OPTS_PROPERTY);

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

    return opts.options;
}

/**
 * init the plugin with the parsed config
 */
static int
network_mysqld_shard_plugin_apply_config(chassis *chas, chassis_plugin_config *config)
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
    network_mysqld_shard_connection_init(con);

    if (network_address_set_address(listen_sock->dst, config->address)) {
        return -1;
    }

    if (network_socket_bind(listen_sock, 1)) {
        return -1;
    }
    g_message("shard module listening on port %s, con:%p", config->address, con);

    plugin_add_backends(chas, config->backend_addresses, config->read_only_backend_addresses);

    char *shard_json = NULL;
    gboolean ok = chassis_config_query_object(chas->config_manager,
                                              "sharding", &shard_json, 0);
    if (!ok || !shard_json || !shard_conf_load(chas->is_partition_mode, shard_json, g->backends->groups->len)) {
        g_critical("sharding configuration load error, exit program.");
        exit(0);
    }
    g_free(shard_json);

    g_assert(chas->priv->monitor);

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
        struct timeval check_interval = {10, 0};
        chassis_event_add_with_timeout(chas, &chas->auto_create_conns_event, &check_interval);
    }
    chassis_config_register_service(chas->config_manager, config->address, "shard");

    sql_filter_vars_shard_load_default_rules();
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
network_mysqld_shard_plugin_stop_listening(chassis *chas,
        chassis_plugin_config *config)
{
    g_message("%s:call network_mysqld_shard_plugin_stop_listening", G_STRLOC);
    if (config->listen_con) {
        g_message("%s:close listen socket:%d", G_STRLOC, config->listen_con->server->fd);
        network_socket_free(config->listen_con->server);
        config->listen_con = NULL;
    }
}


G_MODULE_EXPORT int
plugin_init(chassis_plugin *p)
{
    p->magic = CHASSIS_PLUGIN_MAGIC;
    p->name = g_strdup("shard");
    p->version = g_strdup(PLUGIN_VERSION);

    p->init = network_mysqld_shard_plugin_new;
    p->get_options = network_mysqld_shard_plugin_get_options;
    p->apply_config = network_mysqld_shard_plugin_apply_config;
    p->stop_listening = network_mysqld_shard_plugin_stop_listening;
    p->destroy = network_mysqld_shard_plugin_free;

    return 0;
}
