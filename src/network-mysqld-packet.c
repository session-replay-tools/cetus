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

/**
 * codec's for the MySQL client protocol
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "network-mysqld-packet.h"
#include "glib-ext.h"

network_mysqld_com_query_result_t *
network_mysqld_com_query_result_new()
{
    network_mysqld_com_query_result_t *com_query;

    com_query = g_new0(network_mysqld_com_query_result_t, 1);
    com_query->state = PARSE_COM_QUERY_INIT;
    /* 
     * can have 3 values: NULL for unknown, OK for a OK packet, 
     * ERR for a error-packet
     */
    com_query->query_status = MYSQLD_PACKET_NULL;

    return com_query;
}

void
network_mysqld_com_query_result_free(network_mysqld_com_query_result_t *udata)
{
    if (!udata)
        return;

    g_free(udata);
}

/**
 * @return -1 on error
 *         0  on success and done
 *         1  on success and need more
 */
int
network_mysqld_proto_get_com_query_result(network_packet *packet,
                                          network_mysqld_com_query_result_t *query, gboolean use_binary_row_data)
{
    int is_finished = 0;
    guint8 status;
    int err = 0;
    network_mysqld_eof_packet_t *eof_packet;
    network_mysqld_ok_packet_t *ok_packet;

    /**
     * if we get a OK in the first packet there will be no result-set
     */
    switch (query->state) {
    case PARSE_COM_QUERY_INIT:
        err = network_mysqld_proto_peek_int8(packet, &status);
        if (err)
            break;

        switch (status) {
        case MYSQLD_PACKET_ERR:
        {
            g_debug("%s: status error, query state:%d, status:%d, packet offset:%d",
                    G_STRLOC, query->state, status, (int)packet->offset);
            query->query_status = MYSQLD_PACKET_ERR;
            is_finished = 1;
            break;
        }
        case MYSQLD_PACKET_OK: /* e.g. DELETE FROM tbl */
            query->query_status = MYSQLD_PACKET_OK;

            ok_packet = network_mysqld_ok_packet_new();

            err = network_mysqld_proto_get_ok_packet(packet, ok_packet);

            if (!err) {
                if (!(ok_packet->server_status & SERVER_MORE_RESULTS_EXISTS)) {
                    is_finished = 1;
                }

                query->server_status = ok_packet->server_status;
                g_debug("%s: server status in ok packet, got: %d", G_STRLOC, ok_packet->server_status);
                query->warning_count = ok_packet->warnings;
                query->affected_rows = ok_packet->affected_rows;
                query->insert_id = ok_packet->insert_id;
                query->was_resultset = 0;
                query->binary_encoded = use_binary_row_data;
            }

            network_mysqld_ok_packet_free(ok_packet);
            break;
        case MYSQLD_PACKET_NULL:
            /* OH NO, LOAD DATA INFILE :) */
            query->state = PARSE_COM_QUERY_LOCAL_INFILE_DATA;
            is_finished = 1;
            break;
        case MYSQLD_PACKET_EOF:
            g_critical("%s: COM_QUERY should not be (EOF), got: 0x%02x", G_STRLOC, status);
            err = 1;
            break;
        default:
            query->query_status = MYSQLD_PACKET_OK;
            /* looks like a result */
            query->state = PARSE_COM_QUERY_FIELD;
            break;
        }
        break;
    case PARSE_COM_QUERY_FIELD:

        err = err || network_mysqld_proto_peek_int8(packet, &status);
        if (err)
            break;

        switch (status) {
        case MYSQLD_PACKET_ERR:
        case MYSQLD_PACKET_OK:
        case MYSQLD_PACKET_NULL:
            g_critical("%s: COM_QUERY !=(OK|NULL|ERR), got: 0x%02x", G_STRLOC, status);

            err = 1;

            break;
        case MYSQLD_PACKET_EOF:
                    /**
                     * in 5.0 we have CURSORs
                     *
                     * COM_STMT_EXECUTE would have _CURSOR_EXISTS set in 
                     * the EOF and no resultset
                     * COM_STMT_FETCH would be executed afterwards to 
                     * fetch the rows from the cursor
                     *
                     * Other commands may have that flag set too, 
                     * with no special meaning
                     * 
                     */
            if (packet->data->len == 9) {
                eof_packet = network_mysqld_eof_packet_new();

                err = network_mysqld_proto_get_eof_packet(packet, eof_packet);

                if (!err) {
#if MYSQL_VERSION_ID >= 50000
                    /* 5.5 may send a SERVER_MORE_RESULTS_EXISTS as part of the first 
                     * EOF together with SERVER_STATUS_CURSOR_EXISTS. In that case,
                     * we aren't finished. (#61998)
                     *
                     * Only if _CURSOR_EXISTS is set alone AND this is COM_STMT_EXECUTE,
                     * we have a field-definition-only resultset
                     *
                     * CURSOR_EXISTS indications that COM_STMT_FETCH should be used to
                     * fetch data for this cursor, but can only be don't if we have 
                     * a prepared statement
                     */
                    if (use_binary_row_data &&
                        eof_packet->server_status & SERVER_STATUS_CURSOR_EXISTS &&
                        !(eof_packet->server_status & SERVER_MORE_RESULTS_EXISTS)) {
                        is_finished = 1;
                        g_message("%s: is_finished here without PARSE_COM_QUERY_RESULT", G_STRLOC);
                    } else {
                        query->state = PARSE_COM_QUERY_RESULT;
                    }
#else
                    query->state = PARSE_COM_QUERY_RESULT;
#endif
                    g_debug("%s: set query state:%d for parse.data:%p", G_STRLOC, query->state, query);

                    /* track the server_status of the 1st EOF packet */
                    query->server_status = eof_packet->server_status;
                    g_debug("%s: server status in eof packet, got: %d", G_STRLOC, eof_packet->server_status);
                }

                network_mysqld_eof_packet_free(eof_packet);
            } else {
                query->state = PARSE_COM_QUERY_RESULT;
                g_debug("%s: set query state:%d for parse.data:%p", G_STRLOC, query->state, query);
            }
            break;
        default:
            break;
        }
        break;
    case PARSE_COM_QUERY_RESULT:
        err = err || network_mysqld_proto_peek_int8(packet, &status);
        if (err)
            break;

        switch (status) {
        case MYSQLD_PACKET_EOF:
            if (packet->data->len == 9) {
                eof_packet = network_mysqld_eof_packet_new();

                err = err || network_mysqld_proto_get_eof_packet(packet, eof_packet);

                if (!err) {
                    query->was_resultset = 1;

#ifndef SERVER_PS_OUT_PARAMS
#define SERVER_PS_OUT_PARAMS 4096
#endif
                            /**
                             * a PS_OUT_PARAMS is set if a COM_STMT_EXECUTE executes 
                             * a CALL sp(?) where sp is a PROCEDURE with OUT params 
                             *
                             * ...
                             * 05 00 00 12 fe 00 00 0a 10 -- end column-def 
                             * (auto-commit, more-results, ps-out-params)
                             * ...
                             * 05 00 00 14 fe 00 00 02 00 -- end of rows (auto-commit), 
                             * see the missing (more-results, ps-out-params)
                             * 07 00 00 15 00 00 00 02 00 00 00 -- OK for the CALL
                             *
                             * for all other resultsets we trust the status-flags of 
                             * the 2nd EOF packet
                             */
                    if (!(query->server_status & SERVER_PS_OUT_PARAMS)) {
                        query->server_status = eof_packet->server_status;
                    }
                    query->warning_count = eof_packet->warnings;

                    if (query->server_status & SERVER_MORE_RESULTS_EXISTS) {
                        query->state = PARSE_COM_QUERY_INIT;
                        g_debug("%s: here query state:%d", G_STRLOC, query->state);
                    } else {
                        is_finished = 1;
                        g_debug("%s: here query state:%d", G_STRLOC, query->state);
                    }
                }

                network_mysqld_eof_packet_free(eof_packet);
            }

            break;
        case MYSQLD_PACKET_ERR:
            /* like 
             * 
             * EXPLAIN SELECT *FROM dual; returns an error
             * 
             * EXPLAIN SELECT 1 FROM dual; returns a result-set
             * */
            is_finished = 1;
            break;
        case MYSQLD_PACKET_OK:
        case MYSQLD_PACKET_NULL:
        default:
            query->rows++;
            query->bytes += packet->data->len;
            break;
        }
        break;
    case PARSE_COM_QUERY_LOCAL_INFILE_DATA:
        /* we will receive a empty packet if we are done */
        if (packet->data->len == packet->offset) {
            query->state = PARSE_COM_QUERY_LOCAL_INFILE_RESULT;
            is_finished = 1;
        }
        break;
    case PARSE_COM_QUERY_LOCAL_INFILE_RESULT:
        err = err || network_mysqld_proto_get_int8(packet, &status);
        if (err)
            break;

        switch (status) {
        case MYSQLD_PACKET_OK:
            is_finished = 1;
            break;
        case MYSQLD_PACKET_NULL:
        case MYSQLD_PACKET_ERR:
        case MYSQLD_PACKET_EOF:
        default:
            g_critical("%s: COM_QUERY,should be (OK), got: 0x%02x", G_STRLOC, status);

            err = 1;

            break;
        }

        break;
    }

    if (err)
        return -1;

    return is_finished;
}

