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

#ifndef _CHASSIS_OPTIONS_UTILS_H_
#define _CHASSIS_OPTIONS_UTILS_H_

#include <glib.h>
#include "chassis-exports.h"
#include "chassis-options.h"
#include "chassis-mainloop.h"

#define ASSIGN_OPTS_PROPERTY 0x01
#define SHOW_OPTS_PROPERTY 0x02
#define SAVE_OPTS_PROPERTY 0x04

#define SHOW_SAVE_OPTS_PROPERTY (SHOW_OPTS_PROsPERTY|SAVE_OPTS_PROPERTY)
#define ALL_OPTS_PROPERTY (ASSIGN_OPTS_PROPERTY|SHOW_OPTS_PROPERTY|SAVE_OPTS_PROPERTY)

#define CAN_ASSIGN_OPTS_PROPERTY(opt_property) ((opt_property) & ASSIGN_OPTS_PROPERTY)
#define CAN_SHOW_OPTS_PROPERTY(opt_property) ((opt_property) & SHOW_OPTS_PROPERTY)
#define CAN_SAVE_OPTS_PROPERTY(opt_property) ((opt_property) & SAVE_OPTS_PROPERTY)

enum {
    ASSIGN_OK,
    ASSIGN_ERROR, 
    ASSIGN_NOT_SUPPORT,
    ASSIGN_VALUE_INVALID,
    SAVE_ERROR,
    CHMOD_ERROR,
    CHANGE_SAVE_ERROR
};

/* show utils */
CHASSIS_API gchar* show_verbose_shutdown(gpointer param);
CHASSIS_API gchar* show_daemon_mode(gpointer param);
CHASSIS_API gchar* show_user(gpointer param);
CHASSIS_API gchar* show_basedir(gpointer param);
CHASSIS_API gchar* show_confdir(gpointer param);
CHASSIS_API gchar* show_pidfile(gpointer param);
CHASSIS_API gchar* show_plugindir(gpointer param);
CHASSIS_API gchar* show_plugins(gpointer param);
CHASSIS_API gchar* show_log_level(gpointer param);
CHASSIS_API gchar* show_log_file(gpointer param);
CHASSIS_API gchar* show_log_xa_file(gpointer param);
CHASSIS_API gchar* show_log_backtrace_on_crash(gpointer param);
CHASSIS_API gchar* show_keepalive(gpointer param);
CHASSIS_API gchar* show_max_open_files(gpointer param);
CHASSIS_API gchar* show_default_charset(gpointer param);
CHASSIS_API gchar* show_default_username(gpointer param);
CHASSIS_API gchar* show_default_db(gpointer param);
CHASSIS_API gchar* show_ifname(gpointer param);
CHASSIS_API gchar* show_default_pool_size(gpointer param);
CHASSIS_API gchar* show_max_pool_size(gpointer param);
CHASSIS_API gchar* show_worker_processes(gpointer param);
CHASSIS_API gchar* show_max_resp_len(gpointer param);
CHASSIS_API gchar* show_max_alive_time(gpointer param);
CHASSIS_API gchar* show_merged_output_size(gpointer param);
CHASSIS_API gchar* show_max_header_size(gpointer param);
CHASSIS_API gchar* show_worker_id(gpointer param);
CHASSIS_API gchar* show_disable_threads(gpointer param);
CHASSIS_API gchar* show_enable_back_compress(gpointer param);
CHASSIS_API gchar* show_enable_client_compress(gpointer param);
CHASSIS_API gchar* show_check_slave_delay(gpointer param);
CHASSIS_API gchar* show_slave_delay_down(gpointer param);
CHASSIS_API gchar* show_slave_delay_recover(gpointer param);
CHASSIS_API gchar* show_default_query_cache_timeout(gpointer param);
CHASSIS_API gchar* show_default_client_idle_timeout(gpointer param);
CHASSIS_API gchar* show_default_incomplete_tran_idle_timeout(gpointer param);
CHASSIS_API gchar* show_default_maintained_client_idle_timeout(gpointer param);
CHASSIS_API gchar* show_long_query_time(gpointer param);
CHASSIS_API gchar* show_enable_client_found_rows(gpointer param);
CHASSIS_API gchar* show_reduce_connections(gpointer param);
CHASSIS_API gchar* show_enable_query_cache(gpointer param);
CHASSIS_API gchar* show_enable_tcp_stream(gpointer param);
CHASSIS_API gchar* show_enable_fast_stream(gpointer param);
CHASSIS_API gchar* show_enable_partition(gpointer param);
CHASSIS_API gchar* show_enable_sql_special_processed(gpointer param);
CHASSIS_API gchar* show_check_sql_loosely(gpointer param);
CHASSIS_API gchar* show_log_xa_in_detail(gpointer param);
CHASSIS_API gchar* show_disable_dns_cache(gpointer param);
CHASSIS_API gchar* show_master_preferred(gpointer param);
CHASSIS_API gchar* show_max_allowed_packet(gpointer param);
CHASSIS_API gchar* show_remote_conf_url(gpointer param);
CHASSIS_API gchar* show_trx_isolation_level(gpointer param);
CHASSIS_API gchar* show_group_replication_mode(gpointer param);
CHASSIS_API gchar* show_sql_log_bufsize(gpointer param);
CHASSIS_API gchar* show_sql_log_switch(gpointer param);
CHASSIS_API gchar* show_sql_log_prefix(gpointer param);
CHASSIS_API gchar* show_sql_log_path(gpointer param);
CHASSIS_API gchar* show_sql_log_maxsize(gpointer param);
CHASSIS_API gchar* show_sql_log_mode(gpointer param);
CHASSIS_API gchar* show_sql_log_idletime(gpointer param);
CHASSIS_API gchar* show_sql_log_maxnum(gpointer param);
CHASSIS_API gchar* show_check_dns(gpointer param);
CHASSIS_API gchar* show_ssl(gpointer param);

