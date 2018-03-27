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

#include <sys/socket.h>

#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
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
#include <glib.h>
#include <glib/gstdio.h>

#include "network-address.h"
#include "glib-ext.h"
#include "cetus-util.h"

network_address *
network_address_new()
{
    network_address *addr;

    addr = g_new0(network_address, 1);
    addr->len = sizeof(addr->addr);
    addr->name = g_string_new(NULL);
    g_debug("%s: create socket name %p successful for add:%p", G_STRLOC, addr->name, addr);

    return addr;
}

void
network_address_free(network_address *addr)
{

    if (!addr)
        return;

    /*
     * if the name we're freeing starts with a '/', we're
     * looking at a unix socket which needs to be removed
     */
    if (addr->can_unlink_socket == TRUE && addr->name != NULL && addr->name->str != NULL) {
        gchar *name = addr->name->str;
        if (name[0] == '/') {
            int ret = g_remove(name);
            if (ret == 0) {
                g_debug("%s: removing socket %s successful", G_STRLOC, name);
            } else {
                if (errno != EPERM && errno != EACCES) {
                    g_critical("%s: removing socket %s failed: %s (%d)", G_STRLOC, name, strerror(errno), errno);
                }
            }
        }
    }

    g_string_free(addr->name, TRUE);
    g_free(addr);
}

void
network_address_reset(network_address *addr)
{
    addr->len = sizeof(addr->addr.common);
}