network_mysqld_com_stmt_prep_result_t *
network_mysqld_com_stmt_prepare_result_new()
{
    network_mysqld_com_stmt_prep_result_t *udata;

    udata = g_new0(network_mysqld_com_stmt_prep_result_t, 1);
    udata->first_packet = TRUE;

    return udata;
}

void
network_mysqld_com_stmt_prepare_result_free(network_mysqld_com_stmt_prep_result_t *udata)
{
    if (!udata)
        return;

    g_free(udata);
}

int
network_mysqld_proto_get_com_stmt_prep_result(network_packet *packet, network_mysqld_com_stmt_prep_result_t *udata)
{
    guint8 status;
    int is_finished = 0;
    int err = 0;

    err = err || network_mysqld_proto_get_int8(packet, &status);

    if (udata->first_packet == 1) {
        udata->first_packet = 0;

        switch (status) {
        case MYSQLD_PACKET_OK:
            g_assert(packet->data->len == 12 + NET_HEADER_SIZE);

            /* the header contains the number of EOFs we expect to see
             * - no params -> 0
             * - params | fields -> 1
             * - params + fields -> 2 
             */
            udata->want_eofs = 0;

            if (packet->data->str[NET_HEADER_SIZE + 5] != 0 || packet->data->str[NET_HEADER_SIZE + 6] != 0) {
                udata->want_eofs++;
            }
            if (packet->data->str[NET_HEADER_SIZE + 7] != 0 || packet->data->str[NET_HEADER_SIZE + 8] != 0) {
                udata->want_eofs++;
            }

            if (udata->want_eofs == 0) {
                is_finished = 1;
            }

            g_debug("%s: want_eofs value:%d", G_STRLOC, udata->want_eofs);
            udata->status = status;
            break;
        case MYSQLD_PACKET_ERR:
            is_finished = 1;
            g_message("%s: network_mysqld_proto_get_com_stmt_prep_rs get packet err:%d", G_STRLOC, status);
            udata->status = status;
            break;
        default:
            g_error("%s: COM_STMT_PREPARE should either get a (OK|ERR), got %02x", G_STRLOC, status);
            break;
        }
    } else {
        switch (status) {
        case MYSQLD_PACKET_OK:
        case MYSQLD_PACKET_NULL:
        case MYSQLD_PACKET_ERR:
            g_error("%s: COM_STMT_PREPARE should not be (OK|ERR|NULL), got: %02x", G_STRLOC, status);
            break;
        case MYSQLD_PACKET_EOF:
            if (--udata->want_eofs == 0) {
                is_finished = 1;
            }
            g_debug("%s: other want_eofs value:%d", G_STRLOC, udata->want_eofs);
            break;
        default:
            break;
        }
    }

    if (err)
        return -1;

    return is_finished;
}

network_mysqld_com_init_db_result_t *
network_mysqld_com_init_db_result_new()
{
    network_mysqld_com_init_db_result_t *udata;

    udata = g_new0(network_mysqld_com_init_db_result_t, 1);
    udata->db_name = NULL;

    return udata;
}

void
network_mysqld_com_init_db_result_free(network_mysqld_com_init_db_result_t *udata)
{
    if (udata->db_name)
        g_string_free(udata->db_name, TRUE);

    g_free(udata);
}

int
network_mysqld_com_init_db_result_track_state(network_packet *packet, network_mysqld_com_init_db_result_t *udata)
{
    network_mysqld_proto_skip_network_header(packet);
    if (network_mysqld_proto_skip(packet, 1) == -1) {
        return -1;
    }

    if (packet->offset != packet->data->len) {
        udata->db_name = g_string_new(NULL);

        network_mysqld_proto_get_gstr_len(packet, packet->data->len - packet->offset, udata->db_name);
    } else {
        if (udata->db_name)
            g_string_free(udata->db_name, TRUE);
        udata->db_name = NULL;
    }

    return 0;
}

