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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <sys/uio.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/uio.h>            /* writev */
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif

#ifdef HAVE_SYS_FILIO_H
#include <sys/filio.h>
#endif

#include <arpa/inet.h> /** inet_ntoa */
#include <netinet/in.h>
#include <netinet/tcp.h>

#include <netdb.h>
#include <unistd.h>

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/version.h>
#include <linux/filter.h>


#ifdef HAVE_WRITEV
#define USE_BUFFERED_NETIO
#else
#undef USE_BUFFERED_NETIO
#endif

#define E_NET_CONNRESET ECONNRESET
#define E_NET_CONNABORTED ECONNABORTED
#define E_NET_INPROGRESS EINPROGRESS
#if EWOULDBLOCK == EAGAIN
/**
 * some system make EAGAIN == EWOULDBLOCK which would lead to a 
 * error in the case handling
 *
 * set it to -1 as this error should never happen
 */
#define E_NET_WOULDBLOCK -1
#else
#define E_NET_WOULDBLOCK EWOULDBLOCK
#endif

#include "network-socket.h"
#include "network-mysqld-proto.h"
#include "network-mysqld-packet.h"
#include "cetus-util.h"
#include "network-compress.h"
#include "glib-ext.h"
#include "network-ssl.h"

network_socket *
network_socket_new()
{
    network_socket *s;

    s = g_new0(network_socket, 1);

    s->send_queue = network_queue_new();
    s->send_queue_compressed = network_queue_new();

    s->recv_queue = network_queue_new();
    s->recv_queue_raw = network_queue_new();
    s->recv_queue_uncompress_raw = network_queue_new();
    s->recv_queue_decrypted_raw = network_queue_new();

    s->default_db = g_string_new(NULL);
    s->username = g_string_new(NULL);
    s->charset = g_string_new(NULL);
    s->charset_client = g_string_new(NULL);
    s->charset_connection = g_string_new(NULL);
    s->charset_results = g_string_new(NULL);
    s->sql_mode = g_string_new(NULL);

    s->fd = -1;
    s->socket_type = SOCK_STREAM;   /* let's default to TCP */
    s->packet_id_is_reset = TRUE;

    s->src = network_address_new();
    s->dst = network_address_new();
    s->create_time = time(0);
    s->update_time = s->create_time;

    return s;
}

void
network_socket_send_quit_and_free(network_socket *s)
{
    int len = NET_HEADER_SIZE + 1;
    GString *new_packet = g_string_sized_new(len);
    new_packet->len = NET_HEADER_SIZE;
    g_string_append_c(new_packet, (char)COM_QUIT);
    network_mysqld_proto_set_packet_id(new_packet, 0);
    network_mysqld_proto_set_packet_len(new_packet, 1);
    g_queue_push_tail(s->send_queue->chunks, new_packet);

    network_socket_write(s, -1);

    network_socket_free(s);
}

void
network_socket_free(network_socket *s)
{
    if (!s)
        return;

    if (s->last_compressed_packet) {
        g_string_free(s->last_compressed_packet, TRUE);
        s->last_compressed_packet = NULL;
    }
    network_queue_free(s->send_queue);
    network_queue_free(s->send_queue_compressed);
    network_queue_free(s->recv_queue);
    network_queue_free(s->recv_queue_raw);
    network_queue_free(s->recv_queue_uncompress_raw);
    network_queue_free(s->recv_queue_decrypted_raw);

    if (s->response)
        network_mysqld_auth_response_free(s->response);
    if (s->challenge)
        network_mysqld_auth_challenge_free(s->challenge);

    network_address_free(s->dst);
    network_address_free(s->src);

    if (s->event.ev_base) {     /* if .ev_base isn't set, the event never got added */
        event_del(&(s->event));
    }
#ifdef HAVE_OPENSSL
    network_ssl_free_connection(s);
#endif
    if (s->fd != -1) {
        closesocket(s->fd);
    }

    g_string_free(s->default_db, TRUE);
    g_string_free(s->charset_client, TRUE);
    g_string_free(s->charset_connection, TRUE);
    g_string_free(s->charset, TRUE);
    g_string_free(s->charset_results, TRUE);
    g_string_free(s->username, TRUE);
    g_string_free(s->sql_mode, TRUE);

    g_free(s);
}

