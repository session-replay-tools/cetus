#include "chassis-sql-log.h"
#include "network-mysqld-packet.h"
#include "cetus-process.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

const COM_STRING com_command_name[]={
    { C("Sleep") },
    { C("Quit") },
    { C("Init DB") },
    { C("Query") },
    { C("Field List") },
    { C("Create DB") },
    { C("Drop DB") },
    { C("Refresh") },
    { C("Shutdown") },
    { C("Statistics") },
    { C("Processlist") },
    { C("Connect") },
    { C("Kill") },
    { C("Debug") },
    { C("Ping") },
    { C("Time") },
    { C("Delayed insert") },
    { C("Change user") },
    { C("Binlog Dump") },
    { C("Table Dump") },
    { C("Connect Out") },
    { C("Register Slave") },
    { C("Prepare") },
    { C("Execute") },
    { C("Long Data") },
    { C("Close stmt") },
    { C("Reset stmt") },
    { C("Set option") },
    { C("Fetch") },
    { C("Daemon") },
    { C("Binlog Dump GTID") },
    { C("Error") }  // Last command number
};

const char *com_dis_tras_state[]={
    "UNKNOWN",
    "NEXT_ST_XA_START",
    "NEXT_ST_XA_QUERY",
    "NEXT_ST_XA_END",
    "NEXT_ST_XA_PREPARE",
    "NEXT_ST_XA_COMMIT",
    "NEXT_ST_XA_ROLLBACK",
    "NEXT_ST_XA_CANDIDATE_OVER",
    "NEXT_ST_XA_OVER"
};

static guint roundup_pow_of_two(const guint x) {
    if (x == 0) return 0;
    if (x == 1) return 2;
    unsigned int ret = 1U;
    while(ret < x) {
        ret <<= 1;
    }
    return ret;
}

static void get_current_time_str(GString *str) {
    if (!str) {
        g_critical("str is NULL when call get_current_time_str()");
        return;
    }
    struct tm *tm;
    GTimeVal tv;
    time_t  t;

    g_get_current_time(&tv);
    t = (time_t) tv.tv_sec;
    tm = localtime(&t);

    str->len = strftime(str->str, str->allocated_len, "%Y-%m-%d %H:%M:%S", tm);
    g_string_append_printf(str, ".%.3d", (int) tv.tv_usec/1000);
}

static struct rfifo *rfifo_alloc(guint size) {
    struct rfifo *ret = (struct rfifo *)g_malloc0(sizeof(struct rfifo));

    if (!ret) {
        return NULL;
    }

    if (size & (size - 1)) {
        size = roundup_pow_of_two(size);
    }
    ret->buffer = (unsigned char *)g_malloc0(size);
    if (!ret->buffer) {
        g_free(ret);
        return NULL;
    }
    ret->size = size;
    ret->in = ret->out = 0;
    return ret;
}

static void rfifo_free(struct rfifo *fifo) {
    if (!fifo) return;
    g_free(fifo->buffer);
    g_free(fifo);
}

static guint rfifo_write(struct rfifo *fifo, guchar *buffer, guint len) {
    if (!fifo) {
        g_critical("struct fifo is NULL when call rfifo_write()");
        return -1;
    }
    guint l;
    len = min(len, (fifo->size - fifo->in + fifo->out));
    l = min(len, fifo->size - (fifo->in & (fifo->size -1)));
    memcpy(fifo->buffer + (fifo->in & (fifo->size -1)), buffer, l);//g_strlcpy
    memcpy(fifo->buffer, buffer + l, len - l);
    fifo->in += len;
    return len;
}

static guint rfifo_flush(struct sql_log_mgr *mgr) {
    if (!mgr) {
        g_critical("struct mgr is NULL when call rfifo_flush()");
        return -1;
    }

    struct rfifo *fifo = mgr->fifo;
    if (!fifo) {
        g_critical("struct fifo is NULL when call rfifo_flush()");
        return -1;
    }
    if (!mgr->sql_log_fp) {
        g_critical("sql_log_fp is NULL when call rfifo_flush()");
        return -1;
    }

    guint len = fifo->in - fifo->out;
    guint l = min(len, fifo->size - (fifo->out & (fifo->size -1)));
    gint fd = fileno(mgr->sql_log_fp);
    guint s1 = pwrite(fd, fifo->buffer + (fifo->out & (fifo->size - 1)),
            l, (off_t)mgr->sql_log_cursize);
    mgr->sql_log_cursize += s1;
    fifo->out += s1;
    guint s2 = pwrite(fd, fifo->buffer, len -l, (off_t)mgr->sql_log_cursize);
    mgr->sql_log_cursize += s2;
    fifo->out += s2;
    if(mgr->sql_log_switch == REALTIME) {
        fsync(fd);
    }
    return (s1 + s2);
}

