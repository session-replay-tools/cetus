/* $%BEGINLICENSE%$
 Copyright (c) 2007, 2012, Oracle and/or its affiliates. All rights reserved.

 This program is free software; you can redistribute it and/or
 modify it under the terms of the GNU General Public License as
 published by the Free Software Foundation; version 2 of the
 License.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 02110-1301  USA

 $%ENDLICENSE%$ */

#include <stdlib.h>
#include <string.h>

#include "chassis-shutdown-hooks.h"
#include "glib-ext.h"

void
chassis_shutdown_hook_free(chassis_shutdown_hook_t *hook)
{
    g_slice_free(chassis_shutdown_hook_t, hook);
}

chassis_shutdown_hooks_t *
chassis_shutdown_hooks_new()
{
    chassis_shutdown_hooks_t *hooks;

    hooks = g_slice_new0(chassis_shutdown_hooks_t);
    hooks->hooks = g_hash_table_new_full((GHashFunc) g_string_hash,
                                         (GEqualFunc) g_string_equal,
                                         g_string_true_free, (GDestroyNotify) chassis_shutdown_hook_free);

    return hooks;
}

void
chassis_shutdown_hooks_free(chassis_shutdown_hooks_t *hooks)
{
    g_hash_table_destroy(hooks->hooks);

    g_slice_free(chassis_shutdown_hooks_t, hooks);
}

void
chassis_shutdown_hooks_call(chassis_shutdown_hooks_t *hooks)
{
    GHashTableIter iter;
    GString *key;
    chassis_shutdown_hook_t *hook;

    g_hash_table_iter_init(&iter, hooks->hooks);
    while (g_hash_table_iter_next(&iter, (void **)&key, (void **)&hook)) {
        if (hook->func && !hook->is_called)
            hook->func(hook->udata);
        hook->is_called = TRUE;
    }
}
