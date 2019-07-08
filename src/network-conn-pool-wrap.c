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

#ifdef HAVE_SYS_FILIO_H
#include <sys/filio.h>
#endif

#include <sys/ioctl.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#define ioctlsocket ioctl

#include <errno.h>

#include "glib-ext.h"

#include "network-mysqld.h"
#include "network-mysqld-packet.h"
#include "chassis-event.h"

#include "network-conn-pool.h"
#include "network-conn-pool-wrap.h"
#include "cetus-util.h"

/**
 * handle the events of a idling server connection in the pool
 *
 * make sure we know about connection close from the server side
 * - wait_timeout
 */
static void
network_mysqld_con_idle_handle(int event_fd, short events, void *user_data)
{
    g_debug("%s:visit network_mysqld_con_idle_handle", G_STRLOC);
    network_connection_pool_entry *pool_entry = user_data;
    network_connection_pool *pool = pool_entry->pool;

    if (events == EV_READ) {
        int b = -1;

        /**
         * @todo we have to handle the case that the server really sent use something
         *        up to now we just ignore it
         */
        if (ioctlsocket(event_fd, FIONREAD, &b)) {
            g_critical("ioctl(%d, FIONREAD) failed: %s", event_fd, g_strerror(errno));
        } else {
            if (b != 0) {
                g_critical("ioctl(%d, FIONREAD) said something to read, oops: %d", event_fd, b);
            }
            /* the server decided the close the connection (wait_timeout, crash, ... )
             *
             * remove us from the connection pool and close the connection */

            network_connection_pool_remove(pool_entry);
            if (pool->srv) {
                chassis *srv = pool->srv;
                srv->complement_conn_flag = 1;
            }

            g_debug("%s:the server decided to close the connection:%d for sock:%p",
                    G_STRLOC, pool_entry->pool->cur_idle_connections, pool_entry->sock);
        }
    } else if (events == EV_TIMEOUT) {
        if (pool->srv) {
            chassis *srv = pool->srv;
            srv->complement_conn_flag = 1;
        }
        network_connection_pool_remove(pool_entry);
        g_debug("%s:idle connection timeout:%d for sock:%p", G_STRLOC,
                pool_entry->pool->cur_idle_connections, pool_entry->sock);
    }
}

int
network_pool_add_idle_conn(network_connection_pool *pool, chassis *srv, network_socket *server)
{
    network_connection_pool_entry *pool_entry = NULL;
    pool_entry = network_connection_pool_add(pool, server);
    event_set(&(server->event), server->fd, EV_READ, network_mysqld_con_idle_handle, pool_entry);
    int surplus_time = srv->current_time - server->create_time;
    surplus_time = srv->max_alive_time - surplus_time + g_random_int_range(0, 240);

    if (surplus_time < 60) {
        surplus_time = 60;
    }

    struct timeval timeout;
    timeout.tv_sec = surplus_time;
    timeout.tv_usec = 0;
    g_debug("%s: ev:%p add network_mysqld_con_idle_handle for server:%p, fd:%d, timeout:%d",
            G_STRLOC, &(server->event), server, server->fd, surplus_time);

    chassis_event_add_with_timeout(srv, &(server->event), &timeout);

    return 0;
}

/**
 * move the con->server into connection pool and disconnect the
 * proxy from its backend *only RW-edition
 */
