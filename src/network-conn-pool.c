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

#include <glib.h>

#include "network-conn-pool.h"
#include "network-mysqld-packet.h"
#include "glib-ext.h"
#include "sys-pedantic.h"

/** @file
 * connection pools
 *
 * in the pool we manage idle connections
 * - keep them up as long as possible
 * - make sure we don't run out of seconds
 * - if the client is authed, we have to pick connection with the same user
 * - ...  
 */

/**
 * create a empty connection pool entry
 *
 * @return a connection pool entry
 */
network_connection_pool_entry *
network_connection_pool_entry_new(void)
{
    network_connection_pool_entry *e;

    e = g_new0(network_connection_pool_entry, 1);

    return e;
}

/**
 * free a conn pool entry
 *
 * @param e the pool entry to free
 * @param free_sock if true, the attached server-socket will be freed too
 */
void
network_connection_pool_entry_free(network_connection_pool_entry *e, gboolean free_sock)
{
    if (!e)
        return;

    if (e->sock && free_sock) {
        network_socket *sock = e->sock;
        network_socket_send_quit_and_free(sock);
    }

    g_free(e);
}

/**
 * free all pool entries of the queue
 *
 * used as GDestroyFunc in the user-hash of the pool
 *
 * @param q a GQueue to free
 *
 * @see network_connection_pool_new
 * @see GDestroyFunc
 */
static void
g_queue_free_all(gpointer q)
{
    GQueue *queue = q;
    network_connection_pool_entry *entry;

    while ((entry = g_queue_pop_head(queue))) {
        network_connection_pool_entry_free(entry, TRUE);
    }

    g_queue_free(queue);
}

/**
 * init a connection pool
 */
network_connection_pool *
network_connection_pool_new(void)
{
    network_connection_pool *pool;

    pool = g_new0(network_connection_pool, 1);

    pool->max_idle_connections = 20;
    pool->mid_idle_connections = 10;
    pool->min_idle_connections = 1;
    pool->cur_idle_connections = 0;
    pool->users = g_hash_table_new_full(g_hash_table_string_hash,
                                        g_hash_table_string_equal, g_hash_table_string_free, g_queue_free_all);

    return pool;
}

/**
 * free all entries of the pool
 *
 */
void
network_connection_pool_free(network_connection_pool *pool)
{
    if (!pool)
        return;

    g_hash_table_foreach_remove(pool->users, g_hash_table_true, NULL);

    g_hash_table_destroy(pool->users);

    g_free(pool);
}

/**
 * find the entry which has more than max_idle connections idling
 * 
 * @return TRUE for the first entry having more than _user_data idling connections
 * @see network_connection_pool_get_conns 
 */
static gboolean
find_idle_conns(gpointer UNUSED_PARAM(_key), gpointer _val, gpointer _user_data)
{
    guint idle_conns_threshold = *(gint *)_user_data;
    GQueue *conns = _val;

    g_debug("%s: conns length:%d, idle_conns_threshold:%d", G_STRLOC, conns->length, idle_conns_threshold);
    return (conns->length > idle_conns_threshold);
}

GQueue *
network_connection_pool_get_conns(network_connection_pool *pool, GString *username, int *is_robbed)
{
    GQueue *conns = NULL;

    if (username && username->len > 0) {
        conns = g_hash_table_lookup(pool->users, username);
        /**
         * if we know this use, return a authed connection 
         */
        g_debug("%s: get user-specific idling connection for '%s' -> %p", G_STRLOC, username->str, conns);
        if (conns && conns->length > 0) {
            return conns;
        }
    }

    /**
     * we don't have a entry yet, check the others if we have more than 
     * min_idle waiting
     */

    conns = g_hash_table_find(pool->users, find_idle_conns, &(pool->min_idle_connections));

    g_debug("%s: (get_conns) try to find max-idling conns for user '%s' -> %p",
            G_STRLOC, username ? username->str : "", conns);

    if (conns != NULL && is_robbed) {
        *is_robbed = 1;
    }

    return conns;
}

/**
 * get a connection from the pool
 *
 * make sure we have at least <min-conns> for each user
 * if we have more, reuse a connect to reauth it to another user
 *
 * @param pool connection pool to get the connection from
 * @param username (optional) name of the auth connection
 * @param default_db (unused) unused name of the default-db
 */
