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

/** @file
 * the user-interface for the cetus @see main()
 *
 *  -  command-line handling
 *  -  config-file parsing
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <sys/types.h>
#include <sys/stat.h>

#ifdef HAVE_SIGNAL_H
#include <signal.h>
#endif
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <malloc.h>

#include <unistd.h>
#include <sys/wait.h>
#include <sys/resource.h>       /* for rusage in wait() */

#include <glib.h>
#include <gmodule.h>

#include "chassis-options-utils.h"

#ifdef HAVE_VALGRIND_VALGRIND_H
#include <valgrind/valgrind.h>
#endif

#ifdef HAVE_SIGACTION
#include <execinfo.h>
#endif

#ifndef HAVE_VALGRIND_VALGRIND_H
#define RUNNING_ON_VALGRIND 0
#endif

#include "glib-ext.h"
#include "network-mysqld.h"
#include "network-mysqld-proto.h"
#include "sys-pedantic.h"

#include "cetus-log.h"
#include "chassis-timings.h"
#include "chassis-log.h"
#include "chassis-keyfile.h"
#include "chassis-mainloop.h"
#include "chassis-path.h"
#include "chassis-limits.h"
#include "chassis-filemode.h"
#include "chassis-unix-daemon.h"
#include "chassis-frontend.h"
#include "chassis-options.h"
#include "cetus-monitor.h"
#include "chassis-sql-log.h"
#include "lib/sql-expression.h"

#define GETTEXT_PACKAGE "cetus"

extern pid_t       cetus_pid;

/**
 * options of the cetus frontend
 */
struct chassis_frontend_t {
    int print_version;
    int verbose_shutdown;

    int daemon_mode;
    int set_client_found_rows;
    int default_pool_size;
    int max_pool_size;
    int worker_processes;
    int merged_output_size;
    int max_header_size;
    int max_alive_time;
    int master_preferred;
#ifndef SIMPLE_PARSER
    int worker_id;
#endif
    int config_port;
    int disable_threads;
    int is_tcp_stream_enabled;
    int is_fast_stream_enabled;
    int is_partition_mode;
    int check_sql_loosely;
    int is_sql_special_processed;
    int is_back_compressed;
    int is_client_compress_support;
    int check_slave_delay;
    int is_reduce_conns;
    int long_query_time;
    int xa_log_detailed;
    int cetus_max_allowed_packet;
    int default_query_cache_timeout;
    int client_idle_timeout;
    int incomplete_tran_idle_timeout;
    int maintained_client_idle_timeout;
    int query_cache_enabled;
    int disable_dns_cache;
    long long max_resp_len;
    double slave_delay_down_threshold_sec;
    double slave_delay_recover_threshold_sec;

    guint invoke_dbg_on_crash;
    /* the --keepalive option isn't available on Unix */
    gint max_files_number;

    gchar *user;

    gchar *base_dir;
    gchar *conf_dir;

    gchar *default_file;
    GKeyFile *keyfile;

    chassis_plugin *p;
    GOptionEntry *config_entries;

    gchar *pid_file;

    gchar *plugin_dir;
    gchar **plugin_names;

    gchar *log_level;
    gchar *log_filename;
    gchar *log_xa_filename;
    char *default_username;
    char *default_charset;
    char *default_db;
    char *ifname;

    char *remote_config_url;
    char *trx_isolation_level;

    gint group_replication_mode;

    guint sql_log_bufsize;
    gchar *sql_log_switch;
    gchar *sql_log_prefix;
    gchar *sql_log_path;
    gint sql_log_maxsize;
    gchar *sql_log_mode;
    guint sql_log_idletime;
    gint sql_log_maxnum;

    gint ssl;

    int check_dns;
};

/**
 * create a new the frontend for the chassis
 */
struct chassis_frontend_t *
chassis_frontend_new(void)
{
    struct chassis_frontend_t *frontend;

    frontend = g_slice_new0(struct chassis_frontend_t);
    frontend->max_files_number = 0;
    frontend->disable_threads = 0;
    frontend->is_back_compressed = 0;
    frontend->is_client_compress_support = 0;
    frontend->xa_log_detailed = 0;

    frontend->default_pool_size = DEFAULT_POOL_SIZE;
    frontend->worker_processes = 1;
    frontend->max_resp_len = 10 * 1024 * 1024;  /* 10M */
    frontend->max_alive_time = DEFAULT_LIVE_TIME;
    frontend->merged_output_size = 8192;
    frontend->max_header_size = 65536;
    frontend->config_port = 3306;

    frontend->check_slave_delay = 1;
    frontend->slave_delay_down_threshold_sec = 10.0;
    frontend->default_query_cache_timeout = 100;
    frontend->client_idle_timeout = 8 * HOURS;
    frontend->incomplete_tran_idle_timeout = 3600;
    frontend->maintained_client_idle_timeout = 30;
    frontend->long_query_time = 1000;
    frontend->cetus_max_allowed_packet = MAX_ALLOWED_PACKET_DEFAULT;
    frontend->disable_dns_cache = 0;

#ifndef SIMPLE_PARSER
    frontend->is_tcp_stream_enabled = 0;
#else
    frontend->is_tcp_stream_enabled = 1;
#endif
    frontend->is_fast_stream_enabled = 0;
    frontend->is_partition_mode = 0;
    frontend->check_sql_loosely = 0;
    frontend->is_sql_special_processed = 0;
    frontend->group_replication_mode = 0;
    frontend->sql_log_bufsize = 0;
    frontend->sql_log_switch = NULL;
    frontend->sql_log_prefix = NULL;
    frontend->sql_log_path = NULL;
    frontend->sql_log_maxsize = -1;
    frontend->sql_log_mode = NULL;
    frontend->sql_log_idletime = 0;
    frontend->sql_log_maxnum = -1;

    frontend->check_dns = 0;

    frontend->ssl = 0;

    return frontend;
}

/**
 * free the frontend of the chassis
 */
void
chassis_frontend_free(struct chassis_frontend_t *frontend)
{
    if (!frontend)
        return;

    if (frontend->keyfile)
        g_key_file_free(frontend->keyfile);
    g_free(frontend->default_file);
    g_free(frontend->log_xa_filename);
    g_free(frontend->log_filename);

    g_free(frontend->base_dir);
    g_free(frontend->conf_dir);
    g_free(frontend->user);
    g_free(frontend->pid_file);
    g_free(frontend->log_level);
    g_free(frontend->plugin_dir);
    g_free(frontend->default_username);
    g_free(frontend->default_db);
    g_free(frontend->ifname);
    g_free(frontend->default_charset);

    if (frontend->plugin_names) {
        g_strfreev(frontend->plugin_names);
    }

    g_free(frontend->remote_config_url);
    g_free(frontend->trx_isolation_level);
    g_free(frontend->sql_log_switch);
    g_free(frontend->sql_log_prefix);
    g_free(frontend->sql_log_path);
    g_free(frontend->sql_log_mode);

    g_slice_free(struct chassis_frontend_t, frontend);
}