/**
 * portable 'set non-blocking io'
 *
 * @param sock    a socket
 * @return        NETWORK_SOCKET_SUCCESS on success, NETWORK_SOCKET_ERROR on error
 */
network_socket_retval_t
network_socket_set_non_blocking(network_socket *sock)
{
    int ret;
    ret = fcntl(sock->fd, F_SETFL, O_NONBLOCK | O_RDWR);
    if (ret != 0) {
        g_critical("%s: set_non_blocking() failed: %s (%d)", G_STRLOC, g_strerror(errno), errno);
        return NETWORK_SOCKET_ERROR;
    }
    return NETWORK_SOCKET_SUCCESS;
}

network_socket_retval_t
network_socket_set_send_buffer_size(network_socket *sock, int size)
{

    if (setsockopt(sock->fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) != 0) {
        g_critical("%s: setsockopt SO_SNDBUF failed: %s (%d)", G_STRLOC, g_strerror(errno), errno);
        return NETWORK_SOCKET_ERROR;
    }

    return NETWORK_SOCKET_SUCCESS;
}

/**
 * accept a connection
 *
 * event handler for listening connections
 *
 * @param srv    a listening socket 
 * 
 */
network_socket *
network_socket_accept(network_socket *srv, int *reason)
{
    network_socket *client;

    g_return_val_if_fail(srv, NULL);
    /* accept() only works on stream sockets */
    g_return_val_if_fail(srv->socket_type == SOCK_STREAM, NULL);

    client = network_socket_new();
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 28)
    g_debug("%s: call accept4", G_STRLOC);
    if (-1 == (client->fd = accept4(srv->fd, &client->src->addr.common, &(client->src->len), SOCK_NONBLOCK))) {
        *reason = errno;
        network_socket_free(client);
        return NULL;
    }
#else
    if (-1 == (client->fd = accept(srv->fd, &client->src->addr.common, &(client->src->len)))) {
        *reason = errno;
        network_socket_free(client);
        return NULL;
    }
    network_socket_set_non_blocking(client);
#endif

    if (network_address_refresh_name(client->src)) {
        network_socket_free(client);
        return NULL;
    }

    /* 
     * the listening side may be INADDR_ANY, 
     * let's get which address the client really connected to
     */
    if (-1 == getsockname(client->fd, &client->dst->addr.common, &(client->dst->len))) {
        network_address_reset(client->dst);
    } else if (network_address_refresh_name(client->dst)) {
        network_address_reset(client->dst);
    }

    return client;
}

static network_socket_retval_t
network_socket_connect_setopts(network_socket *sock)
{
    int val;
    /**
     * set the same options as the mysql client 
     */
#ifdef IP_TOS
    val = 8;
    if (setsockopt(sock->fd, IPPROTO_IP, IP_TOS, &val, sizeof(val)) != 0) {
        g_critical("%s: setsockopt IP_TOS failed: %s (%d)", G_STRLOC, g_strerror(errno), errno);
    }
#endif
    val = 1;
    if (setsockopt(sock->fd, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val)) != 0) {
        g_critical("%s: setsockopt TCP_NODELAY failed: %s (%d)", G_STRLOC, g_strerror(errno), errno);
    }
    val = 1;
    if (setsockopt(sock->fd, SOL_SOCKET, SO_KEEPALIVE, &val, sizeof(val)) != 0) {
        g_critical("%s: setsockopt SO_KEEPALIVE failed: %s (%d)", G_STRLOC, g_strerror(errno), errno);
    }
    val = 30;
    if (setsockopt(sock->fd, SOL_TCP, TCP_KEEPIDLE, &val, sizeof(val)) != 0) {
        g_critical("%s: setsockopt TCP_KEEPIDLE failed: %s (%d)", G_STRLOC, g_strerror(errno), errno);
    }
    val = 5;
    if (setsockopt(sock->fd, SOL_TCP, TCP_KEEPINTVL, &val, sizeof(val)) != 0) {
        g_critical("%s: setsockopt TCP_KEEPINTVL failed: %s (%d)", G_STRLOC, g_strerror(errno), errno);
    }
    val = 3;
    if (setsockopt(sock->fd, SOL_TCP, TCP_KEEPCNT, &val, sizeof(val)) != 0) {
        g_critical("%s: setsockopt TCP_KEEPCNT failed: %s (%d)", G_STRLOC, g_strerror(errno), errno);
    }

    /* 
     * the listening side may be INADDR_ANY, 
     * let's get which address the client really connected to 
     */
    if (-1 == getsockname(sock->fd, &sock->src->addr.common, &(sock->src->len))) {
        g_debug("%s: getsockname() failed: %s (%d)", G_STRLOC, g_strerror(errno), errno);
        network_address_reset(sock->src);
    } else if (network_address_refresh_name(sock->src)) {
        g_debug("%s: network_address_refresh_name() failed", G_STRLOC);
        network_address_reset(sock->src);
    }

    return NETWORK_SOCKET_SUCCESS;
}

