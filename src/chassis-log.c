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

#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>             /* close */
#define EVENTLOG_ERROR_TYPE	0x0001
#define EVENTLOG_WARNING_TYPE	0x0002
#define EVENTLOG_INFORMATION_TYPE	0x0004
#include <glib.h>

#ifdef HAVE_SYSLOG_H
#include <syslog.h>
#else
/* placeholder values for platforms not having syslog support */
#define LOG_USER	0           /* placeholder for user-level syslog facility */
#define LOG_CRIT	0
#define LOG_ERR	0
#define LOG_WARNING	0
#define LOG_NOTICE	0
#define LOG_INFO	0
#define LOG_DEBUG	0
#endif


#include "sys-pedantic.h"
#include "chassis-log.h"

#define MAX_BUF_LEN 2048
#define LOG_MAX_LEN 2000
#define cetus_cpymem(d, s, l) (((char *) memcpy(d, (void *) s, l)) + (l))
#define S(x) x->str, x->len

extern pid_t       cetus_pid;

/**
 * the mapping of our internal log levels various log systems
 *
 * Attention: this needs to be adjusted should glib ever 
 * change its log level ordering 
 */
#define G_LOG_ERROR_POSITION 3
const struct {
    char *name;
    GLogLevelFlags lvl;
    int syslog_lvl;
    int win_evtype;
} log_lvl_map[] = {             /* syslog levels are different to the glib ones */
    {
    "error", G_LOG_LEVEL_ERROR, LOG_CRIT, EVENTLOG_ERROR_TYPE}, {
    "critical", G_LOG_LEVEL_CRITICAL, LOG_ERR, EVENTLOG_ERROR_TYPE}, {
    "warning", G_LOG_LEVEL_WARNING, LOG_WARNING, EVENTLOG_WARNING_TYPE}, {
    "message", G_LOG_LEVEL_MESSAGE, LOG_NOTICE, EVENTLOG_INFORMATION_TYPE}, {
    "info", G_LOG_LEVEL_INFO, LOG_INFO, EVENTLOG_INFORMATION_TYPE}, {
    "debug", G_LOG_LEVEL_DEBUG, LOG_DEBUG, EVENTLOG_INFORMATION_TYPE}, {
    NULL, 0, 0, 0}
};

static gboolean
 chassis_log_rotate_reopen(chassis_log *log, gpointer userdata, GError **gerr);

chassis_log *
chassis_log_new(void)
{
    chassis_log *log;

    log = g_new0(chassis_log, 1);

    log->log_file_fd = -1;
    log->log_ts_str = g_string_sized_new(sizeof("2004-01-01T00:00:00.000Z"));
    log->log_ts_resolution = CHASSIS_RESOLUTION_DEFAULT;
    log->min_lvl = G_LOG_LEVEL_CRITICAL;

    log->last_msg = g_string_new(NULL);
    log->last_msg_ts = 0;
    log->last_msg_count = 0;
    log->rotate_func = NULL;

    chassis_log_set_rotate_func(log, chassis_log_rotate_reopen, NULL, NULL);

    return log;
}

int
chassis_log_set_level(chassis_log *log, const gchar *level)
{
    gint i;

    for (i = 0; log_lvl_map[i].name; i++) {
        if (0 == strcmp(log_lvl_map[i].name, level)) {
            log->min_lvl = log_lvl_map[i].lvl;
            return 0;
        }
    }

    return -1;
}

/**
 * open the log-file
 *
 * open the log-file set in log->log_filename
 * if no log-filename is set, returns TRUE
 *
 * FIXME: the return value is not following 'unix'-style (0 on success, -1 on error),
 *        nor does it say it is a gboolean. Has to be fixed in 0.9.0
 *
 * @return TRUE on success, FALSE on error
 */
int
chassis_log_open(chassis_log *log)
{
    if (!log->log_filename)
        return TRUE;

    log->log_file_fd = open(log->log_filename, O_RDWR | O_CREAT | O_APPEND, 0660);

    return (log->log_file_fd != -1);
}

/**
 * close the log-file
 *
 * @return 0 on success
 *
 * @see chassis_log_open
 */
int
chassis_log_close(chassis_log *log)
{
    if (log->log_file_fd == -1)
        return 0;

    close(log->log_file_fd);

    log->log_file_fd = -1;

    return 0;
}