/**
 * setup the options of the chassis
 */
int
chassis_frontend_set_chassis_options(struct chassis_frontend_t *frontend, chassis_options_t *opts, chassis* srv)
{
    chassis_options_add(opts,
                        "verbose-shutdown",
                        0, 0, OPTION_ARG_NONE, &(frontend->verbose_shutdown),
                        "Always log the exit code when shutting down", NULL,
                        NULL, show_verbose_shutdown, SHOW_OPTS_PROPERTY|SAVE_OPTS_PROPERTY);

    chassis_options_add(opts, "daemon", 0, 0, OPTION_ARG_NONE, &(frontend->daemon_mode), "Start in daemon-mode", NULL,
                        NULL, show_daemon_mode, SHOW_OPTS_PROPERTY|SAVE_OPTS_PROPERTY);

    chassis_options_add(opts, "user", 0, 0, OPTION_ARG_STRING, &(frontend->user), "Run cetus as user", "<user>",
                        NULL, show_user, SHOW_OPTS_PROPERTY|SAVE_OPTS_PROPERTY);

    chassis_options_add(opts,
                        "basedir",
                        0, 0, OPTION_ARG_STRING, &(frontend->base_dir),
                        "Base directory to prepend to relative paths in the config", "<absolute path>",
                        NULL, show_basedir, SHOW_OPTS_PROPERTY|SAVE_OPTS_PROPERTY);

    chassis_options_add(opts,
                        "conf-dir",
                        0, 0, OPTION_ARG_STRING, &(frontend->conf_dir), "Configuration directory", "<absolute path>",
                        NULL, show_confdir, SHOW_OPTS_PROPERTY|SAVE_OPTS_PROPERTY);

    chassis_options_add(opts,
                        "pid-file",
                        0, 0, OPTION_ARG_STRING, &(frontend->pid_file),
                        "PID file in case we are started as daemon", "<file>",
                        NULL, show_pidfile, SHOW_OPTS_PROPERTY|SAVE_OPTS_PROPERTY);

    chassis_options_add(opts,
                        "plugin-dir",
                        0, 0, OPTION_ARG_STRING, &(frontend->plugin_dir), "Path to the plugins", "<path>",
                        NULL, show_plugindir, SHOW_OPTS_PROPERTY|SAVE_OPTS_PROPERTY);

    chassis_options_add(opts,
                        "plugins",
                        0, 0, OPTION_ARG_STRING_ARRAY, &(frontend->plugin_names), "Plugins to load", "<name>",
                        NULL, show_plugins, SHOW_OPTS_PROPERTY|SAVE_OPTS_PROPERTY);

    chassis_options_add(opts,
                        "log-level",
                        0, 0, OPTION_ARG_STRING, &(frontend->log_level),
                        "Log all messages of level ... or higher", "(error|warning|info|message|debug)",
                        assign_log_level, show_log_level, ALL_OPTS_PROPERTY);

    chassis_options_add(opts,
                        "log-file",
                        0, 0, OPTION_ARG_STRING, &(frontend->log_filename), "Log all messages in a file", "<file>",
                        NULL, show_log_file, SHOW_OPTS_PROPERTY|SAVE_OPTS_PROPERTY);

    chassis_options_add(opts,
                        "log-xa-file",
                        0, 0, OPTION_ARG_STRING, &(frontend->log_xa_filename),
                        "Log all xa messages in a file", "<file>",
                        NULL, show_log_xa_file, SHOW_OPTS_PROPERTY|SAVE_OPTS_PROPERTY);

    chassis_options_add(opts,
                        "log-backtrace-on-crash",
                        0, 0, OPTION_ARG_NONE, &(frontend->invoke_dbg_on_crash),
                        "Try to invoke debugger on crash", NULL,
                        NULL, show_log_backtrace_on_crash, SHOW_OPTS_PROPERTY|SAVE_OPTS_PROPERTY);

    chassis_options_add(opts,
                        "max-open-files",
                        0, 0, OPTION_ARG_INT, &(frontend->max_files_number),
                        "Maximum number of open files (ulimit -n)", NULL,
                        NULL, show_max_open_files, SHOW_OPTS_PROPERTY|SAVE_OPTS_PROPERTY);

    chassis_options_add(opts,
                        "default-charset",
                        0, 0, OPTION_ARG_STRING, &(frontend->default_charset),
                        "Set the default character set for backends", "<string>",
                        assign_default_charset, show_default_charset, ALL_OPTS_PROPERTY);

    chassis_options_add(opts,
                        "default-username",
                        0, 0, OPTION_ARG_STRING, &(frontend->default_username),
                        "Set the default username for visiting backends", "<string>",
                        assign_default_username, show_default_username, ALL_OPTS_PROPERTY);

    chassis_options_add(opts,
                        "default-db",
                        0, 0, OPTION_ARG_STRING, &(frontend->default_db),
                        "Set the default db for visiting backends", "<string>",
                        assign_default_db, show_default_db, ALL_OPTS_PROPERTY);

    chassis_options_add(opts,
                        "ifname",
                        0, 0, OPTION_ARG_STRING, &(frontend->ifname),
                        "Set the network interface for distinguishing cetus instances", "<string>",
                        assign_ifname, show_ifname, ALL_OPTS_PROPERTY);

    chassis_options_add(opts,
                        "default-pool-size",
                        0, 0, OPTION_ARG_INT, &(frontend->default_pool_size),
                        "Set the default pool szie for visiting backends", "<integer>",
                        assign_default_pool_size, show_default_pool_size, ALL_OPTS_PROPERTY);

    chassis_options_add(opts,
                        "max-pool-size",
                        0, 0, OPTION_ARG_INT, &(frontend->max_pool_size),
                        "Set the max pool szie for visiting backends", "<integer>",
                        assign_max_pool_size, show_max_pool_size, ALL_OPTS_PROPERTY);

    chassis_options_add(opts,
                        "worker-processes",
                        0, 0, OPTION_ARG_INT, &(frontend->worker_processes),
                        "Set worker processes for processing client requests", "<integer>",
                        assign_worker_processes, show_worker_processes, ALL_OPTS_PROPERTY);

    chassis_options_add(opts,
                        "max-resp-size",
                        0, 0, OPTION_ARG_INT64, &(frontend->max_resp_len),
                        "Set the max response size for one backend", "<integer(64)>",
                        assign_max_resp_len, show_max_resp_len, ALL_OPTS_PROPERTY);

    chassis_options_add(opts,
                        "max-alive-time",
                        0, 0, OPTION_ARG_INT, &(frontend->max_alive_time),
                        "Set the max alive time for server connection", "<integer>",
                        assign_max_alive_time, show_max_alive_time, ALL_OPTS_PROPERTY);

    chassis_options_add(opts,
                        "merged-output-size",
                        0, 0, OPTION_ARG_INT, &(frontend->merged_output_size),
                        "set the merged output size for tcp streaming", "<integer>",
                        assign_merged_output_size, show_merged_output_size, ALL_OPTS_PROPERTY);

    chassis_options_add(opts,
                        "max-header-size",
                        0, 0, OPTION_ARG_INT, &(frontend->max_header_size),
                        "set the max header size for tcp streaming", "<integer>",
                        assign_max_header_size, show_max_header_size, ALL_OPTS_PROPERTY);

#ifndef SIMPLE_PARSER
    chassis_options_add(opts,
                        "worker-id",
                        0, 0, OPTION_ARG_INT, &(frontend->worker_id),
                        "Set the worker id and the maximum value allowed is 63 and the min value is 1", "<integer>",
                        NULL, show_worker_id, SHOW_OPTS_PROPERTY|SAVE_OPTS_PROPERTY);
#endif

    chassis_options_add(opts,
                        "disable-threads",
                        0, 0, OPTION_ARG_NONE, &(frontend->disable_threads), "Disable all threads creation", NULL,
                        NULL, show_disable_threads, SHOW_OPTS_PROPERTY|SAVE_OPTS_PROPERTY);

    chassis_options_add(opts,
                        "ssl",
                        0, 0, OPTION_ARG_NONE, &(frontend->ssl), "Specifies that the server permits but does not require"
                        " encrypted connections. This option is disabled by default", NULL,
                        NULL, show_ssl, SHOW_OPTS_PROPERTY|SAVE_OPTS_PROPERTY);

    chassis_options_add(opts,
                        "enable-back-compress",
                        0, 0, OPTION_ARG_NONE, &(frontend->is_back_compressed),
                        "enable compression for backend interactions", NULL,
                        NULL, show_enable_back_compress, SHOW_OPTS_PROPERTY|SAVE_OPTS_PROPERTY);

    chassis_options_add(opts,
                        "enable-client-compress",
                        0, 0, OPTION_ARG_NONE, &(frontend->is_client_compress_support),
                        "enable compression for client interactions", NULL,
                        NULL, show_enable_client_compress, SHOW_OPTS_PROPERTY|SAVE_OPTS_PROPERTY);

    chassis_options_add(opts,
                        "check-slave-delay",
                        0, 0, OPTION_ARG_NONE, &(frontend->check_slave_delay),
                        "Check ro backends with heartbeat", NULL,
                        NULL, show_check_slave_delay, SHOW_OPTS_PROPERTY|SAVE_OPTS_PROPERTY);

    chassis_options_add(opts,
                        "slave-delay-down",
                        0, 0, OPTION_ARG_DOUBLE, &(frontend->slave_delay_down_threshold_sec),
                        "Slave will be set down after reach this delay secondes", "<double>",
                        assign_slave_delay_down, show_slave_delay_down, ALL_OPTS_PROPERTY);

    chassis_options_add(opts,
                        "slave-delay-recover",
                        0, 0, OPTION_ARG_DOUBLE, &(frontend->slave_delay_recover_threshold_sec),
                        "Slave will recover after below this delay secondes", "<double>",
                        assign_slave_delay_recover, show_slave_delay_recover, ALL_OPTS_PROPERTY);

    chassis_options_add(opts,
                        "default-query-cache-timeout",
                        0, 0, OPTION_ARG_INT, &(frontend->default_query_cache_timeout),
                        "default query cache timeout in ms", "<integer>",
                        assign_default_query_cache_timeout, show_default_query_cache_timeout, ALL_OPTS_PROPERTY);

    chassis_options_add(opts,
                        "default-client-idle-timeout",
                        0, 0, OPTION_ARG_INT, &(frontend->client_idle_timeout),
                        "set client idle timeout in seconds(default 28800 seconds)", "<integer>",
                        assign_default_client_idle_timeout, show_default_client_idle_timeout, ALL_OPTS_PROPERTY);

    chassis_options_add(opts,
                        "default-incomplete-tran-idle-timeout",
                        0, 0, OPTION_ARG_INT, &(frontend->incomplete_tran_idle_timeout),
                        "set client incomplete transaction idle timeout in seconds(default 3600 seconds)", "<integer>",
                        assign_default_incomplete_tran_idle_timeout, show_default_incomplete_tran_idle_timeout, ALL_OPTS_PROPERTY);

    chassis_options_add(opts,
                        "default-maintained-client-idle-timeout",
                        0, 0, OPTION_ARG_INT, &(frontend->maintained_client_idle_timeout),
                        "set maintained client idle timeout in seconds(default 30 seconds)", "<integer>",
                        assign_default_maintained_client_idle_timeout, 
                        show_default_maintained_client_idle_timeout, ALL_OPTS_PROPERTY);

    chassis_options_add(opts,
                        "long-query-time",
                        0, 0, OPTION_ARG_INT, &(frontend->long_query_time), "Long query time in ms", "<integer>",
                        assign_long_query_time, show_long_query_time, ALL_OPTS_PROPERTY);

    chassis_options_add(opts,
                        "enable-client-found-rows",
                        0, 0, OPTION_ARG_NONE, &(frontend->set_client_found_rows), "Set client found rows flag", NULL,
                        NULL, show_enable_client_found_rows, SHOW_OPTS_PROPERTY|SAVE_OPTS_PROPERTY);

    chassis_options_add(opts,
                        "reduce-connections",
                        0, 0, OPTION_ARG_NONE, &(frontend->is_reduce_conns),
                        "Reduce connections when idle connection num is too high", NULL,
                        NULL, show_reduce_connections, SHOW_OPTS_PROPERTY|SAVE_OPTS_PROPERTY);

    chassis_options_add(opts, "enable-query-cache", 0, 0, OPTION_ARG_NONE, &(frontend->query_cache_enabled), "", NULL,
                        NULL, show_enable_query_cache, SHOW_OPTS_PROPERTY|SAVE_OPTS_PROPERTY);

    chassis_options_add(opts, "enable-tcp-stream", 0, 0, OPTION_ARG_NONE, &(frontend->is_tcp_stream_enabled), "", NULL,
                        NULL, show_enable_tcp_stream, SHOW_OPTS_PROPERTY|SAVE_OPTS_PROPERTY);

    chassis_options_add(opts, "enable-fast-stream", 0, 0, OPTION_ARG_NONE, &(frontend->is_fast_stream_enabled), "", NULL,
                        NULL, show_enable_fast_stream, SHOW_OPTS_PROPERTY|SAVE_OPTS_PROPERTY);

    chassis_options_add(opts, "enable-sql-special-processed", 0, 0, OPTION_ARG_NONE, &(frontend->is_sql_special_processed), "", NULL,
                        NULL, show_enable_sql_special_processed, SHOW_OPTS_PROPERTY|SAVE_OPTS_PROPERTY);

    chassis_options_add(opts, "partition-mode", 0, 0, OPTION_ARG_NONE, &(frontend->is_partition_mode), "", NULL,
                        NULL, show_enable_partition, SHOW_OPTS_PROPERTY|SAVE_OPTS_PROPERTY);

    chassis_options_add(opts, "check-sql-loosely", 0, 0, OPTION_ARG_NONE, &(frontend->check_sql_loosely), "", NULL,
                        NULL, show_check_sql_loosely, SHOW_OPTS_PROPERTY|SAVE_OPTS_PROPERTY);

    chassis_options_add(opts,
                        "log-xa-in-detail",
                        0, 0, OPTION_ARG_NONE, &(frontend->xa_log_detailed), "log xa in detail", NULL,
                        NULL, show_log_xa_in_detail, SHOW_OPTS_PROPERTY|SAVE_OPTS_PROPERTY);

    chassis_options_add(opts,
                        "disable-dns-cache",
                        0, 0, OPTION_ARG_NONE, &(frontend->disable_dns_cache),
                        "Every new connection to backends will resolve domain name", NULL,
                        NULL, show_disable_dns_cache, SHOW_OPTS_PROPERTY|SAVE_OPTS_PROPERTY);

    chassis_options_add(opts,
                        "master-preferred",
                        0, 0, OPTION_ARG_NONE, &(frontend->master_preferred), "Access to master preferentially", NULL,
                        NULL, show_master_preferred, SHOW_OPTS_PROPERTY|SAVE_OPTS_PROPERTY);
    chassis_options_add(opts,
                        "max-allowed-packet",
                        0, 0, OPTION_ARG_INT, &(frontend->cetus_max_allowed_packet),
                        "Max allowed packet as in mysql", "<int>",
                        assign_max_allowed_packet, show_max_allowed_packet, ALL_OPTS_PROPERTY);
    chassis_options_add(opts,
                        "remote-conf-url",
                        0, 0, OPTION_ARG_STRING, &(frontend->remote_config_url),
                        "Remote config url, mysql://xx", "<string>",
                        NULL, show_remote_conf_url, SHOW_OPTS_PROPERTY);
    chassis_options_add(opts,
                        "trx-isolation-level",
                        0, 0, OPTION_ARG_STRING, &(frontend->trx_isolation_level),
                        "transaction isolation level, default: REPEATABLE READ", "<string>",
                        NULL, show_trx_isolation_level, SHOW_OPTS_PROPERTY);
    chassis_options_add(opts,
                        "group-replication-mode",
                        0, 0, OPTION_ARG_INT, &(frontend->group_replication_mode),
                        "mysql group replication mode, 0:not support(defaults) 1:support single primary mode 2:support multi primary mode(not implement yet)", "<int>",
                        assign_group_replication, show_group_replication_mode, ALL_OPTS_PROPERTY);
    chassis_options_add(opts,
                        "sql-log-bufsize",
                        0, 0, OPTION_ARG_INT, &(frontend->sql_log_bufsize),
                        "the buffer size of the log","<int>",
                        NULL, show_sql_log_bufsize, SHOW_OPTS_PROPERTY|SAVE_OPTS_PROPERTY);
    chassis_options_add(opts,
                        "sql-log-switch",
                        0, 0, OPTION_ARG_STRING, &(frontend->sql_log_switch),
                        "the log switch, ON/OFF/REALTIME","<string>",
                        assign_sql_log_switch, show_sql_log_switch, ALL_OPTS_PROPERTY);
    chassis_options_add(opts,
                        "sql-log-prefix",
                         0, 0, OPTION_ARG_STRING, &(frontend->sql_log_prefix),
                         "the log filename","<string>",
                         NULL, show_sql_log_prefix, SHOW_OPTS_PROPERTY|SAVE_OPTS_PROPERTY);
    chassis_options_add(opts,
                         "sql-log-path",
                          0, 0, OPTION_ARG_STRING, &(frontend->sql_log_path),
                          "the log path","<string>",
                          NULL, show_sql_log_path, SHOW_OPTS_PROPERTY|SAVE_OPTS_PROPERTY);
    chassis_options_add(opts,
                         "sql-log-maxsize",
                          0, 0, OPTION_ARG_INT, &(frontend->sql_log_maxsize),
                          "the maxsize of sql file, units is M","<int>",
                          NULL, show_sql_log_maxsize, SHOW_OPTS_PROPERTY|SAVE_OPTS_PROPERTY);
    chassis_options_add(opts,
                         "sql-log-mode",
                          0, 0, OPTION_ARG_STRING, &(frontend->sql_log_mode),
                          "the mode of sql file","<string>",
                          assign_sql_log_mode, show_sql_log_mode, ALL_OPTS_PROPERTY);
    chassis_options_add(opts,
                         "sql-log-idletime",
                          0, 0, OPTION_ARG_INT, &(frontend->sql_log_idletime),
                          "sql log idle time when no log flush to disk","<int>",
                          assign_sql_log_idletime, show_sql_log_idletime, ALL_OPTS_PROPERTY);
    chassis_options_add(opts,
                          "sql-log-maxnum",
                          0, 0, OPTION_ARG_INT, &(frontend->sql_log_maxnum),
                          "aximum number of sql log files","<int>",
                          assign_sql_log_maxnum, show_sql_log_maxnum, ALL_OPTS_PROPERTY);
    chassis_options_add(opts,
                          "check-dns",
                          0, 0, OPTION_ARG_NONE, &(frontend->check_dns),
                          "check dns when hostname changed",NULL,
                          NULL, show_check_dns, SHOW_OPTS_PROPERTY|SAVE_OPTS_PROPERTY);

    return 0;
}

