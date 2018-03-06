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
    int trx_read_write; /* default TF_READ_WRITE */
    int trx_isolation_level; /* default TF_REPEATABLE_READ */

} shard_plugin_con_t;

NETWORK_API shard_plugin_con_t *shard_plugin_con_new();
NETWORK_API void shard_plugin_con_free(network_mysqld_con *con, shard_plugin_con_t *st);

#endif /*_SHARD_PLUGIN_CON_*/
