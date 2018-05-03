#include "network-ssl.h"
#include "glib-ext.h"

#ifdef HAVE_OPENSSL

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/conf.h>
#include <errno.h>

struct network_ssl_connection_s {
    SSL* ssl;
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

static void network_ssl_clear_error()
{
    while (ERR_peek_error()) {
        g_critical("ignoring stale global SSL error");
    }
    ERR_clear_error();
}

static gboolean network_ssl_create_context(char* conf_dir)
{
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
/*
    SSL_CTX_set_options(g_ssl_context, SSL_OP_NO_SSLv2);
    SSL_CTX_set_options(g_ssl_context, SSL_OP_NO_SSLv3);
    SSL_CTX_set_options(g_ssl_context, SSL_OP_NO_TLSv1);
    SSL_CTX_clear_options(g_ssl_context, SSL_OP_NO_TLSv1_1);
    SSL_CTX_set_options(g_ssl_context, SSL_OP_NO_TLSv1_1);
    SSL_CTX_set_options(g_ssl_context, SSL_OP_NO_TLSv1_2);
*/
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
    SSL_CTX_set_info_callback(g_ssl_context, network_ssl_info_callback);
    char* cert = g_build_filename(conf_dir, "server-cert.pem", NULL);
    char* key = g_build_filename(conf_dir, "server-key.pem", NULL);
    SSL_CTX_use_certificate_file(g_ssl_context, cert , SSL_FILETYPE_PEM);
    SSL_CTX_use_PrivateKey_file(g_ssl_context, key, SSL_FILETYPE_PEM);
    g_free(cert);
    g_free(key);

    return TRUE;
}

gboolean network_ssl_init(char* conf_dir)
{
#if OPENSSL_VERSION_NUMBER >= 0x10100003L

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

network_socket_retval_t network_ssl_write(network_socket* sock)
{
	GString *s;
    network_ssl_clear_error();
	for (s = g_queue_peek_head(sock->send_queue->chunks); s; ) {
		g_assert(sock->send_queue->offset < s->len);

        gssize len = SSL_write(sock->ssl->ssl, s->str + sock->send_queue->offset,
                        s->len - sock->send_queue->offset);

        if (len < 0) {
            int sslerr = SSL_get_error(sock->ssl->ssl, len);
            g_debug(G_STRLOC " SSL_get_error: %d", sslerr);

            if (sslerr == SSL_ERROR_WANT_WRITE) {
                g_debug(G_STRLOC " SSL_write() WANT_WRITE");
				return NETWORK_SOCKET_WAIT_FOR_WRITABLE;
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

		sock->send_queue->offset += len;

		if (sock->send_queue->offset == s->len) {
			g_queue_pop_head(sock->send_queue->chunks);
            g_string_free(s, TRUE);

			sock->send_queue->offset = 0;

			s = g_queue_peek_head(sock->send_queue->chunks);
		} else {
			return NETWORK_SOCKET_WAIT_FOR_EVENT;
		}
	}
	return NETWORK_SOCKET_SUCCESS;
}

static int write_to_rbio(network_socket* sock)
{
    BIO* rbio = SSL_get_rbio(sock->ssl->ssl);
    network_queue* queue = sock->recv_queue_raw;
    int bytes_written = 0;

    /* TODO: peek then pop, waste of memory */
    while (g_queue_get_length(queue->chunks) > 0) {
        GString* hint = g_queue_peek_head(queue->chunks);
        GString* str = network_queue_peek_str(queue, hint->len, NULL);
        int len = BIO_write(rbio, str->str, str->len);
        if (len == str->len) {
            bytes_written += len;
            GString* p = network_queue_pop_str(queue, hint->len, NULL);
            g_string_free(p, TRUE);
            g_string_free(str, TRUE);
        } else if (len >= 0) {
            bytes_written += len;
            GString* p = network_queue_pop_str(queue, hint->len, NULL);
            g_string_free(p, TRUE);
            g_string_free(str, TRUE);
            return bytes_written;
        } else {
            return -1;
        }
    }
    return bytes_written;
}

gboolean network_ssl_decrypt_packet(network_socket* sock)
{
    char buf[1500];/*TODO: size? */
    while (TRUE) {
        int written = write_to_rbio(sock);
        if (written == 0) {
            return TRUE;
        } else if (written < 0) {
            return FALSE;
        }
        int len = SSL_read(sock->ssl->ssl, buf, sizeof(buf));
        if (len < 0) {
            int sslerr = SSL_get_error(sock->ssl->ssl, len);
            g_debug(G_STRLOC " SSL_get_error: %d", sslerr);
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

network_socket_retval_t network_ssl_handshake(network_socket* sock)
{
    network_ssl_clear_error();

    while (TRUE) {
        int len = write_to_rbio(sock);
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

gboolean network_ssl_is_init_finished(network_socket* sock)
{
    return sock->ssl->ssl && SSL_is_init_finished(sock->ssl->ssl);
}


#endif /* HAVE_OPENSSL */
