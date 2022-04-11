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
#include <arpa/inet.h>

#include "cetus-users.h"
#include "cetus-util.h"
#include "chassis-timings.h"
#include "chassis-event.h"
#include "glib-ext.h"
#include "sharding-config.h"

#include <netdb.h>
#include <arpa/inet.h>

#define CHECK_ALIVE_INTERVAL 1
#define CHECK_ALIVE_TIMES 2
#define CHECK_DELAY_INTERVAL 500 * 1000 /* 500ms */

#define ADDRESS_LEN 64

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

    unsigned int mysql_init_called:1;
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
  monitor->mysql_init_called = 1;
  if (!conn) return NULL;

  unsigned int timeout = 6 * SECONDS;
  mysql_options(conn, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
  mysql_options(conn, MYSQL_OPT_READ_TIMEOUT, &timeout);
  mysql_options(conn, MYSQL_OPT_WRITE_TIMEOUT, &timeout);

  char **ip_port = g_strsplit(addr, ":", -1);
  int port = atoi(ip_port[1]);
  char *user = monitor->chas->default_username;
  if (mysql_real_connect(conn, ip_port[0], user, monitor->db_passwd->str, NULL,
                         port, NULL, 0) == NULL) {
    g_critical("monitor thread cannot connect to backend: %s@%s",
               monitor->chas->default_username, addr);
    mysql_conn_free(conn);
    g_strfreev(ip_port);
    return NULL;
  }

  const char *sql = "set session time_zone='+08:00'";

  if (mysql_real_query(conn, L(sql))) {
    g_message(
        "set session timezone failed. error: %d, "
        "text: %s, backend: %s",
        mysql_errno(conn), mysql_error(conn), addr);
    mysql_conn_free(conn);
    return NULL;
  } else {
    MYSQL_RES *rs_set = mysql_store_result(conn);
    if (rs_set == NULL) {
      if (mysql_field_count(conn) != 0) {
        g_message(
            "set session timezone failed. "
            "error: %d, text: %s, backend: %s",
            mysql_errno(conn), mysql_error(conn), addr);
      }
    } else {
      mysql_free_result(rs_set);
    }
  }

  g_hash_table_insert(monitor->backend_conns, g_strdup(addr), conn);
  g_message("monitor thread connected to backend: %s, cached %d conns", addr,
            g_hash_table_size(monitor->backend_conns));
  g_strfreev(ip_port);
  return conn;
}

static gint
get_ip_by_name(const gchar *name, gchar *ip) {
    if(ip == NULL || name == NULL) return -1;
    char **pptr;
    struct hostent *hptr;
    hptr = gethostbyname(name);
    if(hptr == NULL) {
        g_debug("gethostbyname failed.");
        return -1;
    }
    for(pptr = hptr->h_addr_list; *pptr != NULL; pptr++) {
        if(inet_ntop(hptr->h_addrtype, *pptr, ip, ADDRESS_LEN)) {
            return 0;
        }
    }
    return -1;
}

static gint
slave_list_compare(gconstpointer a, gconstpointer b) {
    gchar *old_value = (gchar *)a;
    gchar *search_value = (gchar *)b;
    return strcasecmp(old_value, search_value);
}

