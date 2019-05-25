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

#ifndef _NETWORK_MYSQLD_H_
#define _NETWORK_MYSQLD_H_

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_SYS_TIME_H
/**
 * event.h needs struct timeval and doesn't include sys/time.h itself
 */
#include <sys/time.h>
#endif

#include <sys/types.h>

#include <unistd.h>

#include <mysql.h>

#include <glib.h>

#include "network-exports.h"

#include "network-injection.h"
#include "network-socket.h"
#include "network-conn-pool.h"
#include "chassis-plugin.h"
#include "chassis-mainloop.h"
#include "chassis-timings.h"
#include "sys-pedantic.h"
#include "network-backend.h"
#include "cetus-error.h"

#define ANALYSIS_PACKET_LEN 5
#define RECORD_PACKET_LEN 11
#define COMPRESS_BUF_SIZE 1048576

typedef enum {
    PROXY_NO_DECISION,
    PROXY_NO_CONNECTION,        /* TODO: this one shouldn't be here, it's not a dicsion */
    PROXY_SEND_QUERY,
    PROXY_WAIT_QUERY_RESULT,
    PROXY_SEND_RESULT,
    PROXY_SEND_INJECTION,
    PROXY_SEND_NONE,
    PROXY_IGNORE_RESULT,       /** for read_query_result */
    PROXY_CLIENT_QUIT
} network_mysqld_stmt_ret;

typedef struct network_mysqld_con network_mysqld_con;   /* forward declaration */

/**
 * A macro that produces a plugin callback function pointer declaration.
 */
#define NETWORK_MYSQLD_PLUGIN_FUNC(x) network_socket_retval_t (*x)(chassis *, network_mysqld_con *)
/**
 * The prototype for plugin callback functions.
 * 
 * Some plugins don't use the global "chas" pointer, thus it is marked "unused" for GCC.
 */
#define NETWORK_MYSQLD_PLUGIN_PROTO(x) static network_socket_retval_t x(chassis G_GNUC_UNUSED *chas, network_mysqld_con *con)

/**
 * The function pointers to plugin callbacks 
 * for each customizable state in the MySQL Protocol.
 * 
 * Any of these callbacks can be NULL, 
 * in which case the default pass-through behavior will be used.
 * 
 * The function prototype is defined by #NETWORK_MYSQLD_PLUGIN_PROTO, 
 * which is used in each plugin to define the callback.
 * #NETWORK_MYSQLD_PLUGIN_FUNC can be used to create a function pointer declaration.
 */
