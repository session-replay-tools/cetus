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

#ifndef _NETWORK_SOCKET_H_
#define _NETWORK_SOCKET_H_

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "network-exports.h"
#include "network-queue.h"

#ifdef HAVE_SYS_TIME_H
/**
 * event.h needs struct timeval and doesn't include sys/time.h itself
 */
#include <sys/time.h>
#endif

#include <sys/types.h>      /** u_char */
#include <sys/socket.h>     /** struct sockaddr */

#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>     /** struct sockaddr_in */
#endif
#include <netinet/tcp.h>

#ifdef HAVE_SYS_UN_H
#include <sys/un.h>         /** struct sockaddr_un */
#endif
#define closesocket(x) close(x)
#include <glib.h>
#include <event.h>

#include "network-address.h"

typedef enum {
    NETWORK_SOCKET_SUCCESS,
    NETWORK_SOCKET_WAIT_FOR_EVENT,
    NETWORK_SOCKET_WAIT_FOR_WRITABLE,
    NETWORK_SOCKET_ERROR,
    NETWORK_SOCKET_ERROR_RETRY
} network_socket_retval_t;

#define MAX_QUERY_CACHE_SIZE 65536

typedef struct network_mysqld_auth_challenge network_mysqld_auth_challenge;
typedef struct network_mysqld_auth_response network_mysqld_auth_response;

typedef struct server_state_data {
    guint32 len;
    int command;

    int qs_state;

    union {
        struct {
            int want_eofs;
            int first_packet;
        } prepare;

        int query_type;

        struct {
            char state;
        } auth_result;

        struct {
            GString *db_name;
        } init_db;
    } state;
} server_state_data;

typedef struct {
    int server_status;
    int warning_count;
    guint64 affected_rows;
    guint64 insert_id;

    int was_resultset;

    int query_status;
} server_query_status;

typedef struct network_ssl_connection_s network_ssl_connection_t;

#define XID_LEN 128

typedef struct {
    int socket_type; /**< SOCK_STREAM or SOCK_DGRAM for now */
    int fd;             /**< socket-fd */
    time_t create_time;
    time_t update_time;

    struct event event; /**< events for this fd */

    network_address *src; /**< getsockname() */
    network_address *dst; /**< getpeername() */


    /**< internal tracking of the packet_id's the automaticly set the next good packet-id */
    guint8 last_packet_id;
    guint8 compressed_packet_id;

    /**< internal tracking of the packet_id sequencing */
    gboolean packet_id_is_reset;

    network_queue *recv_queue;
    network_queue *recv_queue_raw;
    network_queue *recv_queue_uncompress_raw;
    network_queue *recv_queue_decrypted_raw;

    network_queue *send_queue;
    network_queue *send_queue_compressed;
    network_queue *cache_queue;

    GString *last_compressed_packet;
    int compressed_unsend_offset;
    int total_output;

    off_t to_read;
    long long resp_len;

    /**
     * data extracted from the handshake  
     */
    network_mysqld_auth_challenge *challenge;
    network_mysqld_auth_response *response;

    unsigned int is_authed:1;           /** did a client already authed this connection */
    unsigned int is_server_conn_reserved:1;

    /* if not in a trx, use a "short timeout" to trigger (proxy_timeout),
       so that server conns will be returned to pool immediately  */
    unsigned int is_need_q_peek_exec:1;
    unsigned int is_multi_stmt_set:1;
    unsigned int is_closed:1;
    unsigned int unavailable:1;
    unsigned int is_reset_conn_supported:1;
    unsigned int is_in_sess_context:1;
    unsigned int is_in_tran_context:1;
    unsigned int is_robbed:1;
    unsigned int is_waiting:1;
    unsigned int is_read_only:1;
    unsigned int is_read_finished:1;
    unsigned int query_cache_too_long:1;
    unsigned int max_header_size_reached:1;
    unsigned int do_compress:1;
    unsigned int do_strict_compress:1;
    unsigned int do_query_cache:1;
    unsigned int write_uncomplete:1; /* only valid for compresssion */

    guint8 charset_code;

    /**
     * store the default-db of the socket
     *
     * the client might have a different default-db than the server-side due to
     * statement balancing
     */
    GString *default_db;     /** default-db of this side of the connection */
    /* only used for server */
    GString *username;
    GString *group;

    GString *charset;
    GString *charset_client;
    GString *charset_connection;
    GString *charset_results;
    GString *sql_mode;
    server_state_data parse;
    server_query_status qstat;

    network_ssl_connection_t *ssl;

#ifndef SIMPLE_PARSER
    unsigned long long xa_id;
    char xid_str[XID_LEN];
#endif

} network_socket;

NETWORK_API network_socket *network_socket_new(void);
NETWORK_API void network_socket_free(network_socket *s);
NETWORK_API network_socket_retval_t network_socket_write(network_socket *con, int send_chunks);
NETWORK_API void network_socket_send_quit_and_free(network_socket *s);
NETWORK_API network_socket_retval_t network_socket_read(network_socket *con);
NETWORK_API network_socket_retval_t network_socket_to_read(network_socket *sock);
NETWORK_API network_socket_retval_t network_socket_set_non_blocking(network_socket *sock);
NETWORK_API network_socket_retval_t network_socket_connect(network_socket *con);
NETWORK_API network_socket_retval_t network_socket_connect_finish(network_socket *sock);
NETWORK_API network_socket_retval_t network_socket_bind(network_socket *con, int advanced_mode);
NETWORK_API network_socket *network_socket_accept(network_socket *srv, int *reason);
NETWORK_API network_socket_retval_t network_socket_set_send_buffer_size(network_socket *sock, int size);

#endif
