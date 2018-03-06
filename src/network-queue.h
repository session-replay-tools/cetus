#ifndef _NETWORK_QUEUE_H_
#define _NETWORK_QUEUE_H_

#include "network-exports.h"

#include <glib.h>

/* a input or output stream */
typedef struct {
    GQueue *chunks;

    size_t len;    /* len in all chunks (w/o the offset) */
    size_t offset; /* offset in the first chunk */
} network_queue;

NETWORK_API network_queue *network_queue_new(void);
NETWORK_API void network_queue_free(network_queue *queue);
void network_queue_clear(network_queue *queue);
NETWORK_API int network_queue_append(network_queue *queue, GString *chunk);
NETWORK_API GString *network_queue_pop_str(network_queue *queue, gsize steal_len, GString *dest);
NETWORK_API GString *network_queue_peek_str(network_queue *queue, gsize peek_len, GString *dest);

#endif
