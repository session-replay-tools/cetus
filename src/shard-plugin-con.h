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

#ifndef _SHARD_PLUGIN_CON_
#define _SHARD_PLUGIN_CON_

/* TODO: move to plugins/shard/ */
#include "network-backend.h"
#include "network-mysqld.h"

/**
 * Contains extra connection state used for shard-plugin.
 */
typedef struct {
    network_backend_t *backend;
    /**< index into the backend-array, start from 0 */
    int backend_ndx;

    struct sql_context_t *sql_context;
    int trx_read_write;         /* default TF_READ_WRITE */
    int trx_isolation_level;    /* default TF_REPEATABLE_READ */

} shard_plugin_con_t;

NETWORK_API shard_plugin_con_t *shard_plugin_con_new();
NETWORK_API void shard_plugin_con_free(network_mysqld_con *con, shard_plugin_con_t *st);

#endif /*_SHARD_PLUGIN_CON_*/