static gint
network_address_set_address_ip(network_address *addr, const gchar *address, guint port)
{
    g_return_val_if_fail(addr, -1);

    if (port > 65535) {
        g_critical("%s: illegal value %u for port, only 1 ... 65535 allowed", G_STRLOC, port);
        return -1;
    }

    if (NULL == address || address[0] == '\0') {
        /* no ip */
#if 0
        /* disabled as it breaks the default behaviour on FreeBSD and windows
         *
         * FreeBSD doesn't do IPv6+IPv4 sockets by default, other unixes do.
         * while we could change that to with setsockopt(..., IPV6_V6ONLY, ...)
         * it should be fixed by adding support for multiple sockets instead.
         */
        struct in6_addr addr6 = IN6ADDR_ANY_INIT;

        memset(&addr->addr.ipv6, 0, sizeof(struct sockaddr_in6));

        addr->addr.ipv6.sin6_addr = addr6;
        addr->addr.ipv6.sin6_family = AF_INET6;
        addr->addr.ipv6.sin6_port = htons(port);
        addr->len = sizeof(struct sockaddr_in6);

#else
        memset(&addr->addr.ipv4, 0, sizeof(struct sockaddr_in));

        addr->addr.ipv4.sin_addr.s_addr = htonl(INADDR_ANY);
        addr->addr.ipv4.sin_family = AF_INET;   /* "default" family */
        addr->addr.ipv4.sin_port = htons(port);
        addr->len = sizeof(struct sockaddr_in);
#endif
    } else if (0 == strcmp("0.0.0.0", address)) {
        /* that's any IPv4 address, so bind to IPv4-any only */
        memset(&addr->addr.ipv4, 0, sizeof(struct sockaddr_in));

        addr->addr.ipv4.sin_addr.s_addr = htonl(INADDR_ANY);
        addr->addr.ipv4.sin_family = AF_INET;   /* "default" family */
        addr->addr.ipv4.sin_port = htons(port);
        addr->len = sizeof(struct sockaddr_in);
    } else {
#ifdef HAVE_GETADDRINFO
        struct addrinfo *first_ai = NULL;
        struct addrinfo hint;
        struct addrinfo *ai;
        int ret;

        /* AI_ADDRCONFIG filters out ::1 and link-local addresses if there is no
         * global IPv6 address assigned to the local interfaces
         *
         * 1) try to resolve with ADDRCONFIG
         * 2) if 1) fails, try without ADDRCONFIG
         *
         * this should handle DNS problems where
         * - only IPv4 is configured, but DNS returns IPv6 records + IPv4 and 
         *   we would pick IPv6 (and fail)
         * - 
         */

        memset(&hint, 0, sizeof(hint));
        hint.ai_family = PF_UNSPEC;
        hint.ai_socktype = SOCK_STREAM;
        hint.ai_protocol = 0;
        hint.ai_flags = AI_ADDRCONFIG;
        if ((ret = getaddrinfo(address, NULL, &hint, &first_ai)) != 0) {
            if (
                   /* FreeBSD doesn't provide EAI_ADDRFAMILY (since 2003) */
#ifdef EAI_ADDRFAMILY
                   EAI_ADDRFAMILY == ret || /* AI_ADDRCONFIG with a non-global address */
#endif
                   EAI_BADFLAGS == ret) {   /* AI_ADDRCONFIG isn't supported */
                if (first_ai)
                    freeaddrinfo(first_ai);
                first_ai = NULL;

                hint.ai_flags &= ~AI_ADDRCONFIG;

                ret = getaddrinfo(address, NULL, &hint, &first_ai);
            }

            if (ret != 0) {
                g_critical("getaddrinfo(\"%s\") failed: %s (%d)", address, gai_strerror(ret), ret);
                return -1;
            }
        }

        ret = 0;                /* bogus, just to make it explicit */

        for (ai = first_ai; ai; ai = ai->ai_next) {
            int family = ai->ai_family;

            if (family == PF_INET6) {
                memcpy(&addr->addr.ipv6, (struct sockaddr_in6 *)ai->ai_addr, sizeof(addr->addr.ipv6));
                addr->addr.ipv6.sin6_port = htons(port);
                addr->len = sizeof(struct sockaddr_in6);

                break;
            } else if (family == PF_INET) {
                memcpy(&addr->addr.ipv4, (struct sockaddr_in *)ai->ai_addr, sizeof(addr->addr.ipv4));
                addr->addr.ipv4.sin_port = htons(port);
                addr->len = sizeof(struct sockaddr_in);
                break;
            }
        }

        if (ai == NULL) {
            /* no matching address-info found */
            g_debug("%s: %s:%d", G_STRLOC, address, port);
            ret = -1;
        }

        freeaddrinfo(first_ai);

        if (ret != 0)
            return ret;
#else
        struct hostent *he;

        he = gethostbyname(address);
        if (NULL == he) {
            return -1;
        }

        g_assert(he->h_addrtype == AF_INET);
        g_assert(he->h_length == sizeof(struct in_addr));

        memcpy(&(addr->addr.ipv4.sin_addr.s_addr), he->h_addr_list[0], he->h_length);
        addr->addr.ipv4.sin_family = AF_INET;
        addr->addr.ipv4.sin_port = htons(port);
        addr->len = sizeof(struct sockaddr_in);
#endif /* HAVE_GETADDRINFO */
    }

    (void)network_address_refresh_name(addr);

    return 0;
}

static gint
network_address_set_address_un(network_address *addr, const gchar *address)
{
    g_return_val_if_fail(addr, -1);
    g_return_val_if_fail(address, -1);

    if (strlen(address) >= sizeof(addr->addr.un.sun_path) - 1) {
        g_critical("unix-path is too long: %s", address);
        return -1;
    }

    addr->addr.un.sun_family = AF_UNIX;
    strcpy(addr->addr.un.sun_path, address);
    addr->len = sizeof(struct sockaddr_un);

    network_address_refresh_name(addr);

    return 0;
}

/**
 * translate a address-string into a network_address structure
 *
 * - if the address contains a colon we assume IPv4, 
 *   - ":3306" -> (tcp) "0.0.0.0:3306"
 * - if it starts with a / it is a unix-domain socket 
 *   - "/tmp/socket" -> (unix) "/tmp/socket"
 *
 * @param addr     the address-struct
 * @param address  the address string
 * @return 0 on success, -1 otherwise
 */
