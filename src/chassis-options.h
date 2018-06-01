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

#ifndef __CHASSIS_OPTIONS_H__
#define __CHASSIS_OPTIONS_H__

#include <glib.h>

struct external_param {
	struct chassis *chas;
	gint opt_type;
};

typedef gint (*chas_opt_assign_hook)(const gchar *newval, gpointer param);
typedef gchar* (*chas_opt_show_hook)(gpointer param);

enum option_type {              // arg_data type
    OPTION_ARG_NONE,            // bool *
    OPTION_ARG_INT,             // int *
    OPTION_ARG_INT64,           // int64 *
    OPTION_ARG_DOUBLE,          // double *
    OPTION_ARG_STRING,          // char **
    OPTION_ARG_STRING_ARRAY,    // char **
};

enum option_flags {
    OPTION_FLAG_REVERSE = 0x04,

    OPTION_FLAG_CMDLINE = 0x10,
    OPTION_FLAG_CONF_FILE = 0x20,
    OPTION_FLAG_REMOTE_CONF = 0x40,
};
/**
 * @file
 *
 * a _new()/_free()-able version of GOptionEntry
 */

/**
 * 'const'-free version of GOptionEntry
 */
typedef struct {
    const char *long_name;
    const char *description;
    const char *arg_description;
    gpointer arg_data;
    enum option_type arg;
    enum option_flags flags;
    gchar short_name;

    chas_opt_assign_hook assign_hook;
    chas_opt_show_hook show_hook;
    gint opt_property;
} chassis_option_t;

/**
 * create a chassis_option_t 
 */
chassis_option_t *chassis_option_new(void);
void chassis_option_free(chassis_option_t *opt);
int chassis_option_set(chassis_option_t *opt,
                       const char *long_name,
                       gchar short_name,
                       gint flags,
                       enum option_type arg,
                       gpointer arg_data,
                       const char *description,
                       const char *arg_desc,
                       chas_opt_assign_hook assign_hook, chas_opt_show_hook show_hook, gint opt_property);

typedef struct chassis_options_t {
    GList *options;             /* List of chassis_option_t */

    /* We keep a list of change so we can revert them */
    GList *changes;

    /* We also keep track of all argv elements
     * that should be NULLed or modified.
     */
    GList *pending_nulls;

    gboolean ignore_unknown;
    gboolean help_enabled;
} chassis_options_t;

chassis_options_t *chassis_options_new(void);
void chassis_options_free(chassis_options_t *opts);
int chassis_options_add_option(chassis_options_t *opts, chassis_option_t *opt);
void chassis_options_add_options(chassis_options_t *opts, GList *list);
int chassis_options_add(chassis_options_t *opts,
                        const char *long_name,
                        gchar short_name,
                        int flags,
                        enum option_type arg,
                        gpointer arg_data,
                        const char *description,
                        const char *arg_desc,
                        chas_opt_assign_hook assign_hook, chas_opt_show_hook show_hook, gint opt_property);

gboolean chassis_options_parse_cmdline(chassis_options_t *context, int *argc, char ***argv, GError **error);
chassis_option_t *chassis_options_get(GList *opts, const char *long_name);
#endif
