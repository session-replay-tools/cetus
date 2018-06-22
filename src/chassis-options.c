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

#include <stdlib.h>
#include <string.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <errno.h>

#include <glib.h>

#include "chassis-options.h"

/**
 * create a command-line option
 */
chassis_option_t *
chassis_option_new()
{
    chassis_option_t *opt;

    opt = g_slice_new0(chassis_option_t);

    return opt;
}

/**
 * free the option
 */
void
chassis_option_free(chassis_option_t *opt)
{
    if (!opt)
        return;

    g_slice_free(chassis_option_t, opt);
}

/**
 * add a option
 *
 * GOptionEntry
 */
int
chassis_option_set(chassis_option_t *opt,
                   const char *long_name,
                   gchar short_name,
                   gint flags,
                   enum option_type arg,
                   gpointer arg_data,
                   const char *description,
                   const char *arg_desc,
                   chas_opt_assign_hook assign_hook, chas_opt_show_hook show_hook, gint opt_property)
{
    opt->long_name = long_name;
    opt->short_name = short_name;
    opt->flags = flags;
    opt->arg = arg;
    opt->arg_data = arg_data;
    opt->description = description;
    opt->arg_description = arg_desc;
    opt->assign_hook = assign_hook;
    opt->show_hook = show_hook;
    opt->opt_property = opt_property;
    return 0;
}


/**
 * create a command-line option
 */
chassis_options_t *
chassis_options_new()
{
    chassis_options_t *opt;

    opt = g_slice_new0(chassis_options_t);

    return opt;
}

/**
 * add a option
 *
 * GOptionEntry
 */
int
chassis_options_add_option(chassis_options_t *opts, chassis_option_t *opt)
{

    opts->options = g_list_append(opts->options, opt);

    return 0;
}

void
chassis_options_add_options(chassis_options_t *opts, GList *list)
{
    opts->options = g_list_concat(opts->options, list);
}

int
chassis_options_add(chassis_options_t *opts,
                    const char *long_name,
                    gchar short_name,
                    int flags,
                    enum option_type arg,
                    gpointer arg_data,
                    const char *description,
                    const char *arg_desc,
                    chas_opt_assign_hook assign_hook, chas_opt_show_hook show_hook, gint opt_property)
{
    chassis_option_t *opt = chassis_option_new();
    if (0 != chassis_option_set(opt,
                                long_name,
                                short_name,
                                flags,
                                arg,
                                arg_data,
                                description,
                                arg_desc, assign_hook, show_hook, opt_property) || 0 != chassis_options_add_option(opts, opt)) {
        chassis_option_free(opt);
        return -1;
    } else {
        return 0;
    }
}

#define NO_ARG(entry) ((entry)->arg == OPTION_ARG_NONE)

#define OPTIONAL_ARG(entry) FALSE

struct opt_change {
    enum option_type arg_type;
    gpointer arg_data;
    union {
        gboolean bool;
        gint integer;
        gchar *str;
        gchar **array;
        gdouble dbl;
        gint64 int64;
    } prev;
    union {
        gchar *str;
        struct {
            gint len;
            gchar **data;
        } array;
    } allocated;
};

struct pending_null {
    gchar **ptr;
    gchar *value;
};

static gboolean
context_has_h_entry(chassis_options_t *context)
{
    GList *l;
    for (l = context->options; l; l = l->next) {
        chassis_option_t *entry = l->data;
        if (entry->short_name == 'h')
            return TRUE;
    }
    return FALSE;
}

static void
print_help(chassis_options_t *context)
{
    int max_length = 50;
    g_print("%s\n  %s", "Usage:", g_get_prgname());
    if (context->options)
        g_print(" %s", "[OPTION...]");

    g_print("\n\nHelp Options:\n");
    char token = context_has_h_entry(context) ? '?' : 'h';
    g_print("  -%c, --%-*s %s\n", token, max_length - 12, "help", "Show help options");

    g_print("\n\nApplication Options:\n");
    GList *l;
    for (l = context->options; l; l = l->next) {
        chassis_option_t *entry = l->data;
        int len = 0;
        if (entry->short_name) {
            g_print("  -%c, --%s", entry->short_name, entry->long_name);
            len = 8 + strlen(entry->long_name);
        } else {
            g_print("  --%s", entry->long_name);
            len = 4 + strlen(entry->long_name);
        }
        if (entry->arg_description) {
            g_print("=%s", entry->arg_description);
            len += 1 + strlen(entry->arg_description);
        }
        g_print("%*s %s\n", max_length - len, "", entry->description ? entry->description : "");
    }
    exit(0);
}