gint
network_address_set_address(network_address *addr, const gchar *address)
{
    const gchar *port_part = NULL;
    gchar *ip_part = NULL;
    gint ret;

    g_return_val_if_fail(addr, -1);

    /* split the address:port */
    if (address[0] == '/') {
        return network_address_set_address_un(addr, address);
    } else if (address[0] == '[') {
        const gchar *s;
        if (NULL == (s = strchr(address + 1, ']'))) {
            return -1;
        }
        /* may be NULL for strdup(..., 0) */
        ip_part = g_strndup(address + 1, s - (address + 1));

        if (*(s + 1) == ':') {
            port_part = s + 2;
        }
    } else if (NULL != (port_part = strchr(address, ':'))) {
        /* may be NULL for strdup(..., 0) */
        ip_part = g_strndup(address, port_part - address);
        port_part++;
    } else {
        ip_part = g_strdup(address);
    }

    /* if there is a colon, there should be a port number */
    if (NULL != port_part) {
        char *port_err = NULL;
        guint port;

        port = strtoul(port_part, &port_err, 10);

        if (*port_part == '\0') {
            g_critical("%s: IP-addr in the form [<ip>][:<port>], is '%s'. No port", G_STRLOC, address);
            ret = -1;
        } else if (*port_err != '\0') {
            g_critical("%s: IP-addr in the form [<ip>][:<port>], is '%s'. port at '%s'", G_STRLOC, address, port_err);
            ret = -1;
        } else {
            ret = network_address_set_address_ip(addr, ip_part, port);
        }
    } else {
        /* perhaps it is a plain IP address, lets add the default-port */
        ret = network_address_set_address_ip(addr, ip_part, 3306);
    }

    if (ip_part)
        g_free(ip_part);

    return ret;
}

GQuark
network_address_error(void)
{
    return g_quark_from_static_string("network-address-error");
}

#ifdef HAVE_INET_NTOP
static const gchar *
network_address_tostring_inet_ntop(network_address *addr, gchar *dst, gsize *dst_len, GError **gerr)
{
    const char *addr_str;
    gsize initial_dst_len = *dst_len;

    /* resolve the peer-addr if we haven't done so yet */
    switch (addr->addr.common.sa_family) {
    case AF_INET:
        addr_str = inet_ntop(AF_INET, &addr->addr.ipv4.sin_addr, dst, *dst_len);
        if (NULL == addr_str) {
            if (ENOSPC == errno) {
                g_set_error(gerr,
                            NETWORK_ADDRESS_ERROR,
                            NETWORK_ADDRESS_ERROR_DST_TOO_SMALL,
                            "inet_ntop() failed: %s (%d)", g_strerror(errno), errno);
            } else {
                g_set_error(gerr,
                            NETWORK_ADDRESS_ERROR,
                            NETWORK_ADDRESS_ERROR_UNKNOWN, "inet_ntop() failed: %s (%d)", g_strerror(errno), errno);
            }
            return NULL;
        }
        *dst_len = strlen(addr_str) + 1;
        return addr_str;
    case AF_INET6:
        addr_str = inet_ntop(AF_INET6, &addr->addr.ipv6.sin6_addr, dst, *dst_len);

        if (NULL == addr_str) {
            if (ENOSPC == errno) {
                g_set_error(gerr,
                            NETWORK_ADDRESS_ERROR,
                            NETWORK_ADDRESS_ERROR_DST_TOO_SMALL,
                            "inet_ntop() failed: %s (%d)", g_strerror(errno), errno);
            } else {
                g_set_error(gerr,
                            NETWORK_ADDRESS_ERROR,
                            NETWORK_ADDRESS_ERROR_UNKNOWN, "inet_ntop() failed: %s (%d)", g_strerror(errno), errno);
            }
            return NULL;
        }
        *dst_len = strlen(addr_str) + 1;

        return addr_str;
    case AF_UNIX:
        *dst_len = g_strlcpy(dst, addr->addr.un.sun_path, *dst_len);

        if (*dst_len >= initial_dst_len) {
            /* g_strlcpy() got overrun */
            g_set_error(gerr, NETWORK_ADDRESS_ERROR, NETWORK_ADDRESS_ERROR_DST_TOO_SMALL, "dst too small");
            return NULL;
        }
        /* g_strlcpy() returns the size without \0, we return the size with \0 */
        *dst_len += 1;

        return dst;
    default:
        g_set_error(gerr,
                    NETWORK_ADDRESS_ERROR,
                    NETWORK_ADDRESS_ERROR_INVALID_ADDRESS_FAMILY,
                    "can't convert a address of family '%d' into a string", addr->addr.common.sa_family);
        return NULL;
    }
}