int
network_mysqld_proto_get_com_init_db(network_packet *packet,
                                     network_mysqld_com_init_db_result_t *udata, network_mysqld_con *con)
{
    guint8 status;
    int is_finished;
    int err = 0;

    /**
     * in case we have a init-db statement we track the db-change on the server-side
     * connection
     */
    err = err || network_mysqld_proto_get_int8(packet, &status);

    g_debug("%s: COM_INIT_DB got %02x", G_STRLOC, status);

    switch (status) {
    case MYSQLD_PACKET_ERR:
        is_finished = 1;
        if (udata->db_name && udata->db_name->len) {
            g_message("%s: COM_INIT_DB failed, want db:%s, client default db still:%s",
                      G_STRLOC, udata->db_name->str, con->client->default_db->str);
        } else {
            g_message("%s: COM_INIT_DB failed, client default db still:%s", G_STRLOC, con->client->default_db->str);
        }
        break;
    case MYSQLD_PACKET_OK:
            /**
             * track the change of the init_db */
        if (con->server)
            g_string_truncate(con->server->default_db, 0);

        if (udata->db_name && udata->db_name->len) {

            g_string_truncate(con->client->default_db, 0);
            if (con->server) {
                g_string_append_len(con->server->default_db, S(udata->db_name));
                g_debug("%s:set server default db:%s for con:%p", G_STRLOC, con->server->default_db->str, con);
            }

            g_string_append_len(con->client->default_db, S(udata->db_name));
            g_debug("%s: COM_INIT_DB set default db success:%s", G_STRLOC, con->client->default_db->str);
        } else {
            if (con->server) {
                g_string_append_len(con->server->default_db, S(con->client->default_db));
            }
        }

        is_finished = 1;
        break;
    default:
        g_critical("%s: COM_INIT_DB should be (ERR|OK), got %02x", G_STRLOC, status);

        return -1;
    }

    if (err)
        return -1;

    return is_finished;
}

/**
 * init the tracking of the sub-states of the protocol
 */
int
network_mysqld_con_command_states_init(network_mysqld_con *con, network_packet *packet)
{
    guint8 cmd;
    int err = 0;

    err = err || network_mysqld_proto_skip_network_header(packet);
    err = err || network_mysqld_proto_get_int8(packet, &cmd);

    if (err)
        return -1;

    con->parse.command = cmd;

    g_debug("%s: reset command:%d ", G_STRLOC, cmd);
    packet->offset = 0;         /* reset the offset again for the next functions */

    /* init the parser for the commands */
    switch (con->parse.command) {
    case COM_QUERY:
    case COM_PROCESS_INFO:
    case COM_STMT_EXECUTE:
        con->parse.data = network_mysqld_com_query_result_new();
        con->parse.data_free = (GDestroyNotify) network_mysqld_com_query_result_free;
        break;
    case COM_STMT_PREPARE:
        con->parse.data = network_mysqld_com_stmt_prepare_result_new();
        con->parse.data_free = (GDestroyNotify) network_mysqld_com_stmt_prepare_result_free;
        break;
    case COM_INIT_DB:
        con->parse.data = network_mysqld_com_init_db_result_new();
        con->parse.data_free = (GDestroyNotify) network_mysqld_com_init_db_result_free;

        if (network_mysqld_com_init_db_result_track_state(packet, con->parse.data) != 0) {
            return -1;
        }

        break;
    case COM_QUIT:
        /* track COM_QUIT going to the server, to be able to tell if the server
         * a) simply went away or
         * b) closed the connection because the client asked it to
         * If b) we should not print a message at the next EV_READ event from the server fd
         */
        con->com_quit_seen = TRUE;
    default:
        break;
    }

    return 0;
}

/**
 * @param packet the current packet that is passing by
 *
 *
 * @return -1 on invalid packet, 
 *          0 need more packets, 
 *          1 for the last packet 
 */
