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

#include "chassis-path.h"
#include "chassis-keyfile.h"
#include "chassis-options.h"

/**
 * map options from the keyfile to the config-options
 *
 * @returns FALSE on error, TRUE on success
 * @added in 0.8.3
 */
gboolean
chassis_keyfile_to_options_with_error(GKeyFile *keyfile,
                                      const gchar *ini_group_name, GList *config_entries, GError **_gerr)
{
    GError *gerr = NULL;
    gboolean ret = TRUE;
    int j;

    if (NULL == keyfile) {
        g_set_error(_gerr, G_FILE_ERROR, G_FILE_ERROR_INVAL, "keyfile has to be set");
        return FALSE;
    }

    if (!g_key_file_has_group(keyfile, ini_group_name)) {
        /* the group doesn't exist, no config-entries to map */
        g_warning("(keyfile) has no group [%s]", ini_group_name);
        return TRUE;
    }

    GList *l;
    /* set the defaults */
    for (l = config_entries; l; l = l->next) {
        chassis_option_t *entry = l->data;
        gchar *arg_string;
        gchar **arg_string_array;
        gboolean arg_bool = 0;
        gint arg_int = 0;
        gint64 arg_int64 = 0L;
        gdouble arg_double = 0;
        gsize len = 0;

        /* already set by command line */
        if (entry->flags & OPTION_FLAG_CMDLINE)
            continue;

        switch (entry->arg) {
        case OPTION_ARG_STRING:
            /* is this option set already */
            if (entry->arg_data == NULL || *(char **)entry->arg_data != NULL)
                break;

            arg_string = g_key_file_get_string(keyfile, ini_group_name, entry->long_name, &gerr);
            if (!gerr) {
                /* strip trailing spaces */
                *(gchar **)(entry->arg_data) = g_strchomp(arg_string);
            }
            break;
        case OPTION_ARG_STRING_ARRAY:
            /* is this option set already */
            if (entry->arg_data == NULL || *(char **)entry->arg_data != NULL)
                break;

            arg_string_array = g_key_file_get_string_list(keyfile, ini_group_name, entry->long_name, &len, &gerr);
            if (!gerr) {
                for (j = 0; arg_string_array[j]; j++) {
                    arg_string_array[j] = g_strstrip(arg_string_array[j]);
                }
                *(gchar ***)(entry->arg_data) = arg_string_array;
            }
            break;
        case OPTION_ARG_NONE:
            arg_bool = g_key_file_get_boolean(keyfile, ini_group_name, entry->long_name, &gerr);
            if (!gerr) {
                *(gboolean *)(entry->arg_data) = arg_bool;
            }
            break;
        case OPTION_ARG_INT:
            arg_int = g_key_file_get_integer(keyfile, ini_group_name, entry->long_name, &gerr);
            if (!gerr) {
                *(gint *)(entry->arg_data) = arg_int;
            }
            break;
        case OPTION_ARG_INT64:
            arg_int64 = g_key_file_get_int64(keyfile, ini_group_name, entry->long_name, &gerr);           
            if (!gerr) {
                *(gint64 *)(entry->arg_data) = arg_int64;
            }
            break;
        case OPTION_ARG_DOUBLE:
            arg_double = g_key_file_get_double(keyfile, ini_group_name, entry->long_name, &gerr);
            if (!gerr) {
                *(gdouble *)(entry->arg_data) = arg_double;
            }
            break;
        default:
            g_error("%s: (keyfile) the option %d can't be handled", G_STRLOC, entry->arg);
            break;
        }

        if (gerr) {
            if (gerr->code == G_KEY_FILE_ERROR_KEY_NOT_FOUND) {
                /* ignore if this key isn't set in the config-file */
                g_error_free(gerr);
            } else {
                /* otherwise propage the error the higher level */
                g_propagate_error(_gerr, gerr);
                ret = FALSE;
                break;
            }
            gerr = NULL;
        } else {
            entry->flags |= OPTION_FLAG_CONF_FILE;
        }
    }
    return ret;
}