#else

/**
 * resolve a struct sockaddr into a string 
 */
static const gchar *
network_address_tostring_inet_ntoa(network_address *addr, gchar *dst, gsize *dst_len, GError **gerr)
{
    const char *addr_str;
    gsize initial_dst_len = *dst_len;

    /* resolve the peer-addr if we haven't done so yet */
    switch (addr->addr.common.sa_family) {
    case AF_INET:
        addr_str = inet_ntoa(addr->addr.ipv4.sin_addr);

        if (NULL == addr_str) {
            g_set_error(gerr, NETWORK_ADDRESS_ERROR, NETWORK_ADDRESS_ERROR_UNKNOWN, "inet_ntoa() failed: %d", errno);
            return NULL;
        }
        *dst_len = g_strlcpy(dst, addr_str, *dst_len);

        return dst;
    case AF_UNIX:
        *dst_len = g_strlcpy(dst, addr->addr.un.sun_path, *dst_len);
        if (*dst_len >= initial_dst_len) {
            /* g_strlcpy() got overrun */
            g_set_error(gerr, NETWORK_ADDRESS_ERROR, NETWORK_ADDRESS_ERROR_DST_TOO_SMALL, "dst too small");
            return NULL;
        }
        /* g_strlcpy() returns the size without \0, we return the size with \0 */
        *dst_len += 1;

        return dst;
    default:
        g_set_error(gerr,
                    NETWORK_ADDRESS_ERROR,
                    NETWORK_ADDRESS_ERROR_INVALID_ADDRESS_FAMILY,
                    "can't convert a address of family '%d' into a string", addr->addr.common.sa_family);
        return NULL;
    }
}

#endif

char *
network_address_tostring(network_address *addr, char *dst, gsize *dst_len, GError **gerr)
{
    const char *addr_str;

    if (NULL == dst) {
        g_set_error(gerr, NETWORK_ADDRESS_ERROR, NETWORK_ADDRESS_ERROR_INVALID, "dst is NULL");
        return NULL;
    }
    if (NULL == dst_len) {
        g_set_error(gerr, NETWORK_ADDRESS_ERROR, NETWORK_ADDRESS_ERROR_INVALID, "dst_len is NULL");
        return NULL;
    }
#if defined(HAVE_INET_NTOP)
    addr_str = network_address_tostring_inet_ntop(addr, dst, dst_len, gerr);
#else
    addr_str = network_address_tostring_inet_ntoa(addr, dst, dst_len, gerr);
#endif

    if (NULL == addr_str) {
        /* gerr is already set, just return NULL */
        return NULL;
    }

    return dst;
}

gint
network_address_refresh_name(network_address *addr)
{
    GError *gerr = NULL;
    char buf[255];
    gsize buf_len = sizeof(buf);

    if (NULL == network_address_tostring(addr, buf, &buf_len, &gerr)) {
        g_critical("%s: %s", G_STRLOC, gerr->message);
        g_clear_error(&gerr);
        return -1;
    }

    if (addr->addr.common.sa_family == AF_INET) {
        g_string_printf(addr->name, "%s:%d", buf, ntohs(addr->addr.ipv4.sin_port));
    } else if (addr->addr.common.sa_family == AF_INET6) {
        g_string_printf(addr->name, "[%s]:%d", buf, ntohs(addr->addr.ipv6.sin6_port));
    } else {
        g_string_assign(addr->name, buf);
    }

    return 0;
}

network_address *
network_address_copy(network_address *dst, network_address *src)
{
    if (!dst)
        dst = network_address_new();

    dst->len = src->len;
    dst->addr = src->addr;
    g_string_assign_len(dst->name, S(src->name));

    return dst;
}