/**
 * finish the non-blocking connect()
 *
 * sets 'errno' as if connect() would have failed
 *
 */
network_socket_retval_t
network_socket_connect_finish(network_socket *sock)
{
    int so_error = 0;
    network_socklen_t so_error_len = sizeof(so_error);

    /**
     * we might get called a 2nd time after a connect() == EINPROGRESS
     */
    if (getsockopt(sock->fd, SOL_SOCKET, SO_ERROR, &so_error, &so_error_len)) {
        /* getsockopt failed */
        g_critical("%s: getsockopt(%s) failed: %s (%d)", G_STRLOC, sock->dst->name->str, g_strerror(errno), errno);
        return NETWORK_SOCKET_ERROR;
    }

    switch (so_error) {
    case 0:
        network_socket_connect_setopts(sock);

        return NETWORK_SOCKET_SUCCESS;
    default:
        errno = so_error;

        return NETWORK_SOCKET_ERROR_RETRY;
    }
}

/**
 * connect a socket
 *
 * the sock->addr has to be set before 
 * 
 * @param sock    a socket 
 * @return        NETWORK_SOCKET_SUCCESS on connected, 
 *                NETWORK_SOCKET_ERROR on error, 
 *                NETWORK_SOCKET_ERROR_RETRY for try again
 * @see network_address_set_address()
 */
network_socket_retval_t
network_socket_connect(network_socket *sock)
{
    /* our _new() allocated it already */
    g_return_val_if_fail(sock->dst, NETWORK_SOCKET_ERROR);
    /* we want to use the ->name in the error-msgs */
    g_return_val_if_fail(sock->dst->name->len, NETWORK_SOCKET_ERROR);
    /* we already have a valid fd, we don't want to leak it */
    g_return_val_if_fail(sock->fd < 0, NETWORK_SOCKET_ERROR);
    g_return_val_if_fail(sock->socket_type == SOCK_STREAM, NETWORK_SOCKET_ERROR);

    /**
     * create a socket for the requested address
     *
     * if the dst->addr isn't set yet, socket() will fail with unsupported type
     */
    if (-1 == (sock->fd = socket(sock->dst->addr.common.sa_family, sock->socket_type, 0))) {
        g_critical("%s: socket(%s) failed: %s (%d)", G_STRLOC, sock->dst->name->str, g_strerror(errno), errno);
        return NETWORK_SOCKET_ERROR;
    }

    /**
     * make the connect() call non-blocking
     *
     */
    network_socket_set_non_blocking(sock);

    if (-1 == connect(sock->fd, &sock->dst->addr.common, sock->dst->len)) {
        /**
         * in most TCP cases we connect() will return with 
         * EINPROGRESS ... 3-way handshake
         */
        switch (errno) {
        case E_NET_INPROGRESS:
        case E_NET_WOULDBLOCK:
            return NETWORK_SOCKET_ERROR_RETRY;
        default:
            g_critical("%s: connect(%s) failed: %s (%d)", G_STRLOC, sock->dst->name->str, g_strerror(errno), errno);
            return NETWORK_SOCKET_ERROR;
        }
    }

    network_socket_connect_setopts(sock);

    return NETWORK_SOCKET_SUCCESS;
}