#ifdef HAVE_SIGACTION
static void
log_backtrace(void)
{
    void *array[16];
    int size, i;
    char **strings;

    size = backtrace(array, 16);
    strings = backtrace_symbols(array, size);
    g_warning("Obtained %d stack frames.", size);

    for (i = 0; i < size; i++) {
        g_warning("%s", strings[i]);
    }

    free(strings);
}

static void
sigsegv_handler(int G_GNUC_UNUSED signum)
{
    log_backtrace();

    abort();                    /* trigger a SIGABRT instead of just exiting */
}
#endif

static gboolean
check_plugin_mode_valid(struct chassis_frontend_t *frontend, chassis *srv)
{
    int i, proxy_mode = 0, sharding_mode = 0;

    for (i = 0; frontend->plugin_names[i]; ++i) {
        char *name = frontend->plugin_names[i];
        if (strcmp(name, "shard") == 0) {
            sharding_mode = 1;
            g_message("set sharding mode true");
        }
        if (strcmp(name, "proxy") == 0) {
            proxy_mode = 1;
        }
    }

    if (sharding_mode && proxy_mode) {
        g_critical("shard & proxy is mutual exclusive");
        return FALSE;
    }
#ifdef SIMPLE_PARSER
    if (sharding_mode) {
        g_critical("try loading shard-plugin.so from rw-edition, exit");
        return FALSE;
    }
#else
    if (proxy_mode) {
        g_critical("try loading proxy-plugin.so from shard-edition, exit");
        return FALSE;
    }
#endif

    return TRUE;
}