/* assign utils */
CHASSIS_API gint assign_log_level(const gchar *newval, gpointer param);
CHASSIS_API gint assign_default_charset(const gchar *newval, gpointer param);
CHASSIS_API gint assign_default_username(const gchar *newval, gpointer param);
CHASSIS_API gint assign_default_db(const gchar *newval, gpointer param);
CHASSIS_API gint assign_ifname(const gchar *newval, gpointer param);
CHASSIS_API gint assign_default_pool_size(const gchar *newval, gpointer param);
CHASSIS_API gint assign_max_pool_size(const gchar *newval, gpointer param);
CHASSIS_API gint assign_worker_processes(const gchar *newval, gpointer param);
CHASSIS_API gint assign_max_resp_len(const gchar *newval, gpointer param);
CHASSIS_API gint assign_max_alive_time(const gchar *newval, gpointer param);
CHASSIS_API gint assign_merged_output_size(const gchar *newval, gpointer param);
CHASSIS_API gint assign_max_header_size(const gchar *newval, gpointer param);
CHASSIS_API gint assign_slave_delay_recover(const gchar *newval, gpointer param);
CHASSIS_API gint assign_slave_delay_down(const gchar *newval, gpointer param);
CHASSIS_API gint assign_default_query_cache_timeout(const gchar *newval, gpointer param);
CHASSIS_API gint assign_default_client_idle_timeout(const gchar *newval, gpointer param);
CHASSIS_API gint assign_default_incomplete_tran_idle_timeout(const gchar *newval, gpointer param);
CHASSIS_API gint assign_default_maintained_client_idle_timeout(const gchar *newval, gpointer param);
CHASSIS_API gint assign_long_query_time(const gchar *newval, gpointer param);
CHASSIS_API gint assign_max_allowed_packet(const gchar *newval, gpointer param);
CHASSIS_API gint assign_group_replication(const gchar *newval, gpointer param);
CHASSIS_API gint assign_sql_log_switch(const gchar *newval, gpointer param);
CHASSIS_API gint assign_sql_log_mode(const gchar *newval, gpointer param);
CHASSIS_API gint assign_sql_log_idletime(const gchar *newval, gpointer param);
CHASSIS_API gint assign_sql_log_maxnum(const gchar *newval, gpointer param);
CHASSIS_API gint assign_check_dns(const gchar *newval, gpointer param);

CHASSIS_API gint chassis_options_save(GKeyFile *keyfile, chassis_options_t *opts, chassis  *chas);

#endif
