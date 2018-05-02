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

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <errno.h>

#include <glib.h>
#include <gmodule.h>
#include <event.h>

#include "glib-ext.h"
#include "chassis-config.h"
#include "chassis-frontend.h"
#include "chassis-path.h"
#include "chassis-plugin.h"
#include "chassis-keyfile.h"
#include "chassis-filemode.h"
#include "chassis-options.h"
#include "cetus-util.h"
#include "chassis-options-utils.h"

/**
 * initialize the basic components of the chassis
 */
int
chassis_frontend_init_glib()
{
    const gchar *check_str = NULL;
#if 0
    g_mem_set_vtable(glib_mem_profiler_table);
#endif

    if (!GLIB_CHECK_VERSION(2, 6, 0)) {
        g_critical("the glib header is too old, need at least 2.6.0, got: %d.%d.%d",
                   GLIB_MAJOR_VERSION, GLIB_MINOR_VERSION, GLIB_MICRO_VERSION);

        return -1;
    }

    check_str = glib_check_version(GLIB_MAJOR_VERSION, GLIB_MINOR_VERSION, GLIB_MICRO_VERSION);

    if (check_str) {
        g_critical("%s, got: lib=%d.%d.%d, headers=%d.%d.%d",
                   check_str,
                   glib_major_version, glib_minor_version, glib_micro_version,
                   GLIB_MAJOR_VERSION, GLIB_MINOR_VERSION, GLIB_MICRO_VERSION);

        return -1;
    }

    if (!g_module_supported()) {
        g_critical("loading modules is not supported on this platform");
        return -1;
    }
#if !GLIB_CHECK_VERSION(2, 32, 0)
    /* GLIB below 2.32 must call thread_init */
    g_thread_init(NULL);
#endif

    return 0;
}

/**
 * setup and check the basedir if nessesary
 */
int
chassis_frontend_init_basedir(const char *prg_name, char **_base_dir)
{
    char *base_dir = *_base_dir;

    if (base_dir) {             /* basedir is already known, check if it is absolute */
        if (!g_path_is_absolute(base_dir)) {
            g_critical("%s: --basedir option must be an absolute path, but was %s", G_STRLOC, base_dir);
            return -1;
        } else {
            return 0;
        }
    }

    /* find our installation directory if no basedir was given
     * this is necessary for finding files when we daemonize
     */
    base_dir = chassis_get_basedir(prg_name);
    if (!base_dir) {
        g_critical("%s: Failed to get base directory", G_STRLOC);
        return -1;
    }

    *_base_dir = base_dir;

    return 0;

}

int
chassis_frontend_init_plugin_dir(char **_plugin_dir, const char *base_dir)
{
    char *plugin_dir = *_plugin_dir;

    if (plugin_dir)
        return 0;

    plugin_dir = g_build_filename(base_dir, "lib", PACKAGE, "plugins", NULL);

    *_plugin_dir = plugin_dir;

    return 0;
}

int
chassis_frontend_load_plugins(GPtrArray *plugins, const gchar *plugin_dir, gchar **plugin_names)
{
    int i;

    /* load the plugins */
    for (i = 0; plugin_names && plugin_names[i]; i++) {
        chassis_plugin *p;
#define G_MODULE_PREFIX "lib"
        /* we have to hack around some glib distributions that
         * don't set the correct G_MODULE_SUFFIX, notably MacPorts
         */
#ifndef SHARED_LIBRARY_SUFFIX
#define SHARED_LIBRARY_SUFFIX G_MODULE_SUFFIX
#endif
        char *plugin_filename;
        /* skip trying to load a plugin when the parameter was --plugins=
           that will never work...
         */
        if (!g_strcmp0("", plugin_names[i])) {
            continue;
        }

        plugin_filename = g_strdup_printf("%s%c%s%s.%s",
                                          plugin_dir,
                                          G_DIR_SEPARATOR, G_MODULE_PREFIX, plugin_names[i], SHARED_LIBRARY_SUFFIX);

        p = chassis_plugin_load(plugin_filename);
        g_free(plugin_filename);

        if (NULL == p) {
            g_critical("setting --plugin-dir=<dir> might help");
            return -1;
        }
        p->option_grp_name = g_strdup(plugin_names[i]);

        g_ptr_array_add(plugins, p);
    }
    return 0;
}

