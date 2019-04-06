#ifndef _CHASSIS_SQL_LOG_H
#define _CHASSIS_SQL_LOG_H

#include <stdio.h>
#include <stdlib.h>
#include <glib.h>
#include "network-mysqld.h"
#include "network-injection.h"

#define SQL_LOG_BUFFER_DEF_SIZE 1024*1024*10
#define SQL_LOG_DEF_FILE_PREFIX "cetus"
#define SQL_LOG_DEF_SUFFIX "clg"
#define SQL_LOG_DEF_PATH "/var/log/"
#define SQL_LOG_DEF_IDLETIME 10000
#define MEGABYTES 1024*1024

#define min(a, b) ((a) < (b) ? (a) : (b))

#define GET_COM_NAME(com_type) \
     ((((gushort)com_type) <= 31) ? com_command_name[((gushort)com_type)].com_str : "UNKNOWN TYPE")

#define GET_COM_STRING(query) ((query)->len > 1 ? ((query)->str + 1) : "")

typedef struct com_string {
    char *com_str;
    size_t length;
} COM_STRING;

typedef enum {
    OFF,
    ON,
    REALTIME
} SQL_LOG_SWITCH;

typedef enum {
    CONNECT = 1,
    CLIENT = 2,
    FRONT = 3,
    BACKEND = 4,
    ALL = 7
} SQL_LOG_MODE;

typedef enum {
    SQL_LOG_UNKNOWN,
    SQL_LOG_START,
    SQL_LOG_STOP
} SQL_LOG_ACTION;

struct rfifo {
    guchar *buffer;
    guint size;
    guint in;
    guint out;
};

struct sql_log_mgr {
    guint sql_log_bufsize;
    SQL_LOG_SWITCH sql_log_switch;
    SQL_LOG_MODE sql_log_mode;
    guint sql_log_maxsize;
    gulong sql_log_cursize;
    volatile SQL_LOG_ACTION sql_log_action;

    volatile guint sql_log_idletime;
    volatile guint sql_log_maxnum;

    gchar *sql_log_prefix;
    gchar *sql_log_path;
    GThread *thread;
    FILE *sql_log_fp;
    gchar *sql_log_fullname;
    struct rfifo *fifo;
    GQueue *sql_log_filelist;
};

struct sql_log_mgr *sql_log_alloc();
void sql_log_free(struct sql_log_mgr *mgr);

gpointer sql_log_mainloop(gpointer user_data);
void cetus_sql_log_start_thread_once(struct sql_log_mgr *mgr);
void sql_log_thread_start(struct sql_log_mgr *mgr);

void log_sql_connect(network_mysqld_con *con, gchar *errmsg);
void log_sql_client(network_mysqld_con *con);
void log_sql_backend(network_mysqld_con *con, injection *inj);
void log_sql_backend_sharding(network_mysqld_con *con, server_session_t *session);
#endif
