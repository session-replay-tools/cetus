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

#include <glib.h>

#include <errno.h>
#include <stdlib.h>             /* for realpath */
#include <string.h>

#include "glib-ext.h"
#include "chassis-path.h"

gchar *
chassis_get_basedir(const gchar *prgname)
{
    gchar *absolute_path;
    gchar *bin_dir;
    gchar r_path[PATH_MAX];
    gchar *base_dir;

    if (g_path_is_absolute(prgname)) {
        /* No need to dup, just to get free right */
        absolute_path = g_strdup(prgname);
    } else {
        /**
         * the path wasn't absolute
         *
         * Either it is
         * - in the $PATH 
         * - relative like ./bin/... or
         */

        absolute_path = g_find_program_in_path(prgname);
        if (absolute_path == NULL) {
            g_critical("can't find myself (%s) in PATH", prgname);

            return NULL;
        }

        if (!g_path_is_absolute(absolute_path)) {
            gchar *cwd = g_get_current_dir();

            g_free(absolute_path);

            absolute_path = g_build_filename(cwd, prgname, NULL);

            g_free(cwd);
        }
    }

    /* assume that the binary is in ./s?bin/ and 
     * that the the basedir is right above it
     *
     * to get this working we need a "clean" basedir, no .../foo/./bin/ 
     */
    if (NULL == realpath(absolute_path, r_path)) {
        g_critical("%s: realpath(%s) failed: %s", G_STRLOC, absolute_path, g_strerror(errno));

        return NULL;
    }
    bin_dir = g_path_get_dirname(r_path);
    base_dir = g_path_get_dirname(bin_dir);

    /* don't free base_dir, because we need it later */
    g_free(absolute_path);
    g_free(bin_dir);

    return base_dir;
}

gchar *
chassis_resolve_path(const char *base_dir, gchar *filename)
{

    if (!base_dir || !filename)
        return NULL;

    if (g_path_is_absolute(filename))
        return filename;

    return g_build_filename(base_dir, G_DIR_SEPARATOR_S, filename, NULL);
}