typedef struct {
    /**
     * Called when a new client connection to cetus was created.
     */
    NETWORK_MYSQLD_PLUGIN_FUNC(con_init);
    /**
     * Called when cetus needs to establish a connection to a backend server
     *
     * Returning a handshake response packet from this callback will 
     * cause the con_read_handshake step to be skipped.
     *
     * The next state then is con_send_handshake.
     */
    NETWORK_MYSQLD_PLUGIN_FUNC(con_connect_server);
    /**
     * Called when cetus has read the handshake packet from the server.
     */
    NETWORK_MYSQLD_PLUGIN_FUNC(con_read_handshake);
    /**
     * Called when cetus wants to send the handshake packet to the client.
     * 
     * @note No known plugins actually implement this step right now, 
     * but rather return a handshake challenge from con_init instead.
     */
    NETWORK_MYSQLD_PLUGIN_FUNC(con_send_handshake);
    /**
     * Called when cetus has read the authentication packet from the client.
     */
    NETWORK_MYSQLD_PLUGIN_FUNC(con_read_auth);
    /**
     * Called when cetus wants to send the authentication packet to the server.
     * 
     * @note No known plugins actually implement this step.
     */
    NETWORK_MYSQLD_PLUGIN_FUNC(con_send_auth);
    /**
     * Called when cetus has read the authentication result 
     * from the backend server, in response to con_send_auth.
     */
    NETWORK_MYSQLD_PLUGIN_FUNC(con_read_auth_result);
    /**
     * Called when cetus wants to send the authentication response packet to the client.
     * 
     * @note No known plugins implement this callback, but the default 
     * implementation deals with the important case that
     *
     * the authentication response used the pre-4.1 password hash method, but the client didn't.
     * @see network_mysqld_con::auth_result_state
     */
    NETWORK_MYSQLD_PLUGIN_FUNC(con_send_auth_result);
    /**
     * Called when cetus receives a COM_QUERY packet from a client.
     */
    NETWORK_MYSQLD_PLUGIN_FUNC(con_read_query);
    NETWORK_MYSQLD_PLUGIN_FUNC(con_get_server_conn_list);
    /**
     * Called when cetus receives a result set from the server.
     */
    NETWORK_MYSQLD_PLUGIN_FUNC(con_read_query_result);
    /**
     * Called when cetus sends a result set to the client.
     * 
     * The proxy plugin, for example, uses this state to inject more queries 
     * into the connection, possibly in response to a result set received from a server.
     * 
     * This callback should not cause multiple result sets to be sent to the client.
     * @see network_mysqld_con_injection::sent_resultset
     */
    NETWORK_MYSQLD_PLUGIN_FUNC(con_send_query_result);
    /**
     * Called when either side of a connection was either 
     * closed or some network error occurred.
     * 
     * Usually this is called because a client has disconnected. 
     * Plugins might want to preserve the server connection in this case
     * and reuse it later. In this case the connection state will be ::ST_CLOSE_CLIENT.
     * 
     * When an error on the server connection occurred, this callback is 
     * usually used to close the client connection as well.
     *
     * In this case the connection state will be ::ST_CLOSE_SERVER.
     * 
     * @note There are no two separate callback functions for the two possibilities, 
     * which probably is a deficiency.
     */
    NETWORK_MYSQLD_PLUGIN_FUNC(con_cleanup);

    NETWORK_MYSQLD_PLUGIN_FUNC(con_exectute_sql);

    NETWORK_MYSQLD_PLUGIN_FUNC(con_timeout);
} network_mysqld_hooks;

/**
 * A structure containing the parsed packet for a command packet as well 
 * as the common parts necessary to find the correct packet parsing function.
 * 
 * The correct parsing function is chose by looking at both the current state 
 * as well as the command in this structure.
 * 
 * @todo Currently the plugins are responsible for setting the first two 
 * fields of this structure. We have to investigate
 * how we can refactor this into a more generic way.
 */
struct network_mysqld_con_parse {
    /**< The command indicator from the MySQL Protocol */
    enum enum_server_command command;

    /**< An opaque pointer to a parsed command structure, parsed resultset */
    gpointer data;

    /**< A function pointer to the appropriate "free" function of data */
    void (*data_free) (gpointer);
};

/**
 * The possible states in the MySQL Protocol.
 * 
 * Not all of the states map directly to plugin callbacks. 
 * Those states that have no corresponding plugin callbacks are marked as
 *
 * <em>internal state</em>.
 */
typedef enum {
    /**< A new client connection was established */
    ST_INIT = 0,

    /**< A connection to a backend is about to be made */
    ST_CONNECT_SERVER,

    /**< A handshake packet is to be sent to a client */
    ST_SEND_HANDSHAKE,

    /**< An authentication packet is to be read from a client */
    ST_READ_AUTH,

    ST_FRONT_SSL_HANDSHAKE,

    /**< The result of an authentication attempt is to be sent to a client */
    ST_SEND_AUTH_RESULT,

    /**< COM_QUERY packets are to be read from a client */
    ST_READ_QUERY,

    ST_GET_SERVER_CONNECTION_LIST,

    /**< COM_QUERY packets are to be sent to a server */
    ST_SEND_QUERY,

    /**< Result set packets are to be read from a server */
    ST_READ_QUERY_RESULT,

    ST_READ_M_QUERY_RESULT,

    /**< Result set packets are to be sent to a client */
    ST_SEND_QUERY_RESULT,

    /**< The client connection should be closed */
    ST_CLOSE_CLIENT,

    ST_CLIENT_QUIT,

    /**
     * < An unrecoverable error occurred, leads to sending a MySQL ERR packet 
     * to the client and closing the client connection
     */
    ST_SEND_ERROR,

    /**
     * < An error occurred (malformed/unexpected packet, 
     * unrecoverable network error), internal state 
     */
    ST_ERROR,

    /**< The server connection should be closed */
    ST_CLOSE_SERVER,

} network_mysqld_con_state_t;

