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

#ifndef _CHASSIS_MAINLOOP_H_
#define _CHASSIS_MAINLOOP_H_

#include <glib.h>               /* GPtrArray */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>           /* event.h needs struct tm */
#endif
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#include <stdio.h>
#include <event.h>              /* struct event_base */

#include "chassis-exports.h"
#include "chassis-log.h"
#include "chassis-shutdown-hooks.h"
#include "cetus-util.h"
#include "chassis-config.h"

/** @defgroup chassis Chassis
 *
 * the chassis contains the set of functions that are used by all programs
 *
 * */
/*@{*/
typedef struct chassis_private chassis_private;
typedef struct chassis chassis;

#define MAX_SERVER_NUM 64
#define MAX_SERVER_NUM_FOR_PREPARE 32
#define MAX_WORK_PROCESSES 64
#define MAX_WORK_PROCESSES_SHIFT 6
#define MAX_QUERY_TIME 1000
#define MAX_WAIT_TIME 1024
#define MAX_TRY_NUM 6
#define MAX_CREATE_CONN_NUM 512
#define MAX_DIST_TRAN_PREFIX 64
#define DEFAULT_LIVE_TIME 7200

#define MAX_ALLOWED_PACKET_CEIL    (1 * GB)
#define MAX_ALLOWED_PACKET_DEFAULT (32 * MB)
#define MAX_ALLOWED_PACKET_FLOOR   (1 * KB)

typedef struct rw_op_t {
    uint64_t ro;
    uint64_t rw;
} rw_op_t;

typedef struct query_stats_t {
    rw_op_t client_query;
    rw_op_t proxyed_query;
    uint64_t com_select;
    uint64_t com_insert;
    uint64_t com_update;
    uint64_t com_delete;
    uint64_t com_select_shard;
    uint64_t com_insert_shard;
    uint64_t com_update_shard;
    uint64_t com_delete_shard;
    uint64_t com_select_global;
    uint64_t com_select_bad_key;
    uint64_t xa_count;
    uint64_t query_time_table[MAX_QUERY_TIME];
    uint64_t query_wait_table[MAX_WAIT_TIME];
    rw_op_t server_query_details[MAX_SERVER_NUM];
} query_stats_t;

#ifndef SIMPLE_PARSER
/* For generating unique global ids for MySQL */
struct incremental_guid_state_t {
    unsigned int last_sec;
    unsigned int last_msec;
    int worker_id;
    int seq_id;
};

void incremental_guid_init(struct incremental_guid_state_t *s);
uint64_t incremental_guid_get_next(struct incremental_guid_state_t *s);
#endif

struct chassis {
    struct event_base *event_base;
    gchar *event_hdr_version;

    /**< array(chassis_plugin) */
    GPtrArray *modules;
    void *admin_plugin;

    /**< base directory for all relative paths referenced */
    gchar *base_dir;
    /**< plugin dir for save settings */
    gchar *plugin_dir;
    gchar *conf_dir;
    /**< user to run as */
    gchar *user;

    char *proxy_address;
    char *default_db;
    char *ifname;
    char *default_username;
    char *default_charset;
    char *default_hashed_pwd;
    char *unix_socket_name;

    guint64 sess_key;
    unsigned int maintain_close_mode;
    unsigned int disable_threads;
    int ssl;
    unsigned int is_tcp_stream_enabled;
    unsigned int query_cache_enabled;
    unsigned int is_back_compressed;
    unsigned int compress_support;
    unsigned int client_found_rows;
    unsigned int master_preferred;
    unsigned int is_manual_down;
    unsigned int is_reduce_conns;
    unsigned int xa_log_detailed;
    unsigned int check_slave_delay;
    int socketpair_mutex;
    int complement_conn_flag;
    int default_query_cache_timeout;
    int client_idle_timeout;
    int maintained_client_idle_timeout;
    double slave_delay_down_threshold_sec;
    double slave_delay_recover_threshold_sec;
    unsigned int long_query_time;
    unsigned int min_req_time_for_cache;
    unsigned int internal_trx_isolation_level;
    int cetus_max_allowed_packet;
    int disable_dns_cache;
    int enable_admin_listen;

    int worker_processes;
    int cpus;
    int child_instant_exit_times;

    int max_alive_time;
    int merged_output_size;
    int max_header_size;
    int compressed_merged_output_size;
    int is_need_to_create_conns;

    /* Conn-pool initialize settings */
    int max_idle_connections;
    int mid_idle_connections;

    long long max_resp_len;
    unsigned long long dist_tran_id;

    char dist_tran_prefix[MAX_DIST_TRAN_PREFIX];

    chassis_private *priv;
    void (*priv_shutdown) (chassis *chas, chassis_private *priv);
    void (*priv_finally_free_shared) (chassis *chas, chassis_private *priv);
    void (*priv_free) (chassis *chas, chassis_private *priv);

    chassis_log *log;

    chassis_shutdown_hooks_t *shutdown_hooks;

    query_stats_t query_stats;


#ifndef SIMPLE_PARSER
    struct incremental_guid_state_t guid_state;
#endif
    time_t startup_time;
    time_t child_exit_time;
    time_t current_time;
    struct chassis_options_t *options;
    chassis_config_t *config_manager;
    GHashTable *query_cache_table;
    GQueue *cache_index;
    unsigned long long last_cache_purge_time;
    gboolean allow_new_conns;

    gint verbose_shutdown;
    gint daemon_mode;
    char **argv;
    int    argc;
    gchar *pid_file;
    gchar *old_pid_file;
    gchar *log_level;
    gchar **plugin_names;
    gchar *log_xa_filename;
    guint invoke_dbg_on_crash;
    gint max_files_number;
    char *remote_config_url;
    char *trx_isolation_level;
    gchar *default_file;
    gint print_version;

    gint group_replication_mode;

    struct event auto_create_conns_event;
    struct event update_timer_event;

    struct sql_log_mgr *sql_mgr;
    gint check_dns;
};

CHASSIS_API chassis *chassis_new(void);
CHASSIS_API void chassis_free(chassis *chas);
CHASSIS_API int chassis_check_version(const char *lib_version, const char *hdr_version);

/**
 * the mainloop for all chassis apps
 *
 */
CHASSIS_API int chassis_mainloop(void *user_data);

CHASSIS_API void chassis_set_shutdown_location(const gchar *location);
CHASSIS_API gboolean chassis_is_shutdown(void);

#define chassis_set_shutdown() chassis_set_shutdown_location(G_STRLOC)

/*@}*/

#endif
