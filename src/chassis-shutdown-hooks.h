#ifndef _CHASSIS_SHUTDOWN_HOOKS_H_
#define _CHASSIS_SHUTDOWN_HOOKS_H_

#include <glib.h>    /* GPtrArray */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "chassis-exports.h"

typedef struct {
    void (*func)(gpointer _udata);
    gpointer udata;
    gboolean is_called;
} chassis_shutdown_hook_t;

CHASSIS_API void chassis_shutdown_hook_free(chassis_shutdown_hook_t *);

typedef struct {
    GHashTable *hooks;
} chassis_shutdown_hooks_t;

CHASSIS_API chassis_shutdown_hooks_t *chassis_shutdown_hooks_new(void);
CHASSIS_API void chassis_shutdown_hooks_free(chassis_shutdown_hooks_t *);
CHASSIS_API void chassis_shutdown_hooks_call(chassis_shutdown_hooks_t *hooks);

#endif
