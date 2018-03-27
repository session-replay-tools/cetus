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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <glib.h>
#include <gmodule.h>

#ifdef HAVE_SIGNAL_H
#include <signal.h>
#endif

#include "chassis-plugin.h"
#include "chassis-options.h"

chassis_plugin *
chassis_plugin_new(void)
{
    chassis_plugin *p;

    p = g_new0(chassis_plugin, 1);

    return p;
}

void
chassis_plugin_free(chassis_plugin *p)
{
    if (p->option_grp_name)
        g_free(p->option_grp_name);
    if (p->name)
        g_free(p->name);
    if (p->version)
        g_free(p->version);

    if (p->stats && p->free_stats)
        p->free_stats(p->stats);

    /* closing the plugin has to be the last call to make sure
     * we don't free/call/... stuff that is already unmapped
     * from memory
     */
#if (!VALGRIND_SUPPORT)
    if (p->module)
        g_module_close(p->module);
#endif

    g_free(p);
}

chassis_plugin *
chassis_plugin_load(const gchar *name)
{
    int (*plugin_init) (chassis_plugin *p);
    chassis_plugin *p = chassis_plugin_new();

    p->module = g_module_open(name, G_MODULE_BIND_LOCAL);

    if (!p->module) {
        g_critical("loading module '%s' failed: %s", name, g_module_error());

        chassis_plugin_free(p);

        return NULL;
    }

    /* each module has to have a plugin_init function */
    if (!g_module_symbol(p->module, "plugin_init", (gpointer) & plugin_init)) {
        g_critical("module '%s' doesn't have a init-function: %s", name, g_module_error());
        chassis_plugin_free(p);
        return NULL;
    }

    if (0 != plugin_init(p)) {
        g_critical("init-function for module '%s' failed", name);
        chassis_plugin_free(p);
        return NULL;
    }

    if (p->magic != CHASSIS_PLUGIN_MAGIC) {
        g_critical("'%s' doesn't match the current interface (plugin is %lx, chassis is %lx)",
                   name, p->magic, CHASSIS_PLUGIN_MAGIC);
        chassis_plugin_free(p);
        return NULL;
    }

    if (p->init) {
        p->config = p->init();
    }

    /* if the plugins haven't set p->name provide our own name */
    if (!p->name)
        p->name = g_strdup(name);
    /* set dummy version number if the plugin doesn't provide a real one */
    if (!p->version) {
        g_critical("plugin '%s' doesn't set a version num, refusing to load this plugin", name);
        chassis_plugin_free(p);
        return NULL;
    }

    if (p->new_stats) {
        p->stats = p->new_stats();
    }

    return p;
}

GList *
chassis_plugin_get_options(chassis_plugin *p)
{
    GList *options = NULL;

    if (!p->get_options)
        return NULL;

    if (NULL == (options = p->get_options(p->config))) {
        g_critical("adding config options for module '%s' failed", p->name);
    }
    return options;
}
