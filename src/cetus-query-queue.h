#ifndef _CETUS_QUERY_QUEUE_H_
#define _CETUS_QUERY_QUEUE_H_

#include <glib.h>

typedef struct query_queue_t {
    GQueue *chunks;
    int max_len;
} query_queue_t;

query_queue_t *query_queue_new(int max_len);
void query_queue_free(query_queue_t *);
void query_queue_append(query_queue_t *, GString *);
void query_queue_dump(query_queue_t *);

#endif /* _CETUS_QUERY_QUEUE_H_*/