struct sql_log_mgr *sql_log_alloc() {
    struct sql_log_mgr *mgr = (struct sql_log_mgr *)g_malloc0(sizeof(struct sql_log_mgr));

    if(!mgr) {
        return (struct sql_log_mgr *)NULL;
    }
    mgr->sql_log_fp = NULL;
    mgr->sql_log_prefix = NULL;
    mgr->sql_log_path = NULL;
    mgr->sql_log_bufsize = SQL_LOG_BUFFER_DEF_SIZE;
    mgr->sql_log_mode = BACKEND;
    mgr->sql_log_switch = OFF;
    mgr->sql_log_cursize = 0;
    mgr->sql_log_maxsize = 1024;
    mgr->sql_log_fullname = NULL;
    mgr->sql_log_idletime = SQL_LOG_DEF_IDLETIME;
    mgr->sql_log_action = SQL_LOG_STOP;
    mgr->fifo = NULL;
    mgr->sql_log_filelist = NULL;
    mgr->sql_log_maxnum = 3;
    return mgr;
}

static void free_queue_manually(GQueue *queue, GDestroyNotify free_func) {
    if (!queue || !free_func) return;
    gint num = g_queue_get_length(queue);
    while(num) {
        gchar *fn = (gchar *)g_queue_pop_head(queue);
        free_func(fn);
        num --;
    }
    g_queue_free(queue);
}

void sql_log_free(struct sql_log_mgr *mgr) {
    if (!mgr) return;

    g_free(mgr->sql_log_prefix);
    g_free(mgr->sql_log_path);

    g_free(mgr->sql_log_fullname);

    rfifo_free(mgr->fifo);

    free_queue_manually(mgr->sql_log_filelist, g_free);

    mgr->sql_log_filelist = NULL;

    g_free(mgr);
}

static void sql_log_check_filenum(struct sql_log_mgr *mgr, gchar *filename) {
    if (!mgr || !filename) {
        g_warning("struct mgr or filename is NULL when call sql_log_check_filenum()");
        return;
    }
    if (!mgr->sql_log_maxnum) {
        return;
    }
    if (!mgr->sql_log_filelist) {
        mgr->sql_log_filelist = g_queue_new();
    } else {
        GList *pos = g_queue_find_custom(mgr->sql_log_filelist, filename, (GCompareFunc)strcmp);
        if (pos) {
            g_warning("file %s is exist", filename);
            return;
        }
    }
    g_queue_push_tail(mgr->sql_log_filelist, g_strdup(filename));

    gint num = g_queue_get_length(mgr->sql_log_filelist) - mgr->sql_log_maxnum;
    while (num > 0) {
        gchar *fn = (gchar *)g_queue_pop_head(mgr->sql_log_filelist);
        if (unlink(fn) != 0) {
            g_warning("rm log file %s failed", fn);
        } else {
            g_message("rm log file %s success", fn);
        }
        g_free(fn);
        num --;
    }
    return ;
}