static void
g_query_cache_item_free(gpointer q)
{
    query_cache_item *item = q;
    network_queue_free(item->queue);
    g_free(item);
}

/* strdup with 1) default value & 2) NULL check */
#define DUP_STRING(STR, DEFAULT) \
        (STR) ? g_strdup(STR) : ((DEFAULT) ? g_strdup(DEFAULT) : NULL)

static void
init_parameters(struct chassis_frontend_t *frontend, chassis *srv)
{
    srv->default_username = DUP_STRING(frontend->default_username, NULL);
    srv->default_charset = DUP_STRING(frontend->default_charset, "utf8");
    srv->default_db = DUP_STRING(frontend->default_db, NULL);
    srv->ifname = DUP_STRING(frontend->ifname, "eth0");

#if defined(SO_REUSEPORT)
    g_message("%s:SO_REUSEPORT is defined", G_STRLOC);
    if (frontend->worker_processes < 0) {
        srv->worker_processes = 1;
    } else if (frontend->worker_processes > MAX_WORK_PROCESSES) {
        srv->worker_processes = MAX_WORK_PROCESSES;
    } else {
        srv->worker_processes = frontend->worker_processes;
    }
#else
    g_message("%s:SO_REUSEPORT is undefined", G_STRLOC);
    srv->worker_processes = 1;
#endif

    g_message("set worker processes:%d", srv->worker_processes);

    if (frontend->default_pool_size < DEFAULT_POOL_SIZE) {
        frontend->default_pool_size = DEFAULT_POOL_SIZE;
    }

    srv->mid_idle_connections = frontend->default_pool_size;
    g_message("set default pool size:%d", srv->mid_idle_connections);

    int connections_created_per_time = srv->mid_idle_connections / srv->worker_processes;
    if (connections_created_per_time > MAX_CREATE_CONN_NUM) {
        srv->connections_created_per_time = MAX_CREATE_CONN_NUM;
    } else {
        srv->connections_created_per_time = connections_created_per_time;
    }

    if (frontend->max_pool_size >= srv->mid_idle_connections) {
        srv->max_idle_connections = frontend->max_pool_size;
    } else {
        srv->max_idle_connections = srv->mid_idle_connections << 1;
    }
    g_message("set max pool size:%d", srv->max_idle_connections);

    srv->max_resp_len = frontend->max_resp_len;
    g_message("set max resp len:%lld", srv->max_resp_len);

    srv->current_time = time(0);
    if (frontend->max_alive_time < 60) {
        frontend->max_alive_time = 60;
    }
    srv->max_alive_time = frontend->max_alive_time;
    g_message("set max alive time:%d", srv->max_alive_time);

    srv->merged_output_size = frontend->merged_output_size;
    srv->compressed_merged_output_size = srv->merged_output_size << 3;
    g_message("%s:set merged output size:%d", G_STRLOC, srv->merged_output_size);

    srv->max_header_size = frontend->max_header_size;
    g_message("%s:set max header size:%d", G_STRLOC, srv->max_header_size);

#ifndef SIMPLE_PARSER
    if (frontend->worker_id > 0) {
        srv->guid_state.worker_id = frontend->worker_id & 0x3f;
    } else {
        struct timeval tp;
        gettimeofday(&tp, NULL);
        unsigned int seed = tp.tv_usec;
        srv->guid_state.worker_id = (int)((rand_r(&seed) / (RAND_MAX + 1.0)) * 64);
        g_warning("%s:please set worker id first, different instances should have different worker ids", G_STRLOC);
        g_message("%s: the system chooses worker id automatically although it may have potential conflicts:%d",
                G_STRLOC, srv->guid_state.worker_id);
    }
#endif

#undef DUP_STRING

    srv->client_found_rows = frontend->set_client_found_rows;
    g_message("set client_found_rows %s", srv->client_found_rows ? "true" : "false");

    srv->xa_log_detailed = frontend->xa_log_detailed;
    if (srv->xa_log_detailed) {
        g_message("%s:xa_log_detailed true", G_STRLOC);
    } else {
        g_message("%s:xa_log_detailed false", G_STRLOC);
    }
    srv->query_cache_enabled = frontend->query_cache_enabled;
    if (srv->query_cache_enabled) {
        srv->query_cache_table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_query_cache_item_free);
        srv->cache_index = g_queue_new();
    }
    srv->is_tcp_stream_enabled = frontend->is_tcp_stream_enabled;
    if (srv->is_tcp_stream_enabled) {
        g_message("%s:tcp stream enabled", G_STRLOC);
    }
    srv->is_fast_stream_enabled = frontend->is_fast_stream_enabled;
    if (srv->is_fast_stream_enabled) {
        g_message("%s:fast stream enabled", G_STRLOC);
    }
