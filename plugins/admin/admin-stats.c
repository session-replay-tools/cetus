#include "glib-ext.h"
#include "admin-stats.h"
#include "admin-plugin.h"
#include "chassis-event.h"

/* ring buffer from: https://github.com/AndersKaloer/Ring-Buffer */
#define RING_BUFFER_SIZE 128 /* must be power of 2, !! index [0, 126] !!*/
#define RING_BUFFER_MASK (RING_BUFFER_SIZE-1)
typedef struct ring_buffer_t {
    int head;
    int tail;
    guint64 buffer[RING_BUFFER_SIZE];
} ring_buffer_t;

static void ring_buffer_add(ring_buffer_t *buffer, guint64 data) {
  if (((buffer->head - buffer->tail) & RING_BUFFER_MASK) == RING_BUFFER_MASK)
      buffer->tail = ((buffer->tail + 1) & RING_BUFFER_MASK);
  buffer->buffer[buffer->head] = data;
  buffer->head = ((buffer->head + 1) & RING_BUFFER_MASK);
}

static guint64 ring_buffer_get(ring_buffer_t *buffer, int index) {
  if (index >= ((buffer->head - buffer->tail) & RING_BUFFER_MASK))
      return 0;
  int data_index = ((buffer->tail + index) & RING_BUFFER_MASK);
  return buffer->buffer[data_index];
}


struct admin_stats_t {
    struct event sampling_timer;
    chassis* chas;
    ring_buffer_t sql_count_ring;
    ring_buffer_t trx_count_ring;
};

/* sample interval is 10-sec, 127 samples takes about 21-min */
static void sql_stats_sampling_func(int fd, short what, void *arg)
{
    admin_stats_t* a = arg;
    query_stats_t* stats = &(a->chas->query_stats);
    ring_buffer_add(&a->sql_count_ring,
                    stats->client_query.ro + stats->client_query.rw);
    ring_buffer_add(&a->trx_count_ring, stats->xa_count);

    static struct timeval ten_sec = {10, 0};
    /* EV_PERSIST not work for libevent1.4, re-activate timer each time */
    chassis_event_add_with_timeout(a->chas, &a->sampling_timer, &ten_sec);
}

admin_stats_t* admin_stats_init(chassis* chas)
{
    admin_stats_t* stats = g_new0(admin_stats_t, 1);
    stats->chas = chas;
    stats->sql_count_ring.head = 126;
    stats->trx_count_ring.head = 126;

    /* EV_PERSIST not working for libevent 1.4 */
    evtimer_set(&stats->sampling_timer, sql_stats_sampling_func, stats);
    struct timeval ten_sec = {10, 0};
    chassis_event_add_with_timeout(chas, &stats->sampling_timer, &ten_sec);
    return stats;
}

void admin_stats_free(admin_stats_t* stats)
{
    evtimer_del(&stats->sampling_timer);
    g_free(stats);
}

void admin_stats_get_average(admin_stats_t* stats, int type, char* buf, int len)
{
    ring_buffer_t* ring = type==1 ? &stats->sql_count_ring : &stats->trx_count_ring;
    const int MOST_RECENT = 126;
    guint64 c_now = ring_buffer_get(ring, MOST_RECENT);
    guint64 c_1min = ring_buffer_get(ring, MOST_RECENT - 6);
    guint64 c_5min = ring_buffer_get(ring, MOST_RECENT - 6*5);
    guint64 c_15min = ring_buffer_get(ring, MOST_RECENT - 6*15);
    snprintf(buf, len, "%.2f, %.2f, %.2f",
             (c_now-c_1min)/60.0, (c_now-c_5min)/300.0, (c_now-c_15min)/900.0);
}