#if defined(SO_REUSEPORT)
#ifdef BPF_ENABLED
static void attach_bpf(int fd) 
{
    struct sock_filter code[] = {
        /* A = raw_smp_processor_id() */
        { BPF_LD  | BPF_W | BPF_ABS, 0, 0, SKF_AD_OFF + SKF_AD_CPU },
        /* return A */
        { BPF_RET | BPF_A, 0, 0, 0 },
    };
    struct sock_fprog p = {
        .len = 2,
        .filter = code,
    };

    if (setsockopt(fd, SOL_SOCKET, SO_ATTACH_REUSEPORT_CBPF, &p, sizeof(p))) {
        g_critical("%s:failed to set SO_ATTACH_REUSEPORT_CBPF, err:%s",
                G_STRLOC, strerror(errno));
    }
}
#endif
#endif

/**
 * connect a socket
 *
 * the con->dst->addr has to be set before 
 * 
 * @param con    a socket 
 * @return       NETWORK_SOCKET_SUCCESS on connected, NETWORK_SOCKET_ERROR on error
 *
 * @see network_address_set_address()
 */
network_socket_retval_t
network_socket_bind(network_socket *con,  int advanced_mode)
{
    /* 
     * HPUX:       int setsockopt(int s, int level, int optname, 
     *                            const void *optval, int optlen);
     * all others: int setsockopt(int s, int level, int optname, 
     *                            const void *optval, socklen_t optlen);
     */
#define SETSOCKOPT_OPTVAL_CAST (void *)

    /* socket is already bound */
    g_return_val_if_fail(con->fd < 0, NETWORK_SOCKET_ERROR);
    g_return_val_if_fail((con->socket_type == SOCK_DGRAM) || (con->socket_type == SOCK_STREAM), NETWORK_SOCKET_ERROR);

    if (con->socket_type == SOCK_STREAM) {
        g_return_val_if_fail(con->dst, NETWORK_SOCKET_ERROR);
        g_return_val_if_fail(con->dst->name->len > 0, NETWORK_SOCKET_ERROR);

        if (-1 == (con->fd = socket(con->dst->addr.common.sa_family, con->socket_type, 0))) {
            g_critical("%s: socket(%s) failed: %s (%d)", G_STRLOC, con->dst->name->str, g_strerror(errno), errno);
            return NETWORK_SOCKET_ERROR;
        }

        if (con->dst->addr.common.sa_family == AF_INET || con->dst->addr.common.sa_family == AF_INET6) {
            int val;

            val = 1;
            if (0 != setsockopt(con->fd, IPPROTO_TCP, TCP_NODELAY, SETSOCKOPT_OPTVAL_CAST & val, sizeof(val))) {
                g_critical("%s: setsockopt(%s, IPPROTO_TCP, TCP_NODELAY) failed: %s (%d)",
                           G_STRLOC, con->dst->name->str, g_strerror(errno), errno);
                return NETWORK_SOCKET_ERROR;
            }

            if (0 != setsockopt(con->fd, SOL_SOCKET, SO_REUSEADDR, SETSOCKOPT_OPTVAL_CAST & val, sizeof(val))) {
                g_critical("%s: setsockopt(%s, SOL_SOCKET, SO_REUSEADDR) failed: %s (%d)",
                           G_STRLOC, con->dst->name->str, g_strerror(errno), errno);
                return NETWORK_SOCKET_ERROR;
            }

#if defined(SO_REUSEPORT)
            if (advanced_mode) {
                g_message("%s:set SO_REUSEPORT for fd:%d", G_STRLOC, con->fd);
                if (0 != setsockopt(con->fd, SOL_SOCKET, SO_REUSEPORT, SETSOCKOPT_OPTVAL_CAST & val, sizeof(val))) {
                    g_critical("%s: setsockopt(%s, SOL_SOCKET, SO_REUSEPORT) failed: %s (%d)",
                            G_STRLOC, con->dst->name->str, g_strerror(errno), errno);
                    return NETWORK_SOCKET_ERROR;
                }

#ifdef BPF_ENABLED
                attach_bpf(con->fd);
#endif
            }
#endif
        }

        if (con->dst->addr.common.sa_family == AF_INET6) {
#ifdef IPV6_V6ONLY
            /* disable dual-stack IPv4-over-IPv6 sockets
             *
             * ... if it is supported:
             * - Linux
             * - Mac OS X
             * - FreeBSD
             * - Solaris 10 and later
             *
             * no supported on:
             * - Solaris 9 and earlier
             */

            /* IPV6_V6ONLY is int on unix */
            int val;

            val = 0;
            if (0 != setsockopt(con->fd, IPPROTO_IPV6, IPV6_V6ONLY, SETSOCKOPT_OPTVAL_CAST & val, sizeof(val))) {
                g_critical("%s: setsockopt(%s, IPPROTO_IPV6, IPV6_V6ONLY) failed: %s (%d)",
                           G_STRLOC, con->dst->name->str, g_strerror(errno), errno);
                return NETWORK_SOCKET_ERROR;
            }
#endif
        }

        if (-1 == bind(con->fd, &con->dst->addr.common, con->dst->len)) {
            /* binding failed so the address/socket is already being used
             * let's check if we can connect to it so we check if is being used 
             * by some app
             */
            if (-1 == connect(con->fd, &con->dst->addr.common, con->dst->len)) {
                g_debug("%s: connect(%s) failed: %s (%d)", G_STRLOC, con->dst->name->str, g_strerror(errno), errno);
                /* we can't connect to the socket so no one is listening on it. We need
                 * to unlink it (delete the name from the file system) to be able to
                 * re-use it.
                 * network_address_free does the unlink, but to re-use it we need
                 * to store the pathname associated with the socket before unlink it and
                 * create a new socket with it.
                 */
                gchar *address_copy = g_strdup(con->dst->name->str);
                con->dst->can_unlink_socket = TRUE;
                network_address_free(con->dst);

                con->dst = network_address_new();

                if (network_address_set_address(con->dst, address_copy) == -1) {
                    g_free(address_copy);
                    return NETWORK_SOCKET_ERROR;
                }

                /* we can now free the address copy */
                g_free(address_copy);

                g_debug("%s: retrying to bind(%s)", G_STRLOC, con->dst->name->str);

                /* let's bind again with the new socket */
                if (-1 == bind(con->fd, &con->dst->addr.common, con->dst->len)) {
                    g_critical("%s: bind(%s) failed: %s (%d)", G_STRLOC, con->dst->name->str, g_strerror(errno), errno);

                    return NETWORK_SOCKET_ERROR;
                }
            } else {
                g_critical("%s: bind(%s) failed: %s (%d)", G_STRLOC, con->dst->name->str, g_strerror(errno), errno);

                return NETWORK_SOCKET_ERROR;
            }
        }

        if (con->dst->addr.common.sa_family == AF_INET && con->dst->addr.ipv4.sin_port == 0) {
            struct sockaddr_in a;
            socklen_t a_len = sizeof(a);

            if (0 != getsockname(con->fd, (struct sockaddr *)&a, &a_len)) {
                g_critical("%s: getsockname(%s) failed: %s (%d)",
                           G_STRLOC, con->dst->name->str, g_strerror(errno), errno);
                return NETWORK_SOCKET_ERROR;
            }
            con->dst->addr.ipv4.sin_port = a.sin_port;
        } else if (con->dst->addr.common.sa_family == AF_INET6 && con->dst->addr.ipv6.sin6_port == 0) {
            struct sockaddr_in6 a;
            socklen_t a_len = sizeof(a);

            if (0 != getsockname(con->fd, (struct sockaddr *)&a, &a_len)) {
                g_critical("%s: getsockname(%s) failed: %s (%d)",
                           G_STRLOC, con->dst->name->str, g_strerror(errno), errno);
                return NETWORK_SOCKET_ERROR;
            }
            con->dst->addr.ipv6.sin6_port = a.sin6_port;
        }

        if (-1 == listen(con->fd, 128)) {
            g_critical("%s: listen(%s, 128) failed: %s (%d)", G_STRLOC, con->dst->name->str, g_strerror(errno), errno);
            return NETWORK_SOCKET_ERROR;
        }
    } else {
        /* UDP sockets bind the ->src address */
        g_return_val_if_fail(con->src, NETWORK_SOCKET_ERROR);
        g_return_val_if_fail(con->src->name->len > 0, NETWORK_SOCKET_ERROR);

        if (-1 == (con->fd = socket(con->src->addr.common.sa_family, con->socket_type, 0))) {
            g_critical("%s: socket(%s) failed: %s (%d)", G_STRLOC, con->src->name->str, g_strerror(errno), errno);
            return NETWORK_SOCKET_ERROR;
        }

        if (-1 == bind(con->fd, &con->src->addr.common, con->src->len)) {
            g_critical("%s: bind(%s) failed: %s (%d)", G_STRLOC, con->src->name->str, g_strerror(errno), errno);
            return NETWORK_SOCKET_ERROR;
        }
    }

    con->dst->can_unlink_socket = TRUE;
    return NETWORK_SOCKET_SUCCESS;
}