int
network_mysqld_proto_get_query_result(network_packet *packet, network_mysqld_con *con)
{
    guint8 status;
    int is_finished = 0;
    int err = 0;

    err = err || network_mysqld_proto_skip_network_header(packet);
    if (err) {
        g_message("%s: skip header error, command:%d.", G_STRLOC, con->parse.command);
        return -1;
    }

    /* forward the response to the client */
    switch (con->parse.command) {
    case COM_RESET_CONNECTION:
        err = err || network_mysqld_proto_get_int8(packet, &status);
        if (err)
            return -1;

        switch (status) {
        case MYSQLD_PACKET_ERR:
            con->is_changed_user_failed = 1;
            is_finished = 1;
            break;
        case MYSQLD_PACKET_OK:
            is_finished = 1;
            break;
        default:
            g_debug_hexdump(G_STRLOC, S(packet->data));
            g_message("%s: got a 0x%02x packet for COM_[0%02x], expected only (ERR|EOF)",
                      G_STRLOC, con->parse.command, (guint8)status);
            return -1;
        }

        break;

    case COM_CHANGE_USER:
            /**
             * - OK
             * - ERR
             * - EOF for auth switch TODO
             */
        err = err || network_mysqld_proto_get_int8(packet, &status);
        if (err)
            return -1;

        switch (status) {
        case MYSQLD_PACKET_ERR:
            con->is_changed_user_failed = 1;
            int offset = packet->offset;
            packet->offset = NET_HEADER_SIZE;
            network_mysqld_err_packet_t *err_packet;
            err_packet = network_mysqld_err_packet_new();
            if (!network_mysqld_proto_get_err_packet(packet, err_packet)) {
                g_warning("%s:error code:%d,errmsg:%s,sqlstate:%s",
                          G_STRLOC, (int)err_packet->errcode, err_packet->errmsg->str, err_packet->sqlstate->str);
            } else {
                g_warning("%s: change user failed", G_STRLOC);
            }
            network_mysqld_err_packet_free(err_packet);
            packet->offset = offset;
            is_finished = 1;
            break;

        case MYSQLD_PACKET_OK:
            g_string_assign_len(con->server->response->username, S(con->client->response->username));
            g_debug("%s: save username for server, con:%p", G_STRLOC, con);

            if (con->client->default_db->len > 0) {
                g_string_truncate(con->server->default_db, 0);
                g_string_append(con->server->default_db, con->client->default_db->str);
                g_debug("%s:set server default db:%s for con:%p", G_STRLOC, con->server->default_db->str, con);
            }
            is_finished = 1;
            break;
        case MYSQLD_PACKET_EOF:
            /* TODO:
             * - added extra states to the state-engine in network-mysqld.c to 
             *   track the packets that are sent back and forth
             *   to switch the auth-method in COM_CHANGE_USER
             */
            g_message("%s: COM_CHANGE_USER's auth-method-switch not supported.", G_STRLOC);
            return -1;
        default:
            g_debug_hexdump(G_STRLOC, S(packet->data));
            g_message("%s: got a 0x%02x packet for COM_[0%02x], expected only (ERR|OK)",
                      G_STRLOC, con->parse.command, (guint8)status);
            return -1;
        }
        break;
    case COM_INIT_DB:
        g_debug("%s: COM_INIT_DB finish check.", G_STRLOC);
        is_finished = network_mysqld_proto_get_com_init_db(packet, con->parse.data, con);

        break;
    case COM_REFRESH:
    case COM_STMT_RESET:
    case COM_PING:
    case COM_TIME:
    case COM_REGISTER_SLAVE:
    case COM_PROCESS_KILL:
        err = err || network_mysqld_proto_get_int8(packet, &status);
        if (err)
            return -1;

        switch (status) {
        case MYSQLD_PACKET_ERR:
        case MYSQLD_PACKET_OK:
            is_finished = 1;
            break;
        default:
            g_debug_hexdump(G_STRLOC, S(packet->data));
            g_message("%s: got a 0x%02x packet for COM_[0%02x],expected only (ERR|OK)",
                      G_STRLOC, con->parse.command, (guint8)status);
            return -1;
        }
        break;
    case COM_DEBUG:
    case COM_SET_OPTION:
    case COM_SHUTDOWN:
        err = err || network_mysqld_proto_get_int8(packet, &status);
        if (err)
            return -1;

        switch (status) {
        case MYSQLD_PACKET_ERR:    /* COM_DEBUG may not have the right permissions */
        case MYSQLD_PACKET_EOF:
            is_finished = 1;
            break;
        default:
            g_debug_hexdump(G_STRLOC, S(packet->data));
            g_message("%s: got a 0x%02x packet for COM_[0%02x], expected only (ERR|EOF)",
                      G_STRLOC, con->parse.command, (guint8)status);
            return -1;
        }
        break;

    case COM_FIELD_LIST:
        err = err || network_mysqld_proto_get_int8(packet, &status);
        if (err)
            return -1;

        /* we transfer some data and wait for the EOF */
        switch (status) {
        case MYSQLD_PACKET_ERR:
        case MYSQLD_PACKET_EOF:
            is_finished = 1;
            break;

        case MYSQLD_PACKET_NULL:
        case MYSQLD_PACKET_OK:
            g_debug_hexdump(G_STRLOC, S(packet->data));
            g_message("%s: got a 0x%02x for COM_[0%02x], expected ERR, EOF or field data",
                      G_STRLOC, con->parse.command, (guint8)status);
            return -1;
        default:
            break;
        }
        break;
#if MYSQL_VERSION_ID >= 50000
    case COM_STMT_FETCH:
        /*  */
        err = err || network_mysqld_proto_peek_int8(packet, &status);
        if (err)
            return -1;

        switch (status) {
        case MYSQLD_PACKET_EOF:{
            network_mysqld_eof_packet_t *eof_packet = network_mysqld_eof_packet_new();

            err = err || network_mysqld_proto_get_eof_packet(packet, eof_packet);
            if (!err) {
                if ((eof_packet->server_status & SERVER_STATUS_LAST_ROW_SENT) ||
                    (eof_packet->server_status & SERVER_STATUS_CURSOR_EXISTS)) {
                    is_finished = 1;
                }
            }

            network_mysqld_eof_packet_free(eof_packet);

            break;
        }
        case MYSQLD_PACKET_ERR:
            is_finished = 1;
            break;
        default:
            break;
        }
        break;
#endif
    case COM_QUIT:             /* sometimes we get a packet before the connection closes */
    case COM_STATISTICS:
        /* just one packet, no EOF */
        is_finished = 1;

        break;
    case COM_STMT_PREPARE:
        is_finished = network_mysqld_proto_get_com_stmt_prep_result(packet, con->parse.data);
        break;
    case COM_STMT_EXECUTE:
        /* COM_STMT_EXECUTE result packets are basically the same as COM_QUERY ones,
         * the only difference is the encoding of the actual data - fields are in there, too.
         */
        is_finished = network_mysqld_proto_get_com_query_result(packet, con->parse.data, TRUE);
        break;
    case COM_PROCESS_INFO:
    case COM_QUERY:
        is_finished = network_mysqld_proto_get_com_query_result(packet, con->parse.data, FALSE);
        break;
    case COM_BINLOG_DUMP:
        /**
         * the binlog-dump event stops, forward all packets as we see them
         * and keep the command active
         */
        is_finished = 1;
        break;
    default:
        err = 1;
        break;
    }

    if (err)
        return -1;

    return is_finished;
}

int
network_mysqld_proto_get_fielddef(network_packet *packet, network_mysqld_proto_fielddef_t * field, guint32 capabilities)
{
    int err = 0;

    if (capabilities & CLIENT_PROTOCOL_41) {
        guint16 field_charsetnr;
        guint32 field_length;
        guint8 field_type;
        guint16 field_flags;
        guint8 field_decimals;

        err = err || network_mysqld_proto_get_lenenc_str(packet, &field->catalog, NULL);
        err = err || network_mysqld_proto_get_lenenc_str(packet, &field->db, NULL);
        err = err || network_mysqld_proto_get_lenenc_str(packet, &field->table, NULL);
        err = err || network_mysqld_proto_get_lenenc_str(packet, &field->org_table, NULL);
        err = err || network_mysqld_proto_get_lenenc_str(packet, &field->name, NULL);
        err = err || network_mysqld_proto_get_lenenc_str(packet, &field->org_name, NULL);

        err = err || network_mysqld_proto_skip(packet, 1);  /* filler */

        err = err || network_mysqld_proto_get_int16(packet, &field_charsetnr);
        err = err || network_mysqld_proto_get_int32(packet, &field_length);
        err = err || network_mysqld_proto_get_int8(packet, &field_type);
        err = err || network_mysqld_proto_get_int16(packet, &field_flags);
        err = err || network_mysqld_proto_get_int8(packet, &field_decimals);

        err = err || network_mysqld_proto_skip(packet, 2);  /* filler */
        if (!err) {
            field->charsetnr = field_charsetnr;
            field->length = field_length;
            field->type = field_type;
            field->flags = field_flags;
            field->decimals = field_decimals;
        }
    } else {
        guint8 len = 0;
        guint32 field_length;
        guint8 field_type;
        guint8 field_decimals;

        /* see protocol.cc Protocol::send_fields */

        err = err || network_mysqld_proto_get_lenenc_str(packet, &field->table, NULL);
        err = err || network_mysqld_proto_get_lenenc_str(packet, &field->name, NULL);
        err = err || network_mysqld_proto_get_int8(packet, &len);
        err = err || (len != 3);
        err = err || network_mysqld_proto_get_int24(packet, &field_length);
        err = err || network_mysqld_proto_get_int8(packet, &len);
        err = err || (len != 1);
        err = err || network_mysqld_proto_get_int8(packet, &field_type);
        err = err || network_mysqld_proto_get_int8(packet, &len);
        if (len == 3) {         /* the CLIENT_LONG_FLAG is set */
            guint16 field_flags;

            err = err || network_mysqld_proto_get_int16(packet, &field_flags);

            if (!err)
                field->flags = field_flags;
        } else if (len == 2) {
            guint8 field_flags;

            err = err || network_mysqld_proto_get_int8(packet, &field_flags);

            if (!err)
                field->flags = field_flags;
        } else {
            err = -1;
        }
        err = err || network_mysqld_proto_get_int8(packet, &field_decimals);

        if (!err) {
            field->charsetnr = 0x08 /* latin1_swedish_ci */ ;
            field->length = field_length;
            field->type = field_type;
            field->decimals = field_decimals;
        }
    }

    return err ? -1 : 0;
}

