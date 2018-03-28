#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "cetus-log.h"

#define TC_PREFIX  "/var/log/"
#define TC_ERROR_LOG_PATH  "xa.log"
#define TC_ERR_LOG_TIME_LEN (sizeof("2012-07-31 12:35:00 +999") - 1)
#define TC_ERR_LOG_TIME_STR_LEN (TC_ERR_LOG_TIME_LEN + 1)

#define tc_cpymem(d, s, l) (((char *) memcpy(d, (void *) s, l)) + (l))

static int log_fd = -1;
static int last_hour = -1;
static const char *file_name_prefix = NULL;
static char tc_error_log_time[TC_ERR_LOG_TIME_STR_LEN];

static int tc_update_time();

struct tc_log_level_t {
    char *level;
    int len;
};

static struct tc_log_level_t tc_log_levels[] = {
    {"[unknown]", 9},
    {"[emerg]", 7},
    {"[alert]", 7},
    {"[crit]", 6},
    {"[error]", 7},
    {"[warn]", 6},
    {"[notice]", 8},
    {"[info]", 6},
    {"[debug]", 7}
};

static int
tc_vscnprintf(char *buf, size_t size, const char *fmt, va_list args)
{
    int i;

    /*
     * Attention for vsnprintf: http://lwn.net/Articles/69419/
     */
    i = vsnprintf(buf, size, fmt, args);

    if (i < (int)size) {
        return i;
    }

    if (size >= 1) {
        return size - 1;
    } else {
        return 0;
    }
}

static void
tc_localtime(time_t sec, struct tm *tm)
{
#if (HAVE_LOCALTIME_R)
    (void)localtime_r(&sec, tm);
#else
    struct tm *t;

    t = localtime(&sec);
    *tm = *t;
#endif

    tm->tm_mon++;
    tm->tm_year += 1900;
}

static int
tc_scnprintf(char *buf, size_t size, const char *fmt, ...)
{
    int i;
    va_list args;

    va_start(args, fmt);
    i = tc_vscnprintf(buf, size, fmt, args);
    va_end(args);

    return i;
}

static int
tc_create_new_file(const char *file, int hour)
{
    int len;
    char new_file_path[512], *p;

    if (file == NULL) {
        len = strlen(TC_PREFIX);
        if (len >= 256) {
            fprintf(stderr, "file prefix too long: %s\n", TC_PREFIX);
            return -1;
        }
        strncpy(new_file_path, TC_PREFIX, len);
        p = new_file_path + len;
        len += strlen(TC_ERROR_LOG_PATH);
        if (len >= 256) {
            fprintf(stderr, "file path too long: %s\n", TC_PREFIX);
            return -1;
        }
        strcpy(p, TC_ERROR_LOG_PATH);
        p = new_file_path + len;

        sprintf(p, "_%2d", hour);
    } else {
        p = new_file_path;
        len = strlen(file);
        strcpy(p, file);
        p = p + len;
        sprintf(p, "_%02d", hour);
    }

    file = new_file_path;

#if (PROXY_O_SYNC)
    log_fd = open(file, O_RDWR | O_CREAT | O_APPEND | O_SYNC, 0644);
#else
    log_fd = open(file, O_RDWR | O_CREAT | O_APPEND, 0644);
#endif

    if (log_fd == -1) {
        fprintf(stderr, "Open log file:%s error\n", file);
    }

    return log_fd;
}

int
tc_log_init(const char *file)
{
    file_name_prefix = file;
    tc_update_time();
    tc_create_new_file(file, last_hour);
    return log_fd;
}

int
tc_get_log_hour()
{
    if (last_hour == -1) {
        tc_update_time();
    }

    return last_hour;
}

void
tc_log_end(void)
{
    if (log_fd != -1) {
        close(log_fd);
    }

    log_fd = -1;
}

static int
tc_update_time()
{
    int status;
    time_t sec;
    struct tm tm;
    struct timeval tv;

    status = gettimeofday(&tv, NULL);
    if (status >= 0) {
        sec = tv.tv_sec;
        long msec = tv.tv_usec / 1000;

        tc_localtime(sec, &tm);

        snprintf(tc_error_log_time, TC_ERR_LOG_TIME_STR_LEN,
                 "%4d/%02d/%02d %02d:%02d:%02d +%03d",
                 tm.tm_year, tm.tm_mon, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, (int)msec);
        if (tm.tm_hour != last_hour) {
            if (last_hour == -1) {
                last_hour = tm.tm_hour;
            } else {
                last_hour = tm.tm_hour;
                return 1;
            }
        }
    }

    return 0;
}

void
tc_log_info(int level, int err, const char *fmt, ...)
{
    int n, len;
    char buffer[LOG_MAX_LEN], *p;
    va_list args;
    struct tc_log_level_t *ll;

    if (log_fd == -1) {
        return;
    }

    if (tc_update_time()) {
        tc_log_end();
        tc_create_new_file(file_name_prefix, last_hour);
    }

    ll = &tc_log_levels[level];

    p = buffer;

    p = tc_cpymem(p, tc_error_log_time, TC_ERR_LOG_TIME_LEN);
    *p++ = ' ';

    p = tc_cpymem(p, ll->level, ll->len);
    *p++ = ' ';

    n = len = TC_ERR_LOG_TIME_LEN + ll->len + 2;
    va_start(args, fmt);
    len += tc_vscnprintf(p, LOG_MAX_LEN - n, fmt, args);
    va_end(args);

    if (len < n) {
        return;
    }

    p = buffer + len;

    if (err > 0) {
        len += tc_scnprintf(p, LOG_MAX_LEN - len, " (%s)", strerror(err));
        if (len < (p - buffer)) {
            return;
        }

        p = buffer + len;
    }

    *p++ = '\n';

    write(log_fd, buffer, p - buffer);
}
