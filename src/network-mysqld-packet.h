
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

#ifndef __NETWORK_MYSQLD_PACKET__
#define __NETWORK_MYSQLD_PACKET__

#include <glib.h>

#include "network-exports.h"

#include "network-mysqld-proto.h"
#include "network-mysqld.h"

#ifndef COM_RESET_CONNECTION
#define COM_RESET_CONNECTION 0x1F
#endif

/**
 * mid-level protocol 
 *
 * the MySQL protocal is split up in three layers:
 *
 * - low-level (encoding of fields in a packet)
 * - mid-level (encoding of packets)
 * - high-level (grouping packets into a sequence)
 */

typedef enum {
    NETWORK_MYSQLD_PROTOCOL_VERSION_41
} network_mysqld_protocol_t;

/**
 * tracking the state of the response of a COM_QUERY packet
 */
typedef struct {
    enum {
        PARSE_COM_QUERY_INIT,
        PARSE_COM_QUERY_FIELD,
        PARSE_COM_QUERY_RESULT,
        PARSE_COM_QUERY_LOCAL_INFILE_DATA,
        PARSE_COM_QUERY_LOCAL_INFILE_RESULT
    } state;

    guint16 server_status;
    guint16 warning_count;
    guint64 affected_rows;
    guint64 insert_id;

    gboolean was_resultset;
    gboolean binary_encoded;

    guint64 rows;
    guint64 bytes;

    guint8 query_status;
} network_mysqld_com_query_result_t;

/**
 * these capability flags are introduced in later versions of mysql,
 * and default used in CLIENT_BASIC_FLAGS, clear them in CETUS_DEFAULT_FLAGS
 */
#ifndef CLIENT_CONNECT_ATTRS
#define CLIENT_CONNECT_ATTRS (1UL << 20)
#endif
#ifndef CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA
#define CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA (1UL << 21)
#endif
#ifndef CLIENT_CAN_HANDLE_EXPIRED_PASSWORDS
#define CLIENT_CAN_HANDLE_EXPIRED_PASSWORDS (1UL << 22)
#endif
#ifndef CLIENT_SESSION_TRACK
#define CLIENT_SESSION_TRACK (1UL << 23)
#endif
#ifndef CLIENT_DEPRECATE_EOF
#define CLIENT_DEPRECATE_EOF (1UL << 24)
#endif
#ifndef CLIENT_PLUGIN_AUTH
#define CLIENT_PLUGIN_AUTH (1UL << 19)
#endif

#ifndef CLIENT_BASIC_FLAGS /* for mariadb version 10^ */
#define CLIENT_BASIC_FLAGS CLIENT_DEFAULT_FLAGS
#endif
#ifndef SERVER_MORE_RESULTS_EXISTS /* for mariadb version 10^ */
#define SERVER_MORE_RESULTS_EXISTS SERVER_MORE_RESULTS_EXIST
#endif
#ifndef CLIENT_PROGRESS /* mariadb progress reporting */
#define CLIENT_PROGRESS (1UL << 29)
#endif

#if MYSQL_VERSION_ID < 50606
#define COMPATIBLE_BASIC_FLAGS (CLIENT_BASIC_FLAGS                      \
                                |CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA  \
                                |CLIENT_CAN_HANDLE_EXPIRED_PASSWORDS    \
                                |CLIENT_SESSION_TRACK                   \
                                |CLIENT_PLUGIN_AUTH)
#else
#define COMPATIBLE_BASIC_FLAGS CLIENT_BASIC_FLAGS
#endif

#define CETUS_DEFAULT_FLAGS (COMPATIBLE_BASIC_FLAGS                     \
                             & ~CLIENT_NO_SCHEMA /* permit database.table.column */ \
                             & ~CLIENT_IGNORE_SPACE                     \
                             & ~CLIENT_DEPRECATE_EOF                    \
                             & ~CLIENT_LOCAL_FILES                      \
                             & ~CLIENT_PROGRESS                         \
                             & ~CLIENT_CONNECT_ATTRS)

NETWORK_API network_mysqld_com_query_result_t *network_mysqld_com_query_result_new(void);
NETWORK_API void network_mysqld_com_query_result_free(network_mysqld_com_query_result_t *);
NETWORK_API gboolean network_mysqld_com_query_result_is_local_infile(network_mysqld_com_query_result_t *);
NETWORK_API int network_mysqld_proto_get_com_query_result(network_packet *packet,
                                                          network_mysqld_com_query_result_t *udata,
                                                          gboolean use_binary_row_data);

/**
 * tracking the response of a COM_STMT_PREPARE command
 *
 * depending on the kind of statement that was prepare we will receive 0-2 EOF packets
 */
typedef struct {
    gboolean first_packet;
    gint want_eofs;
    int status;                 /* MYSQLD_PACKET_[OK/ERR] */
} network_mysqld_com_stmt_prep_result_t;

NETWORK_API network_mysqld_com_stmt_prep_result_t *network_mysqld_com_stmt_prepare_result_new(void);
NETWORK_API void network_mysqld_com_stmt_prepare_result_free(network_mysqld_com_stmt_prep_result_t *udata);
NETWORK_API int network_mysqld_proto_get_com_stmt_prep_result(network_packet *packet,
                                                              network_mysqld_com_stmt_prep_result_t *udata);

/**
 * tracking the response of a COM_INIT_DB command
 *
 * we have to track the default internally can only accept it
 * if the server side OK'ed it
 */
typedef struct {
    GString *db_name;
} network_mysqld_com_init_db_result_t;

