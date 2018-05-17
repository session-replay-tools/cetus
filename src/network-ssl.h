#ifndef NETWORK_SSL_H
#define NETWORK_SSL_H

#include "config.h"

#ifdef HAVE_OPENSSL

#include "network-socket.h"

#define NETWORK_SSL_CLIENT 1
#define NETWORK_SSL_SERVER 2

gboolean network_ssl_init(char* conf_dir);

gboolean network_ssl_create_connection(network_socket* sock, int flags);

void network_ssl_free_connection(network_socket* sock);

network_socket_retval_t network_ssl_write(network_socket* sock, int send_chunks);

gboolean network_ssl_decrypt_packet(network_socket* sock);

network_socket_retval_t network_ssl_handshake(network_socket* sock);

#endif /* HAVE_OPENSSL */

#endif /* NETWORK_SSL_H */