/**
 * read a data from the socket
 *
 * @param sock the socket
 */
network_socket_retval_t
network_socket_read(network_socket *sock)
{
    gssize len;

    if (sock->to_read > 0) {
        GString *packet = g_string_sized_new(calculate_alloc_len(sock->to_read));

        g_queue_push_tail(sock->recv_queue_raw->chunks, packet);

        g_debug("%s: recv queue length:%d, sock:%p, client addr:%s, to read:%d",
                G_STRLOC, sock->recv_queue_raw->chunks->length, sock, sock->src->name->str, (int)sock->to_read);

        g_debug("%s: tcp read:%d for fd:%d", G_STRLOC, (int)sock->to_read, sock->fd);
        len = recv(sock->fd, packet->str, sock->to_read, 0);

        if (-1 == len) {
            switch (errno) {
            case E_NET_CONNABORTED:
                    /** nothing to read, let's let ioctl() handle the close for us */
            case E_NET_CONNRESET:
            case E_NET_WOULDBLOCK:    /** the buffers are empty, try again later */
            case EAGAIN:
                g_message("%s:server to read:%d, but empty", G_STRLOC, (int)sock->to_read);
                return NETWORK_SOCKET_WAIT_FOR_EVENT;
            default:
                g_message("%s: recv() failed: %s (errno=%d), to read:%d", G_STRLOC,
                          g_strerror(errno), errno, (int)sock->to_read);
                return NETWORK_SOCKET_ERROR;
            }
        } else if (len == 0) {
            /**
             * connection close
             *
             * let's call the ioctl() and let it handle it for use
             */
            return NETWORK_SOCKET_WAIT_FOR_EVENT;
        }

        sock->to_read -= len;
        sock->recv_queue_raw->len += len;
        packet->len = len;
    }

    return NETWORK_SOCKET_SUCCESS;
}


