#include "network-ssl.h"
#include "glib-ext.h"

#ifdef HAVE_OPENSSL

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/conf.h>
#include <errno.h>

enum network_ssl_error_t {
    SSL_OK = 1,
    SSL_RBIO_BUFFER_FULL = -10,
};
struct network_ssl_connection_s {
    SSL* ssl;
    enum network_ssl_error_t error;
};

static SSL_CTX *g_ssl_context = NULL;

static void network_ssl_info_callback(const SSL *s, int where, int ret)
{
    const char *str;
    int w;

    w=where& ~SSL_ST_MASK;

    if (w & SSL_ST_CONNECT) str="SSL_connect";
    else if (w & SSL_ST_ACCEPT) str="SSL_accept";
    else str="undefined";

    if (where & SSL_CB_LOOP)
    {
        g_warning("%s:%s",str,SSL_state_string_long(s));
    }
    else if (where & SSL_CB_ALERT)
    {
        str=(where & SSL_CB_READ)?"read":"write";
        g_warning("SSL3 alert %s:%s:%s",
                   str,
                   SSL_alert_type_string_long(ret),
                   SSL_alert_desc_string_long(ret));
    }
    else if (where & SSL_CB_EXIT)
    {
        if (ret == 0)
            g_warning("%s:failed in %s",
                       str,SSL_state_string_long(s));
        else if (ret < 0)
        {
            g_warning("%s:error in %s",
                       str,SSL_state_string_long(s));
        }
    }
}

static void network_ssl_clear_error(network_ssl_connection_t* ssl)
{
    while (ERR_peek_error()) {
        g_critical("ignoring stale global SSL error");
    }
    ERR_clear_error();
    ssl->error = 0;
}

static gboolean network_ssl_create_context(char* conf_dir)
{
    gboolean ret = TRUE;
    g_ssl_context = SSL_CTX_new(TLSv1_method());
    if (g_ssl_context == NULL) {
        g_critical(G_STRLOC " SSL_CTX_new failed");
        return FALSE;
    }

    /* server side options */

#ifdef SSL_OP_SSLREF2_REUSE_CERT_TYPE_BUG
    SSL_CTX_set_options(g_ssl_context, SSL_OP_SSLREF2_REUSE_CERT_TYPE_BUG);
#endif

#ifdef SSL_OP_MSIE_SSLV2_RSA_PADDING
    /* this option allow a potential SSL 2.0 rollback (CAN-2005-2969) */
    SSL_CTX_set_options(g_ssl_context, SSL_OP_MSIE_SSLV2_RSA_PADDING);
#endif

#ifdef SSL_OP_SSLEAY_080_CLIENT_DH_BUG
    SSL_CTX_set_options(g_ssl_context, SSL_OP_SSLEAY_080_CLIENT_DH_BUG);
#endif

#ifdef SSL_OP_TLS_D5_BUG
    SSL_CTX_set_options(g_ssl_context, SSL_OP_TLS_D5_BUG);
#endif

#ifdef SSL_OP_TLS_BLOCK_PADDING_BUG
    SSL_CTX_set_options(g_ssl_context, SSL_OP_TLS_BLOCK_PADDING_BUG);
#endif

#ifdef SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS
    SSL_CTX_set_options(g_ssl_context, SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS);
#endif
#ifdef SSL_CTRL_CLEAR_OPTIONS
    /* only in 0.9.8m+ */
    SSL_CTX_clear_options(g_ssl_context,
                          SSL_OP_NO_SSLv2|SSL_OP_NO_SSLv3|SSL_OP_NO_TLSv1);
#endif

#ifdef SSL_OP_NO_COMPRESSION
    SSL_CTX_set_options(g_ssl_context, SSL_OP_NO_COMPRESSION);
#endif

#ifdef SSL_MODE_RELEASE_BUFFERS
    SSL_CTX_set_mode(g_ssl_context, SSL_MODE_RELEASE_BUFFERS);
#endif

#ifdef SSL_MODE_NO_AUTO_CHAIN
    SSL_CTX_set_mode(g_ssl_context, SSL_MODE_NO_AUTO_CHAIN);
#endif

    SSL_CTX_set_read_ahead(g_ssl_context, 1); /* SSL_read clear data on the wire */
    SSL_CTX_set_options(g_ssl_context, SSL_OP_SINGLE_DH_USE);
#if 0
    SSL_CTX_set_info_callback(g_ssl_context, network_ssl_info_callback);
#endif
    char* cert = g_build_filename(conf_dir, "server-cert.pem", NULL);
    char* key = g_build_filename(conf_dir, "server-key.pem", NULL);
    if (1 != SSL_CTX_use_certificate_file(g_ssl_context, cert , SSL_FILETYPE_PEM)) {
        g_warning("server-cert.pem open error");
        ret = FALSE;
    }
    if (1 != SSL_CTX_use_PrivateKey_file(g_ssl_context, key, SSL_FILETYPE_PEM)) {
        g_warning("server-key.pem open error");
        ret = FALSE;
    }
    g_free(cert);
    g_free(key);
    if (ret == FALSE) {
        SSL_CTX_free(g_ssl_context);
        g_ssl_context = NULL;
    }
    return ret;
}

