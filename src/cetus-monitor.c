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

#include "cetus-monitor.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <fcntl.h>
#include <limits.h>
#include <mysql.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>

#include "cetus-users.h"
#include "cetus-util.h"
#include "chassis-timings.h"
#include "chassis-event.h"
#include "glib-ext.h"
#include "sharding-config.h"

#define CHECK_ALIVE_INTERVAL 3
#define CHECK_ALIVE_TIMES 2
#define CHECK_DELAY_INTERVAL 300 * 1000 /* 300ms */

/* Each backend should have db <proxy_heart_beat> and table <tb_heartbeat> */
#define HEARTBEAT_DB "proxy_heart_beat"

struct cetus_monitor_t {
    struct chassis *chas;
    GThread *thread;
    chassis_event_loop_t *evloop;

    struct event check_alive_timer;
    struct event write_master_timer;
    struct event read_slave_timer;
    struct event check_config_timer;

    GString *db_passwd;
    GHashTable *backend_conns;

    GList *registered_objects;
    char *config_id;
};

static void
mysql_conn_free(gpointer e)
{
    MYSQL *conn = e;
    if (conn) {
        mysql_close(conn);
    }
}

static char *
get_current_sys_timestr(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
#define BUFSIZE 32
    char *time_sec = g_malloc(BUFSIZE);
    strftime(time_sec, BUFSIZE, "%Y-%m-%d %H:%M:%S", localtime(&tv.tv_sec));
    char *time_micro = g_strdup_printf("%s.%06ld", time_sec, tv.tv_usec);
    g_free(time_sec);
    return time_micro;
}

