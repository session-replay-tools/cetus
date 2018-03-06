#ifndef _NETWORK_CONN_POOL_H_
#define _NETWORK_CONN_POOL_H_

#include <glib.h>

#include "network-socket.h"
#include "network-exports.h"

typedef struct {
    /** GHashTable<GString, GQueue<network_connection_pool_entry>> */
    GHashTable *users; 
    void       *srv;

    int   cur_idle_connections;

    guint max_idle_connections;
    guint mid_idle_connections;
    guint min_idle_connections;

} network_connection_pool;

typedef struct {
    network_socket *sock;          /** the idling socket */
    network_connection_pool *pool; /** a pointer back to the pool */
} network_connection_pool_entry;

NETWORK_API network_socket *network_connection_pool_get(network_connection_pool *pool,
        GString *username, int *is_robbed);

NETWORK_API network_connection_pool_entry *
network_connection_pool_add(network_connection_pool *, network_socket *);

NETWORK_API void network_connection_pool_remove(network_connection_pool *pool, 
        network_connection_pool_entry *entry);
NETWORK_API GQueue *network_connection_pool_get_conns(network_connection_pool *, 
        GString *, int *);

NETWORK_API network_connection_pool *network_connection_pool_new(void);
NETWORK_API void network_connection_pool_free(network_connection_pool *pool);
NETWORK_API int network_connection_pool_total_conns_count(network_connection_pool *pool);

NETWORK_API gboolean network_conn_pool_do_reduce_conns_verdict(network_connection_pool *, int);
#endif
