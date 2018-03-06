#include "shard-plugin-con.h"

#include <mysql.h>
#include <mysqld_error.h>
#include <errno.h>
#include <string.h>

#include "glib-ext.h"
#include "server-session.h"

shard_plugin_con_t *shard_plugin_con_new() {
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
            server_session_t *pmd = g_ptr_array_index(con->servers, i);
            pmd->sql = NULL;
            g_free(pmd);
        }
        con->server = NULL;
    } else {
        if (con->server) {
            st->backend->connected_clients--;
        }
    }
    g_free(st);
}

