/*
 * tio - a serial device I/O tool
 *
 * Copyright (c) 2026  Martin Lund
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

#include <errno.h>
#include <netdb.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "net.h"
#include "print.h"

#define NET_PORT_DEFAULT 3333
#define NET_PORT_MAX 65535

// Target prefixes which select socket mode instead of tty mode
static const char *socket_prefix[] =
{
    "unix:",
    "inet:",
    "inet6:",
};

// Address resolved by net_resolve(). The reconnect loop retries against this
// copy so that no retry ever blocks in the resolver.
static net_address_t resolved_address;
static bool resolved = false;

bool net_target_is_socket(const char *target)
{
    if (target == NULL)
    {
        return false;
    }

    for (size_t i = 0; i < sizeof(socket_prefix) / sizeof(socket_prefix[0]); i++)
    {
        if (strncmp(target, socket_prefix[i], strlen(socket_prefix[i])) == 0)
        {
            return true;
        }
    }

    return false;
}

static long net_parse_port(const char *port_string, const char *target)
{
    char *endptr;
    long port;

    if (port_string[0] == '\0')
    {
        tio_error_printf("Missing port number in '%s'", target);
        exit(EXIT_FAILURE);
    }

    errno = 0;
    port = strtol(port_string, &endptr, 10);

    if ((*endptr != '\0') || (errno != 0))
    {
        tio_error_printf("Invalid port number '%s' in '%s'", port_string, target);
        exit(EXIT_FAILURE);
    }

    if ((port < 0) || (port > NET_PORT_MAX))
    {
        tio_error_printf("Port number %ld out of range (0-%d) in '%s'", port, NET_PORT_MAX, target);
        exit(EXIT_FAILURE);
    }

    if (port == 0)
    {
        // No port given -> fall back to the default, matching the socket
        // server behavior
        port = NET_PORT_DEFAULT;
    }

    return port;
}

static void net_copy_host(char *host, size_t size, const char *source, size_t length, const char *target)
{
    if (length == 0)
    {
        tio_error_printf("Missing host address in '%s'", target);
        exit(EXIT_FAILURE);
    }

    if (length >= size)
    {
        tio_error_printf("Host address too long in '%s'", target);
        exit(EXIT_FAILURE);
    }

    memcpy(host, source, length);
    host[length] = '\0';
}

static void net_resolve_inet(int family, const char *host, long port, const char *target)
{
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    char service[6];
    int status;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = family;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICSERV;

    snprintf(service, sizeof(service), "%ld", port);

    // Resolved once here on the connect path only - the reconnect loop reuses
    // the cached result so that it never blocks on a name lookup
    status = getaddrinfo(host, service, &hints, &result);
    if (status != 0)
    {
        tio_error_printf("Could not resolve '%s' in '%s': %s", host, target, gai_strerror(status));
        exit(EXIT_FAILURE);
    }

    resolved_address.family = result->ai_family;
    resolved_address.addrlen = result->ai_addrlen;
    memcpy(&resolved_address.addr, result->ai_addr, result->ai_addrlen);

    freeaddrinfo(result);
}

static void net_resolve_unix(const char *target)
{
    struct sockaddr_un *sockaddr_unix = (struct sockaddr_un *) &resolved_address.addr;
    const char *path = target + strlen("unix:");

    if (strlen(path) == 0)
    {
        tio_error_printf("Missing socket filename in '%s'", target);
        exit(EXIT_FAILURE);
    }

    if (strlen(path) > sizeof(sockaddr_unix->sun_path) - 1)
    {
        tio_error_printf("Socket file path %s too long", path);
        exit(EXIT_FAILURE);
    }

    sockaddr_unix->sun_family = AF_UNIX;
    memcpy(sockaddr_unix->sun_path, path, strlen(path) + 1);

    resolved_address.family = AF_UNIX;
    resolved_address.addrlen = sizeof(struct sockaddr_un);
}

static void net_resolve_inet4(const char *target)
{
    const char *remainder = target + strlen("inet:");
    const char *colon = strrchr(remainder, ':');
    long port = NET_PORT_DEFAULT;
    char host[NI_MAXHOST];

    net_copy_host(host, sizeof(host), remainder,
                  (colon != NULL) ? (size_t) (colon - remainder) : strlen(remainder), target);

    if (colon != NULL)
    {
        port = net_parse_port(colon + 1, target);
    }

    net_resolve_inet(AF_INET, host, port, target);
}

static void net_resolve_inet6(const char *target)
{
    const char *remainder = target + strlen("inet6:");
    long port = NET_PORT_DEFAULT;
    char host[NI_MAXHOST];

    if (remainder[0] == '[')
    {
        // Brackets are what introduce a port: an unbracketed address followed
        // by a port cannot be told apart from a longer address
        const char *bracket = strchr(remainder, ']');

        if (bracket == NULL)
        {
            tio_error_printf("Missing closing bracket in '%s'", target);
            exit(EXIT_FAILURE);
        }

        net_copy_host(host, sizeof(host), remainder + 1, (size_t) (bracket - remainder - 1), target);

        if (bracket[1] == ':')
        {
            port = net_parse_port(bracket + 2, target);
        }
        else if (bracket[1] != '\0')
        {
            tio_error_printf("Unexpected characters after ']' in '%s'", target);
            exit(EXIT_FAILURE);
        }
    }
    else
    {
        // Whole remainder is the address, default port
        net_copy_host(host, sizeof(host), remainder, strlen(remainder), target);
    }

    net_resolve_inet(AF_INET6, host, port, target);
}

void net_resolve(const char *target)
{
    memset(&resolved_address, 0, sizeof(resolved_address));

    if (strncmp(target, "unix:", strlen("unix:")) == 0)
    {
        net_resolve_unix(target);
    }
    else if (strncmp(target, "inet6:", strlen("inet6:")) == 0)
    {
        net_resolve_inet6(target);
    }
    else if (strncmp(target, "inet:", strlen("inet:")) == 0)
    {
        net_resolve_inet4(target);
    }
    else
    {
        tio_error_printf("%s: Invalid socket scheme, must be prefixed with 'unix:', 'inet:', or 'inet6:'", target);
        exit(EXIT_FAILURE);
    }

    resolved = true;
}

const net_address_t *net_address_get(void)
{
    return resolved ? &resolved_address : NULL;
}