int
network_pool_add_conn(network_mysqld_con *con, int is_swap)
{
    proxy_plugin_con_t *st = con->plugin_con_state;
    chassis *srv = con->srv;
    chassis_private *g = srv->priv;

    /* con-server is already disconnected, got out */
    if (!con->server)
        return -1;

    if (!con->server->response) {
        g_warning("%s: server response is null:%p", G_STRLOC, con);
        return -1;
    }

    if (st->backend == NULL) {
        g_warning("%s: st backend is null:%p", G_STRLOC, con);
        return -1;
    }

    if (con->prepare_stmt_count > 0) {
        g_debug("%s: con valid_prepare_stmt_cnt:%d for con:%p", G_STRLOC, con->prepare_stmt_count, con);
        return -1;
    }

    if (con->is_in_sess_context) {
        if (st->backend->type == BACKEND_TYPE_RW) {
            g_message("%s: transact feature is changed:%p", G_STRLOC, con);
            return -1;
        } else {
            g_message("%s: now transact feature is changed, orig is read server:%p", G_STRLOC, con);
        }
    }

    if (con->server->is_in_sess_context) {
        g_message("%s: server is in sess context true:%p", G_STRLOC, con);
        return -1;
    }

    gboolean to_be_put_to_pool = TRUE;

    if (con->server_in_tran_and_auto_commit_received) {
        g_critical("%s:server in tran and auto commit and put to pool:%p",
                G_STRLOC, con);
        to_be_put_to_pool = FALSE;
    }

    if (to_be_put_to_pool == TRUE && !is_swap && con->servers == NULL) {
        if (con->srv->is_reduce_conns) {
            if (network_conn_pool_do_reduce_conns_verdict(st->backend->pool, st->backend->connected_clients)) {
                to_be_put_to_pool = FALSE;
            }
        }
    }

    if (to_be_put_to_pool == FALSE) {
        if (con->server->recv_queue->chunks->length > 0) {
            g_critical("%s: recv queue length :%d, state:%s",
                       G_STRLOC, con->server->recv_queue->chunks->length, network_mysqld_con_st_name(con->state));
        }

        GString *packet;
        while ((packet = g_queue_pop_head(con->server->recv_queue->chunks))) {
            g_string_free(packet, TRUE);
        }

        st->backend->connected_clients--;

        network_socket_send_quit_and_free(con->server);

        st->backend = NULL;
        st->backend_ndx = -1;
        con->server = NULL;

        return -1;
    }

    con->server->is_authed = 1;

    if (con->servers != NULL) {
        int i, checked = 0;
        network_socket *server;
        network_backend_t *backend;

        for (i = 0; i < MAX_SERVER_NUM_FOR_PREPARE; i++) {

            if (st->backend_ndx_array == NULL) {
                g_message("%s: st backend ndx array is null:%p", G_STRLOC, con);
            } else {
                if (st->backend_ndx_array[i] <= 0) {
                    g_message("%s: i:%d backend_ndx_array value:%d for con:%p",
                            G_STRLOC, i, st->backend_ndx_array[i], con);
                    continue;
                }

                int index = st->backend_ndx_array[i] - 1;
                server = g_ptr_array_index(con->servers, index);
                backend = network_backends_get(g->backends, i);
                CHECK_PENDING_EVENT(&(server->event));

                if (con->srv->server_conn_refresh_time <= server->create_time) {
                    g_debug("%s: add conn fd:%d to pool:%p ", G_STRLOC, server->fd, backend->pool);
                    server->is_multi_stmt_set = con->client->is_multi_stmt_set;
                    network_pool_add_idle_conn(backend->pool, con->srv, server);
                } else {
                    g_message("%s: old connection for con:%p", G_STRLOC, con);
                    network_socket_send_quit_and_free(server);
                    con->srv->complement_conn_flag = 1;
                }

                backend->connected_clients--;
                g_debug("%s, con:%p, backend ndx:%d:connected_clients sub, clients:%d",
                        G_STRLOC, con, st->backend_ndx_array[i], backend->connected_clients);
                checked++;
                if (checked >= con->servers->len) {
                    break;
                }
            }
        }

        g_ptr_array_free(con->servers, TRUE);
        con->servers = NULL;
        con->multiple_server_mode = 0;

        if (st->backend_ndx_array) {
            g_free(st->backend_ndx_array);
            st->backend_ndx_array = NULL;
        }

    } else {
        con->is_prepared = 0;
        con->prepare_stmt_count = 0;
        g_debug("%s: con:%p, set prepare_stmt_count 0", G_STRLOC, con);
        CHECK_PENDING_EVENT(&(con->server->event));

        if (con->srv->server_conn_refresh_time <= con->server->create_time) {
            g_debug("%s: add conn fd:%d to pool:%p ", G_STRLOC, con->server->fd, st->backend->pool);
            con->server->is_multi_stmt_set = con->client->is_multi_stmt_set;
            network_pool_add_idle_conn(st->backend->pool, con->srv, con->server);
        } else {
            g_message("%s: old connection for con:%p", G_STRLOC, con);
            network_socket_send_quit_and_free(con->server);
            con->srv->complement_conn_flag = 1;
        }

        st->backend->connected_clients--;
        g_debug("%s, con:%p, backend ndx:%d:connected_clients sub, clients:%d",
                G_STRLOC, con, st->backend_ndx, st->backend->connected_clients);
    }

    st->backend = NULL;
    st->backend_ndx = -1;

    con->server = NULL;

    return 0;
}