static void
group_replication_detect(network_backends_t *bs, cetus_monitor_t *monitor)
{
    if(bs == NULL) return ;

    GList *slave_list = NULL;
    gchar master_addr[ADDRESS_LEN] = {""};
    gchar slave_addr[ADDRESS_LEN] = {""};
    gchar server_group[64] = {""};
    guint has_master = 0;
    guint i = 0;
    guint backends_num = 0;

    gchar *sql1 = "SELECT `MEMBER_HOST`, `MEMBER_PORT` FROM "
            "performance_schema.replication_group_members "
            "WHERE MEMBER_STATE = 'ONLINE' AND MEMBER_ID = "
            "(SELECT VARIABLE_VALUE FROM performance_schema.global_status "
            "WHERE VARIABLE_NAME = 'group_replication_primary_member')  ";
    gchar *sql2 = "SELECT `MEMBER_HOST`, `MEMBER_PORT` FROM "
            "performance_schema.replication_group_members "
            "WHERE MEMBER_STATE = 'ONLINE' AND MEMBER_ID <> "
            "(SELECT VARIABLE_VALUE FROM performance_schema.global_status "
            "WHERE VARIABLE_NAME = 'group_replication_primary_member')  ";

    backends_num = network_backends_count(bs);
    for (i = 0; i < backends_num; i++) {
        network_backend_t *backend = network_backends_get(bs, i);
        if (backend->state == BACKEND_STATE_MAINTAINING || backend->state == BACKEND_STATE_DELETED)
            continue;

        char *backend_addr = backend->addr->name->str;
        MYSQL *conn = get_mysql_connection(monitor, backend_addr);
        MYSQL_RES *rs_set = NULL;
        MYSQL_ROW row = NULL;
        gchar ip[ADDRESS_LEN] = {""};
        gchar old_master[ADDRESS_LEN] = {""};

        if(conn == NULL) {
            g_message("get connection failed. error: %d, text: %s, backend: %s",
                                                                 mysql_errno(conn), mysql_error(conn), backend_addr);
            continue;
        }

        if(mysql_real_query(conn, L(sql1))) {
            g_message("select primary info failed for group_replication. error: %d, text: %s, backend: %s",
                                           mysql_errno(conn), mysql_error(conn), backend_addr);
            continue;
        }

        rs_set = mysql_store_result(conn);
        if(rs_set == NULL) {
            g_message("get primary info result set failed for group_replication. error: %d, text: %s, backend: %s",
                                                       mysql_errno(conn), mysql_error(conn), backend_addr);
            continue;
        }

        row = mysql_fetch_row(rs_set);
        if(row == NULL || row[0] == NULL || row[1] == NULL) {
            if(backend->state != BACKEND_STATE_OFFLINE) {
                g_message("get primary info rows failed for group_replication. error: %d, text: %s, backend: %s",
                                                                                   mysql_errno(conn), mysql_error(conn), backend_addr);
            }
            mysql_free_result(rs_set);
            continue;
        }

        if((get_ip_by_name(row[0], ip) != 0) || ip[0] == '\0') {
            g_message("get master ip by name failed. error: %d, text: %s, backend: %s",
                                                                   mysql_errno(conn), mysql_error(conn), backend_addr);
            mysql_free_result(rs_set);
            continue;
        }

        memcpy(old_master, master_addr, strlen(master_addr));
        snprintf(master_addr, ADDRESS_LEN, "%s:%s", ip, row[1]);

        if(old_master[0] != '\0' && strcasecmp(old_master, master_addr) != 0) {
            g_warning("exists more than one masters.");
            mysql_free_result(rs_set);
            return ;
        } else if (old_master[0] != '\0' && strcasecmp(old_master, master_addr) == 0) {
            mysql_free_result(rs_set);
            continue;
        }

        mysql_free_result(rs_set);
        rs_set = NULL;

        if(master_addr[0] == '\0') {
            g_message("get master address failed. error: %d, text: %s, backend: %s",
                                                                   mysql_errno(conn), mysql_error(conn), backend_addr);
            continue;
        }

        if(strcasecmp(backend_addr, master_addr)) {
            conn = get_mysql_connection(monitor, master_addr);
            if(conn == NULL) {
                g_message("get connection failed. error: %d, text: %s, backend: %s",
                                                                    mysql_errno(conn), mysql_error(conn), master_addr);
                continue;
            }
        }

        if(mysql_real_query(conn, L(sql2))) {
            g_message("select slave info failed for group_replication. error: %d, text: %s, backend: %s",
                                           mysql_errno(conn), mysql_error(conn), master_addr);
            continue;
        }

        rs_set = mysql_store_result(conn);
        if(rs_set == NULL) {
            g_message("get slave info result set failed for group_replication. error: %d, text: %s",
                                                       mysql_errno(conn), mysql_error(conn));
            continue;
        }
        while(row=mysql_fetch_row(rs_set)) {
            memset(ip, 0, ADDRESS_LEN);
            if((get_ip_by_name(row[0], ip) != 0) || ip[0] == '\0') {
                g_message("get slave ip by name failed. error: %d, text: %s",
                                                       mysql_errno(conn), mysql_error(conn));
                continue;
            }
            memset(slave_addr, 0, ADDRESS_LEN);
            snprintf(slave_addr, ADDRESS_LEN, "%s:%s", ip, row[1]);
            if(slave_addr[0] != '\0') {
                slave_list = g_list_append(slave_list, strdup(slave_addr));
                g_debug("add slave %s in list, %d", slave_addr, g_list_length(slave_list));
            } else {
                g_message("get slave address failed. error: %d, text: %s",
                                                       mysql_errno(conn), mysql_error(conn));
            }
        }
        mysql_free_result(rs_set);
    }

    backends_num = network_backends_count(bs);
    for (i = 0; i < backends_num; i++) {
        network_backend_t *backend = network_backends_get(bs, i);

        char *backend_addr = backend->addr->name->str;

        if(server_group[0] == '\0' && backend->server_group && backend->server_group->len) {
            snprintf(server_group, 32, "%s", backend->server_group->str);
        }
        if(backend->type == BACKEND_TYPE_RW) {
            has_master++;
            if(!strcasecmp(backend_addr, master_addr)) {
                if(backend->state == BACKEND_STATE_OFFLINE) {
                    network_backends_modify(bs, i, BACKEND_TYPE_RW, BACKEND_STATE_UNKNOWN, NO_PREVIOUS_STATE);
                }
                break;
            }
            GList *it = g_list_find_custom(slave_list, backend_addr, slave_list_compare);
            if(it) {
                if(backend->state == BACKEND_STATE_OFFLINE) {
                    network_backends_modify(bs, i, BACKEND_TYPE_RO, BACKEND_STATE_UNKNOWN, NO_PREVIOUS_STATE);
                } else {
                    network_backends_modify(bs, i, BACKEND_TYPE_RO, backend->state, NO_PREVIOUS_STATE);
                }
                slave_list = g_list_remove_link(slave_list, it);
                g_free(it->data);
                g_list_free(it);
            } else {
                if(backend->state != BACKEND_STATE_MAINTAINING && backend->state != BACKEND_STATE_DELETED) {
                    network_backends_modify(bs, i, BACKEND_TYPE_RO, BACKEND_STATE_OFFLINE, NO_PREVIOUS_STATE);
                }
            }
            break;
        }
    }

    backends_num = network_backends_count(bs);
    for (i = 0; i < backends_num; i++) {
        network_backend_t *backend = network_backends_get(bs, i);

        char *backend_addr = backend->addr->name->str;

        if(server_group[0] == '\0' && backend->server_group && backend->server_group->len) {
            snprintf(server_group, 32, "%s", backend->server_group->str);
        }
        if(backend->type == BACKEND_TYPE_RO || backend->type == BACKEND_TYPE_UNKNOWN) {
            GList *it = g_list_find_custom(slave_list, backend_addr, slave_list_compare);
            if(it) {
                if(backend->state == BACKEND_STATE_OFFLINE) {
                    network_backends_modify(bs, i, BACKEND_TYPE_RO, BACKEND_STATE_UNKNOWN, NO_PREVIOUS_STATE);
                } else {
                    network_backends_modify(bs, i, BACKEND_TYPE_RO, backend->state, NO_PREVIOUS_STATE);
                }
                slave_list = g_list_remove_link(slave_list, it);
                g_free(it->data);
                g_list_free(it);
            } else {
                if(master_addr[0] != '\0' && !strcasecmp(backend_addr, master_addr)) {
                    if(backend->state == BACKEND_STATE_OFFLINE) {
                        network_backends_modify(bs, i, BACKEND_TYPE_RW, BACKEND_STATE_UNKNOWN, NO_PREVIOUS_STATE);
                    } else {
                        network_backends_modify(bs, i, BACKEND_TYPE_RW, backend->state, NO_PREVIOUS_STATE);
                    }
                    has_master++;
                } else {
                    if(backend->state != BACKEND_STATE_MAINTAINING && backend->state != BACKEND_STATE_DELETED) {
                        network_backends_modify(bs, i, BACKEND_TYPE_RO, BACKEND_STATE_OFFLINE, NO_PREVIOUS_STATE);
                    }
                }
            }
        }
    }

    if(!has_master && master_addr[0] != '\0') {
        if(server_group[0] != '\0') {
            gchar master_addr_temp[ADDRESS_LEN] = {""};
            snprintf(master_addr_temp, ADDRESS_LEN, "%s@%s", master_addr, server_group);
            network_backends_add(bs, master_addr_temp, BACKEND_TYPE_RW, BACKEND_STATE_UP, monitor->chas);
        } else {
            network_backends_add(bs, master_addr, BACKEND_TYPE_RW, BACKEND_STATE_UP, monitor->chas);
        }
    }

    if(g_list_length(slave_list)) {
        GList *it = NULL;
        for(it = slave_list; it; it = it->next) {
            if(server_group[0] != '\0') {
                gchar slave_addr_temp[ADDRESS_LEN] = {""};
                snprintf(slave_addr_temp, ADDRESS_LEN, "%s@%s", (char *)(it->data), server_group);
                network_backends_add(bs, slave_addr_temp, BACKEND_TYPE_RO, BACKEND_STATE_UNKNOWN, monitor->chas);
            } else {
                network_backends_add(bs, it->data, BACKEND_TYPE_RO, BACKEND_STATE_UNKNOWN, monitor->chas);
            }
        }
    }

    g_list_free_full(slave_list, g_free);
}