static gboolean
parse_int(const gchar *arg_name, const gchar *arg, gint *result, GError **error)
{
    gchar *end;
    glong tmp;

    errno = 0;
    tmp = strtol(arg, &end, 0);

    if (*arg == '\0' || *end != '\0') {
        g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                    "Cannot parse integer value '%s' for %s", arg, arg_name);
        return FALSE;
    }

    *result = tmp;
    if (*result != tmp || errno == ERANGE) {
        g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                    "Integer value '%s' for %s out of range", arg, arg_name);
        return FALSE;
    }
    return TRUE;
}

static gboolean
parse_double(const gchar *arg_name, const gchar *arg, gdouble *result, GError **error)
{
    gchar *end;
    gdouble tmp;

    errno = 0;
    tmp = g_strtod(arg, &end);

    if (*arg == '\0' || *end != '\0') {
        g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                    "Cannot parse double value '%s' for %s", arg, arg_name);
        return FALSE;
    }
    if (errno == ERANGE) {
        g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                    "Double value '%s' for %s out of range", arg, arg_name);
        return FALSE;
    }
    *result = tmp;
    return TRUE;
}

static gboolean
parse_int64(const gchar *arg_name, const gchar *arg, gint64 *result, GError **error)
{
    gchar *end;
    gint64 tmp;

    errno = 0;
    tmp = g_ascii_strtoll(arg, &end, 0);

    if (*arg == '\0' || *end != '\0') {
        g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                    "Cannot parse integer value '%s' for %s", arg, arg_name);
        return FALSE;
    }
    if (errno == ERANGE) {
        g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                    "Integer value '%s' for %s out of range", arg, arg_name);
        return FALSE;
    }
    *result = tmp;
    return TRUE;
}

static struct opt_change *
get_change(chassis_options_t *context, enum option_type arg_type, gpointer arg_data)
{
    GList *list;
    struct opt_change *change = NULL;

    for (list = context->changes; list != NULL; list = list->next) {
        change = list->data;

        if (change->arg_data == arg_data)
            goto found;
    }

    change = g_new0(struct opt_change, 1);
    change->arg_type = arg_type;
    change->arg_data = arg_data;

    context->changes = g_list_prepend(context->changes, change);

  found:
    return change;
}

static void
add_pending_null(chassis_options_t *context, gchar **ptr, gchar *value)
{
    struct pending_null *n;

    n = g_new0(struct pending_null, 1);
    n->ptr = ptr;
    n->value = value;

    context->pending_nulls = g_list_prepend(context->pending_nulls, n);
}

static gboolean
parse_arg(chassis_options_t *context,
          chassis_option_t *entry, const gchar *value, const gchar *option_name, GError **error)
{
    struct opt_change *change;

    g_assert(value || OPTIONAL_ARG(entry) || NO_ARG(entry));

    switch (entry->arg) {
    case OPTION_ARG_NONE:{
        (void)get_change(context, OPTION_ARG_NONE, entry->arg_data);

        *(gboolean *)entry->arg_data = !(entry->flags & OPTION_FLAG_REVERSE);
        break;
    }
    case OPTION_ARG_STRING:{
        gchar *data = g_locale_to_utf8(value, -1, NULL, NULL, error);
        if (!data)
            return FALSE;

        change = get_change(context, OPTION_ARG_STRING, entry->arg_data);
        g_free(change->allocated.str);

        change->prev.str = *(gchar **)entry->arg_data;
        change->allocated.str = data;

        *(gchar **)entry->arg_data = data;
        break;
    }
    case OPTION_ARG_STRING_ARRAY:{
        gchar *data = g_locale_to_utf8(value, -1, NULL, NULL, error);
        if (!data)
            return FALSE;

        change = get_change(context, OPTION_ARG_STRING_ARRAY, entry->arg_data);

        if (change->allocated.array.len == 0) {
            change->prev.array = *(gchar ***)entry->arg_data;
            change->allocated.array.data = g_new(gchar *, 2);
        } else {
            change->allocated.array.data =
                g_renew(gchar *, change->allocated.array.data, change->allocated.array.len + 2);
        }
        change->allocated.array.data[change->allocated.array.len] = data;
        change->allocated.array.data[change->allocated.array.len + 1] = NULL;

        change->allocated.array.len++;

        *(gchar ***)entry->arg_data = change->allocated.array.data;

        break;
    }
    case OPTION_ARG_INT:{
        gint data;
        if (!parse_int(option_name, value, &data, error))
            return FALSE;

        change = get_change(context, OPTION_ARG_INT, entry->arg_data);
        change->prev.integer = *(gint *)entry->arg_data;
        *(gint *)entry->arg_data = data;
        break;
    }
    case OPTION_ARG_DOUBLE:{
        gdouble data;
        if (!parse_double(option_name, value, &data, error)) {
            return FALSE;
        }
        change = get_change(context, OPTION_ARG_DOUBLE, entry->arg_data);
        change->prev.dbl = *(gdouble *)entry->arg_data;
        *(gdouble *)entry->arg_data = data;
        break;
    }
    case OPTION_ARG_INT64:{
        gint64 data;
        if (!parse_int64(option_name, value, &data, error)) {
            return FALSE;
        }
        change = get_change(context, OPTION_ARG_INT64, entry->arg_data);
        change->prev.int64 = *(gint64 *)entry->arg_data;
        *(gint64 *)entry->arg_data = data;
        break;
    }
    default:
        g_assert_not_reached();
    }
    entry->flags |= OPTION_FLAG_CMDLINE;
    return TRUE;
}