gboolean network_ssl_init(char* conf_dir)
{
#if OPENSSL_VERSION_NUMBER >= 0x10100003L

    g_message("%s: OPENSSL_VERSION_NUMBER:%d", G_STRLOC,  OPENSSL_VERSION_NUMBER);
    if (OPENSSL_init_ssl(OPENSSL_INIT_LOAD_CONFIG, NULL) == 0) {
        g_critical(G_STRLOC " OPENSSL_init_ssl() failed");
        return FALSE;
    }

    /*
     * OPENSSL_init_ssl() may leave errors in the error queue
     * while returning success
     */

    ERR_clear_error();

#else

    g_message("%s: call old ssl fun", G_STRLOC);
    OPENSSL_config(NULL);

    SSL_library_init();
    SSL_load_error_strings();

    OpenSSL_add_all_algorithms();

#endif
    return network_ssl_create_context(conf_dir);
}

gboolean
network_ssl_create_connection(network_socket* sock, int flags)
{
    network_ssl_connection_t* conn = g_new0(network_ssl_connection_t, 1);
    SSL* connection = SSL_new(g_ssl_context);
    if (connection == NULL) {
        g_critical(G_STRLOC " SSL_new failed");
        return FALSE;
    }
    BIO* rbio = BIO_new(BIO_s_mem());
    if (!rbio) {
        g_critical(G_STRLOC " BIO_new() failed");
        SSL_free(connection);
        return FALSE;
    }
    BIO* wbio = BIO_new_fd(sock->fd, 0);
    if (!wbio) {
        g_critical(G_STRLOC " BIO_new_fd() failed");
        SSL_free(connection);
        return FALSE;
    }
    SSL_set_bio(connection, rbio, wbio);

    if (flags & NETWORK_SSL_CLIENT) {
        SSL_set_connect_state(connection);
    } else {
        SSL_set_accept_state(connection);
    }
    conn->ssl = connection;
    sock->ssl = conn;
    return TRUE;
}

void network_ssl_free_connection(network_socket* sock)
{
    if (sock->ssl) {
        SSL_free(sock->ssl->ssl);
        g_free(sock->ssl);
    }
}

static ssize_t SSL_writev(SSL *ssl, const struct iovec *vector, int count)
{
    int i;
    /* Find the total number of bytes to be written.  */
    size_t bytes = 0;
    for (i = 0; i < count; ++i)
        bytes += vector[i].iov_len;

    /* Allocate a temporary buffer to hold the data.  */
    char *buffer = (char *) g_malloc(bytes);

    /* Copy the data into BUFFER.  */
    size_t to_copy = bytes;
    char *bp = buffer;
    for (i = 0; i < count; ++i) {
        size_t copy = MIN(vector[i].iov_len, to_copy);

        memcpy((void *) bp, (void *) vector[i].iov_base, copy);
        bp += copy;

        to_copy -= copy;
        if (to_copy == 0)
            break;
    }

    ssize_t written = SSL_write(ssl, buffer, bytes);
    g_free(buffer);
    return written;
}