static MYSQL *
get_mysql_connection(cetus_monitor_t *monitor, char *addr)
{
    MYSQL *conn = g_hash_table_lookup(monitor->backend_conns, addr);
    if (conn) {
        if (mysql_ping(conn) == 0) {
            return conn;
        } else {
            g_hash_table_remove(monitor->backend_conns, addr);
            g_message("monitor: remove dead mysql conn of backend: %s", addr);
        }
    }

    conn = mysql_init(NULL);
    if (!conn)
        return NULL;

    unsigned int timeout = 2 * SECONDS;
    mysql_options(conn, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
    mysql_options(conn, MYSQL_OPT_READ_TIMEOUT, &timeout);
    mysql_options(conn, MYSQL_OPT_WRITE_TIMEOUT, &timeout);

    char **ip_port = g_strsplit(addr, ":", -1);
    int port = atoi(ip_port[1]);
    char *user = monitor->chas->default_username;
    if (mysql_real_connect(conn, ip_port[0], user, monitor->db_passwd->str, NULL, port, NULL, 0) == NULL) {
        g_critical("monitor thread cannot connect to backend: %s@%s", monitor->chas->default_username, addr);
        mysql_conn_free(conn);
        g_strfreev(ip_port);
        return NULL;
    }
    g_hash_table_insert(monitor->backend_conns, g_strdup(addr), conn);
    g_message("monitor thread connected to backend: %s, cached %d conns",
              addr, g_hash_table_size(monitor->backend_conns));
    g_strfreev(ip_port);
    return conn;
}

#define ADD_MONITOR_TIMER(ev_struct, ev_cb, timeout) \
    evtimer_set(&(monitor->ev_struct), ev_cb, monitor);\
    event_base_set(monitor->evloop, &(monitor->ev_struct));\
    evtimer_add(&(monitor->ev_struct), &timeout);

static void
check_backend_alive(int fd, short what, void *arg)
{
    cetus_monitor_t *monitor = arg;
    chassis *chas = monitor->chas;

    int i;
    network_backends_t *bs = chas->priv->backends;
    for (i = 0; i < network_backends_count(bs); i++) {
        network_backend_t *backend = network_backends_get(bs, i);
        backend_state_t oldstate = backend->state;
        gint ret = 0;
        if (backend->state == BACKEND_STATE_DELETED || backend->state == BACKEND_STATE_MAINTAINING)
            continue;

        char *backend_addr = backend->addr->name->str;
        int check_count = 0;
        MYSQL *conn = NULL;
        while (++check_count <= CHECK_ALIVE_TIMES) {
            conn = get_mysql_connection(monitor, backend_addr);
            if (conn)
                break;
        }

        if (conn == NULL) {
            if (backend->state != BACKEND_STATE_DOWN) {
                if (backend->type != BACKEND_TYPE_RW) {
                    ret = network_backends_modify(bs, i, backend->type, BACKEND_STATE_DOWN, oldstate);
                    if(ret == 0) {
                        g_critical("Backend %s is set to DOWN.", backend_addr);
                    } else {
                        g_critical("Backend %s is set to DOWN failed.", backend_addr);
                    }
                } else {
                    g_critical("get null conn from Backend %s.", backend_addr);
                }
            }
            g_debug("Backend %s is not ALIVE!", backend_addr);
        } else {
            if (backend->state != BACKEND_STATE_UP) {
                ret = network_backends_modify(bs, i, backend->type, BACKEND_STATE_UP, oldstate);
                if(ret == 0) {
                    g_message("Backend %s is set to UP.", backend_addr);
                } else {
                    g_message("Backend %s is set to UP failed.", backend_addr);
                }
            }
            g_debug("Backend %s is ALIVE!", backend_addr);
        }
    }

    struct timeval timeout = { 0 };
    timeout.tv_sec = CHECK_ALIVE_INTERVAL;
    ADD_MONITOR_TIMER(check_alive_timer, check_backend_alive, timeout);
}

static void check_slave_timestamp(int fd, short what, void *arg);

static void
update_master_timestamp(int fd, short what, void *arg)
{
    cetus_monitor_t *monitor = arg;
    chassis *chas = monitor->chas;
    int i;
    network_backends_t *bs = chas->priv->backends;
    /* Catch RW time 
     * Need a table to write from master and read from slave.
     * CREATE TABLE `tb_heartbeat` (
     *   `p_id` varchar(128) NOT NULL,
     *   `p_ts` timestamp(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
     *   PRIMARY KEY (`p_id`)
     * ) ENGINE = InnoDB DEFAULT CHARSET = utf8;
     */
    for (i = 0; i < network_backends_count(bs); i++) {
        network_backend_t *backend = network_backends_get(bs, i);
        backend_state_t oldstate = backend->state;
        gint ret = 0;
        if (backend->state == BACKEND_STATE_DELETED || backend->state == BACKEND_STATE_MAINTAINING)
            continue;

        if (backend->type == BACKEND_TYPE_RW) {
            static char sql[1024];
            char *cur_time_str = get_current_sys_timestr();
            snprintf(sql, sizeof(sql), "INSERT INTO %s.tb_heartbeat (p_id, p_ts)"
                     " VALUES ('%s', '%s') ON DUPLICATE KEY UPDATE p_ts='%s'",
                     HEARTBEAT_DB, monitor->config_id, cur_time_str, cur_time_str);

            char *backend_addr = backend->addr->name->str;
            MYSQL *conn = get_mysql_connection(monitor, backend_addr);
            if (conn == NULL) {
                g_critical("Could not connect to Backend %s.", backend_addr);
            } else {
                if (backend->state != BACKEND_STATE_UP) {
                    ret = network_backends_modify(bs, i, backend->type, BACKEND_STATE_UP, oldstate);
                    if(ret == 0) {
                        g_message("Backend %s is set to UP.", backend_addr);
                    } else {
                        g_message("Backend %s is set to UP failed.", backend_addr);
                    }
                }
                static int previous_result[256] = { 0 };    /* for each backend group */
                int result = mysql_real_query(conn, L(sql));
                if (result != previous_result[i] && result != 0) {
                    g_message("Update heartbeat error: %d, text: %s, backend: %s",
                               mysql_errno(conn), mysql_error(conn), backend_addr);
                } else if (result != previous_result[i] && result == 0) {
                    g_message("Update heartbeat success. backend: %s", backend_addr);
                }
                previous_result[i] = result;
            }
            g_free(cur_time_str);
        }
    }

    /* Wait 50ms for RO write data */
    struct timeval timeout = { 0 };
    timeout.tv_usec = 50 * 1000;
    ADD_MONITOR_TIMER(read_slave_timer, check_slave_timestamp, timeout);
}

static void
check_slave_timestamp(int fd, short what, void *arg)
{
    cetus_monitor_t *monitor = arg;
    chassis *chas = monitor->chas;
    int i;
    network_backends_t *bs = chas->priv->backends;

    /* Read delay sec and set slave UP/DOWN according to delay_secs */
    for (i = 0; i < network_backends_count(bs); i++) {
        network_backend_t *backend = network_backends_get(bs, i);
        backend_state_t oldstate = backend->state;
        gint ret = 0;
        if (backend->type == BACKEND_TYPE_RW || backend->state == BACKEND_STATE_DELETED ||
            backend->state == BACKEND_STATE_MAINTAINING)
            continue;

        char *backend_addr = backend->addr->name->str;
        MYSQL *conn = get_mysql_connection(monitor, backend_addr);
        if (conn == NULL) {
            g_critical("Connection error when read delay from RO backend: %s", backend_addr);
            if (backend->state != BACKEND_STATE_DOWN) {
                ret = network_backends_modify(bs, i, backend->type, BACKEND_STATE_DOWN, oldstate);
                if(ret == 0) {
                    g_critical("Backend %s is set to DOWN.", backend_addr);
                } else {
                    g_critical("Backend %s is set to DOWN failed.", backend_addr);
                }
            }
            continue;
        }

        static char sql[512];
        snprintf(sql, sizeof(sql), "select p_ts from %s.tb_heartbeat where p_id='%s'",
                 HEARTBEAT_DB, monitor->config_id);
        static int previous_result[256] = { 0 };    /* for each backend group */
        int result = mysql_real_query(conn, L(sql));
        if (result != previous_result[i] && result != 0) {
            g_critical("Select heartbeat error: %d, text: %s, backend: %s",
                       mysql_errno(conn), mysql_error(conn), backend_addr);
        } else if (result != previous_result[i] && result == 0) {
            g_message("Select heartbeat success. backend: %s", backend_addr);
        }
        previous_result[i] = result;
        if (result == 0) {
            MYSQL_RES *rs_set = mysql_store_result(conn);
            MYSQL_ROW row = mysql_fetch_row(rs_set);
            double ts_slave;
            if (row != NULL) {
                if (strstr(row[0], ".") != NULL) {
                    char **tms = g_strsplit(row[0], ".", -1);
                    glong ts_slave_sec = chassis_epoch_from_string(tms[0], NULL);
                    double ts_slave_msec = atof(tms[1]);
                    ts_slave = ts_slave_sec + ts_slave_msec / 1000;
                    g_strfreev(tms);
                } else {
                    ts_slave = chassis_epoch_from_string(row[0], NULL);
                }
            } else {
                g_critical("Check slave delay no data:%s", sql);
                ts_slave = (double)G_MAXINT32;
            }
            if (ts_slave != 0) {
                struct timeval tv;
                gettimeofday(&tv, NULL);
                double ts_now = tv.tv_sec + ((double)tv.tv_usec) / 1000000;
                double delay_secs = ts_now - ts_slave;
                backend->slave_delay_msec = (int)delay_secs *1000;
                if (delay_secs > chas->slave_delay_down_threshold_sec && backend->state != BACKEND_STATE_DOWN) {
                    ret = network_backends_modify(bs, i, backend->type, BACKEND_STATE_DOWN, oldstate);
                    if(ret == 0) {
                        g_critical("Slave delay %.3f seconds. Set slave to DOWN.", delay_secs);
                    } else {
                        g_critical("Slave delay %.3f seconds. Set slave to DOWN failed.", delay_secs);
                    }
                } else if (delay_secs <= chas->slave_delay_recover_threshold_sec && backend->state != BACKEND_STATE_UP) {
                    ret = network_backends_modify(bs, i, backend->type, BACKEND_STATE_UP, oldstate);
                    if(ret == 0) {
                        g_message("Slave delay %.3f seconds. Recovered. Set slave to UP.", delay_secs);
                    } else {
                        g_message("Slave delay %.3f seconds. Recovered. Set slave to UP failed.", delay_secs);
                    }
                }
            }
            mysql_free_result(rs_set);
        }
    }
    struct timeval timeout = { 0 };
    timeout.tv_usec = CHECK_DELAY_INTERVAL;
    ADD_MONITOR_TIMER(write_master_timer, update_master_timestamp, timeout);
}

#define MON_MAX_NAME_LEN 128
struct monitored_object_t {
    char name[MON_MAX_NAME_LEN];
    monitor_callback_fn func;
    void *arg;
};

struct event config_reload_timer;

static void
check_config_worker(int fd, short what, void *arg)
{
    cetus_monitor_t *monitor = arg;
    chassis *chas = monitor->chas;
    chassis_config_t *conf = chas->config_manager;
    struct timeval timeout = { 0 };
    GList *l;

    for (l = monitor->registered_objects; l; l = l->next) {
        struct monitored_object_t *ob = l->data;
        if (chassis_config_is_object_outdated(conf, ob->name)) {
            /* if (evtimer_pending(&config_reload_timer, NULL))
               break; */
            g_message("monitor: object `%s` is outdated, try updating...", ob->name);

            /* first read in object from remote in monitor thread */
            chassis_config_update_object_cache(conf, ob->name);

            /* then switch to main thread */
            evtimer_set(&config_reload_timer, ob->func, ob->arg);
            event_base_set(chas->event_base, &config_reload_timer);
            evtimer_add(&config_reload_timer, &timeout);
            break;              /* TODO: for now, only update one object each time */
        }
    }

    timeout.tv_sec = 5;
    ADD_MONITOR_TIMER(check_config_timer, check_config_worker, timeout);
}

void
cetus_monitor_open(cetus_monitor_t *monitor, monitor_type_t monitor_type)
{
    struct timeval timeout;
    switch (monitor_type) {
    case MONITOR_TYPE_CHECK_ALIVE:
        timeout.tv_sec = CHECK_ALIVE_INTERVAL;
        timeout.tv_usec = 0;
        ADD_MONITOR_TIMER(check_alive_timer, check_backend_alive, timeout);
        g_message("check_alive monitor open.");
        break;
    case MONITOR_TYPE_CHECK_DELAY:
        timeout.tv_sec = 0;
        timeout.tv_usec = CHECK_DELAY_INTERVAL;
        ADD_MONITOR_TIMER(write_master_timer, update_master_timestamp, timeout);
        g_message("check_slave monitor open.");
        break;
    case MONITOR_TYPE_CHECK_CONFIG:
        timeout.tv_sec = 5;
        timeout.tv_usec = 0;
        ADD_MONITOR_TIMER(check_config_timer, check_config_worker, timeout);
        g_message("check_config monitor open.");
        break;
    default:
        break;
    }
}

void
cetus_monitor_close(cetus_monitor_t *monitor, monitor_type_t monitor_type)
{
    switch (monitor_type) {
    case MONITOR_TYPE_CHECK_ALIVE:
        if (monitor->check_alive_timer.ev_base) {
            evtimer_del(&monitor->check_alive_timer);
        }
        g_message("check_alive monitor close.");
        break;
    case MONITOR_TYPE_CHECK_DELAY:
        if (monitor->write_master_timer.ev_base) {
            evtimer_del(&monitor->write_master_timer);
        }
        if (monitor->read_slave_timer.ev_base) {
            evtimer_del(&monitor->read_slave_timer);
        }
        g_message("check_slave monitor close.");
        break;
    case MONITOR_TYPE_CHECK_CONFIG:
        if (monitor->check_config_timer.ev_base) {
            evtimer_del(&monitor->check_config_timer);
        }
        g_message("check_config monitor close.");
        break;
    default:
        break;
    }
}

static void *
cetus_monitor_mainloop(void *data)
{
    cetus_monitor_t *monitor = data;

    chassis_event_loop_t *loop = chassis_event_loop_new();
    monitor->evloop = loop;

    chassis *chas = monitor->chas;
    monitor->config_id = chassis_config_get_id(chas->config_manager);
    if (!chas->default_username) {
        g_warning("default-username not set, monitor will not work");
        return NULL;
    }

    cetus_users_get_server_pwd(chas->priv->users, chas->default_username, monitor->db_passwd);
    if (monitor->db_passwd->len == 0) { /* TODO: retry */
        g_warning("no password for %s, monitor will not work", chas->default_username);
        return NULL;
    }
    monitor->backend_conns = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, mysql_conn_free);

    if (!chas->check_slave_delay) {
        cetus_monitor_open(monitor, MONITOR_TYPE_CHECK_ALIVE);
    }
    if (chas->check_slave_delay) {
        cetus_monitor_open(monitor, MONITOR_TYPE_CHECK_DELAY);
    }
#if 0
    cetus_monitor_open(monitor, MONITOR_TYPE_CHECK_CONFIG);
#endif
    chassis_event_loop(loop);

    g_message("monitor thread closing %d mysql conns", g_hash_table_size(monitor->backend_conns));
    g_hash_table_destroy(monitor->backend_conns);
    mysql_thread_end();

    g_debug("exiting monitor loop");
    chassis_event_loop_free(loop);
    return NULL;
}