#ifndef SIMPLE_PARSER
    srv->is_partition_mode = frontend->is_partition_mode;
    if (srv->is_partition_mode) {
        g_message("%s:partition mode", G_STRLOC);
    }
#endif
    srv->check_sql_loosely = frontend->check_sql_loosely;

    srv->is_sql_special_processed = frontend->is_sql_special_processed;
    if (srv->is_sql_special_processed) {
        g_message("%s:enable sql special porcessing", G_STRLOC);
    }
    srv->disable_threads = frontend->disable_threads;
    srv->is_back_compressed = frontend->is_back_compressed;
    srv->compress_support = frontend->is_client_compress_support;
    srv->check_slave_delay = frontend->check_slave_delay;
    srv->slave_delay_down_threshold_sec = frontend->slave_delay_down_threshold_sec;
    srv->master_preferred = frontend->master_preferred;
    srv->disable_dns_cache = frontend->disable_dns_cache;
    if (frontend->slave_delay_recover_threshold_sec > 0) {
        srv->slave_delay_recover_threshold_sec = frontend->slave_delay_recover_threshold_sec;
        if (srv->slave_delay_recover_threshold_sec > srv->slave_delay_down_threshold_sec) {
            srv->slave_delay_recover_threshold_sec = srv->slave_delay_down_threshold_sec;
            g_warning("`slave-delay-recover` should be lower than `slave-delay-down`.");
            g_warning("Set slave-delay-recover=%.3f", srv->slave_delay_down_threshold_sec);
        }
    } else {
        srv->slave_delay_recover_threshold_sec = 1.0;
    }

    srv->default_query_cache_timeout = MAX(frontend->default_query_cache_timeout, 1);
    srv->client_idle_timeout = MAX(frontend->client_idle_timeout, 10);
    srv->incomplete_tran_idle_timeout = MAX(frontend->incomplete_tran_idle_timeout, 10);
    srv->maintained_client_idle_timeout = MAX(frontend->maintained_client_idle_timeout, 10);
    srv->long_query_time = MIN(frontend->long_query_time, MAX_QUERY_TIME);
    srv->cetus_max_allowed_packet = CLAMP(frontend->cetus_max_allowed_packet,
                                          MAX_ALLOWED_PACKET_FLOOR, MAX_ALLOWED_PACKET_CEIL);
    srv->check_dns = frontend->check_dns;

    if (frontend->trx_isolation_level != NULL) {
        if (strcasecmp(frontend->trx_isolation_level, "REPEATABLE READ") == 0 ||
                strcasecmp(frontend->trx_isolation_level, "REPEATABLE-READ"))
        {
            srv->internal_trx_isolation_level = TF_REPEATABLE_READ;
            srv->trx_isolation_level = g_strdup("REPEATABLE-READ");
        } else if (strcasecmp(frontend->trx_isolation_level, "READ COMMITTED") == 0 ||
                strcasecmp(frontend->trx_isolation_level, "READ-COMMITTED") == 0)
        {
            srv->internal_trx_isolation_level = TF_READ_COMMITTED;
            srv->trx_isolation_level = g_strdup("READ-COMMITTED");
        } else if (strcasecmp(frontend->trx_isolation_level, "READ UNCOMMITTED") == 0 ||
                strcasecmp(frontend->trx_isolation_level, "READ-UNCOMMITTED") == 0)
        {
            srv->internal_trx_isolation_level = TF_READ_UNCOMMITTED;
            srv->trx_isolation_level = g_strdup("READ-UNCOMMITTED");
        } else if (strcasecmp(frontend->trx_isolation_level, "SERIALIZABLE") == 0) {
            srv->internal_trx_isolation_level = TF_SERIALIZABLE;
            srv->trx_isolation_level = g_strdup("SERIALIZABLE");
        } else {
            srv->internal_trx_isolation_level = TF_READ_COMMITTED;
            g_warning("trx isolation level:%s is not expected, use READ COMMITTED instead",
                    frontend->trx_isolation_level);
            srv->trx_isolation_level = g_strdup("READ-COMMITTED");
        }
    } else {
        g_message("trx isolation level is not set");
        srv->internal_trx_isolation_level = TF_READ_COMMITTED;
        srv->trx_isolation_level = g_strdup("READ-COMMITTED");
    }
    
    g_message("trx isolation level value:%s", srv->trx_isolation_level);
}