network_socket_retval_t
network_ssl_write(network_socket *sock, int send_chunks)
{
    if (send_chunks == 0)
        return NETWORK_SOCKET_SUCCESS;

    network_queue* send_queue = sock->do_compress ?
        sock->send_queue_compressed : sock->send_queue;

    gint chunk_count = send_chunks > 0 ? send_chunks : (gint)send_queue->chunks->length;

    if (chunk_count == 0)
        return NETWORK_SOCKET_SUCCESS;

    gint max_chunk_count = 1024; /*IOV_MAX*/

    chunk_count = chunk_count > max_chunk_count ? max_chunk_count : chunk_count;

    g_assert_cmpint(chunk_count, >, 0); /* make sure it is never negative */

    struct iovec *iov = g_new0(struct iovec, chunk_count);

    GList *chunk;
    gint chunk_id;

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

        if (s->len == 0) {
            g_warning("%s: s->len is zero", G_STRLOC);
        }
    }

    gssize len = SSL_writev(sock->ssl->ssl, iov, chunk_count);

    g_free(iov);

    if (len < 0) {
        int sslerr = SSL_get_error(sock->ssl->ssl, len);
        if (sslerr == SSL_ERROR_WANT_WRITE) {
            g_debug(G_STRLOC " SSL_write() WANT_WRITE");
            return NETWORK_SOCKET_WAIT_FOR_EVENT;
        }
        if (sslerr == SSL_ERROR_WANT_READ) {
            g_warning(G_STRLOC " peer started SSL renegotiation");
            return NETWORK_SOCKET_WAIT_FOR_EVENT; /* TODO: read event */
        }
        g_critical(G_STRLOC " SSL_write() failed");
        return NETWORK_SOCKET_ERROR;
    } else if (len == 0) {
        int sslerr = SSL_get_error(sock->ssl->ssl, len);
        g_critical(G_STRLOC " SSL_write() failed: %d", sslerr);
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
            g_debug("%s:output for sock:%p", G_STRLOC, sock);
#endif
            if (!sock->do_query_cache) {
                g_string_free(s, TRUE);
            } else {
                size_t len = sock->cache_queue->len + s->len;
                if (len > MAX_QUERY_CACHE_SIZE) {
                    if (!sock->query_cache_too_long) {
                        g_message("%s:too long for cache queue:%p, len:%d", G_STRLOC, sock, (int)len);
                        sock->query_cache_too_long = 1;
                    }
                    g_string_free(s, TRUE);
                } else {
                    g_debug("%s:append packet to cache queue:%p, len:%d, total:%d",
                            G_STRLOC, sock, (int)s->len, (int)len);
                    network_queue_append(sock->cache_queue, s);
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

static int network_ssl_write_to_rbio(network_socket* sock)
{
    network_ssl_clear_error(sock->ssl);

    BIO* rbio = SSL_get_rbio(sock->ssl->ssl);
    network_queue* queue = sock->recv_queue_raw;
    int bytes_written = queue->len;

    int i = 0;
    GList* chunk;
    for (chunk = queue->chunks->head, i = 0; chunk; i++, chunk = chunk->next) {
        GString *s = chunk->data;
        char* buf;
        int buf_len;
        if (i == 0) {
            g_assert(queue->offset < s->len);
            buf = s->str + queue->offset;
            buf_len  = s->len - queue->offset;
        } else {
            buf = s->str;
            buf_len  = s->len;
        }
        int len = BIO_write(rbio, buf, buf_len);
        if (len == buf_len) {
            queue->offset += len;
            queue->len -= len;
        } else if (len >= 0) {
            queue->offset += len;
            queue->len -= len;
            sock->ssl->error = SSL_RBIO_BUFFER_FULL;
            break;
        } else {
            return -1;
        }
    }

    bytes_written = bytes_written - queue->len;

    /* delete used chunks, adjust offset */
    for (chunk = queue->chunks->head; chunk; ) {
        GString *s = chunk->data;

        if (queue->offset >= s->len) {
            queue->offset -= s->len;
            g_string_free(s, TRUE);
            g_queue_delete_link(queue->chunks, chunk);
            chunk = queue->chunks->head;
        } else {
            g_message("write_to_rbio have residual");
            return bytes_written; /* have some residual */
        }
    }

    return bytes_written;
}

/**
   [sock->recv_queue_raw] === SSL decrypt ===> [sock->recv_queue_decrypted_raw]
*/
gboolean network_ssl_decrypt_packet(network_socket* sock)
{
/* TODO: the raw packet is very likely a complete SSL record
   better solution: write one-packet to rbio and SSL_read one-packet */
    network_ssl_clear_error(sock->ssl);

    char buf[16*1024];/*TODO: size? */
    while (TRUE) {
        int written = network_ssl_write_to_rbio(sock);
        if (written == 0) {
            return TRUE;
        } else if (written < 0) {
            return FALSE;
        }

        while (TRUE) {
            int len = SSL_read(sock->ssl->ssl, buf, sizeof(buf));
            if (len < 0) {
                int sslerr = SSL_get_error(sock->ssl->ssl, len);
                if (sslerr == SSL_ERROR_WANT_WRITE) {
                    g_warning(G_STRLOC " peer started SSL renegotiation");
                    return TRUE; /*TODO: how to renegotiate? */
                }
                if (sslerr == SSL_ERROR_WANT_READ) {
                    g_debug(G_STRLOC " SSL_read() WANT_READ");
                    return TRUE; /* TODO: read event */
                }
                g_critical(G_STRLOC " SSL_read() failed");
                if (sslerr == SSL_ERROR_SYSCALL) {
                    g_critical(G_STRLOC " %s", strerror(errno));
                }
                return FALSE;
            } else if (len == 0) {
                int sslerr = SSL_get_error(sock->ssl->ssl, len);
                g_critical(G_STRLOC " SSL_read() failed: %d", sslerr);
                return FALSE;
            }
            network_queue_append(sock->recv_queue_decrypted_raw, g_string_new_len(buf, len));
        }
    }
}

network_socket_retval_t network_ssl_handshake(network_socket* sock)
{
    network_ssl_clear_error(sock->ssl);

    while (TRUE) {
        int len = network_ssl_write_to_rbio(sock);
        if (len < 0) {
            return NETWORK_SOCKET_ERROR;
        } else if (len == 0) {
            return NETWORK_SOCKET_WAIT_FOR_EVENT;
        }

        int ret = SSL_do_handshake(sock->ssl->ssl);

        if (ret < 0) {
            int sslerr = SSL_get_error(sock->ssl->ssl, ret);
            g_debug(G_STRLOC " SSL_get_error: %d", sslerr);

            if (sslerr == SSL_ERROR_WANT_WRITE) {
                g_debug(G_STRLOC " handshake continuation WANT_WRITE");
                return NETWORK_SOCKET_WAIT_FOR_EVENT;
            } else if (sslerr == SSL_ERROR_WANT_READ) {
                g_debug(G_STRLOC " handshake continuation WANT_READ");
                return NETWORK_SOCKET_WAIT_FOR_EVENT;
            }
            return NETWORK_SOCKET_ERROR;
        } else if (ret == 0) {
            return NETWORK_SOCKET_ERROR;
        } else {
            g_debug(G_STRLOC " handshake success");
            return NETWORK_SOCKET_SUCCESS;
        }
    }
}


#endif /* HAVE_OPENSSL */
