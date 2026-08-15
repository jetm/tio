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
#include <stddef.h>
#include <sys/types.h>

// Forget any negotiation state. Called per connection, since a reconnect faces
// a peer that has to negotiate again from nothing.
void telnet_reset(void);

// True once the peer has sent a command marker, which is what identifies it as
// speaking Telnet. Until then the stream is carried through untouched, so a
// raw peer - tio's own socket server among them - is not escaped at.
bool telnet_engaged(void);

// Consume the protocol bytes in a block read from the peer, answering any
// negotiation on the way, and compact the remaining data down to the front of
// the same buffer. Returns how many data bytes are left.
size_t telnet_filter_input(int fd, char *buffer, size_t count);

// Write to a peer that speaks Telnet, doubling the command marker so that a
// data byte equal to it is not read as the start of a command. Returns the
// number of bytes of the caller's buffer that were sent, not the number of
// bytes that went out on the wire.
ssize_t telnet_send(int fd, const void *buffer, size_t count);