static gboolean
parse_short_option(chassis_options_t *context,
                   gint idx, gint *new_idx, gchar arg, gint *argc, gchar ***argv, GError **error, gboolean *parsed)
{
    GList *l;
    for (l = context->options; l; l = l->next) {
        chassis_option_t *entry = l->data;

        if (arg == entry->short_name) {
            gchar *option_name = g_strdup_printf("-%c", entry->short_name);
            gchar *value = NULL;
            if (NO_ARG(entry)) {
                value = NULL;
            } else {
                if (*new_idx > idx) {
                    g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED, "Error parsing option %s", option_name);
                    g_free(option_name);
                    return FALSE;
                }

                if (idx < *argc - 1) {
                    if (!OPTIONAL_ARG(entry)) {
                        value = (*argv)[idx + 1];
                        add_pending_null(context, &((*argv)[idx + 1]), NULL);
                        *new_idx = idx + 1;
                    } else {
                        if ((*argv)[idx + 1][0] == '-') {
                            value = NULL;
                        } else {
                            value = (*argv)[idx + 1];
                            add_pending_null(context, &((*argv)[idx + 1]), NULL);
                            *new_idx = idx + 1;
                        }
                    }
                } else if (idx >= *argc - 1 && OPTIONAL_ARG(entry)) {
                    value = NULL;
                } else {
                    g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                                "Missing argument for %s", option_name);
                    g_free(option_name);
                    return FALSE;
                }
            }

            if (!parse_arg(context, entry, value, option_name, error)) {
                g_free(option_name);
                return FALSE;
            }
            g_free(option_name);
            *parsed = TRUE;
        }
    }
    return TRUE;
}

