#ifndef _CHASSIS_EVENT_H_
#define _CHASSIS_EVENT_H_

#include <glib.h>    /* GPtrArray */

#include "chassis-exports.h"
#include "chassis-mainloop.h"

#define CHECK_PENDING_EVENT(ev) \
    if (event_pending((ev), EV_READ|EV_WRITE|EV_TIMEOUT, NULL)) {       \
        g_warning(G_STRLOC ": pending ev:%p, flags:%d", (ev), (ev)->ev_flags); \
        event_del(ev);  \
    }

CHASSIS_API void chassis_event_add(chassis *chas, struct event *ev);
CHASSIS_API void chassis_event_add_with_timeout(chassis *chas,
        struct event *ev, struct timeval *tv);

typedef struct event_base chassis_event_loop_t;

CHASSIS_API chassis_event_loop_t *chassis_event_loop_new();
CHASSIS_API void chassis_event_loop_free(chassis_event_loop_t *e);
CHASSIS_API void chassis_event_set_event_base(chassis_event_loop_t *e, struct event_base *event_base);
CHASSIS_API void *chassis_event_loop(chassis_event_loop_t *);

#endif
