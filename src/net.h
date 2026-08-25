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

#pragma once

#include <stdbool.h>
#include <sys/socket.h>

// A socket address resolved once from a target string. Held so that the
// reconnect loop can retry without resolving again.
typedef struct
{
    int family;
    struct sockaddr_storage addr;
    socklen_t addrlen;
} net_address_t;

bool net_target_is_socket(const char *target);

// Parse and resolve a 'unix:', 'inet:' or 'inet6:' target and cache the
// result. Reports the offending part and exits on a malformed target.
void net_resolve(const char *target);

// The address cached by net_resolve(), or NULL if it has not run
const net_address_t *net_address_get(void);

// Filesystem path of the resolved endpoint, or NULL when it has none. Only a
// 'unix:' target has one, which is what makes it testable while waiting.
const char *net_socket_path(void);

// Create and connect a socket against the address cached by net_resolve().
// Returns the connected file descriptor, or -1 on failure.
int net_connect(void);

// Write to a connected socket without raising SIGPIPE when the peer is gone,
// applying whatever the negotiated protocol requires on the way out
ssize_t net_send(int fd, const void *buffer, size_t count);

// Write the bytes given and nothing else, for a caller that has already
// applied that protocol and must not have it applied twice
ssize_t net_send_raw(int fd, const void *buffer, size_t count);