static void sql_log_check_rotate(struct sql_log_mgr *mgr) {
    if (!mgr) return ;
    if (mgr->sql_log_maxsize == 0) return;
    if (mgr->sql_log_cursize < ((gulong)mgr->sql_log_maxsize) * MEGABYTES) return ;

    time_t t = time(NULL);
    struct tm cur_tm;
    localtime_r(&t, &cur_tm);

    gchar *rotate_filename = g_strdup_printf("%s/%s-%d-%04d%02d%02d%02d%02d%02d.%s",
            mgr->sql_log_path, mgr->sql_log_prefix, cetus_pid,
            cur_tm.tm_year + 1900, cur_tm.tm_mon + 1, cur_tm.tm_mday, cur_tm.tm_hour,
            cur_tm.tm_min, cur_tm.tm_sec, SQL_LOG_DEF_SUFFIX);
    if (!rotate_filename) {
        g_critical("can not get the rotate filename");
        return ;
    }
    if (mgr->sql_log_fp) {
        fclose(mgr->sql_log_fp);
        mgr->sql_log_fp = NULL;
    }
    if (rename(mgr->sql_log_fullname, rotate_filename) != 0) {
        g_critical("rename log file name to %s failed", rotate_filename);
    } else {
        sql_log_check_filenum(mgr, rotate_filename);
    }
    g_free(rotate_filename);
    mgr->sql_log_cursize = 0;
    mgr->sql_log_fp = fopen(mgr->sql_log_fullname, "a");
    if(!mgr->sql_log_fp) {
        g_critical("rotate error, because fopen(%s) failed", mgr->sql_log_fullname);
    }
}

 gpointer sql_log_mainloop(gpointer user_data) {
    struct sql_log_mgr *mgr = (struct sql_log_mgr *)user_data;
    struct stat st;
    mgr->sql_log_fp = fopen(mgr->sql_log_fullname, "a");

    if (!mgr->sql_log_fp) {
        g_critical("sql log thread exit, because fopen(%s) failed", mgr->sql_log_fullname);
        mgr->sql_log_action = SQL_LOG_STOP;
        return NULL;
    }
    gint rst = fstat(fileno(mgr->sql_log_fp), &st);
    if (rst != 0) {
        g_message("fstat() failed in sql_log_mainloop");
    }
    mgr->sql_log_cursize = st.st_size;
    g_message("sql log thread started");
    mgr->sql_log_action = SQL_LOG_START;

    while(!chassis_is_shutdown()) {
        guint len = rfifo_flush(mgr);
        if (!len) {
            usleep(mgr->sql_log_idletime);
        } else {
            sql_log_check_rotate(mgr);
        }
        if (mgr->sql_log_action != SQL_LOG_START) {
            goto sql_log_exit;
        }
    }
sql_log_exit:
    fclose(mgr->sql_log_fp);
    g_message("sql log thread stopped");
    mgr->sql_log_cursize = 0;
    mgr->sql_log_action = SQL_LOG_STOP;
    return NULL;
}

void
sql_log_thread_start(struct sql_log_mgr *mgr) {
    if (!mgr) return;
    GThread *sql_log_thread = NULL;
#if !GLIB_CHECK_VERSION(2, 32, 0)
    GError *error = NULL;
    sql_log_thread = g_thread_create(sql_log_mainloop, mgr, TRUE, &error);
    if (sql_log_thread == NULL && error != NULL) {
        g_critical("Create sql log thread error: %s", error->message);
    }
    if (error != NULL) {
        g_clear_error(&error);
    }
#else
    sql_log_thread = g_thread_new("sql_log-thread", sql_log_mainloop, mgr);
    if (sql_log_thread == NULL) {
        g_critical("Create sql log thread error.");
    }
#endif

    mgr->thread = sql_log_thread;
}

 void
 cetus_sql_log_start_thread_once(struct sql_log_mgr *mgr)
 {

     if (!mgr) {
         g_critical("sql_mgr is null!");
         return;
     }
     if (mgr->sql_log_bufsize == 0) {
         mgr->sql_log_bufsize = SQL_LOG_BUFFER_DEF_SIZE;
     }
     if (mgr->sql_log_prefix == NULL) {
         mgr->sql_log_prefix = g_strdup(SQL_LOG_DEF_FILE_PREFIX);
     }
     if (mgr->sql_log_path == NULL) {
         mgr->sql_log_path = g_strdup(SQL_LOG_DEF_PATH);
     }
     gint result = mkdir(mgr->sql_log_path, 0770);
     if (!result) {
         g_message("sql log path maybe exist, try to mkdir failed");
     } else {
         g_message("sql log path is not exist, try to mkdir success");
     }
     if (mgr->sql_log_fullname == NULL) {
         mgr->sql_log_fullname = g_strdup_printf("%s/%s-%d.%s",
                 mgr->sql_log_path, mgr->sql_log_prefix, cetus_pid, SQL_LOG_DEF_SUFFIX);
     }
     mgr->fifo = rfifo_alloc(mgr->sql_log_bufsize);
     mgr->sql_log_filelist = g_queue_new();

     if (mgr->sql_log_switch == OFF) {
         g_message("sql thread is not start");
         return;
     }
     sql_log_thread_start(mgr);
 }

