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

#include "shard-plugin-con.h"

#include <mysql.h>
#include <mysqld_error.h>
#include <errno.h>
#include <string.h>

#include "glib-ext.h"
#include "server-session.h"

shard_plugin_con_t *
shard_plugin_con_new()
{
    return g_new0(shard_plugin_con_t, 1);
}

void
shard_plugin_con_free(network_mysqld_con *con, shard_plugin_con_t *st)
{
    if (!st)
        return;

    if (con->servers) {
        int i;
        for (i = 0; i < con->servers->len; i++) {
            server_session_t *ss = g_ptr_array_index(con->servers, i);
            ss->sql = NULL;
            g_free(ss);
        }
        con->server = NULL;
    } else {
        if (con->server) {
            g_warning("%s: not expected here, connected_clients--for con:%p", G_STRLOC, con);
        }
    }
    g_free(st);
}