typedef enum {
    /* XA state */
    NEXT_ST_XA_START = 1,
    NEXT_ST_XA_QUERY = 2,
    NEXT_ST_XA_END = 3,
    NEXT_ST_XA_PREPARE = 4,
    NEXT_ST_XA_COMMIT = 5,
    NEXT_ST_XA_ROLLBACK = 6,
    NEXT_ST_XA_CANDIDATE_OVER = 7,
    NEXT_ST_XA_OVER = 8
} network_mysqld_con_dist_tran_state_t;

typedef enum {
    RET_SUCCESS = 10,
    RET_ERROR
} retval_t;

typedef struct server_connection_state_t server_connection_state_t;

typedef enum {
    ST_ASYNC_CONN,
    ST_ASYNC_READ_HANDSHAKE,
    ST_ASYNC_SEND_AUTH,
    ST_ASYNC_READ_AUTH_RESULT,
    ST_ASYNC_SEND_QUERY,
    ST_ASYNC_READ_QUERY_RESULT,
    ST_ASYNC_OVER,
    ST_ASYNC_ERROR,
} self_con_state_t;

typedef enum {
    /* connection attribute adjuestment */
    ATTR_START = 0,
    ATTR_DIF_CHANGE_USER = 1,
    ATTR_DIF_DEFAULT_DB = 2,
    ATTR_DIF_SQL_MODE = 4,
    ATTR_DIF_CHARSET = 8,
    ATTR_DIF_SET_OPTION = 16,
    ATTR_DIF_SET_AUTOCOMMIT = 32,   /* TODO: START TRANSACTION */
} session_attr_flags_t;

typedef struct {
    gint64 offset;
    gint64 row_count;
} limit_t;

typedef struct merge_parameters_s {
    void *heap;
    void *elements;
    network_queue *send_queue;
    GPtrArray *recv_queues;
    GList **candidates;
    GString *err_pack;
    limit_t limit;
    guint pkt_count;
    int pack_err_met;
    int is_distinct;
    int row_cnter;
    int off_pos;
    int is_pack_err;
    int aggr_output_len;

} merge_parameters_t;

struct server_connection_state_t {

    self_con_state_t state;

    struct timeval connect_timeout;
    struct timeval read_timeout;
    struct timeval write_timeout;

    GString *hashed_pwd;
    network_backend_t *backend;
    network_socket *server;
    chassis *srv;
    network_connection_pool *pool;
    unsigned int query_id_to_be_killed;
    unsigned int is_multi_stmt_set:1;
    unsigned int retry_cnt:4;
    guint8 charset_code;
};

typedef enum {
    ST_PROXY_OK = 0,
    ST_PROXY_QUIT = 1
} proxy_session_state_t;

typedef enum {
    RM_SUCCESS,
    RM_FAIL,
} result_merge_status_t;

typedef struct result_merge_t {
    result_merge_status_t status;
    GString *detail;
} result_merge_t;

typedef struct having_condition_t {
    int rel_type;
    int data_type;
    char *condition_value;
    int column_index;
} having_condition_t;

typedef struct mysqld_query_attr_t {
    unsigned int sql_mode_set:1;
    unsigned int charset_set:1;
    unsigned int charset_connection_set:1;
    unsigned int charset_results_set:1;
    unsigned int charset_client_set:1;
    unsigned int charset_reset:1;
    unsigned int conn_reserved:1;
} mysqld_query_attr_t;

typedef struct query_cache_index_item {
    gchar *key;
    unsigned long long expire_ms;
} query_cache_index_item;

typedef struct query_cache_item {
    network_queue *queue;
} query_cache_item;

struct query_queue_t;