void
log_sql_client(network_mysqld_con *con)
{
    if (!con || !con->srv) {
        g_critical("con or con->srv is NULL when call log_sql_client()");
        return;
    }
    struct sql_log_mgr *mgr = con->srv->sql_mgr;
    if (!mgr) {
        g_critical("sql mgr is NULL when call log_sql_client()");
        return;
    }
    if (mgr->sql_log_switch == OFF ||
        !(mgr->sql_log_mode & CLIENT) ||
        (mgr->sql_log_action != SQL_LOG_START)) {
        return;
    }
    GString *message = g_string_sized_new(sizeof("2004-01-01T00:00:00.000Z"));
    get_current_time_str(message);
    g_string_append_printf(message, ": #client# C_ip:%s C_db:%s C_usr:%s C_tx:%s C_retry:%d C_id:%u type:%s %s\n",
                                             con->client->src->name->str,//C_ip
                                             con->client->default_db->str,//C_db
                                             con->client->response->username->str,//C_usr
                                             con->is_in_transaction == 1 ? "true":"false",//C_tx
                                             con->retry_serv_cnt,//C_retry
                                             con->client->challenge->thread_id,//C_id
                                             GET_COM_NAME(con->parse.command),//type
                                             con->orig_sql != NULL ? con->orig_sql->str : "");//sql

    rfifo_write(mgr->fifo, message->str, message->len);
    g_string_free(message, TRUE);
}

 void
log_sql_backend(network_mysqld_con *con, injection *inj)
{
     if (!con || !con->srv || !inj) {
         g_critical("con or con->srv or inj is NULL when call log_sql_backend()");
         return;
     }
     struct sql_log_mgr *mgr = con->srv->sql_mgr;
     if (!mgr) {
         g_critical("sql mgr is NULL when call log_sql_backend()");
         return;
     }
     if (mgr->sql_log_switch == OFF ||
         !(mgr->sql_log_mode & BACKEND) ||
         (mgr->sql_log_action != SQL_LOG_START)) {
         return;
     }
     gdouble latency_ms = (inj->ts_read_query_result_last - inj->ts_read_query)/1000.0;
     GString *message = g_string_sized_new(sizeof("2004-01-01T00:00:00.000Z"));
     get_current_time_str(message);
     g_string_append_printf(message, ": #backend-rw# C_ip:%s C_db:%s C_usr:%s C_tx:%s C_id:%u S_ip:%s S_db:%s S_usr:%s S_id:%u "
                                              "inj(type:%d bytes:%lu rows:%lu) latency:%.3lf(ms) %s type:%s %s\n",
                                              con->client->src->name->str,//C_ip
                                              con->client->default_db->str,//C_db
                                              con->client->response->username->str,//C_usr
                                              con->is_in_transaction == 1 ? "true":"false",//C_tx
                                              con->client->challenge->thread_id,//C_id
                                              con->server->dst->name->str,//S_ip
                                              con->server->default_db->str,//S_db
                                              con->server->response->username->str,//S_usr
                                              con->server->challenge->thread_id,//S_id
                                              inj->id, inj->bytes, inj->rows,
                                              latency_ms, inj->qstat.query_status == MYSQLD_PACKET_OK ? "OK" : "ERR",
                                              GET_COM_NAME(con->parse.command),//type
                                              con->parse.command == COM_STMT_EXECUTE ? "" : (inj->query != NULL ? GET_COM_STRING(inj->query) : ""));//sql

     rfifo_write(mgr->fifo, message->str, message->len);
     g_string_free(message, TRUE);
}

