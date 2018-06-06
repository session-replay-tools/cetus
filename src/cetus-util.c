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

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>
#include <glib.h>

#include "glib-ext.h"
#include "cetus-util.h"

void
cetus_string_dequote(char *z)
{
    int quote;
    int i, j;
    if (z == 0)
        return;
    quote = z[0];
    switch (quote) {
    case '\'':
        break;
    case '"':
        break;
    case '`':
        break;                  /* For MySQL compatibility */
    default:
        return;
    }
    for (i = 1, j = 0; z[i]; i++) {
        if (z[i] == quote) {
            if (z[i + 1] == quote) {    /* quote escape */
                z[j++] = quote;
                i++;
            } else {
                z[j++] = 0;
                break;
            }
        } else if (z[i] == '\\') {  /* slash escape */
            i++;
            z[j++] = z[i];
        } else {
            z[j++] = z[i];
        }
    }
}

gboolean
read_file_to_buffer(const char *filename, char **buffer)
{
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        g_critical(G_STRLOC ":cannot open user conf: %s", filename);
        return FALSE;
    }
    const int MAX_FILE_SIZE = 1024 * 1024;  /* 1M */
    fseek(fp, 0, SEEK_END);
    int len = ftell(fp);
    if (len < 0) {
        g_warning(G_STRLOC ":%s", g_strerror(errno));
        fclose(fp);
        return FALSE;
    }

    if (len > MAX_FILE_SIZE) {
        g_warning(G_STRLOC ":file too large");
        fclose(fp);
        return FALSE;
    }

    rewind(fp);

    *buffer = g_new(char, len + 1);
    if (fread((*buffer), 1, len, fp) != len) {
        g_warning(G_STRLOC ":len is not consistant");
        fclose(fp);
        return FALSE;
    }

    (*buffer)[len] = 0;

    fclose(fp);

    return TRUE;
}

gboolean
try_get_int_value(const gchar *option_value, gint *return_value)
{
    gint ret = sscanf(option_value, "%d", return_value);
    if(1 == ret) {
        return TRUE;
    } else {
        return FALSE;
    }
}


gboolean
try_get_double_value(const gchar *option_value, gdouble *return_value)
{
    gint ret = sscanf(option_value, "%lf", return_value);
    if(1 == ret) {
        return TRUE;
    } else {
        return FALSE;
    }
}

int make_iso8601_timestamp(char *buf, uint64_t utime)
{
    struct tm  my_tm;
    char       tzinfo[7]="Z";  // max 6 chars plus \0
    size_t     len;
    time_t     seconds;

    seconds= utime / 1000000;
    utime = utime % 1000000;
    {
        localtime_r(&seconds, &my_tm);
        long tim= timezone; // seconds West of UTC.
        char dir= '-';

        if (tim < 0) {
            dir= '+';
            tim= -tim;
        }
        snprintf(tzinfo, sizeof(tzinfo), "%c%02d:%02d",
                    dir, (int) (tim / (60 * 60)), (int) ((tim / 60) % 60));
    }

    len = snprintf(buf, 64, "%04d-%02d-%02dT%02d:%02d:%02d.%06lu%s",
                     my_tm.tm_year + 1900,
                     my_tm.tm_mon  + 1,
                     my_tm.tm_mday,
                     my_tm.tm_hour,
                     my_tm.tm_min,
                     my_tm.tm_sec,
                     (unsigned long) utime,
                     tzinfo);
    return len;
}