enum {
    AUTH_SWITCH = 3, /* for now, value not equal to 0 or 0xff is fine */
};
/**
 * get the name of a connection state
 */
NETWORK_API const char *network_mysqld_con_st_name(network_mysqld_con_state_t state);

/**
 * Encapsulates the state and callback functions for a MySQL protocol-based 
 * connection to and from cetus.
 * 
 * New connection structures are created by the function responsible for 
 * handling the accept on a listen socket, which
 * also is a network_mysqld_con structure, but only has a server set 
 * - there is no "client" for connections that we listen on.
 * 
 * The chassis itself does not listen on any sockets, this is left to each plugin. 
 * Plugins are free to create any number of
 * connections to listen on, but most of them will only create one and 
 * reuse the network_mysqld_con_accept function to set up an
 * incoming connection.
 * 
 * Each plugin can register callbacks for the various states 
 * in the MySQL Protocol, these are set in the member plugins.
 * A plugin is not required to implement any callbacks at all, 
 * but only those that it wants to customize. Callbacks that
 * are not set, will cause the cetus core to simply forward the received data.
 */
struct network_mysqld_con {
    /**
     * The current/next state of this connection.
     * 
     * When the protocol state machine performs a transition, 
     * this variable will contain the next state,
     * otherwise, while performing the action at state, 
     * it will be set to the connection's current state
     * in the MySQL protocol.
     * 
     * Plugins may update it in a callback to cause an arbitrary 
     * state transition, however, this may result
     * reaching an invalid state leading to connection errors.
     * 
     * @see network_mysqld_con_handle
     */
    network_mysqld_con_state_t state;

    /* 
     * Save the state before client closed, Then check the state, 
     * If the state is valid, reuse it.
     */
    network_mysqld_con_state_t prev_state;

    network_mysqld_con_dist_tran_state_t dist_tran_state;

    session_attr_flags_t attr_adj_state;

    proxy_session_state_t proxy_state;

    having_condition_t hav_condi;

    /**
     * The server side of the connection as it pertains to 
     * the low-level network implementation.
     * The current working server
     */
    network_socket *server;     /* default = NULL */
    GString *orig_sql;
    GString *modified_sql;

    GPtrArray *servers;

    /**
     * The client side of the connection as it pertains 
     * to the low-level network implementation.
     */
    network_socket *client;

    /**
     * Function pointers to the plugin's callbacks.
     * 
     * Plugins don't need set any of these, but if unset, 
     * the plugin will not have the opportunity to
     * alter the behavior of the corresponding protocol state.
     * 
     * @note In theory you could use functions from different plugins 
     * to handle the various states, but there is no guarantee that
     * this will work. Generally the plugins will assume that 
     * config is their own chassis_plugin_config (a plugin-private struct)
     * and violating this constraint may lead to a crash.
     * @see chassis_plugin_config
     */
    network_mysqld_hooks plugins;

    /**
     * A pointer to a plugin-private struct describing configuration parameters.
     * 
     * @note The actual struct definition used is private to each plugin.
     */
    chassis_plugin_config *config;

    /**
     * A pointer back to the global, singleton chassis structure.
     */
    chassis *srv;               /* our srv object */

    session_attr_flags_t unmatched_attribute;
    /**
     * A boolean flag indicating that this connection should 
     * only be used to accept incoming connections.
     * 
     * It does not follow the MySQL protocol by itself 
     * and its client network_socket will always be NULL.
     */
    int retry_serv_cnt;
    int max_retry_serv_cnt;
    int prepare_stmt_count;
    int resp_expected_num;
    int last_resp_num;
    int num_pending_servers;
    int num_servers_visited;
    int num_write_pending;
    int num_read_pending;
    unsigned int key;

    mysqld_query_attr_t query_attr;