void
log_sql_backend_sharding(network_mysqld_con *con, server_session_t *session)
{
     if (!con || !con->srv || !session) {
         g_critical("con or con->srv or session is NULL when call log_sql_backend_sharding()");
         return;
     }
     struct sql_log_mgr *mgr = con->srv->sql_mgr;
     if (!mgr) {
         g_critical("sql mgr is NULL when call log_sql_backend_sharding()");
         return;
     }
     if (mgr->sql_log_switch == OFF ||
         !(mgr->sql_log_mode & BACKEND) ||
         (mgr->sql_log_action != SQL_LOG_START)) {
         return;
     }
     gdouble latency_ms = (session->ts_read_query_result_last - session->ts_read_query)/1000.0;
     GString *message = g_string_sized_new(sizeof("2004-01-01T00:00:00.000Z"));
     get_current_time_str(message);
     gint xa_state = 0;
     if (session->dist_tran_state > 0 && session->dist_tran_state <= 8) {
         xa_state = session->dist_tran_state;
     }
     if (con->attr_adj_state != 0) {
         g_string_append_printf(message, ": #backend-sharding# C_ip:%s C_db:%s C_usr:%s C_tx:%s C_id:%u "
                                             "trans(in_xa:%s xa_state:%s) attr_adj_state:%d\n",
                                              con->client->src->name->str,//C_ip
                                              con->client->default_db->str,//C_db
                                              con->client->response->username->str,//C_usr
                                              con->is_in_transaction == 1 ? "true":"false",//C_tx
                                              con->client->challenge->thread_id,//C_id
                                              session->is_in_xa ==1 ? "true" : "false", com_dis_tras_state[xa_state],
                                              con->attr_adj_state
                                              );
     } else {
         g_string_append_printf(message, ": #backend-sharding# C_ip:%s C_db:%s C_usr:%s C_tx:%s C_id:%u S_ip:%s S_db:%s S_usr:%s S_id:%u "
                                              "trans(in_xa:%s xa_state:%s) latency:%.3lf(ms) %s type:%s %s\n",
                                              con->client->src->name->str,//C_ip
                                              con->client->default_db->str,//C_db
                                              con->client->response->username->str,//C_usr
                                              con->is_in_transaction == 1 ? "true":"false",//C_tx
                                              con->client->challenge->thread_id,//C_id
                                              session->server->dst->name->str,//S_ip
                                              session->server->default_db->str,//S_db
                                              session->server->response->username->str,//S_usr
                                              session->server->challenge->thread_id,//S_id
                                              session->is_in_xa ==1 ? "true" : "false", com_dis_tras_state[xa_state],
                                              latency_ms, session->query_status == MYSQLD_PACKET_OK ? "OK" : "ERR",
                                              GET_COM_NAME(con->parse.command),//type
                                              con->parse.command == COM_STMT_EXECUTE ? "" : (session->sql != NULL ? session->sql->str : ""));//sql
     }
     rfifo_write(mgr->fifo, message->str, message->len);
     g_string_free(message, TRUE);
}

 void
 log_sql_connect(network_mysqld_con *con, gchar *errmsg)
 {
     if (!con || !con->srv) {
         g_critical("con or con->srv is NULL when call log_sql_connect()");
         return;
     }
     struct sql_log_mgr *mgr = con->srv->sql_mgr;
     if (!mgr) {
         g_critical("sql mgr is NULL when call log_sql_connect()");
         return;
     }
     if (mgr->sql_log_switch == OFF ||
         !(mgr->sql_log_mode & CONNECT) ||
         (mgr->sql_log_action != SQL_LOG_START)) {
         return;
     }
     GString *message = g_string_sized_new(sizeof("2004-01-01T00:00:00.000Z"));
     get_current_time_str(message);
     g_string_append_printf(message, ": #connect# %s@%s Connect Cetus %s msg:%s, C_id:%u C_db:%s C_charset:%u C_auth_plugin:%s C_ssl:%s C_cap:%x S_cap:%x\n",
                                              con->client->response->username->str,//C_usr
                                              con->client->src->name->str,//C_ip
                                              errmsg == NULL ? "success" : "failed",
                                              errmsg == NULL ? "": errmsg,
                                              con->client->challenge->thread_id,//C_id
                                              con->client->response->database->str,//C_db
                                              con->client->response->charset,//C_charset
                                              con->client->response->auth_plugin_name->str,
                                              con->client->response->ssl_request == 1 ? "true" : "false",
                                              con->client->response->client_capabilities,
                                              con->client->response->server_capabilities
                                              );

     rfifo_write(mgr->fifo, message->str, message->len);
     g_string_free(message, TRUE);
 }