static void
release_resouces_when_exit(struct chassis_frontend_t *frontend, chassis *srv, GError *gerr,
                           chassis_options_t *opts, chassis_log *log)
{
    if (gerr)
        g_error_free(gerr);
    if (srv)
        chassis_free(srv);
    g_debug("%s: call chassis_options_free", G_STRLOC);
    if (opts)
        chassis_options_free(opts);
    g_debug("%s: call chassis_log_free", G_STRLOC);
    chassis_log_free(log);
    tc_log_end();

    chassis_frontend_free(frontend);
}

static void
resolve_path(chassis *srv, struct chassis_frontend_t *frontend)
{
    /*
     * these are used before we gathered all the options
     * from the plugins, thus we need to fix them up before
     * dealing with all the rest.
     */
    char *new_path;
    new_path = chassis_resolve_path(srv->base_dir, frontend->log_filename);
    if (new_path && new_path != frontend->log_filename) {
        g_free(frontend->log_filename);
        frontend->log_filename = new_path;
    }

    new_path = chassis_resolve_path(srv->base_dir, frontend->pid_file);
    if (new_path && new_path != frontend->pid_file) {
        g_free(frontend->pid_file);
        frontend->pid_file = new_path;
    }
    srv->pid_file = g_strdup(frontend->pid_file);

    new_path = chassis_resolve_path(srv->base_dir, frontend->plugin_dir);
    if (new_path && new_path != frontend->plugin_dir) {
        g_free(frontend->plugin_dir);
        frontend->plugin_dir = new_path;
    }

    new_path = chassis_resolve_path(srv->base_dir, frontend->conf_dir);
    if (new_path && new_path != frontend->conf_dir) {
        g_free(frontend->conf_dir);
        frontend->conf_dir = new_path;
    }
}

static void
slow_query_log_handler(const gchar *log_domain, GLogLevelFlags log_level, const gchar *message, gpointer user_data)
{
    FILE *fp = user_data;
    fwrite(message, 1, strlen(message), fp);
}

static FILE *
init_slow_query_log(const char *main_log)
{
    if (!main_log) {
        return NULL;
    }
    GString *log_name = g_string_new(main_log);
    g_string_append(log_name, ".slowquery.log");

    FILE *fp = fopen(log_name->str, "a");
    if (fp) {
        g_log_set_handler("slowquery", G_LOG_LEVEL_MASK | G_LOG_FLAG_FATAL
                          | G_LOG_FLAG_RECURSION, slow_query_log_handler, fp);
    }
    g_string_free(log_name, TRUE);
    return fp;
}


/**
 * This is the "real" main which is called on UNIX platforms.
 */