network_mysqld_ok_packet_t *
network_mysqld_ok_packet_new()
{
    network_mysqld_ok_packet_t *ok_packet;

    ok_packet = g_new0(network_mysqld_ok_packet_t, 1);

    return ok_packet;
}

void
network_mysqld_ok_packet_free(network_mysqld_ok_packet_t *ok_packet)
{
    if (!ok_packet)
        return;

    g_free(ok_packet);
}

/**
 * decode a OK packet from the network packet
 */
int
network_mysqld_proto_get_ok_packet(network_packet *packet, network_mysqld_ok_packet_t *ok_packet)
{
    guint8 field_count;
    guint64 affected, insert_id;
    guint16 server_status, warning_count = 0;
    guint32 capabilities = CLIENT_PROTOCOL_41;

    int err = 0;

    err = err || network_mysqld_proto_get_int8(packet, &field_count);
    if (err)
        return -1;

    if (field_count != 0) {
        g_critical("%s: expected the first byte to be 0, got %d", G_STRLOC, field_count);
        return -1;
    }

    err = err || network_mysqld_proto_get_lenenc_int(packet, &affected);
    err = err || network_mysqld_proto_get_lenenc_int(packet, &insert_id);
    err = err || network_mysqld_proto_get_int16(packet, &server_status);
    if (capabilities & CLIENT_PROTOCOL_41) {
        err = err || network_mysqld_proto_get_int16(packet, &warning_count);
    }

    if (!err) {
        ok_packet->affected_rows = affected;
        ok_packet->insert_id = insert_id;
        ok_packet->server_status = server_status;
        ok_packet->warnings = warning_count;
        g_debug("%s: server status, got: %d", G_STRLOC, ok_packet->server_status);
    }

    return err ? -1 : 0;
}

int
network_mysqld_proto_append_ok_packet(GString *packet, network_mysqld_ok_packet_t *ok_packet)
{
    guint32 capabilities = CLIENT_PROTOCOL_41;

    network_mysqld_proto_append_int8(packet, 0);    /* no fields */
    network_mysqld_proto_append_lenenc_int(packet, ok_packet->affected_rows);
    network_mysqld_proto_append_lenenc_int(packet, ok_packet->insert_id);
    network_mysqld_proto_append_int16(packet, ok_packet->server_status);    /* autocommit */
    if (capabilities & CLIENT_PROTOCOL_41) {
        network_mysqld_proto_append_int16(packet, ok_packet->warnings); /* no warnings */
    }

    return 0;
}

static network_mysqld_err_packet_t *
network_mysqld_err_packet_new_full(network_mysqld_protocol_t version)
{
    network_mysqld_err_packet_t *err_packet;

    err_packet = g_new0(network_mysqld_err_packet_t, 1);
    err_packet->sqlstate = g_string_new(NULL);
    err_packet->errmsg = g_string_new(NULL);
    err_packet->version = version;

    return err_packet;
}

network_mysqld_err_packet_t *
network_mysqld_err_packet_new()
{
    return network_mysqld_err_packet_new_full(NETWORK_MYSQLD_PROTOCOL_VERSION_41);
}

void
network_mysqld_err_packet_free(network_mysqld_err_packet_t *err_packet)
{
    if (!err_packet)
        return;

    g_string_free(err_packet->sqlstate, TRUE);
    g_string_free(err_packet->errmsg, TRUE);

    g_free(err_packet);
}

/**
 * decode an ERR packet from the network packet
 */
int
network_mysqld_proto_get_err_packet(network_packet *packet, network_mysqld_err_packet_t *err_packet)
{
    guint8 field_count, marker;
    guint16 errcode;
    gchar *sqlstate = NULL, *errmsg = NULL;
    guint32 capabilities = CLIENT_PROTOCOL_41;

    int err = 0;

    err = err || network_mysqld_proto_get_int8(packet, &field_count);
    if (err)
        return -1;

    if (field_count != MYSQLD_PACKET_ERR) {
        g_critical("%s: expected the first byte to be 0xff, got %d", G_STRLOC, field_count);
        return -1;
    }

    err = err || network_mysqld_proto_get_int16(packet, &errcode);
    if (capabilities & CLIENT_PROTOCOL_41) {
        err = err || network_mysqld_proto_get_int8(packet, &marker);
        err = err || (marker != '#');
        err = err || network_mysqld_proto_get_str_len(packet, &sqlstate, 5);
    }
    if (packet->offset < packet->data->len) {
        err = err || network_mysqld_proto_get_str_len(packet, &errmsg, packet->data->len - packet->offset);
    }

    if (!err) {
        err_packet->errcode = errcode;
        if (errmsg)
            g_string_assign(err_packet->errmsg, errmsg);
        g_string_assign(err_packet->sqlstate, sqlstate);
    }

    if (sqlstate)
        g_free(sqlstate);
    if (errmsg)
        g_free(errmsg);

    return err ? -1 : 0;
}

/**
 * create a ERR packet
 *
 * @note the sqlstate has to match the SQL standard. 
 *  If no matching SQL state is known, leave it at NULL
 *
 * @param packet      network packet
 * @param err_packet  the error structure
 *
 * @return 0 on success
 */
int
network_mysqld_proto_append_err_packet(GString *packet, network_mysqld_err_packet_t *err_packet)
{
    int errmsg_len;

    network_mysqld_proto_append_int8(packet, 0xff); /* ERR */
    network_mysqld_proto_append_int16(packet, err_packet->errcode); /* errorcode */
    if (err_packet->version == NETWORK_MYSQLD_PROTOCOL_VERSION_41) {
        g_string_append_c(packet, '#');
        if (err_packet->sqlstate && (err_packet->sqlstate->len > 0)) {
            g_string_append_len(packet, err_packet->sqlstate->str, 5);
        } else {
            g_string_append_len(packet, C("07000"));
        }
    }

    errmsg_len = err_packet->errmsg->len;
    if (errmsg_len >= 512)
        errmsg_len = 512;
    g_string_append_len(packet, err_packet->errmsg->str, errmsg_len);

    return 0;
}

network_mysqld_eof_packet_t *
network_mysqld_eof_packet_new()
{
    network_mysqld_eof_packet_t *eof_packet;

    eof_packet = g_new0(network_mysqld_eof_packet_t, 1);

    return eof_packet;
}

void
network_mysqld_eof_packet_free(network_mysqld_eof_packet_t *eof_packet)
{
    if (!eof_packet)
        return;

    g_free(eof_packet);
}

/**
 * decode a OK packet from the network packet
 */
