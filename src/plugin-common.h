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

#ifndef _PLUGIN_COMMON_H
#define _PLUGIN_COMMON_H

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

#define XA_CMD_BUF_LEN 256

NETWORK_API network_socket_retval_t do_read_auth(network_mysqld_con *);
NETWORK_API network_socket_retval_t do_connect_cetus(network_mysqld_con *, network_backend_t **, int *);
NETWORK_API network_socket_retval_t plugin_add_backends(chassis *, gchar **, gchar **);
NETWORK_API int do_check_qeury_cache(network_mysqld_con *con);
NETWORK_API int try_to_get_resp_from_query_cache(network_mysqld_con *con);
NETWORK_API gboolean proxy_put_shard_conn_to_pool(network_mysqld_con *con);
NETWORK_API void remove_mul_server_recv_packets(network_mysqld_con *con);
NETWORK_API void truncate_default_db_when_drop_database(network_mysqld_con *con, char *);

#endif