/**
 * write data to the socket
 *
 */
static network_socket_retval_t
network_socket_write_writev(network_socket *con, int send_chunks)
{
    /* send the whole queue */
    GList *chunk;
    struct iovec *iov;
    gint chunk_id;
    gint chunk_count;
    gssize len;
    int os_errno;
    gint max_chunk_count;

    if (send_chunks == 0)
        return NETWORK_SOCKET_SUCCESS;

    network_queue* send_queue = con->do_compress ?
        con->send_queue_compressed : con->send_queue;

    chunk_count = send_chunks > 0 ? send_chunks : (gint)send_queue->chunks->length;

    if (chunk_count == 0)
        return NETWORK_SOCKET_SUCCESS;

    max_chunk_count = UIO_MAXIOV;

    chunk_count = chunk_count > max_chunk_count ? max_chunk_count : chunk_count;

    g_assert_cmpint(chunk_count, >, 0); /* make sure it is never negative */

    iov = g_new0(struct iovec, chunk_count);

    int aggr_len = 0;
    for (chunk = send_queue->chunks->head, chunk_id = 0;
         chunk && chunk_id < chunk_count; chunk_id++, chunk = chunk->next) {
        GString *s = chunk->data;

        if (chunk_id == 0) {
            g_assert(send_queue->offset < s->len);

            iov[chunk_id].iov_base = s->str + send_queue->offset;
            iov[chunk_id].iov_len = s->len - send_queue->offset;
        } else {
            iov[chunk_id].iov_base = s->str;
            iov[chunk_id].iov_len = s->len;
        }

        aggr_len += iov[chunk_id].iov_len;
        if (aggr_len >= 65536) {
            chunk_id++;
            break;
        }

        if (s->len == 0) {
            g_warning("%s: s->len is zero", G_STRLOC);
        }
    }

    g_debug("%s: network socket:%p, send (src:%s, dst:%s) fd:%d",
            G_STRLOC, con, con->src->name->str, con->dst->name->str, con->fd);

    len = writev(con->fd, iov, chunk_id);
    g_debug("%s: tcp write:%d, chunk count:%d", G_STRLOC, (int)len, (int)chunk_id);
    os_errno = errno;

    g_free(iov);

    if (-1 == len) {
        switch (os_errno) {
        case E_NET_WOULDBLOCK:
        case EAGAIN:
            return NETWORK_SOCKET_WAIT_FOR_EVENT;
        case EPIPE:
        case E_NET_CONNRESET:
        case E_NET_CONNABORTED:
                /** remote side closed the connection */
            return NETWORK_SOCKET_ERROR;
        default:
            g_message("%s: writev(%s, ...) failed: %s", G_STRLOC, con->dst->name->str, g_strerror(os_errno));
            return NETWORK_SOCKET_ERROR;
        }
    } else if (len == 0) {
        return NETWORK_SOCKET_ERROR;
    }

    send_queue->offset += len;
    send_queue->len -= len;

    /* check all the chunks which we have sent out */
    for (chunk = send_queue->chunks->head; chunk;) {
        GString *s = chunk->data;

        if (s->len == 0) {
            g_warning("%s: s->len is zero", G_STRLOC);
        }

        if (send_queue->offset >= s->len) {
            send_queue->offset -= s->len;
#if NETWORK_DEBUG_TRACE_IO
            g_debug("%s:output for sock:%p", G_STRLOC, con);
            /* to trace the data we sent to the socket, enable this */
            g_debug_hexdump(G_STRLOC, S(s));
#endif
            if (!con->do_query_cache) {
                g_string_free(s, TRUE);
            } else {
                size_t len = con->cache_queue->len + s->len;
                if (len > MAX_QUERY_CACHE_SIZE) {
                    if (!con->query_cache_too_long) {
                        g_message("%s:too long for cache queue:%p, len:%d", G_STRLOC, con, (int)len);
                        con->query_cache_too_long = 1;
                    }
                    g_string_free(s, TRUE);
                } else {
                    g_debug("%s:append packet to cache queue:%p, len:%d, total:%d",
                            G_STRLOC, con, (int)s->len, (int)len);
                    network_queue_append(con->cache_queue, s);
                }
            }

            g_queue_delete_link(send_queue->chunks, chunk);

            chunk = send_queue->chunks->head;
        } else {
            g_debug("%s:wait for event", G_STRLOC);
            return NETWORK_SOCKET_WAIT_FOR_EVENT;
        }
    }

    return NETWORK_SOCKET_SUCCESS;
}