    unsigned int is_wait_server:1;  /* first connect to backend failed, retrying */
    unsigned int is_calc_found_rows:1;
    unsigned int is_auto_commit:1;
    unsigned int is_start_tran_command:1;
    unsigned int is_prepared:1;
    unsigned int is_in_transaction:1;
    unsigned int xa_tran_conflict:1;
    unsigned int is_timeout:1;
    unsigned int is_in_sess_context:1;
    unsigned int is_changed_user_when_quit:1;
    unsigned int is_changed_user_failed:1;
    unsigned int is_start_trans_buffered:1;
    unsigned int is_auto_commit_trans_buffered:1;
    unsigned int is_commit_or_rollback:1;
    unsigned int is_rollback:1;
    unsigned int xa_start_phase:1;
    unsigned int use_slave_forced:1;
    unsigned int multiple_server_mode:1;
    unsigned int could_be_fast_streamed:1;
    unsigned int could_be_tcp_streamed:1;
    unsigned int process_through_special_tunnel:1;
    unsigned int candidate_tcp_streamed:1;
    unsigned int candidate_fast_streamed:1;
    unsigned int is_new_server_added:1;
    unsigned int is_attr_adjust:1;
    unsigned int sql_modified:1;
    unsigned int dist_tran:1;
    unsigned int partition_dist_tran:1;
    unsigned int is_tran_not_distributed_by_comment:1;
    unsigned int dist_tran_xa_start_generated:1;
    unsigned int dist_tran_failed:1;
    unsigned int dist_tran_decided:1;
    unsigned int server_to_be_closed:1;
    unsigned int server_closed:1;
    unsigned int last_warning_met:1;
    unsigned int conn_attr_check_omit:1;
    unsigned int buffer_and_send_fake_resp:1;
    unsigned int delay_send_auto_commit:1;
    unsigned int server_in_tran_and_auto_commit_received;
    unsigned int resp_too_long:1;
    unsigned int rob_other_conn:1;
    unsigned int master_unavailable:1;
    unsigned int master_conn_shortaged:1;
    unsigned int slave_conn_shortaged:1;
    unsigned int auth_next_packet_is_from_server:1;
    unsigned int is_xa_query_sent:1;
    unsigned int is_read_ro_server_allowed:1;
    unsigned int is_read_op_for_cached:1;
    unsigned int xa_query_status_error_and_abort:1;
    unsigned int use_all_prev_servers:1;
    unsigned int partially_merged:1;
    unsigned int last_record_updated:1;
    unsigned int query_cache_judged:1;
    unsigned int is_client_compressed:1;
    unsigned int write_flag:1;
    unsigned int is_processed_by_subordinate:1;
    unsigned int is_admin_client:1;
    unsigned int is_admin_waiting_resp:1;
    unsigned int resp_err_met:1;
    unsigned int direct_answer:1;
    unsigned int admin_read_merge:1;
    unsigned int ask_one_worker:1;
    unsigned int ask_the_given_worker:1;
    unsigned int is_client_to_be_closed:1;
    /**
     * Flag indicating that we have received a COM_QUIT command.
     * 
     * This is mainly used to differentiate between the case 
     * where the server closed the connection because of some error
     * or if the client asked it to close its side of the connection.
     * cetus would report spurious errors for the latter case,
     * if we failed to track this command.
     */
    unsigned int com_quit_seen:1;

    /** Flag indicating if we the plugin doesn't need the resultset itself.
     * 
     * If set to TRUE, the plugin needs to see 
     * the entire resultset and we will buffer it.
     * If set to FALSE, the plugin is not interested 
     * in the content of the resultset and we'll
     * try to forward the packets to the client directly, 
     * even before the full resultset is parsed.
     */
    unsigned int resultset_is_needed:1;
    unsigned int eof_last_met:1;
    unsigned int fast_stream_need_more:1;
    unsigned int last_backend_type:2;
    unsigned int eof_met_cnt:4;
    unsigned int last_payload_len:4;
    unsigned int last_record_payload_len:4;
    unsigned int fast_stream_last_exec_index:4;
    unsigned int process_index:6;
    unsigned int last_packet_id:8;
    unsigned int write_server_num:8;

    unsigned long long xa_id;
#ifndef SIMPLE_PARSER
    unsigned long long internal_xa_id;
#endif
    guint32 auth_switch_to_round;
    guint32 partically_record_left_cnt;