network_socket *
network_connection_pool_get(network_connection_pool *pool, GString *username, int *is_robbed)
{
    network_connection_pool_entry *entry = NULL;
    GQueue *conns = network_connection_pool_get_conns(pool, username, is_robbed);

    if (conns) {
        if (conns->length > 0) {
            entry = g_queue_pop_head(conns);
            g_debug("%s: (get) entry for user '%s' -> %p",
                    G_STRLOC, username ? username->str : "", entry);
        } else {
            g_debug("%s: conns length is zero for user '%s'", G_STRLOC, username ? username->str : "");
        }
    } else {
        g_debug("%s: conns is null for user '%s'", G_STRLOC, username ? username->str : "");
    }

    if (!entry) {
        g_debug("%s: (get) no entry for user '%s' -> %p", G_STRLOC, username ? username->str : "", conns);
        return NULL;
    }
    network_socket *sock = entry->sock;

    if (sock->recv_queue->chunks->length > 0) {
        g_warning("%s: server recv queue not empty", G_STRLOC);
    }

    g_debug("%s: recv queue length:%d, sock:%p", G_STRLOC, sock->recv_queue->chunks->length, sock);

    network_connection_pool_entry_free(entry, FALSE);

    g_debug("%s:event del, ev:%p", G_STRLOC, &(sock->event));
    /* remove the idle handler from the socket */
    event_del(&(sock->event));

    g_debug("%s: (get) got socket for user '%s' -> %p, charset:%s", G_STRLOC,
            username ? username->str : "", sock, sock->charset->str);

    if (sock->is_in_sess_context) {
        g_message("%s: conn is in sess context for user:'%s'", G_STRLOC, username ? username->str : "");
    }

    pool->cur_idle_connections--;
    g_debug("%s: cur_idle_connections sub:%d for sock:%p", G_STRLOC, pool->cur_idle_connections, sock);

    return sock;
}

/**
 * add a connection to the connection pool
 */
network_connection_pool_entry *
network_connection_pool_add(network_connection_pool *pool, network_socket *sock)
{
    if (!g_queue_is_empty(sock->recv_queue->chunks)) {
        g_warning("%s: server recv queue not empty", G_STRLOC);
    }
    if (!g_queue_is_empty(sock->recv_queue_raw->chunks)) {
        g_warning("%s: server recv queue raw not empty", G_STRLOC);
        network_queue_clear(sock->recv_queue_raw);
    }

    network_connection_pool_entry *entry;
    entry = network_connection_pool_entry_new();
    entry->sock = sock;
    entry->pool = pool;

    sock->is_authed = 1;

    g_debug("%s: (add) adding socket to pool for user '%s' -> %p", G_STRLOC, sock->response->username->str, sock);

    GQueue *conns = NULL;
    if (NULL == (conns = g_hash_table_lookup(pool->users, sock->response->username))) {
        conns = g_queue_new();
        g_hash_table_insert(pool->users, g_string_dup(sock->response->username), conns);
    }

    entry->conns = conns;

    g_queue_push_head(conns, entry);
    entry->link = conns->head;

    pool->cur_idle_connections++;
    g_debug("%s: add cur_idle_connections:%d for sock:%p", G_STRLOC, pool->cur_idle_connections, sock);

    return entry;
}

/**
 * remove the connection referenced by entry from the pool 
 */
void
network_connection_pool_remove(network_connection_pool_entry *entry)
{
    entry->pool->cur_idle_connections--;
    g_queue_delete_link(entry->conns, entry->link);
    network_connection_pool_entry_free(entry, TRUE);
}

gboolean
network_conn_pool_do_reduce_conns_verdict(network_connection_pool *pool, int connected_clients)
{
    if (pool->cur_idle_connections > pool->mid_idle_connections) {
        if (connected_clients < pool->cur_idle_connections) {
            return TRUE;
        }
    }

    return FALSE;
}

int
network_connection_pool_total_conns_count(network_connection_pool *pool)
{
    GHashTable *users = pool->users;
    int total = 0;
    if (users != NULL) {
        GHashTableIter iter;
        GString *key;
        GQueue *queue;
        g_hash_table_iter_init(&iter, users);
        /* count all users' pooled connections */
        while (g_hash_table_iter_next(&iter, (void **)&key, (void **)&queue)) {
            total += queue->length;
        }
    }

    if (pool->cur_idle_connections != total) {
        g_warning("%s: pool cur idle connections stat error:%d, total:%d", G_STRLOC, pool->cur_idle_connections, total);
        pool->cur_idle_connections = total;
    }

    return total;
}