int
network_mysqld_proto_get_eof_packet(network_packet *packet, network_mysqld_eof_packet_t *eof_packet)
{
    guint8 field_count;
    guint16 server_status, warning_count;

    int err = 0;

    err = err || network_mysqld_proto_get_int8(packet, &field_count);
    if (err)
        return -1;

    if (field_count != MYSQLD_PACKET_EOF) {
        g_critical("%s: expected the first byte to be 0xfe, got %d", G_STRLOC, field_count);
        return -1;
    }

    err = err || network_mysqld_proto_get_int16(packet, &warning_count);
    err = err || network_mysqld_proto_get_int16(packet, &server_status);
    if (!err) {
        eof_packet->server_status = server_status;
        eof_packet->warnings = warning_count;
        g_debug("%s: server status, got: %d", G_STRLOC, eof_packet->server_status);
    }

    return err ? -1 : 0;
}

network_mysqld_auth_challenge *
network_mysqld_auth_challenge_new()
{
    network_mysqld_auth_challenge *shake;

    shake = g_new0(network_mysqld_auth_challenge, 1);

    shake->auth_plugin_data = g_string_new("");
    shake->capabilities = CETUS_DEFAULT_FLAGS;
    shake->auth_plugin_name = g_string_new(NULL);

    return shake;
}

void
network_mysqld_auth_challenge_free(network_mysqld_auth_challenge *shake)
{
    if (!shake)
        return;

    if (shake->server_version_str)
        g_free(shake->server_version_str);
    if (shake->auth_plugin_data)
        g_string_free(shake->auth_plugin_data, TRUE);
    if (shake->auth_plugin_name)
        g_string_free(shake->auth_plugin_name, TRUE);

    g_free(shake);
}

void
network_mysqld_auth_challenge_set_challenge(network_mysqld_auth_challenge *shake)
{
    guint i;

    /* 20 chars */

    g_string_set_size(shake->auth_plugin_data, 21);

    for (i = 0; i < 20; i++) {
        /* 33 - 127 are printable characters */
        shake->auth_plugin_data->str[i] = (94.0 * (rand() / (RAND_MAX + 1.0))) + 33;
    }

    shake->auth_plugin_data->len = 21;
    shake->auth_plugin_data->str[shake->auth_plugin_data->len - 1] = '\0';
    g_string_assign(shake->auth_plugin_name, "mysql_native_password");
}

int network_mysqld_proto_append_auth_switch(GString *packet, char *method_name, GString *salt)
{
    network_mysqld_proto_append_int8(packet, 0xfe);
    /*TODO: different algorithms for methods */
    g_string_append_len(packet, method_name, strlen(method_name));
    g_string_append_c(packet, 0);
    g_string_append_len(packet, salt->str, salt->len);
    return 0;
}

int
network_mysqld_proto_get_auth_challenge(network_packet *packet, network_mysqld_auth_challenge *shake)
{
    int maj, min, patch;
    gchar *auth_plugin_data_1 = NULL, *auth_plugin_data_2 = NULL;
    guint16 capabilities1, capabilities2;
    guint8 status;
    int err = 0;
    guint8 auth_plugin_data_len;

    err = err || network_mysqld_proto_get_int8(packet, &status);

    if (err)
        return -1;

    switch (status) {
    case 0xff:
        return -1;
    case 0x0a:
        break;
    default:
        g_debug("%s: unknown protocol %d", G_STRLOC, status);
        return -1;
    }

    err = err || network_mysqld_proto_get_string(packet, &shake->server_version_str);
    err = err || (NULL == shake->server_version_str);   /* the server-version has to be set */

    err = err || network_mysqld_proto_get_int32(packet, &shake->thread_id);

    /**
     * get the scramble buf
     *
     * 8 byte here and some the other 12 sometime later
     */
    err = err || network_mysqld_proto_get_str_len(packet, &auth_plugin_data_1, 8);

    err = err || network_mysqld_proto_skip(packet, 1);

    err = err || network_mysqld_proto_get_int16(packet, &capabilities1);
    err = err || network_mysqld_proto_get_int8(packet, &shake->charset);
    err = err || network_mysqld_proto_get_int16(packet, &shake->server_status);

    /* capabilities is extended in 5.5.x to carry 32bits to announce CLIENT_PLUGIN_AUTH */
    err = err || network_mysqld_proto_get_int16(packet, &capabilities2);
    err = err || network_mysqld_proto_get_int8(packet, &auth_plugin_data_len);

    err = err || network_mysqld_proto_skip(packet, 10);

    if (!err) {
        shake->capabilities = capabilities1 | (capabilities2 << 16);

        if (shake->capabilities & CLIENT_PLUGIN_AUTH) {
            guint8 auth_plugin_data2_len = 0;

            /* CLIENT_PLUGIN_AUTH enforces auth_plugin_data_len
             *
             * we have at least 12 bytes */

            if (auth_plugin_data_len > 8) {
                auth_plugin_data2_len = auth_plugin_data_len - 8;
            }

            err = network_mysqld_proto_get_str_len(packet, &auth_plugin_data_2, auth_plugin_data2_len);
            err = err || network_mysqld_proto_skip(packet, 12 - MIN(12, auth_plugin_data2_len));
            if (!err) {
                /* Bug#59453 ... MySQL 5.5.7-9 and 5.6.0-1 don't send a trailing \0
                 *
                 * if there is no trailing \0, get the rest of the packet
                 */
                if (0 != network_mysqld_proto_get_gstr(packet, shake->auth_plugin_name)) {
                    err = err || network_mysqld_proto_get_gstr_len(packet,
                                                                   packet->data->len - packet->offset,
                                                                   shake->auth_plugin_name);
                }
            }
        } else {
            err = err || network_mysqld_proto_get_str_len(packet, &auth_plugin_data_2, 12);
            err = err || network_mysqld_proto_skip(packet, 1);
        }
    }

    if (!err) {
        /* process the data */

        if (3 != sscanf(shake->server_version_str, "%d.%d.%d%*s", &maj, &min, &patch)) {
            /* can't parse the protocol */

            g_critical("%s: protocol 10, but version number not parsable", G_STRLOC);

            return -1;
        }

        /**
         * out of range 
         */
        if (min < 0 || min > 100 || patch < 0 || patch > 100 || maj < 0 || maj > 10) {
            g_critical("%s: protocol 10, but version number out of range", G_STRLOC);

            return -1;
        }

        shake->server_version = maj * 10000 + min * 100 + patch;

        /**
         * build auth_plugin_data
         *
         * auth_plugin_data_1 + auth_plugin_data_2 == auth_plugin_data
         */
        g_string_truncate(shake->auth_plugin_data, 0);

        if (shake->capabilities & CLIENT_PLUGIN_AUTH) {
            g_string_assign_len(shake->auth_plugin_data, auth_plugin_data_1, MIN(8, auth_plugin_data_len));
            if (auth_plugin_data_len > 8) {
                g_string_append_len(shake->auth_plugin_data, auth_plugin_data_2, auth_plugin_data_len - 8);
            }
        } else {
            g_string_assign_len(shake->auth_plugin_data, auth_plugin_data_1, 8);
            g_string_append_len(shake->auth_plugin_data, auth_plugin_data_2, 12);
        }

        /* some final assertions */
        if (shake->capabilities & CLIENT_PLUGIN_AUTH) {
            if (shake->auth_plugin_data->len != auth_plugin_data_len) {
                err = 1;
            }
        } else {
            if (shake->auth_plugin_data->len != 20) {
                err = 1;
            }
        }
    }

    if (auth_plugin_data_1)
        g_free(auth_plugin_data_1);
    if (auth_plugin_data_2)
        g_free(auth_plugin_data_2);

    return err ? -1 : 0;
}