static gboolean
parse_long_option(chassis_options_t *context,
                  gint *idx, gchar *arg, gboolean aliased, gint *argc, gchar ***argv, GError **error, gboolean *parsed)
{
    GList *l;
    for (l = context->options; l; l = l->next) {
        chassis_option_t *entry = l->data;
        if (*idx >= *argc)
            return TRUE;

        if (NO_ARG(entry) && strcmp(arg, entry->long_name) == 0) {
            gchar *option_name = g_strconcat("--", entry->long_name, NULL);
            gboolean retval = parse_arg(context, entry, NULL, option_name, error);
            g_free(option_name);

            add_pending_null(context, &((*argv)[*idx]), NULL);
            *parsed = TRUE;

            return retval;
        } else {
            gint len = strlen(entry->long_name);

            if (strncmp(arg, entry->long_name, len) == 0 && (arg[len] == '=' || arg[len] == 0)) {
                gchar *value = NULL;
                gchar *option_name;

                add_pending_null(context, &((*argv)[*idx]), NULL);
                option_name = g_strconcat("--", entry->long_name, NULL);

                if (arg[len] == '=') {
                    value = arg + len + 1;
                } else if (*idx < *argc - 1) {
                    if (!OPTIONAL_ARG(entry)) {
                        value = (*argv)[*idx + 1];
                        add_pending_null(context, &((*argv)[*idx + 1]), NULL);
                        (*idx)++;
                    } else {
                        if ((*argv)[*idx + 1][0] == '-') {
                            gboolean retval = parse_arg(context, entry,
                                                        NULL, option_name, error);
                            *parsed = TRUE;
                            g_free(option_name);
                            return retval;
                        } else {
                            value = (*argv)[*idx + 1];
                            add_pending_null(context, &((*argv)[*idx + 1]), NULL);
                            (*idx)++;
                        }
                    }
                } else if (*idx >= *argc - 1 && OPTIONAL_ARG(entry)) {
                    gboolean retval = parse_arg(context, entry, NULL, option_name, error);
                    *parsed = TRUE;
                    g_free(option_name);
                    return retval;
                } else {
                    g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                                "Missing argument for %s", option_name);
                    g_free(option_name);
                    return FALSE;
                }
                if (!parse_arg(context, entry, value, option_name, error)) {
                    g_free(option_name);
                    return FALSE;
                }
                g_free(option_name);
                *parsed = TRUE;
            }
        }
    }
    return TRUE;
}

static gboolean
parse_remaining_arg(chassis_options_t *context, gint *idx, gint *argc, gchar ***argv, GError **error, gboolean *parsed)
{
    GList *l;
    for (l = context->options; l; l = l->next) {
        chassis_option_t *entry = l->data;
        if (*idx >= *argc)
            return TRUE;

        if (entry->long_name[0])
            continue;

        g_return_val_if_fail(entry->arg == OPTION_ARG_STRING_ARRAY, FALSE);

        add_pending_null(context, &((*argv)[*idx]), NULL);

        if (!parse_arg(context, entry, (*argv)[*idx], "", error))
            return FALSE;

        *parsed = TRUE;
        return TRUE;
    }
    return TRUE;
}

static void
free_changes_list(chassis_options_t *context, gboolean revert)
{
    GList *list;
    for (list = context->changes; list != NULL; list = list->next) {
        struct opt_change *change = list->data;

        if (revert) {
            switch (change->arg_type) {
            case OPTION_ARG_NONE:
                *(gboolean *)change->arg_data = change->prev.bool;
                break;
            case OPTION_ARG_INT:
                *(gint *)change->arg_data = change->prev.integer;
                break;
            case OPTION_ARG_STRING:
                g_free(change->allocated.str);
                *(gchar **)change->arg_data = change->prev.str;
                break;
            case OPTION_ARG_STRING_ARRAY:
                g_strfreev(change->allocated.array.data);
                *(gchar ***)change->arg_data = change->prev.array;
                break;
            case OPTION_ARG_DOUBLE:
                *(gdouble *)change->arg_data = change->prev.dbl;
                break;
            case OPTION_ARG_INT64:
                *(gint64 *)change->arg_data = change->prev.int64;
                break;
            default:
                g_assert_not_reached();
            }
        }
        g_free(change);
    }
    g_list_free(context->changes);
    context->changes = NULL;
}

static void
free_pending_nulls(chassis_options_t *context, gboolean perform_nulls)
{
    GList *list;
    for (list = context->pending_nulls; list != NULL; list = list->next) {
        struct pending_null *n = list->data;

        if (perform_nulls) {
            if (n->value) {
                /* Copy back the short options */
                *(n->ptr)[0] = '-';
                strcpy(*n->ptr + 1, n->value);
            } else {
                *n->ptr = NULL;
            }
        }
        g_free(n->value);
        g_free(n);
    }
    g_list_free(context->pending_nulls);
    context->pending_nulls = NULL;
}

/**
 * free the option context
 */
void
chassis_options_free(chassis_options_t *context)
{
    if (!context)
        return;

    g_list_free_full(context->options, (GDestroyNotify) chassis_option_free);

    free_changes_list(context, FALSE);
    free_pending_nulls(context, FALSE);

    g_slice_free(chassis_options_t, context);
}