int
main_cmdline(int argc, char **argv)
{
    chassis *srv = NULL;
#ifdef HAVE_SIGACTION
    static struct sigaction sigsegv_sa;
#endif
    /* read the command-line options */
    struct chassis_frontend_t *frontend = NULL;
    chassis_options_t *opts = NULL;

    GError *gerr = NULL;
    chassis_log *log = NULL;
    FILE *slow_query_log_fp = NULL;

    /*
     * a little helper macro to set the src-location that
     * we stepped out at to exit
     */
#define GOTO_EXIT(status) \
    exit_code = status; \
    exit_location = G_STRLOC; \
    goto exit_nicely;
    int exit_code = EXIT_SUCCESS;
    const gchar *exit_location = G_STRLOC;

    /* init module, ... system */
    if (chassis_frontend_init_glib()) {
        GOTO_EXIT(EXIT_FAILURE);
    }

    /* start the logging ... to stderr */
    log = chassis_log_new();
    /* display messages while parsing or loading plugins */
    log->min_lvl = G_LOG_LEVEL_MESSAGE;
    g_log_set_default_handler(chassis_log_func, log);

    /* may fail on library mismatch */
    if (NULL == (srv = chassis_new())) {
        GOTO_EXIT(EXIT_FAILURE);
    }

    srv->argc = argc;
    srv->argv = g_new0(char *, argc);
    int i;
    for (i = 0; i < argc; i++) {
        srv->argv[i] = g_strdup(argv[i]);
    }
    /* we need the log structure for the log-rotation */
    srv->log = log;

    frontend = chassis_frontend_new();

    if (frontend == NULL) {
        GOTO_EXIT(EXIT_FAILURE);
    }

    /**
     * parse once to get the basic options like --default-file and --version
     *
     * leave the unknown options in the list
     */

    if (chassis_frontend_init_base_options(&argc, &argv, &(frontend->print_version), &(frontend->default_file), &gerr)) {
        g_critical("%s: %s", G_STRLOC, gerr->message);
        g_clear_error(&gerr);

        GOTO_EXIT(EXIT_FAILURE);
    }
    srv->print_version = frontend->print_version;

    if (frontend->default_file) {
        srv->default_file = g_strdup(frontend->default_file);
        if (!(frontend->keyfile = chassis_frontend_open_config_file(frontend->default_file, &gerr))) {
            g_critical("%s: loading config from '%s' failed: %s", G_STRLOC, frontend->default_file, gerr->message);
            g_clear_error(&gerr);
            GOTO_EXIT(EXIT_FAILURE);
        }
    }

    /* print the main version number here, but don't exit
     * we check for print_version again, after loading the plugins (if any)
     * and print their version numbers, too. then we exit cleanly.
     */
    if (frontend->print_version) {
        chassis_frontend_print_version();
    }

    /* add the other options which can also appear in the config file */
    opts = chassis_options_new();
    opts->ignore_unknown = TRUE;
    srv->options = opts;

    chassis_frontend_set_chassis_options(frontend, opts, srv);

    if (FALSE == chassis_options_parse_cmdline(opts, &argc, &argv, &gerr)) {
        g_critical("%s:%s", G_STRLOC, gerr->message);
        GOTO_EXIT(EXIT_FAILURE);
    }

    if (frontend->keyfile) {
        if (FALSE == chassis_keyfile_to_options_with_error(frontend->keyfile, "cetus", opts->options, &gerr)) {
            g_critical("%s", gerr->message);
            GOTO_EXIT(EXIT_FAILURE);
        }
    }

    if (frontend->remote_config_url) {
        srv->remote_config_url = g_strdup(frontend->remote_config_url);
        srv->config_manager = chassis_config_from_url(srv->remote_config_url);
        if (!srv->config_manager) {
            g_critical("remote config init error");
            GOTO_EXIT(EXIT_FAILURE);
        }
        if (!chassis_config_parse_options(srv->config_manager, opts->options)) {
            g_critical("remote_config parse error");
            GOTO_EXIT(EXIT_FAILURE);
        }
    }

    if (chassis_frontend_init_basedir(argv[0], &(frontend->base_dir))) {
        GOTO_EXIT(EXIT_FAILURE);
    }
#ifdef HAVE_SIGACTION
    /* register the sigsegv interceptor */

    memset(&sigsegv_sa, 0, sizeof(sigsegv_sa));
    sigsegv_sa.sa_handler = sigsegv_handler;
    sigemptyset(&sigsegv_sa.sa_mask);

    frontend->invoke_dbg_on_crash = 1;
    srv->invoke_dbg_on_crash = frontend->invoke_dbg_on_crash;

    if (srv->invoke_dbg_on_crash && !(RUNNING_ON_VALGRIND)) {
        sigaction(SIGSEGV, &sigsegv_sa, NULL);
    }
#endif

    /*
     * some plugins cannot see the chassis struct from the point
     * where they open files, hence we must make it available
     */
    srv->base_dir = g_strdup(frontend->base_dir);
    srv->plugin_dir = g_strdup(frontend->plugin_dir);
    chassis_frontend_init_plugin_dir(&frontend->plugin_dir, srv->base_dir);

    if (!frontend->conf_dir)
        frontend->conf_dir = g_strdup("conf");

    resolve_path(srv, frontend);

    srv->conf_dir = g_strdup(frontend->conf_dir);

    if (!srv->config_manager) { /* if no remote-config-url, we use local config */
        srv->config_manager = chassis_config_from_local_dir(srv->conf_dir, frontend->default_file);
    }

    cetus_pid = getpid();

    /*
     * start the logging
     */
    if (frontend->log_filename) {
        log->log_filename = g_strdup(frontend->log_filename);
    }

    if (log->log_filename && FALSE == chassis_log_open(log)) {
        g_critical("can't open log-file '%s': %s", log->log_filename, g_strerror(errno));

        GOTO_EXIT(EXIT_FAILURE);
    }

    /*
     * handle log-level after the config-file is read,
     * just in case it is specified in the file
     */
    if (frontend->log_level) {
        srv->log_level = g_strdup(frontend->log_level);
    } else {
        /* if it is not set, use "critical" as default */
        srv->log_level = g_strdup("critical");
    }
    if (0 != chassis_log_set_level(log, srv->log_level)) {
        g_critical("--log-level=... failed, level '%s' is unknown ", srv->log_level);

        GOTO_EXIT(EXIT_FAILURE);
    }

    g_message("starting " PACKAGE_STRING);
#ifdef CHASSIS_BUILD_TAG
    g_message("build revision: " CHASSIS_BUILD_TAG);
#endif

    g_message("glib version: %d.%d.%d", GLIB_MAJOR_VERSION, GLIB_MINOR_VERSION, GLIB_MICRO_VERSION);
    g_message("libevent version: %s", event_get_version());
    g_message("config dir: %s", frontend->conf_dir);

    srv->ssl = frontend->ssl;

    init_parameters(frontend, srv);

    if (network_mysqld_init(srv) == -1) {
        g_print("network_mysqld_init failed\n");
        GOTO_EXIT(EXIT_FAILURE);
    }

    if (!frontend->plugin_names) {
        frontend->plugin_names = g_new(char *, 2);
#ifdef DEFAULT_PLUGIN
        frontend->plugin_names[0] = g_strdup(DEFAULT_PLUGIN);
#else
        frontend->plugin_names[0] = g_strdup("proxy");
#endif
        frontend->plugin_names[1] = NULL;
    }

    if (chassis_frontend_load_plugins(srv->modules, frontend->plugin_dir, frontend->plugin_names)) {
        GOTO_EXIT(EXIT_FAILURE);
    }

    srv->plugin_names = g_new(char *, (srv->modules->len + 1));
    for (i = 0; frontend->plugin_names[i]; i++) {
        if (!g_strcmp0("", frontend->plugin_names[i])) {
            continue;
        }

        srv->plugin_names[i] = g_strdup(frontend->plugin_names[i]);
    }
    srv->plugin_names[i] = NULL;

    if (chassis_frontend_init_plugins(srv->modules,
                                      opts, srv->config_manager, &argc, &argv, frontend->keyfile, "cetus", &gerr)) {
        g_critical("%s: %s", G_STRLOC, gerr->message);
        g_clear_error(&gerr);

        GOTO_EXIT(EXIT_FAILURE);
    }

    /* if we only print the version numbers, exit and don't do any more work */
    if (frontend->print_version) {
        chassis_frontend_print_plugin_versions(srv->modules);
        GOTO_EXIT(EXIT_SUCCESS);
    }

    /* we know about the options now, lets parse them */
    opts->ignore_unknown = FALSE;
    opts->help_enabled = TRUE;

    /* handle unknown options */
    if (FALSE == chassis_options_parse_cmdline(opts, &argc, &argv, &gerr)) {
        if (gerr->domain == G_OPTION_ERROR && gerr->code == G_OPTION_ERROR_UNKNOWN_OPTION) {
            g_critical("%s: %s (use --help to show all options)", G_STRLOC, gerr->message);
        } else {
            g_critical("%s: %s (code = %d, domain = %s)",
                       G_STRLOC, gerr->message, gerr->code, g_quark_to_string(gerr->domain)
                );
        }

        GOTO_EXIT(EXIT_FAILURE);
    }

    /* after parsing the options we should only have the program name left */
    if (argc > 1) {
        g_critical("unknown option: %s", argv[1]);

        GOTO_EXIT(EXIT_FAILURE);
    }

    signal(SIGPIPE, SIG_IGN);

    srv->daemon_mode = frontend->daemon_mode;

    if (srv->daemon_mode) {
        g_message("%s:daemon mode", G_STRLOC);
        chassis_unix_daemonize();
    }

    if(frontend->group_replication_mode != 0 && frontend->group_replication_mode != 1) {
        g_critical("group-replication-mode is invalid, current value is %d", frontend->group_replication_mode);
        GOTO_EXIT(EXIT_FAILURE);
    }
    srv->group_replication_mode = frontend->group_replication_mode;

    /*
     * log the versions of all loaded plugins
     */
    chassis_frontend_log_plugin_versions(srv->modules);

    srv->verbose_shutdown = frontend->verbose_shutdown;

    srv->is_reduce_conns = frontend->is_reduce_conns;

    /*
     * we have to drop root privileges in chassis_mainloop() after
     * the plugins opened the ports, so we need the user there
     */
    srv->user = g_strdup(frontend->user);

    if (check_plugin_mode_valid(frontend, srv) == FALSE) {
        GOTO_EXIT(EXIT_FAILURE);
    }

    if (frontend->default_username == NULL) {
        g_critical("proxy needs default username");
        GOTO_EXIT(EXIT_FAILURE);
    }


#ifndef SIMPLE_PARSER
    if (!frontend->log_xa_filename)
        frontend->log_xa_filename = g_strdup("logs/xa.log");

    srv->log_xa_filename = g_strdup(frontend->log_xa_filename);
    char *new_path = chassis_resolve_path(srv->base_dir, srv->log_xa_filename);
    if (new_path && new_path != srv->log_xa_filename) {
        g_free(srv->log_xa_filename);
        srv->log_xa_filename = new_path;
    }

    g_message("XA log file: %s", srv->log_xa_filename);

    if (tc_log_init(srv->log_xa_filename) == -1) {
        GOTO_EXIT(EXIT_FAILURE);
    }
#endif

    slow_query_log_fp = init_slow_query_log(log->log_filename);
    if (!slow_query_log_fp) {
        g_warning("cannot open slow-query log");
        GOTO_EXIT(EXIT_FAILURE);
    }

    srv->max_files_number = frontend->max_files_number;
    if (srv->max_files_number) {
        if (0 != chassis_fdlimit_set(srv->max_files_number)) {
            g_critical("%s: setting fdlimit = %d failed: %s (%d)",
                       G_STRLOC, srv->max_files_number, g_strerror(errno), errno);
            GOTO_EXIT(EXIT_FAILURE);
        }
    }
    g_debug("max open file-descriptors = %" G_GINT64_FORMAT, chassis_fdlimit_get());


    if (srv->sql_mgr) {
        if (frontend->sql_log_bufsize) {
            srv->sql_mgr->sql_log_bufsize = frontend->sql_log_bufsize;
        }
        if (frontend->sql_log_switch) {
            if (strcasecmp(frontend->sql_log_switch, "ON") == 0) {
                srv->sql_mgr->sql_log_switch = ON;
            } else if (strcasecmp(frontend->sql_log_switch, "REALTIME") == 0) {
                srv->sql_mgr->sql_log_switch = REALTIME;
            } else if (strcasecmp(frontend->sql_log_switch, "OFF") == 0) {
                srv->sql_mgr->sql_log_switch = OFF;
            } else {
                g_critical("sql-log-switch is invalid, current value is %s", frontend->sql_log_switch);
                GOTO_EXIT(EXIT_FAILURE);
            }
        }
        if (frontend->sql_log_prefix) {
            srv->sql_mgr->sql_log_prefix = g_strdup(frontend->sql_log_prefix);
        }
        if (frontend->sql_log_path) {
            srv->sql_mgr->sql_log_path = g_strdup(frontend->sql_log_path);
        } else if(frontend->base_dir) {
            srv->sql_mgr->sql_log_path = g_strdup_printf("%s/logs", frontend->base_dir);
        }
        if (frontend->sql_log_maxsize >= 0) {
            srv->sql_mgr->sql_log_maxsize = frontend->sql_log_maxsize;
        }

        if (frontend->sql_log_mode) {
            if (strcasecmp(frontend->sql_log_mode, "CLIENT") == 0) {
                srv->sql_mgr->sql_log_mode = CLIENT;
            } else if(strcasecmp(frontend->sql_log_mode, "BACKEND") == 0) {
                srv->sql_mgr->sql_log_mode = BACKEND;
            } else if(strcasecmp(frontend->sql_log_mode, "ALL") == 0) {
                srv->sql_mgr->sql_log_mode = ALL;
            } else if (strcasecmp(frontend->sql_log_mode, "CONNECT") == 0) {
                srv->sql_mgr->sql_log_mode = CONNECT;
            } else if (strcasecmp(frontend->sql_log_mode, "FRONT") == 0) {
                srv->sql_mgr->sql_log_mode = FRONT;
            } else {
                g_critical("sql-log-mode is invalid, current value is %s", frontend->sql_log_mode);
                GOTO_EXIT(EXIT_FAILURE);
            }
        }
        if (frontend->sql_log_idletime) {
            srv->sql_mgr->sql_log_idletime = frontend->sql_log_idletime;
        }
        if (frontend->sql_log_maxnum >= 0) {
            srv->sql_mgr->sql_log_maxnum = frontend->sql_log_maxnum;
        }
    }
    srv->check_dns = frontend->check_dns;

    if (chassis_mainloop(srv)) {
        /* looks like we failed */
        g_critical("%s: Failure from chassis_mainloop. Shutting down.", G_STRLOC);
        GOTO_EXIT(EXIT_FAILURE);
    }

  exit_nicely:
    /* necessary to set the shutdown flag, because the monitor will continue
     * to schedule timers otherwise, causing an infinite loop in cleanup
     */
    if (!exit_code) {
        exit_location = G_STRLOC;
    }

    struct mallinfo m = mallinfo();

    g_message("Total allocated space (bytes): %d", m.uordblks);
    g_message("Total free space (bytes): %d", m.fordblks);
    g_message("Top-most, releasable space (bytes): %d", m.keepcost);

    chassis_set_shutdown_location(exit_location);

    if (frontend && !frontend->print_version) {
        /* add a tag to the logfile */
        g_log(G_LOG_DOMAIN,
              ((srv && srv->verbose_shutdown) ? G_LOG_LEVEL_CRITICAL : G_LOG_LEVEL_MESSAGE),
              "shutting down normally, exit code is: %d", exit_code);
    }
#ifdef HAVE_SIGACTION
    /* reset the handler */
    sigsegv_sa.sa_handler = SIG_DFL;
    if (frontend && srv->invoke_dbg_on_crash && !(RUNNING_ON_VALGRIND)) {
        sigaction(SIGSEGV, &sigsegv_sa, NULL);
    }
#endif

    release_resouces_when_exit(frontend, srv, gerr, opts, log);
    if (slow_query_log_fp)
        fclose(slow_query_log_fp);

    return exit_code;
}

int
main(int argc, char **argv)
{
    return main_cmdline(argc, argv);
}