int
network_mysqld_proto_append_auth_challenge(GString *packet, network_mysqld_auth_challenge *shake)
{
    guint i;

    network_mysqld_proto_append_int8(packet, 0x0a);
    if (shake->server_version_str) {
        g_string_append(packet, shake->server_version_str);
    } else if (shake->server_version > 30000 && shake->server_version < 100000) {
        g_string_append_printf(packet, "%d.%02d.%02d",
                               shake->server_version / 10000,
                               (shake->server_version % 10000) / 100, shake->server_version % 100);
    } else {
        g_string_append_len(packet, C("5.0.99"));
    }
    network_mysqld_proto_append_int8(packet, 0x00);
    network_mysqld_proto_append_int32(packet, shake->thread_id);
    if (shake->auth_plugin_data->len) {
        g_assert_cmpint(shake->auth_plugin_data->len, >=, 8);
        g_string_append_len(packet, shake->auth_plugin_data->str, 8);
    } else {
        g_string_append_len(packet, C("01234567"));
    }
    network_mysqld_proto_append_int8(packet, 0x00); /* filler */
    network_mysqld_proto_append_int16(packet, shake->capabilities & 0xffff);
    network_mysqld_proto_append_int8(packet, shake->charset);
    network_mysqld_proto_append_int16(packet, shake->server_status);
    network_mysqld_proto_append_int16(packet, (shake->capabilities >> 16) & 0xffff);

    if (shake->capabilities & CLIENT_PLUGIN_AUTH) {
        g_assert_cmpint(shake->auth_plugin_data->len, <, 255);
        network_mysqld_proto_append_int8(packet, shake->auth_plugin_data->len);
    } else {
        network_mysqld_proto_append_int8(packet, 0);
    }

    /* add the fillers */
    for (i = 0; i < 10; i++) {
        network_mysqld_proto_append_int8(packet, 0x00);
    }

    if (shake->capabilities & CLIENT_PLUGIN_AUTH) {
        g_assert_cmpint(shake->auth_plugin_data->len, >=, 8);
        g_string_append_len(packet, shake->auth_plugin_data->str + 8, shake->auth_plugin_data->len - 8);

        g_string_append_len(packet, S(shake->auth_plugin_name));
        g_string_append_c(packet, 0x00);
    } else {
        /* if we only have SECURE_CONNECTION it is 0-terminated */
        if (shake->auth_plugin_data->len) {
            g_assert_cmpint(shake->auth_plugin_data->len, >=, 8);
            g_string_append_len(packet, shake->auth_plugin_data->str + 8, shake->auth_plugin_data->len - 8);
        } else {
            g_string_append_len(packet, C("890123456789"));
        }
        network_mysqld_proto_append_int8(packet, 0x00);
    }

    return 0;
}

network_mysqld_auth_response *
network_mysqld_auth_response_new(guint32 server_capabilities)
{
    network_mysqld_auth_response *auth;

    auth = g_new0(network_mysqld_auth_response, 1);

    /* we have to make sure scramble->buf is not-NULL to get
     * the "empty string" and not a "NULL-string"
     */
    auth->auth_plugin_data = g_string_new("");
    auth->auth_plugin_name = g_string_new(NULL);
    auth->username = g_string_new("");
    auth->database = g_string_new("");
    auth->client_capabilities = CETUS_DEFAULT_FLAGS;
    auth->server_capabilities = server_capabilities;

    return auth;
}

void
network_mysqld_auth_response_free(network_mysqld_auth_response *auth)
{
    if (!auth)
        return;

    if (auth->auth_plugin_data)
        g_string_free(auth->auth_plugin_data, TRUE);
    if (auth->auth_plugin_name)
        g_string_free(auth->auth_plugin_name, TRUE);
    if (auth->username)
        g_string_free(auth->username, TRUE);
    if (auth->database)
        g_string_free(auth->database, TRUE);

    g_free(auth);
}

int
network_mysqld_proto_get_auth_response(network_packet *packet, network_mysqld_auth_response *auth)
{
    int err = 0;
    guint16 l_cap;
    /* extract the default db from it */

    /*
     * @\0\0\1
     *  \215\246\3\0 - client-flags
     *  \0\0\0\1     - max-packet-len
     *  \10          - charset-num
     *  \0\0\0\0
     *  \0\0\0\0
     *  \0\0\0\0
     *  \0\0\0\0
     *  \0\0\0\0
     *  \0\0\0       - fillers
     *  root\0       - username
     *  \24          - len of the scrambled buf
     *    ~    \272 \361 \346
     *    \211 \353 D    \351
     *    \24  \243 \223 \257
     *    \0   ^    \n   \254
     *    t    \347 \365 \244
     *  
     *  world\0
     */

    /* 4.0 uses 2 byte, 4.1+ uses 4 bytes, but the proto-flag is in the lower 2 bytes */
    err = err || network_mysqld_proto_peek_int16(packet, &l_cap);
    if (err)
        return -1;

    if (l_cap & CLIENT_PROTOCOL_41) {
        err = err || network_mysqld_proto_get_int32(packet, &auth->client_capabilities);
        err = err || network_mysqld_proto_get_int32(packet, &auth->max_packet_size);
        err = err || network_mysqld_proto_get_int8(packet, &auth->charset);

        err = err || network_mysqld_proto_skip(packet, 23);

        if (err == 0
            && (auth->client_capabilities & CLIENT_SSL)
            && packet->offset == packet->data->len) /* this is a SSLRequest */
        {
            auth->ssl_request = TRUE;
            return 0;
        }

        err = err || network_mysqld_proto_get_gstr(packet, auth->username);

        guint8 len;
        /* new auth is 1-byte-len + data */
        err = err || network_mysqld_proto_get_int8(packet, &len);
        err = err || network_mysqld_proto_get_gstr_len(packet, len, auth->auth_plugin_data);

        if ((auth->server_capabilities & CLIENT_CONNECT_WITH_DB) &&
            (auth->client_capabilities & CLIENT_CONNECT_WITH_DB)) {
            err = err || network_mysqld_proto_get_gstr(packet, auth->database);
        }

        if ((auth->server_capabilities & CLIENT_PLUGIN_AUTH) && (auth->client_capabilities & CLIENT_PLUGIN_AUTH)) {
            err = err || network_mysqld_proto_get_gstr(packet, auth->auth_plugin_name);
        }
    } else {
        err = err || network_mysqld_proto_get_int16(packet, &l_cap);
        err = err || network_mysqld_proto_get_int24(packet, &auth->max_packet_size);
        err = err || network_mysqld_proto_get_gstr(packet, auth->username);
        if (packet->data->len != packet->offset) {
            /* if there is more, it is the password without a terminating \0 */
            err = err || network_mysqld_proto_get_gstr_len(packet,
                                                           packet->data->len - packet->offset, auth->auth_plugin_data);
        }

        if (!err) {
            auth->client_capabilities = l_cap;
        }
    }

    return err ? -1 : 0;
}