    time_t last_check_conn_supplement_time;

    unsigned char last_payload[ANALYSIS_PACKET_LEN];
    unsigned char record_last_payload[RECORD_PACKET_LEN];
    struct timeval req_recv_time;
    struct timeval resp_recv_time;
    struct timeval resp_send_time;

    guint64 resp_cnt;
    guint64 last_insert_id;
    guint64 analysis_next_pos;
    guint64 cur_resp_len;

    /**
     * An integer indicating the result received from a server 
     * after sending an authentication request.
     * 
     * This is used to differentiate between the old, pre-4.1 
     * authentication and the new, 4.1+ one based on the response.
     */
    guint8 auth_result_state;

    /* track the auth-method-switch state */
    GString *auth_switch_to_method;
    /**
     * Contains the parsed packet.
     */
    struct network_mysqld_con_parse parse;

    enum enum_server_command cur_command;

    /**
     * An opaque pointer to a structure describing extra 
     * connection state needed by the plugin.
     * 
     * The content and meaning is completely up to each plugin and 
     * the chassis will not access this in any way.
     * 
     * in proxy-plugin, proxy_plugin_con_t
     * in shard-plugin, shard_plugin_con_t
     * in admin-plugin, not used
     */
    void *plugin_con_state;
    const GString *first_group;
    /* connection specific timeouts */
    struct timeval connect_timeout; /* default = 2 s */
    struct timeval read_timeout;    /* default = 10 min */
    struct timeval write_timeout;   /* default = 10 min */
    struct timeval dist_tran_decided_read_timeout;    /* default = 30 sec */
    struct timeval wait_clt_next_sql;
    char xid_str[XID_LEN];
    char last_backends_type[MAX_SERVER_NUM];

    struct sharding_plan_t *sharding_plan;
    void *data;
};

struct network_mysqld_con_injection {
    network_injection_queue *queries;
    int sent_resultset;
};

typedef enum {
    NET_RW_STATE_NONE,
    NET_RW_STATE_WRITE,
    NET_RW_STATE_READ,
    NET_RW_STATE_ERROR,
    NET_RW_STATE_PART_FINISHED,
    NET_RW_STATE_FINISHED
} read_write_state;

/**
 * A server side session, wraps server side socket
 * For sharding plugin:
 *   proxy-session = 1 * client-socket + n *server-session
 */
typedef struct server_session_t {
    unsigned int fresh:1;
    unsigned int participated:1;
    unsigned int has_xa_write:1;
    unsigned int xa_start_already_sent:1;
    unsigned int dist_tran_participated:1;
    unsigned int xa_query_status_error_and_abort:1;
    unsigned int is_in_xa:1;
    unsigned int is_xa_over:1;
    unsigned int attr_consistent:1;
    unsigned int attr_consistent_checked:1;
    unsigned int attr_adjusted_now:1;
    unsigned int read_cal_flag:1;
    unsigned int index:6;

    network_socket *server;
    const GString *sql;
    network_mysqld_con *con;
    network_backend_t *backend;
    network_mysqld_con_dist_tran_state_t dist_tran_state;
    read_write_state state;
    session_attr_flags_t attr_diff;

    guint64 ts_read_query;
    guint64 ts_read_query_result_last;
    guint8 query_status;
} server_session_t;

typedef struct {
    /**< A list of queries to send to the backend.*/
    struct network_mysqld_con_injection injected;

    network_backend_t *backend;
    /**< index into the backend-array, start from 0 */
    int backend_ndx;

    short *backend_ndx_array;   /* rw-only: map backend index to connections in con->servers */

    struct sql_context_t *sql_context;
    int trx_read_write;         /* default TF_READ_WRITE */
    int trx_isolation_level;    /* default TF_REPEATABLE_READ */

} proxy_plugin_con_t;

NETWORK_API network_mysqld_con *network_mysqld_con_new(void);
NETWORK_API void network_mysqld_con_free(network_mysqld_con *con);

NETWORK_API void network_mysqld_con_accept(int event_fd, short events, void *user_data);

