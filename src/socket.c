/*
 * tio - a serial device I/O tool
 *
 * Copyright (c) 2014-2022  Martin Lund
 * Copyright (c) 2022  Google LLC
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
#include <stdio.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <unistd.h>
#include <string.h>

#include "socket.h"
#include "net.h"
#include "options.h"
#include "print.h"
#include "telnet.h"
#include "tty.h"

#define MAX_SOCKET_CLIENTS 16

static int sockfd;
static int clientfds[MAX_SOCKET_CLIENTS];

/* One negotiation state per client, because they do not agree: one may speak
 * Telnet while the next is a plain nc, and a single shared state would let the
 * first decide what the second receives */
static telnet_t clienttelnet[MAX_SOCKET_CLIENTS];
static int socket_family = AF_UNSPEC;
static int port_number = NET_PORT_DEFAULT;

static int socket_client_count(void)
{
    int count = 0;

    for (int i = 0; i != MAX_SOCKET_CLIENTS; ++i)
    {
        if (clientfds[i] != -1)
        {
            count++;
        }
    }

    return count;
}

static const char *socket_filename(void)
{
    /* skip 'unix:' */
    return option.socket + 5;
}

/* Through net_parse_port, not atoi. This is the same option syntax the client side
 * parses, and it already had a parser that validates; atoi() reports nothing, so
 * `--socket inet:htpp` listened on 3333, `inet:99999` announced 99999 and listened on
 * 34463 once the 16-bit port field cut its high bits off, and `inet:8080x` quietly became
 * 8080. Announcing a port it is not listening on is the worst of the three, because the
 * operator's own evidence is then the thing misleading them.
 *
 * An ABSENT port is not one of those, and it is handled here rather than in the parser
 * because the two callers mean different things by "empty". For the server the port is the
 * whole remainder of the option, so nothing there means no port was given, which
 * tio.1 documents as the default: `--socket inet:` has listened on 3333 for as long as the
 * option has existed. The client only reaches the parser with text that followed a colon,
 * where empty means a colon was typed with nothing behind it and an error is right. Putting
 * the default in the parser would make it substitute one for `inet:host:` too. */
static int socket_port_or_default(const char *remainder)
{
    if (remainder[0] == '\0')
    {
        return NET_PORT_DEFAULT;
    }

    return (int) net_parse_port(remainder, option.socket);
}

static int socket_inet_port(void)
{
    /* skip 'inet:' */
    return socket_port_or_default(option.socket + 5);
}

static int socket_inet6_port(void)
{
    /* skip 'inet6:' */
    return socket_port_or_default(option.socket + 6);
}

static void socket_exit(void)
{
    if (socket_family == AF_UNIX)
    {
        unlink(socket_filename());
    }
}

static bool socket_stale(const char *path)
{
    struct sockaddr_un addr;
    bool stale = false;
    int sfd;

    /* Test if socket file exists */
    if (access(path, F_OK) == 0)
    {
        /* Create test socket  */
        sfd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (sfd < 0)
        {
            tio_warning_printf("Failure opening socket (%s)", strerror(errno));
            return false;
        }

        /* Prepare address */
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

        /* Perform connect to test if socket is active */
        if (connect(sfd, (struct sockaddr *) &addr, sizeof(struct sockaddr_un)) == -1)
        {
            if (errno == ECONNREFUSED)
            {
                // No one is listening on socket file
                stale = true;
            }
        }

        /* Cleanup */
        close(sfd);
    }

    return stale;
}

