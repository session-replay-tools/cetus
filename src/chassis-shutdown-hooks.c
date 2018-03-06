#include <stdlib.h>
#include <string.h>

#include "chassis-shutdown-hooks.h"
#include "glib-ext.h"

void chassis_shutdown_hook_free(chassis_shutdown_hook_t *hook) {
    g_slice_free(chassis_shutdown_hook_t, hook);
}

chassis_shutdown_hooks_t *chassis_shutdown_hooks_new() {
    chassis_shutdown_hooks_t *hooks;

    hooks = g_slice_new0(chassis_shutdown_hooks_t);
    hooks->hooks = g_hash_table_new_full(
            (GHashFunc)g_string_hash,
            (GEqualFunc)g_string_equal,
            g_string_true_free,
            (GDestroyNotify)chassis_shutdown_hook_free);

    return hooks;
}

void chassis_shutdown_hooks_free(chassis_shutdown_hooks_t *hooks) {
    g_hash_table_destroy(hooks->hooks);

    g_slice_free(chassis_shutdown_hooks_t, hooks);
}


void chassis_shutdown_hooks_call(chassis_shutdown_hooks_t *hooks) {
    GHashTableIter iter;
    GString *key;
    chassis_shutdown_hook_t *hook;

    g_hash_table_iter_init(&iter, hooks->hooks);
    while (g_hash_table_iter_next(&iter, (void **)&key, (void **)&hook)) {
        if (hook->func && !hook->is_called) hook->func(hook->udata);
        hook->is_called = TRUE;
    }
}