NETWORK_API int network_mysqld_con_send_ok(network_socket *con);
NETWORK_API int network_mysqld_con_send_ok_full(network_socket *con, guint64 affected_rows,
                                                guint64 insert_id, guint16 server_status, guint16 warnings);
NETWORK_API int network_mysqld_con_send_error(network_socket *con, const gchar *errmsg, gsize errmsg_len);
NETWORK_API int network_mysqld_con_send_error_full(network_socket *con, const char *errmsg,
                                                   gsize errmsg_len, guint errorcode, const gchar *sqlstate);
NETWORK_API int network_mysqld_con_send_resultset(network_socket *con, GPtrArray *fields, GPtrArray *rows);
int network_mysqld_con_send_current_date(network_socket *, char *);
int network_mysqld_con_send_cetus_version(network_socket *);
void network_mysqld_send_xa_start(network_socket *, const char *xid);

NETWORK_API void network_mysqld_con_reset_command_response_state(network_mysqld_con *con);
NETWORK_API void network_mysqld_con_reset_query_state(network_mysqld_con *con);

/**
 * should be socket 
 */
NETWORK_API network_socket_retval_t network_mysqld_read(chassis *srv, network_socket *con);
NETWORK_API network_socket_retval_t network_mysqld_write(network_socket *con);
NETWORK_API network_socket_retval_t network_mysqld_con_get_packet(chassis G_GNUC_UNUSED *chas, network_socket *con);

struct chassis_private {
    GPtrArray *cons;                          /**< array(network_mysqld_con) */
    GList *listen_conns;
    network_backends_t *backends;
    struct cetus_users_t *users;
    struct cetus_variable_t *stats_variables;
    struct cetus_monitor_t *monitor;
    guint32 thread_id;
    guint32 max_thread_id;
    struct cetus_acl_t *acl;
};

NETWORK_API network_socket_retval_t
network_mysqld_read_mul_packets(chassis G_GNUC_UNUSED *chas, network_mysqld_con *con,
                                network_socket *server, int *is_finished);

NETWORK_API void send_part_content_to_client(network_mysqld_con *con);
NETWORK_API void set_conn_attr(network_mysqld_con *con, network_socket *server);
NETWORK_API int network_mysqld_init(chassis *srv);
NETWORK_API void network_mysqld_add_connection(chassis *srv, network_mysqld_con *con, gboolean listen);
gboolean network_mysqld_kill_connection(chassis *srv, guint32 id);
NETWORK_API void network_mysqld_con_handle(int event_fd, short events, void *user_data);
NETWORK_API int network_mysqld_queue_append(network_socket *sock, network_queue *queue, const char *data, size_t len);
NETWORK_API int network_mysqld_queue_append_raw(network_socket *sock, network_queue *queue, GString *data);
NETWORK_API int network_mysqld_queue_reset(network_socket *sock);
NETWORK_API void network_mysqld_con_clear_xa_env_when_not_expected(network_mysqld_con *con);

NETWORK_API void network_connection_pool_create_conn_and_kill_query(network_mysqld_con *con);
NETWORK_API void network_connection_pool_create_conn(network_mysqld_con *con);
NETWORK_API void network_connection_pool_create_conns(chassis *srv);
NETWORK_API void check_and_create_conns_func(int fd, short what, void *arg);
NETWORK_API void update_time_func(int fd, short what, void *arg);
NETWORK_API char *generate_or_retrieve_xid_str(network_mysqld_con *con, network_socket *server, int need_generate_new);

NETWORK_API void record_xa_log_for_mending(network_mysqld_con *con, network_socket *sock);
NETWORK_API gboolean shard_set_autocommit(network_mysqld_con *con);
NETWORK_API gboolean shard_set_charset_consistant(network_mysqld_con *con);
NETWORK_API gboolean shard_set_default_db_consistant(network_mysqld_con *con);
NETWORK_API gboolean shard_set_multi_stmt_consistant(network_mysqld_con *con);
NETWORK_API int shard_build_xa_query(network_mysqld_con *con, server_session_t *ss);

#endif