void socket_configure(void)
{
    struct sockaddr_un sockaddr_unix = {};
    struct sockaddr_in sockaddr_inet = {};
    struct sockaddr_in6 sockaddr_inet6 = {};
    struct sockaddr *sockaddr_p;
    socklen_t socklen;
    int optval = 1;

    /* Parse socket string */

    if (strncmp(option.socket, "unix:", 5) == 0)
    {
        socket_family = AF_UNIX;

        if (strlen(socket_filename()) == 0)
        {
            tio_error_printf("Missing socket filename");
            exit(EXIT_FAILURE);
        }

        if (strlen(socket_filename()) > sizeof(sockaddr_unix.sun_path) - 1)
        {
            tio_error_printf("Socket file path %s too long", option.socket);
            exit(EXIT_FAILURE);
        }
    }

    if (strncmp(option.socket, "inet:", 5) == 0)
    {
        socket_family = AF_INET;

        port_number = socket_inet_port();

        if (port_number < 0)
        {
            tio_error_printf("Invalid port number: %d", port_number);
            exit(EXIT_FAILURE);
        }
    }

    if (strncmp(option.socket, "inet6:", 6) == 0)
    {
        socket_family = AF_INET6;

        port_number = socket_inet6_port();

        if (port_number < 0)
        {
            tio_error_printf("Invalid port number: %d", port_number);
            exit(EXIT_FAILURE);
        }
    }

    if (socket_family == AF_UNSPEC)
    {
        tio_error_printf("%s: Invalid socket scheme, must be prefixed with 'unix:', 'inet:', or 'inet6:'", option.socket);
        exit(EXIT_FAILURE);
    }

    /* Configure socket */

    switch (socket_family)
    {
        case AF_UNIX:
            sockaddr_unix.sun_family = AF_UNIX;
            strncpy(sockaddr_unix.sun_path, socket_filename(), sizeof(sockaddr_unix.sun_path) - 1);
            sockaddr_p = (struct sockaddr *) &sockaddr_unix;
            socklen = sizeof(sockaddr_unix);

            /* Test for stale unix socket file */
            if (socket_stale(socket_filename()))
            {
                tio_printf("Cleaning up old socket file");
                unlink(socket_filename());
            }

            break;

        case AF_INET:
            sockaddr_inet.sin_family = AF_INET;
            sockaddr_inet.sin_addr.s_addr = INADDR_ANY;
            sockaddr_inet.sin_port = htons(port_number);
            sockaddr_p = (struct sockaddr *) &sockaddr_inet;
            socklen = sizeof(sockaddr_inet);
            break;

        case AF_INET6:
            sockaddr_inet6.sin6_family = AF_INET6;
            sockaddr_inet6.sin6_addr = in6addr_any;
            sockaddr_inet6.sin6_port = htons(port_number);
            sockaddr_p = (struct sockaddr *) &sockaddr_inet6;
            socklen = sizeof(sockaddr_inet6);
            break;

        default:
            tio_error_printf("Invalid socket family (%d)", socket_family);
            exit(EXIT_FAILURE);
            break;
    }

    /* Create socket */
    sockfd = socket(socket_family, SOCK_STREAM, 0);
    if (sockfd < 0)
    {
        tio_error_printf("Failed to create socket (%s)", strerror(errno));
        exit(EXIT_FAILURE);
    }

    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)))
    {
        tio_error_printf("Failed to set socket options (%s)", strerror(errno));
        exit(EXIT_FAILURE);
    }

#if defined(SO_NOSIGPIPE) && !defined(MSG_NOSIGNAL)
    if (setsockopt(sockfd, SOL_SOCKET, SO_NOSIGPIPE, &optval, sizeof(optval)))
    {
        tio_error_printf("Failed to set socket options (%s)", strerror(errno));
        exit(EXIT_FAILURE);
    }
#endif

    /* Bind */
    if (bind(sockfd, sockaddr_p, socklen) < 0)
    {
        tio_error_printf("Failed to bind to socket (%s)", strerror(errno));
        exit(EXIT_FAILURE);
    }

    /* Listen */
    if (listen(sockfd, MAX_SOCKET_CLIENTS) < 0)
    {
        tio_error_printf("Failed to listen on socket (%s)", strerror(errno));
        exit(EXIT_FAILURE);
    }

    memset(clientfds, -1, sizeof(clientfds));
    atexit(socket_exit);

    if (socket_family == AF_UNIX)
    {
        tio_printf("Listening on socket %s", socket_filename());
    }
    else
    {
        tio_printf("Listening on socket port %d", port_number);
    }
}

void socket_write(char input_char)
{
    if (!option.socket)
    {
        return;
    }

    for (int i = 0; i != MAX_SOCKET_CLIENTS; ++i)
    {
        if (clientfds[i] != -1)
        {
            /* Only a client that negotiated is written to as a Telnet peer.
             * The rest receive the device's bytes and nothing else, which is
             * what every existing consumer of this socket expects - and
             * telnet_send() reads that from the client's own context, so the
             * doubling rule lives in one place rather than being spelled out
             * again here.
             *
             * The return is the caller's byte count, not the wire's, so one is
             * success whether the marker was doubled or not. Testing the wire
             * length was how a doubled marker sent as one byte took neither the
             * error path nor a retry: the second marker was dropped, and the
             * client read the surviving one as the start of a command and ate the
             * device byte after it - desynchronised for the rest of the session. */
            if (telnet_send(&clienttelnet[i], clientfds[i], &input_char, 1) != 1)
            {
                tio_error_printf_silent("Failed to write to socket (%s)", strerror(errno));
                close(clientfds[i]);
                clientfds[i] = -1;
            }
        }
    }
}