void
chassis_log_free(chassis_log *log)
{
    if (!log)
        return;

    chassis_log_close(log);
    g_string_free(log->log_ts_str, TRUE);
    g_string_free(log->last_msg, TRUE);

    if (log->log_filename)
        g_free(log->log_filename);

    if (log->rotate_func_data_destroy && log->rotate_func_data) {
        log->rotate_func_data_destroy(log->rotate_func_data);
    }

    g_free(log);
}

static int
chassis_log_update_timestamp(chassis_log *log)
{
    struct tm *tm;
    GTimeVal tv;
    time_t t;
    GString *s = log->log_ts_str;

    g_get_current_time(&tv);
    t = (time_t)tv.tv_sec;
    tm = localtime(&t);

    s->len = strftime(s->str, s->allocated_len, "%Y-%m-%d %H:%M:%S", tm);
    if (log->log_ts_resolution == CHASSIS_RESOLUTION_MS)
        g_string_append_printf(s, ".%.3d", (int)tv.tv_usec / 1000);

    return 0;
}

static int
chassis_log_write(chassis_log *log, int log_level, GString *str)
{
    char buffer[MAX_BUF_LEN], *p;
    
    int log_fd = log->log_file_fd;

    p = buffer;

    sprintf(p, "(pid=%d) ", cetus_pid);
    
    p += strlen(p);

    if (str->len < LOG_MAX_LEN) {
        p = cetus_cpymem(p, str->str, str->len);
    } else {
        p = cetus_cpymem(p, str->str, LOG_MAX_LEN - 1);
    }

    *p++ = '\n';

    if (log_fd == -1) {
        write(STDERR_FILENO, buffer, p - buffer);
        return -1;
    } else {
        write(log_fd, buffer, p - buffer);
    }

    return 0;
}

/**
 * skip the 'top_srcdir' from a string starting with G_STRLOC or 
 * __FILE__ if it is absolute
 *
 * <absolute-path>/src/chassis-log.c will become src/chassis-log.c
 *
 * NOTE: the code assumes it is located in src/ or src\. i
 *       If it gets moves somewhere else
 *       it won't crash, but strip too much of pathname
 */
const char *
chassis_log_skip_topsrcdir(const char *message)
{
    const char *my_filename = __FILE__;
    int ndx;

    /*
     * we want to strip everything that is before the src/ 
     * in the above example. If we don't get the srcdir name passed down
     * as part of the __FILE__, don't try to parse it out
     */
    if (!g_path_is_absolute(__FILE__)) {
        return message;
    }

    /* usually the message start with G_STRLOC which may contain a rather 
     * long, absolute path. If
     * it matches the TOP_SRCDIR, we filter it out
     *
     * - strip what is the same as our __FILE__
     * - don't strip our own sub-path 'src/'
     */
    for (ndx = 0; message[ndx]; ndx++) {
        if (0 == strncmp(message + ndx, "src" G_DIR_SEPARATOR_S, sizeof("src" G_DIR_SEPARATOR_S) - 1)) {
            break;
        }
        if (message[ndx] != my_filename[ndx])
            break;
    }

    if (message[ndx] != '\0') {
        message += ndx;
    }

    return message;
}

/*
 * chassis_log_rotate_reopen:
 * @log: a #chassis_log
 * @userdata: ...
 * @gerr: ...
 *
 * default log-rotate function
 *
 * assumes that:
 * - log-file is moved to new name
 * - SIGHUP is sent to process to announce log-file rotation
 * - we reopen the log-file
 *
 * Returns: %TRUE
 */
static gboolean
chassis_log_rotate_reopen(chassis_log *log, gpointer userdata, GError **gerr)
{
    chassis_log_close(log);
    chassis_log_open(log);

    return TRUE;
}

/**
 * chassis_log_rotate:
 * @log: a #chassis_log
 *
 * rotate the logfile
 *
 * calls the rotation function provided by the user 
 * (or the default log-rotation function)
 *
 * Returns: %TRUE if the log-file was rotated, %FALSE on error
 */
gboolean
chassis_log_rotate(chassis_log *log, GError **gerr)
{
    return log->rotate_func(log, log->rotate_func_data, gerr);
}

