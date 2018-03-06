
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>

#include <gmodule.h>

#include "chassis-filemode.h"

/*
 * check whether the given filename points to a file the permissions
 * of which are 0 for group and other (ie read/writable only by owner).
 * return 0 for "OK", -1 of the file cannot be accessed or is the wrong
 * type of file, and 1 if permissions are wrong
 *
 *
 * FIXME? this function currently ignores ACLs
 */
int chassis_filemode_check_full(const gchar *filename, int required_filemask, GError **gerr) {
    struct stat stbuf;
    mode_t		fmode;

    if (stat(filename, &stbuf) == -1) {
        g_set_error(gerr, G_FILE_ERROR, g_file_error_from_errno(errno),
                "cannot stat(%s): %s", filename,
                g_strerror(errno));
        return -1;
    }

    fmode = stbuf.st_mode;
    if ((fmode & S_IFMT) != S_IFREG) {
        g_set_error(gerr, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                "%s isn't a regular file", filename);
        return -1;
    }

    if ((fmode & required_filemask) != 0) {
        g_set_error(gerr, G_FILE_ERROR, G_FILE_ERROR_PERM,
                "permissions of %s aren't secure (0660 or stricter required)", filename);
        return 1;
    }

#undef MASK

    return 0;
}
