#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>
#include <glib.h>

#include "glib-ext.h"
#include "cetus-util.h"

void cetus_string_dequote(char *z)
{
    int quote;
    int i, j;
    if (z == 0) return;
    quote = z[0];
    switch (quote) {
    case '\'':  break;
    case '"':   break;
    case '`':   break;                /* For MySQL compatibility */
    default:    return;
    }
    for (i = 1, j = 0; z[i]; i++) {
        if (z[i] == quote) {
            if (z[i+1] == quote) { /* quote escape */
                z[j++] = quote;
                i++;
            } else {
                z[j++] = 0;
                break;
            }
        } else if (z[i] == '\\') { /* slash escape */
            i++;
            z[j++] = z[i];
        } else {
            z[j++] = z[i];
        }
    }
}


gboolean read_file_to_buffer(const char *filename, char **buffer)
{
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        g_critical(G_STRLOC ":cannot open user conf: %s", filename);
        return FALSE;
    }
    const int  MAX_FILE_SIZE = 1024 * 1024; /* 1M */
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

