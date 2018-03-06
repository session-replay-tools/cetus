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

NETWORK_API network_socket_retval_t do_read_auth(network_mysqld_con *, GHashTable *, GHashTable *);
NETWORK_API network_socket_retval_t do_connect_cetus(network_mysqld_con *, network_backend_t **, int *);
NETWORK_API network_socket_retval_t plugin_add_backends(chassis *, gchar **, gchar **);
NETWORK_API int do_check_qeury_cache(network_mysqld_con *con);
NETWORK_API int try_to_get_resp_from_query_cache(network_mysqld_con *con);
NETWORK_API gboolean proxy_put_shard_conn_to_pool(network_mysqld_con *con);
NETWORK_API void remove_mul_server_recv_packets(network_mysqld_con *con);

#endif