NETWORK_API network_mysqld_com_init_db_result_t *network_mysqld_com_init_db_result_new(void);
NETWORK_API void network_mysqld_com_init_db_result_free(network_mysqld_com_init_db_result_t *com_init_db);
NETWORK_API int network_mysqld_com_init_db_result_track_state(network_packet *, network_mysqld_com_init_db_result_t *);
NETWORK_API int network_mysqld_proto_get_com_init_db_result(network_packet *,
                                                            network_mysqld_com_init_db_result_t *,
                                                            network_mysqld_con *);

NETWORK_API int network_mysqld_proto_get_query_result(network_packet *, network_mysqld_con *);
NETWORK_API int network_mysqld_con_command_states_init(network_mysqld_con *, network_packet *);

typedef struct {
    guint64 affected_rows;
    guint64 insert_id;
    guint16 server_status;
    guint16 warnings;

    gchar *msg;
} network_mysqld_ok_packet_t;

NETWORK_API network_mysqld_ok_packet_t *network_mysqld_ok_packet_new(void);
NETWORK_API void network_mysqld_ok_packet_free(network_mysqld_ok_packet_t *udata);

NETWORK_API int network_mysqld_proto_get_ok_packet(network_packet *, network_mysqld_ok_packet_t *);
NETWORK_API int network_mysqld_proto_append_ok_packet(GString *, network_mysqld_ok_packet_t *);

typedef struct {
    GString *errmsg;
    GString *sqlstate;

    guint16 errcode;
    network_mysqld_protocol_t version;
} network_mysqld_err_packet_t;

NETWORK_API network_mysqld_err_packet_t *network_mysqld_err_packet_new(void);
NETWORK_API void network_mysqld_err_packet_free(network_mysqld_err_packet_t *udata);

NETWORK_API int network_mysqld_proto_get_err_packet(network_packet *, network_mysqld_err_packet_t *);
NETWORK_API int network_mysqld_proto_append_err_packet(GString *, network_mysqld_err_packet_t *);

typedef struct {
    guint16 server_status;
    guint16 warnings;
} network_mysqld_eof_packet_t;

NETWORK_API network_mysqld_eof_packet_t *network_mysqld_eof_packet_new(void);
NETWORK_API void network_mysqld_eof_packet_free(network_mysqld_eof_packet_t *udata);

NETWORK_API int network_mysqld_proto_get_eof_packet(network_packet *, network_mysqld_eof_packet_t *);

struct network_mysqld_auth_challenge {
    guint8 protocol_version;
    gchar *server_version_str;
    guint32 server_version;
    guint32 thread_id;
    GString *auth_plugin_data;
    guint32 capabilities;
    guint8 charset;
    guint16 server_status;
    GString *auth_plugin_name;
};

NETWORK_API network_mysqld_auth_challenge *network_mysqld_auth_challenge_new(void);
NETWORK_API void network_mysqld_auth_challenge_free(network_mysqld_auth_challenge *shake);
NETWORK_API int network_mysqld_proto_get_auth_challenge(network_packet *, network_mysqld_auth_challenge *);
NETWORK_API int network_mysqld_proto_append_auth_challenge(GString *, network_mysqld_auth_challenge *);
NETWORK_API void network_mysqld_auth_challenge_set_challenge(network_mysqld_auth_challenge *);

struct network_mysqld_auth_response {
    guint32 client_capabilities;
    guint32 server_capabilities;
    guint32 max_packet_size;
    guint8 charset;
    GString *username;
    GString *auth_plugin_data;
    GString *database;
    GString *auth_plugin_name;
    gboolean ssl_request;
};

NETWORK_API network_mysqld_auth_response *network_mysqld_auth_response_new(guint server_capabilities);
NETWORK_API void network_mysqld_auth_response_free(network_mysqld_auth_response *);
NETWORK_API int network_mysqld_proto_append_auth_response(GString *, network_mysqld_auth_response *);
int network_mysqld_proto_append_auth_switch(GString *, char *method_name, GString* salt);
NETWORK_API int network_mysqld_proto_get_auth_response(network_packet *, network_mysqld_auth_response *);

/* COM_STMT_* */

typedef struct {
    GString *stmt_text;
} network_mysqld_stmt_prep_pack_t;

typedef struct {
    guint32 stmt_id;
    guint16 num_columns;
    guint16 num_params;
    guint16 warnings;
} network_mysqld_stmt_prep_ok_pack_t;

typedef struct {
    guint32 stmt_id;
    guint8 flags;
    guint32 iteration_count;
    guint8 new_params_bound;
    GPtrArray *params; /**< array<network_mysqld_type *> */
} network_mysqld_stmt_exec_pack_t;

NETWORK_API int network_mysqld_proto_get_stmt_id(network_packet *packet, guint32 *stmt_id);
NETWORK_API int network_mysqld_proto_change_stmt_id_from_ok_packet(network_packet *packet, int server_index);
NETWORK_API int network_mysqld_proto_change_stmt_id_from_ok(network_packet *packet, int server_index);
NETWORK_API int network_mysqld_proto_change_stmt_id_from_clt_stmt(network_packet *packet);

NETWORK_API int network_mysqld_proto_append_query_packet(GString *, const char *);

typedef struct {
    guint8 charset;
    const GString *database;
    const GString *auth_plugin_data;
    const GString *username;
    const GString *hashed_pwd;
} mysqld_change_user_packet_t;

int mysqld_proto_append_change_user_packet(GString *, mysqld_change_user_packet_t *);

#endif