/**
 * chassis_log_set_rotate_func:
 * @log: a #chassis_log
 * @rotate_func: (allow-none): log-file rotation function
 * @userdata: (allow-none): userdata that is passed to @rotate_func
 * @userdata_free: (allow-none): a #GDestroyNotify that is called 
 *                               when the rotate-function gets unset
 *   
 * set a rotate function that is called when logfile rotate is requested
 *
 * if @rotate_func is %NULL, the default log-rotation function is set
 */
void
chassis_log_set_rotate_func(chassis_log *log, chassis_log_rotate_func rotate_func,
                            gpointer userdata, GDestroyNotify userdata_free)
{

    if (log->rotate_func_data && log->rotate_func_data_destroy) {
        log->rotate_func_data_destroy(log->rotate_func_data);
        log->rotate_func_data = NULL;
    }

    if (NULL == rotate_func) {
        log->rotate_func = chassis_log_rotate_reopen;
    } else {
        log->rotate_func = rotate_func;
    }

    log->rotate_func_data = userdata;
    log->rotate_func_data_destroy = userdata_free;

    return;

}

static void
chassis_log_func_locked(const gchar G_GNUC_UNUSED *log_domain, GLogLevelFlags log_level,
                        const gchar *message, gpointer user_data)
{
    chassis_log *log = user_data;
    int i;
    gchar *log_lvl_name = "(error)";
    gboolean is_duplicate = FALSE;
    const char *stripped_message = chassis_log_skip_topsrcdir(message);

    /**
     * rotate logs straight away if log->rotate_logs is true
     * we do this before ignoring any log levels, so that rotation 
     * happens straight away - see Bug#55711 
     */
    if (-1 != log->log_file_fd) {
        if (log->rotate_logs) {
            gboolean is_rotated;

            is_rotated = chassis_log_rotate(log, NULL);

            log->rotate_logs = FALSE;

            /* ->is_rotated stays set until the first message is logged
             * again to make sure we don't hide it as duplicate
             */
            if (log->is_rotated == FALSE && is_rotated == TRUE) {
                log->is_rotated = TRUE;
            }
        }
    }

    /* ignore the verbose log-levels */
    if (log_level > log->min_lvl) {
        return;
    }

    for (i = 0; log_lvl_map[i].name; i++) {
        if (log_lvl_map[i].lvl == log_level) {
            log_lvl_name = log_lvl_map[i].name;
            break;
        }
    }

    if (log->last_msg->len > 0 && 0 == strcmp(log->last_msg->str, stripped_message)) {
        is_duplicate = TRUE;
    }

    /**
     * if the log has been rotated, we always dump the last message even if it 
     * was a duplicate. Otherwise, do not print duplicates unless they have been
     * ignored at least 100 times, or they were last printed greater than 
     * 30 seconds ago.
     */
    if (log->is_rotated || !is_duplicate || log->last_msg_count > 100 || time(NULL) - log->last_msg_ts > 30) {

        /* if we lave the last message repeating, log it */
        if (log->last_msg_count) {
            chassis_log_update_timestamp(log);
            g_string_append_printf(log->log_ts_str, ": (%s) last message repeated %d times",
                                   log_lvl_name, log->last_msg_count);

            chassis_log_write(log, log_level, log->log_ts_str);
        }
        chassis_log_update_timestamp(log);
        g_string_append(log->log_ts_str, ": (");
        g_string_append(log->log_ts_str, log_lvl_name);
        g_string_append(log->log_ts_str, ") ");

        g_string_append(log->log_ts_str, stripped_message);

        /* reset the last-logged message */
        g_string_assign(log->last_msg, stripped_message);
        log->last_msg_count = 0;
        log->last_msg_ts = time(NULL);

        chassis_log_write(log, log_level, log->log_ts_str);
    } else {
        log->last_msg_count++;
    }

    log->is_rotated = FALSE;
}

void
chassis_log_func(const gchar *log_domain, GLogLevelFlags log_level, const gchar *message, gpointer user_data)
{
    chassis_log_func_locked(log_domain, log_level, message, user_data);
}

void
chassis_log_set_logrotate(chassis_log *log)
{
    log->rotate_logs = TRUE;
}