/**
 * write a content of con->send_queue to the socket
 *
 * @param con         socket to read from
 * @param send_chunks number of chunks to send, if < 0 send all
 *
 * @returns NETWORK_SOCKET_SUCCESS on success, 
 *          NETWORK_SOCKET_ERROR on error and 
 *          NETWORK_SOCKET_WAIT_FOR_EVENT if the call would have blocked 
 */
network_socket_retval_t
network_socket_write(network_socket *sock, int send_chunks)
{
    if (sock->socket_type == SOCK_STREAM) {
        return network_socket_write_writev(sock, send_chunks);
    } else {
        g_critical("%s: udp write is not supported", G_STRLOC);
        return NETWORK_SOCKET_ERROR;
    }
}

network_socket_retval_t
network_socket_to_read(network_socket *sock)
{
    int b = -1;

    if (0 != ioctl(sock->fd, FIONREAD, &b)) {
        g_critical("%s: ioctl(%d, FIONREAD, ...) failed: %s (%d)", G_STRLOC, sock->fd, g_strerror(errno), errno);
        return NETWORK_SOCKET_ERROR;
    } else if (b < 0) {
        g_critical("%s: ioctl(%d, FIONREAD, ...) succeeded, but is negative: %d", G_STRLOC, sock->fd, b);

        return NETWORK_SOCKET_ERROR;
    } else {
        sock->to_read = b;
        return NETWORK_SOCKET_SUCCESS;
    }

}
