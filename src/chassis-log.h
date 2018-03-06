#ifndef _CHASSIS_LOG_H_
#define _CHASSIS_LOG_H_

#include <glib.h>

#include "chassis-exports.h"

#define CHASSIS_RESOLUTION_SEC	0x0
#define CHASSIS_RESOLUTION_MS	0x1

#define CHASSIS_RESOLUTION_DEFAULT	CHASSIS_RESOLUTION_SEC

/** @addtogroup chassis */
/*@{*/

typedef struct _chassis_log chassis_log;

/**
 * chassis_log_rotate_func:
 *
 * prototype of the log rotation function
 *
 * used by #chassis_log_set_rotate_func and #chassis_log_rotate()
 *
 * the function has to 
 * - return %TRUE if log-file rotation was successful,
 *   %FALSE otherwise and set the @gerr accordingly
 * - @user_data is passed through from #chassis_log_set_rotate_func()
 * - @gerr is passed in from #chassis_log_rotate()
 *
 */
typedef gboolean (*chassis_log_rotate_func)(chassis_log *log, gpointer user_data, GError **gerr);


struct _chassis_log {
    GLogLevelFlags min_lvl;

    gchar *log_filename;
    gint log_file_fd;

    gboolean rotate_logs;

    GString *log_ts_str;
    gint	 log_ts_resolution;	/*<< timestamp resolution (sec, ms) */

    GString *last_msg;
    time_t   last_msg_ts;
    guint    last_msg_count;

    /* private */
    chassis_log_rotate_func rotate_func;
    gpointer rotate_func_data;
    GDestroyNotify rotate_func_data_destroy;

    gboolean is_rotated;
};


CHASSIS_API chassis_log *chassis_log_new(void);
CHASSIS_API int chassis_log_set_level(chassis_log *log, const gchar *level);
CHASSIS_API void chassis_log_free(chassis_log *log);
CHASSIS_API int chassis_log_open(chassis_log *log);
CHASSIS_API int chassis_log_close(chassis_log *log);
CHASSIS_API void chassis_log_func(const gchar *log_domain, GLogLevelFlags log_level, 
        const gchar *message, gpointer user_data);
CHASSIS_API void chassis_log_set_logrotate(chassis_log *log);
CHASSIS_API const char *chassis_log_skip_topsrcdir(const char *message);

CHASSIS_API void
chassis_log_set_rotate_func(chassis_log *log, chassis_log_rotate_func rotate_func,
        gpointer userdata, GDestroyNotify userdata_free);

/*@}*/

#endif