gboolean
chassis_options_parse_cmdline(chassis_options_t *context, int *argc, char ***argv, GError **error)
{
    if (!argc || !argv)
        return FALSE;
    /* Set program name */
    if (!g_get_prgname()) {
        if (*argc) {
            gchar *prgname = g_path_get_basename((*argv)[0]);
            g_set_prgname(prgname ? prgname : "<unknown>");
            g_free(prgname);
        }
    }

    gboolean stop_parsing = FALSE;
    gboolean has_unknown = FALSE;
    gint separator_pos = 0;
    int i, j, k;
    for (i = 1; i < *argc; i++) {
        gchar *arg;
        gboolean parsed = FALSE;

        if ((*argv)[i][0] == '-' && (*argv)[i][1] != '\0' && !stop_parsing) {
            if ((*argv)[i][1] == '-') {
                /* -- option */

                arg = (*argv)[i] + 2;

                /* '--' terminates list of arguments */
                if (*arg == 0) {
                    separator_pos = i;
                    stop_parsing = TRUE;
                    continue;
                }

                /* Handle help options */
                if (context->help_enabled && strcmp(arg, "help") == 0)
                    print_help(context);

                if (!parse_long_option(context, &i, arg, FALSE, argc, argv, error, &parsed))
                    goto fail;

                if (parsed)
                    continue;

                if (context->ignore_unknown)
                    continue;
            } else {            /* short option */
                gint new_i = i, arg_length;
                gboolean *nulled_out = NULL;
                gboolean has_h_entry = context_has_h_entry(context);
                arg = (*argv)[i] + 1;
                arg_length = strlen(arg);
                nulled_out = g_newa(gboolean, arg_length);
                memset(nulled_out, 0, arg_length * sizeof(gboolean));

                for (j = 0; j < arg_length; j++) {
                    if (context->help_enabled && (arg[j] == '?' || (arg[j] == 'h' && !has_h_entry)))
                        print_help(context);
                    parsed = FALSE;
                    if (!parse_short_option(context, i, &new_i, arg[j], argc, argv, error, &parsed))
                        goto fail;

                    if (context->ignore_unknown && parsed)
                        nulled_out[j] = TRUE;
                    else if (context->ignore_unknown)
                        continue;
                    else if (!parsed)
                        break;
                    /* !context->ignore_unknown && parsed */
                }
                if (context->ignore_unknown) {
                    gchar *new_arg = NULL;
                    gint arg_index = 0;
                    for (j = 0; j < arg_length; j++) {
                        if (!nulled_out[j]) {
                            if (!new_arg)
                                new_arg = g_malloc(arg_length + 1);
                            new_arg[arg_index++] = arg[j];
                        }
                    }
                    if (new_arg)
                        new_arg[arg_index] = '\0';
                    add_pending_null(context, &((*argv)[i]), new_arg);
                    i = new_i;
                } else if (parsed) {
                    add_pending_null(context, &((*argv)[i]), NULL);
                    i = new_i;
                }
            }

            if (!parsed)
                has_unknown = TRUE;

            if (!parsed && !context->ignore_unknown) {
                g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_UNKNOWN_OPTION, "Unknown option %s", (*argv)[i]);
                goto fail;
            }
        } else {
            //if (context->strict_posix)
            stop_parsing = TRUE;

            /* Collect remaining args */
            if (!parse_remaining_arg(context, &i, argc, argv, error, &parsed))
                goto fail;
            if (!parsed && (has_unknown || (*argv)[i][0] == '-'))
                separator_pos = 0;
        }
    }

    if (separator_pos > 0)
        add_pending_null(context, &((*argv)[separator_pos]), NULL);
    if (argc && argv) {
        free_pending_nulls(context, TRUE);
        for (i = 1; i < *argc; i++) {
            for (k = i; k < *argc; k++)
                if ((*argv)[k] != NULL)
                    break;

            if (k > i) {
                k -= i;
                for (j = i + k; j < *argc; j++) {
                    (*argv)[j - k] = (*argv)[j];
                    (*argv)[j] = NULL;
                }
                *argc -= k;
            }
        }
    }
    return TRUE;

  fail:
    free_changes_list(context, TRUE);
    free_pending_nulls(context, FALSE);
    return FALSE;
}

chassis_option_t *chassis_options_get(GList *opts, const char *long_name)
{
    GList *l = opts;
    for (l = opts; l; l = l->next) {
        chassis_option_t *opt = l->data;
        if (strcmp(opt->long_name, long_name)==0) {
            return opt;
        }
    }
    return NULL;
}
