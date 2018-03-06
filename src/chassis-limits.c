#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <glib.h>

#include <sys/types.h>
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#ifdef HAVE_SYS_RESOURCE_H
#include <sys/resource.h>
#endif
#include <errno.h>
#include <stdlib.h>

#include "chassis-limits.h"

gint64 chassis_fdlimit_get() {
    struct rlimit max_files_rlimit;

    if (-1 == getrlimit(RLIMIT_NOFILE, &max_files_rlimit)) {
        return -1;
    } else {
        return max_files_rlimit.rlim_cur;
    }
}


/**
 * set the upper limit of open files
 *
 * @return -1 on error, 0 on success
 */
int chassis_fdlimit_set(gint64 max_files_number) {
    struct rlimit max_files_rlimit;
    rlim_t hard_limit;

    if (-1 == getrlimit(RLIMIT_NOFILE, &max_files_rlimit)) {
        return -1;
    }

    hard_limit = max_files_rlimit.rlim_max;

    max_files_rlimit.rlim_cur = max_files_number;
    /*
     * raise the hard-limit too in case it is smaller 
     * than the soft-limit, otherwise we get a EINVAL 
     */
    if (hard_limit < max_files_number) { 
        max_files_rlimit.rlim_max = max_files_number;
    }

    if (-1 == setrlimit(RLIMIT_NOFILE, &max_files_rlimit)) {
        return -1;
    }

    return 0;
}