int
chassis_frontend_init_plugins(GPtrArray *plugins,
                              chassis_options_t *opts, chassis_config_t *config_manager,
                              int *argc_p, char ***argv_p,
                              GKeyFile *keyfile, const char *keyfile_section_name, GError **gerr)
{
    guint i;
    for (i = 0; i < plugins->len; i++) {
        GList *config_entries = NULL;
        chassis_plugin *p = g_ptr_array_index(plugins, i);

        if (NULL != (config_entries = chassis_plugin_get_options(p))) {
            chassis_options_add_options(opts, config_entries);

            if (FALSE == chassis_options_parse_cmdline(opts, argc_p, argv_p, gerr)) {
                return -1;
            }
            /* parse the new options */
            if (keyfile) {
                if (FALSE == chassis_keyfile_to_options_with_error(keyfile, keyfile_section_name, config_entries, gerr)) {
                    return -1;
                }
            }
            /* Load from remote config first */
            if (config_manager) {
                chassis_config_parse_options(config_manager, config_entries);
            }
        }
    }

    return 0;
}

int
chassis_frontend_init_base_options(int *argc_p, char ***argv_p, int *print_version, char **config_file, GError **gerr)
{
    int ret = 0;
    chassis_options_t *opts = chassis_options_new();
    chassis_options_set_cmdline_only_options(opts, print_version, config_file);
    opts->ignore_unknown = TRUE;

    if (FALSE == chassis_options_parse_cmdline(opts, argc_p, argv_p, gerr)) {
        ret = -1;
    }

    chassis_options_free(opts);
    return ret;
}

GKeyFile *
chassis_frontend_open_config_file(const char *filename, GError **gerr)
{
    GKeyFile *keyfile;

    if (chassis_filemode_check_full(filename, CHASSIS_FILEMODE_SECURE_MASK, gerr) != 0) {
        return NULL;
    }
    keyfile = g_key_file_new();
    g_key_file_set_list_separator(keyfile, ',');

    if (FALSE == g_key_file_load_from_file(keyfile, filename, G_KEY_FILE_NONE, gerr)) {
        g_key_file_free(keyfile);

        return NULL;
    }

    return keyfile;
}

/**
 * setup the options that can only appear on the command-line
 */
int
chassis_options_set_cmdline_only_options(chassis_options_t *opts, int *print_version, char **config_file)
{

    chassis_options_add(opts, "version", 'V', 0, OPTION_ARG_NONE, print_version, "Show version", NULL, NULL, NULL, 0);

    chassis_options_add(opts, "defaults-file", 0, 0, OPTION_ARG_STRING, config_file, "configuration file", "<file>", NULL, NULL, 0);

    return 0;
}

int
chassis_frontend_print_version()
{
    /*
     * allow to pass down a build-tag at build-time
     * which gets hard-coded into the binary
     */
    g_print("  %s\n", PACKAGE_STRING);
#ifdef CHASSIS_BUILD_TAG
    g_print("  build: %s\n", CHASSIS_BUILD_TAG);
#endif
    g_print("  glib2: %d.%d.%d\n", GLIB_MAJOR_VERSION, GLIB_MINOR_VERSION, GLIB_MICRO_VERSION);
    g_print("  libevent: %s\n", event_get_version());
    return 0;
}

int
chassis_frontend_print_plugin_versions(GPtrArray *plugins)
{
    guint i;

    g_print("-- modules\n");

    for (i = 0; i < plugins->len; i++) {
        chassis_plugin *p = plugins->pdata[i];

        g_print("  %s: %s\n", p->name, p->version);
    }

    return 0;
}

/**
 * log the versions of the initialized plugins
 */
int
chassis_frontend_log_plugin_versions(GPtrArray *plugins)
{
    guint i;

    for (i = 0; i < plugins->len; i++) {
        chassis_plugin *p = plugins->pdata[i];

        g_message("plugin %s %s started", p->name, p->version);
    }

    return 0;
}

int
chassis_frontend_write_pidfile(const char *pid_file, GError **gerr)
{
    int fd;
    int ret = 0;

    gchar *pid_str;

    /**
     * write the PID file
     */

    if (-1 == (fd = open(pid_file, O_WRONLY | O_TRUNC | O_CREAT, 0600))) {
        g_set_error(gerr,
                    G_FILE_ERROR,
                    g_file_error_from_errno(errno), "%s: open(%s) failed: %s", G_STRLOC, pid_file, g_strerror(errno));

        return -1;
    }

    pid_str = g_strdup_printf("%d", getpid());

    if (write(fd, pid_str, strlen(pid_str)) < 0) {
        g_set_error(gerr,
                    G_FILE_ERROR,
                    g_file_error_from_errno(errno),
                    "%s: write(%s) of %s failed: %s", G_STRLOC, pid_file, pid_str, g_strerror(errno));
        ret = -1;
    }
    g_free(pid_str);

    close(fd);

    return ret;
}
