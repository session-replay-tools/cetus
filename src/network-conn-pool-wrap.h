#ifndef __NETWORK_CONN_POOL_LUA_H__
#define __NETWORK_CONN_POOL_LUA_H__

#include "network-socket.h"
#include "network-mysqld.h"

#include "network-exports.h"

NETWORK_API int network_pool_add_conn(network_mysqld_con *con, int is_swap);
NETWORK_API int network_pool_add_idle_conn(network_connection_pool *pool, chassis *srv, network_socket *server);
NETWORK_API network_socket *network_connection_pool_swap(network_mysqld_con *con, int backend_ndx);

#endif