int socket_add_fds(fd_set *rdfs, bool connected)
{
    if (!option.socket)
    {
        return 0;
    }

    int numclients = 0, maxfd = 0;
    for (int i = 0; i != MAX_SOCKET_CLIENTS; ++i)
    {
        if (clientfds[i] != -1)
        {
            /* let clients block if they try to send while we're disconnected */
            if (connected)
            {
                FD_SET(clientfds[i], rdfs);
                maxfd = MAX(maxfd, clientfds[i]);
            }
            numclients++;
        }
    }
    /* don't bother to accept clients if we're already full */
    if (numclients != MAX_SOCKET_CLIENTS)
    {
        FD_SET(sockfd, rdfs);
        maxfd = MAX(maxfd, sockfd);
    }
    return maxfd;
}

bool socket_map_input_char(char *character)
{
    /* If INLCR is set, a received NL character shall be translated into a CR character */
    if (*character == '\n' && option.map_i_nl_cr)
    {
        *character = '\r';
    }
    else if (*character == '\r')
    {
        /* If IGNCR is set, a received CR character shall be ignored (not read). */
        if (option.map_ign_cr)
        {
            return false;
        }

        /* If IGNCR is not set and ICRNL is set, a received CR character shall be translated into an NL character. */
        if (option.map_i_cr_nl)
        {
            *character = '\n';
        }
    }

    return true;
}

bool socket_handle_input(fd_set *rdfs, char *output_char)
{
    if (!option.socket)
    {
        return false;
    }

    if (FD_ISSET(sockfd, rdfs))
    {
        int clientfd = accept(sockfd, NULL, NULL);

        /* Suppress SIGPIPE on this connection the way the client and the
         * listening socket already do. Every write to a client goes to a
         * descriptor from accept(), so relying on accept() to inherit the option
         * from the listener would make the whole server path depend on a
         * platform detail rather than on a call. */
#if defined(SO_NOSIGPIPE) && !defined(MSG_NOSIGNAL)
        if (clientfd >= 0)
        {
            int optval = 1;
            if (setsockopt(clientfd, SOL_SOCKET, SO_NOSIGPIPE, &optval, sizeof(optval)))
            {
                tio_warning_printf("Could not set socket options (%s)", strerror(errno));
            }
        }
#endif

        if (option.socket_rfc2217 && socket_client_count() > 0)
        {
            /* One client at a time when the port is configurable. The serial
             * settings are process-global, so a second client's baud rate,
             * framing, DTR or break silently lands on the line the first client
             * is using - and there is no NOTIFY-MODEMSTATE to tell it, nothing
             * restores anything when either leaves, so a third inherits
             * whatever the second did. Refusing is the honest answer: RFC 2217
             * describes one client configuring one port, and an unsynchronised
             * second client driving break on somebody's console is the fault
             * this option exists to avoid rather than to spread.
             *
             * Only the configurable case is limited. Without the option the
             * socket carries bytes, several readers of one console are a
             * reasonable thing to want, and all sixteen slots stay available. */
            static const char busy[] =
                "tio: RFC 2217 serves one client at a time; already in use\r\n";

            net_send_raw(clientfd, busy, sizeof(busy) - 1);
            close(clientfd);
            clientfd = -1;
        }

        /* this loop should always succeed because we don't select on sockfd when full */
        for (int i = 0; clientfd != -1 && i != MAX_SOCKET_CLIENTS; ++i)
        {
            if (clientfds[i] == -1)
            {
                clientfds[i] = clientfd;
                telnet_reset(&clienttelnet[i], TELNET_ROLE_SERVER, option.socket_rfc2217);

                /* Speak first, since a client has no way to know the option is
                 * on offer otherwise. Without the flag the offer is not made and
                 * the socket stays the raw pipe it was. */
                telnet_server_offer(&clienttelnet[i], clientfd);
                break;
            }
        }
    }
    for (int i = 0; i != MAX_SOCKET_CLIENTS; ++i)
    {
        if (clientfds[i] != -1 && FD_ISSET(clientfds[i], rdfs))
        {
            int status = read(clientfds[i], output_char, 1);
            if (status == 0)
            {
                close(clientfds[i]);
                clientfds[i] = -1;
                continue;
            }
            if (status < 0)
            {
                tio_error_printf_silent("Failed to read from socket (%s)", strerror(errno));
                close(clientfds[i]);
                clientfds[i] = -1;
                continue;
            }

            /* Only a socket that was told to speak Telnet parses it, which the
             * context itself knows. Without the option this is the byte pipe it
             * has always been, in both directions, and a client cannot reach the
             * serial port's configuration by sending bytes that happen to look
             * like protocol. */
            if (telnet_filter_input(&clienttelnet[i], clientfds[i], output_char, 1) == 0)
            {
                return false;
            }

            if (!socket_map_input_char(output_char))
            {
                /* Character dropped by IGNCR - nothing to forward */
                return false;
            }
            return true;
        }
    }
    return false;
}