#define ADD_MONITOR_TIMER(ev_struct, ev_cb, timeout) \
    ev_now_update((struct ev_loop *) monitor->evloop);\
    evtimer_set(&(monitor->ev_struct), ev_cb, monitor);\
    event_base_set(monitor->evloop, &(monitor->ev_struct));\
    evtimer_add(&(monitor->ev_struct), &timeout);

gint
check_hostname(network_backend_t *backend)
{
     gint ret = 0;
     if (!backend) return ret;

     gchar old_addr[INET_ADDRSTRLEN] = {""};
     inet_ntop(AF_INET, &(backend->addr->addr.ipv4.sin_addr), old_addr, sizeof(old_addr));
     if (0 != network_address_set_address(backend->addr, backend->address->str)) {
         return ret;
     }
     char new_addr[INET_ADDRSTRLEN] = {""};
     inet_ntop(AF_INET, &(backend->addr->addr.ipv4.sin_addr), new_addr, sizeof(new_addr));
     if (strcmp (old_addr, new_addr) != 0) {
         ret = 1;
     }
     return ret;
 }

static void
check_backend_alive(int fd, short what, void *arg)
{
    cetus_monitor_t *monitor = arg;
    chassis *chas = monitor->chas;

    int i;
    network_backends_t *bs = chas->priv->backends;

    if(chas->group_replication_mode ==1) {
        group_replication_detect(bs, monitor);
    }

    int backends_num = network_backends_count(bs);
    for (i = 0; i < backends_num; i++) {
        network_backend_t *backend = network_backends_get(bs, i);
        backend_state_t oldstate = backend->state;
        gint ret = 0;
        if (backend->state == BACKEND_STATE_DELETED || backend->state == BACKEND_STATE_MAINTAINING || backend->state == BACKEND_STATE_OFFLINE)
            continue;

        char *backend_addr = backend->addr->name->str;
        int check_count = 0;
        MYSQL *conn = NULL;
hostnameloop:
        while (++check_count <= CHECK_ALIVE_TIMES) {
            conn = get_mysql_connection(monitor, backend_addr);
            if (conn)
                break;
        }

        if (conn == NULL) {
            if (chas->check_dns) {
                if (check_hostname(backend)) {
                    goto hostnameloop;
                }
            }
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

    if(chas->group_replication_mode ==1) {
        group_replication_detect(bs, monitor);
    }

    /* Catch RW time 
     * Need a table to write from master and read from slave.
     * CREATE TABLE if not exists `tb_heartbeat` (
     *   `p_id` varchar(128) NOT NULL,
     *   `p_ts` timestamp(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
     *   PRIMARY KEY (`p_id`)
     * ) ENGINE = InnoDB DEFAULT CHARSET = utf8;
     */
    int backends_num = network_backends_count(bs);
    for (i = 0; i < backends_num; i++) {
        network_backend_t *backend = network_backends_get(bs, i);
        backend_state_t oldstate = backend->state;
        gint ret = 0;
        if (backend->state == BACKEND_STATE_DELETED || backend->state == BACKEND_STATE_MAINTAINING || backend->state == BACKEND_STATE_OFFLINE)
            continue;

        if (backend->type == BACKEND_TYPE_RW) {
            static char sql[1024];
            char *cur_time_str = get_current_sys_timestr();
            snprintf(sql, sizeof(sql), "INSERT INTO %s.tb_heartbeat (p_id, p_ts)"
                     " VALUES ('%s', '%s') ON DUPLICATE KEY UPDATE p_ts='%s'",
                     HEARTBEAT_DB, monitor->config_id, cur_time_str, cur_time_str);

            char *backend_addr = backend->addr->name->str;
hostnameloop:;
            MYSQL *conn = get_mysql_connection(monitor, backend_addr);
            if (conn == NULL) {
                if (chas->check_dns) {
                    if (check_hostname(backend)) {
                        goto hostnameloop;
                    }
                }   
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
    timeout.tv_usec = 10 * 1000;
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

    int backends_num = network_backends_count(bs);
    for (i = 0; i < backends_num; i++) {
        network_backend_t *backend = network_backends_get(bs, i);
        backend_state_t oldstate = backend->state;
        gint ret = 0;
        if (backend->type == BACKEND_TYPE_RW || backend->state == BACKEND_STATE_DELETED ||
            backend->state == BACKEND_STATE_MAINTAINING || backend->state == BACKEND_STATE_OFFLINE)
            continue;

        char *backend_addr = backend->addr->name->str;
hostnameloop:;
        MYSQL *conn = get_mysql_connection(monitor, backend_addr);
        if (conn == NULL) {
            if (chas->check_dns) {
                if (check_hostname(backend)) {
                    goto hostnameloop;
                }
            }
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
            if(!rs_set) {
                g_warning("fetch heartbeat result failed, errno:%d, errmsg:%s", mysql_errno(conn), mysql_error(conn));
            }
            MYSQL_ROW row = mysql_fetch_row(rs_set);
            double ts_slave = 0;
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
            }
            double delay_secs = G_MAXINT32/1000.0;
            if (ts_slave != 0) {
                struct timeval tv;
                gettimeofday(&tv, NULL);
                double ts_now = tv.tv_sec + ((double)tv.tv_usec) / 1000000;
                delay_secs = ts_now - ts_slave;
                backend->slave_delay_msec = (int)(delay_secs *1000);
            } else {
                backend->slave_delay_msec = G_MAXINT32;
            }
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

void
cetus_monitor_open(cetus_monitor_t *monitor, monitor_type_t monitor_type)
{
    struct timeval timeout;
    switch (monitor_type) {
    case MONITOR_TYPE_CHECK_ALIVE:
        timeout.tv_sec = 0;
        timeout.tv_usec = 1000;
        ADD_MONITOR_TIMER(check_alive_timer, check_backend_alive, timeout);
        g_message("check_alive monitor open.");
        break;
    case MONITOR_TYPE_CHECK_DELAY:
        timeout.tv_sec = 0;
        timeout.tv_usec = 1000;
        ADD_MONITOR_TIMER(write_master_timer, update_master_timestamp, timeout);
        g_message("check_slave monitor open.");
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
    chassis_event_loop(loop, NULL);

    g_message("monitor thread closing %d mysql conns", g_hash_table_size(monitor->backend_conns));
    g_hash_table_destroy(monitor->backend_conns);

    if (monitor->mysql_init_called) {
        mysql_thread_end();
        g_message("%s:mysql_thread_end is called", G_STRLOC);
    }

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
        g_clear_error(&error);
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