/**
 * append the auth struct to the mysqld packet
 */
int
network_mysqld_proto_append_auth_response(GString *packet, network_mysqld_auth_response *auth)
{
    if (!(auth->client_capabilities & CLIENT_PROTOCOL_41)) {
        network_mysqld_proto_append_int16(packet, auth->client_capabilities);
        network_mysqld_proto_append_int24(packet, auth->max_packet_size);   /* max-allowed-packet */

        if (auth->username->len)
            g_string_append_len(packet, S(auth->username));
        network_mysqld_proto_append_int8(packet, 0x00); /* trailing \0 */

        if (auth->auth_plugin_data->len) {
            g_string_append_len(packet, S(auth->auth_plugin_data)); /* no trailing \0 */
        }
    } else {
        network_mysqld_proto_append_int32(packet, auth->client_capabilities);
        network_mysqld_proto_append_int32(packet, auth->max_packet_size);   /* max-allowed-packet */

        network_mysqld_proto_append_int8(packet, auth->charset);    /* charset */

        int i;
        for (i = 0; i < 23; i++) {  /* filler */
            network_mysqld_proto_append_int8(packet, 0x00);
        }

        if (auth->username->len)
            g_string_append_len(packet, S(auth->username));
        network_mysqld_proto_append_int8(packet, 0x00); /* trailing \0 */

        /* scrambled password */

        /* server supports the secure-auth (4.1+) which is 255 bytes max
         *
         * if ->len is longer than 255, wrap around ... should be reported back
         * to the upper layers
         */
        network_mysqld_proto_append_int8(packet, auth->auth_plugin_data->len);
        g_string_append_len(packet, auth->auth_plugin_data->str, auth->auth_plugin_data->len & 0xff);

        if ((auth->server_capabilities & CLIENT_CONNECT_WITH_DB) && (auth->database->len > 0)) {
            g_string_append_len(packet, S(auth->database));
            network_mysqld_proto_append_int8(packet, 0x00); /* trailing \0 */
        }

        if ((auth->client_capabilities & CLIENT_PLUGIN_AUTH) && (auth->server_capabilities & CLIENT_PLUGIN_AUTH)) {
            g_string_append_len(packet, S(auth->auth_plugin_name));
            network_mysqld_proto_append_int8(packet, 0x00); /* trailing \0 */
        }
    }

    return 0;
}

int
network_mysqld_proto_get_stmt_id(network_packet *packet, guint32 *stmt_id)
{
    guint8 packet_type;
    int err = 0;

    err = err || network_mysqld_proto_get_int8(packet, &packet_type);
    if (err)
        return -1;

    if (COM_STMT_EXECUTE != packet_type && COM_STMT_CLOSE != packet_type) {
        g_critical("%s: expected the first byte to be %02x or %02x, got %02x",
                   G_STRLOC, COM_STMT_EXECUTE, COM_STMT_CLOSE, packet_type);
        return -1;
    }

    err = err || network_mysqld_proto_get_int32(packet, stmt_id);

    return err ? -1 : 0;
}

int
network_mysqld_proto_change_stmt_id_from_ok_packet(network_packet *packet, int server_index)
{
    guint8 packet_type;
    int err = 0;
    int *p = NULL;

    err = err || network_mysqld_proto_get_int8(packet, &packet_type);
    if (err)
        return -1;

    if (0x00 != packet_type) {
        g_debug("%s: expected the first byte to be %02x, got %02x", G_STRLOC, 0x00, packet_type);
        return -1;
    }

    p = (int *)(((unsigned char *)packet->data->str) + packet->offset);

    /* if (*p > MAX_STMT_ID) {
       } */

    g_debug("%s: stmt id:%d, server index:%d", G_STRLOC, *p, server_index);

    *p = *p & 0x00007fff;

    *p = *p | (server_index << 16);

    g_debug("%s: new stmt id:%d, server index:%d", G_STRLOC, *p, server_index);

    return 0;
}

int
network_mysqld_proto_change_stmt_id_from_clt_stmt(network_packet *packet)
{
    guint8 packet_type;
    int err = 0;
    int *p = NULL;

    err = err || network_mysqld_proto_get_int8(packet, &packet_type);
    if (err)
        return -1;

    if (COM_STMT_EXECUTE != packet_type && COM_STMT_CLOSE != packet_type) {
        g_critical("%s: expected the first byte to be %02x or %02x, got %02x",
                   G_STRLOC, COM_STMT_EXECUTE, COM_STMT_CLOSE, packet_type);
        return -1;
    }

    p = (int *)(((unsigned char *)packet->data->str) + packet->offset);

    *p = *p & 0x00007fff;

    return 0;
}

int
network_mysqld_proto_append_query_packet(GString *packet, const char *query)
{
    network_mysqld_proto_append_int8(packet, COM_QUERY);
    g_string_append_len(packet, query, strlen(query));
    return 0;
}

int
mysqld_proto_append_change_user_packet(GString *packet, mysqld_change_user_packet_t *chuser)
{
    network_mysqld_proto_append_int8(packet, COM_CHANGE_USER);

    /* user name as string.NUL */
    g_string_append_len(packet, chuser->username->str, chuser->username->len + 1);

    if (!chuser->auth_plugin_data) {
        g_string_append_c(packet, 0);
    } else {
        GString *auth_response = g_string_new(NULL);
        network_mysqld_proto_password_scramble(auth_response, S(chuser->auth_plugin_data), S(chuser->hashed_pwd));

        /* auth response length as int<1> */
        network_mysqld_proto_append_int8(packet, auth_response->len);
        /* auth response as string.var_len */
        g_string_append_len(packet, S(auth_response));
        g_string_free(auth_response, TRUE);
    }
    /* schema name as string.NUL */
    g_string_append_len(packet, chuser->database->str, chuser->database->len + 1);
    /* charset as int<2> */
    network_mysqld_proto_append_int16(packet, chuser->charset);
    return 0;
}