void
cetus_monitor_start_thread(cetus_monitor_t *monitor, chassis *chas)
{
    monitor->chas = chas;
    if (chas->disable_threads) {
        g_message("monitor thread is disabled");
        return;
    }

    g_assert(monitor->thread == 0);

    GThread *new_thread = NULL;
#if !GLIB_CHECK_VERSION(2, 32, 0)
    GError *error = NULL;
    new_thread = g_thread_create(cetus_monitor_mainloop, monitor, TRUE, &error);
    if (new_thread == NULL && error != NULL) {
        g_critical("Create thread error: %s", error->message);
    }
#else
    new_thread = g_thread_new("monitor-thread", cetus_monitor_mainloop, monitor);
    if (new_thread == NULL) {
        g_critical("Create thread error.");
    }
#endif

    monitor->thread = new_thread;
    g_message("monitor thread started");
}

void
cetus_monitor_stop_thread(cetus_monitor_t *monitor)
{
    if (monitor->thread) {
        g_message("Waiting for monitor thread to quit ...");
        g_thread_join(monitor->thread);
        g_message("Monitor thread stopped");
    }
}

cetus_monitor_t *
cetus_monitor_new()
{
    cetus_monitor_t *monitor = g_new0(cetus_monitor_t, 1);

    monitor->db_passwd = g_string_new(0);
    return monitor;
}

void
cetus_monitor_free(cetus_monitor_t *monitor)
{
    /* backend_conns should be freed in its own thread, not here */
    g_string_free(monitor->db_passwd, TRUE);
    g_list_free_full(monitor->registered_objects, g_free);
    if (monitor->config_id)
        g_free(monitor->config_id);
    g_free(monitor);
}

void
cetus_monitor_register_object(cetus_monitor_t *monitor, const char *name, monitor_callback_fn func, void *arg)
{
    GList *l;
    struct monitored_object_t *object = NULL;
    for (l = monitor->registered_objects; l; l = l->next) {
        struct monitored_object_t *ob = l->data;
        if (strcmp(ob->name, name) == 0) {
            object = ob;
            break;
        }
    }
    if (!object) {
        object = g_new0(struct monitored_object_t, 1);
        strncpy(object->name, name, MON_MAX_NAME_LEN - 1);
        monitor->registered_objects = g_list_append(monitor->registered_objects, object);
    }
    object->func = func;
    object->arg = arg;
}