void
mysqld_con_reserved_connections_add(network_mysqld_con *con, network_socket *sock, int backend_idx)
{
    proxy_plugin_con_t *st = con->plugin_con_state;
    if (st->backend_ndx_array == NULL) {
        st->backend_ndx_array = g_new0(short, MAX_SERVER_NUM_FOR_PREPARE);
        st->backend_ndx_array[st->backend_ndx] = 1; /* current sock index = 0 */
        g_debug("%s: set st->backend_ndx:%d ndx array 1", G_STRLOC, st->backend_ndx);
    }

    if (con->servers == NULL) {
        con->servers = g_ptr_array_new();
        g_ptr_array_add(con->servers, con->server); /* current sock index = 0 */
        con->multiple_server_mode = 1;  /* TODO: redundant var */
    }
    g_ptr_array_add(con->servers, sock);
    st->backend_ndx_array[backend_idx] = con->servers->len;
    g_debug("%s: set backend_ndx:%d ndx array:%d", G_STRLOC, backend_idx, con->servers->len);
}

network_socket *
mysqld_con_reserved_connections_get(network_mysqld_con *con, int backend_idx)
{
    proxy_plugin_con_t *st = con->plugin_con_state;
    if (con->servers) {
        g_debug("%s: backend_idx:%d backend_ndx_array value:%d for con:%p",
                            G_STRLOC, backend_idx, st->backend_ndx_array[backend_idx], con);
        int conn_idx = st->backend_ndx_array[backend_idx];
        if (conn_idx > 0) {
            conn_idx -= 1;
            network_socket *sock = g_ptr_array_index(con->servers, conn_idx);
            return sock;
        }
    }
    return NULL;
}

/**
 * swap the server connection with a connection from
 * the connection pool , only RW-edition
 *
 * we can only switch backends if we have an authed connection in the pool.
 *
 * @return NULL if swapping failed
 *         the new backend on success
 */
network_socket *
network_connection_pool_swap(network_mysqld_con *con, int backend_ndx)
{
    network_backend_t *backend = NULL;
    proxy_plugin_con_t *st = con->plugin_con_state;
    chassis_private *g = con->srv->priv;

    /*
     * we can only change to another backend if the backend is already
     * in the connection pool and connected
     */
    backend = network_backends_get(g->backends, backend_ndx);
    if (!backend)
        return NULL;

    g_debug("%s: check server switch for conn:%p, prep_stmt_cnt:%d, orig ndx:%d, now:%d",
            G_STRLOC, con, con->prepare_stmt_count, st->backend_ndx, backend_ndx);
    /**
     * TODO only valid for successful prepare statements, not valid for data partition
     */
    gboolean server_switch_need_add = FALSE;
    if (st->backend_ndx != -1 && con->prepare_stmt_count > 0 && st->backend_ndx != backend_ndx) {
        if (backend->type == BACKEND_TYPE_RW || con->use_slave_forced) {
            server_switch_need_add = TRUE;
            g_debug("%s: server switch is true", G_STRLOC);
        } else {
            if (backend->type == BACKEND_TYPE_RO) {
                g_debug("%s: use orig server", G_STRLOC);
                return NULL;
            }
        }
        /* first try reserved connections, before query the pool */
        network_socket *sock = mysqld_con_reserved_connections_get(con, backend_ndx);
        if (sock) {
            return sock;
        }
    }

    /**
     * get a connection from the pool which matches our basic requirements
     * - username has to match
     */
    int is_robbed = 0;
    GString empty_name = { "", 0, 0 };
    GString *name = con->client->response ? con->client->response->username : &empty_name;
    network_socket *sock = network_connection_pool_get(backend->pool, name, &is_robbed);
    if (sock == NULL) {
        if (server_switch_need_add) {
            g_message("%s:retrieve master conn failed, but still hold read server", G_STRLOC);
            return NULL;
        }

        if (con->server) {
            if (network_pool_add_conn(con, 1) != 0) {
                g_warning("%s: move the curr conn back into the pool failed:%p",
                        G_STRLOC, con);
                return NULL;
            }
        }
        st->backend_ndx = -1;
        st->backend = NULL;
        con->server = NULL;
        return NULL;
    }
    con->rob_other_conn = is_robbed;

    if (server_switch_need_add) {
        mysqld_con_reserved_connections_add(con, sock, backend_ndx);
    } else {
        if (con->server) {
            if (network_pool_add_conn(con, 1) != 0) {
                g_debug("%s: take and move the current backend into the pool failed", G_STRLOC);
                return NULL;
            } else {
                g_debug("%s: take and move the current backend into the pool", G_STRLOC);
            }
        }
    }

    /* connect to the new backend */
    st->backend = backend;
    st->backend->connected_clients++;
    st->backend_ndx = backend_ndx;

    g_debug("%s, con:%p, backend ndx:%d:connected_clients add, clients:%d, sock:%p",
            G_STRLOC, con, backend_ndx, st->backend->connected_clients, sock);

    return sock;
}
