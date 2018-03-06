#ifndef _CHASSIS_REMOTE_CONFIGS_H_
#define _CHASSIS_REMOTE_CONFIGS_H_

#include <glib.h>    /* GPtrArray */
#include "chassis-mainloop.h"

typedef struct cetus_monitor_t cetus_monitor_t;

typedef enum {
    MONITOR_TYPE_CHECK_ALIVE,
    MONITOR_TYPE_CHECK_DELAY,
    MONITOR_TYPE_CHECK_CONFIG
} monitor_type_t;

typedef void (*monitor_callback_fn)(int, short, void *);

cetus_monitor_t *cetus_monitor_new();

void cetus_monitor_free(cetus_monitor_t *);

void cetus_monitor_open(cetus_monitor_t *, monitor_type_t);

void cetus_monitor_close(cetus_monitor_t *, monitor_type_t);

void cetus_monitor_open(cetus_monitor_t *, monitor_type_t);

void cetus_monitor_start_thread(cetus_monitor_t *, chassis *data);

void cetus_monitor_stop_thread(cetus_monitor_t *);

void cetus_monitor_register_object(cetus_monitor_t *,
                                    const char *, monitor_callback_fn, void *);

#endif
