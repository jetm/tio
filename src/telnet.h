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

#define TELNET_OPTION_COUNT 256
#define TELNET_SUBNEG_MAX   64

typedef enum
{
    TELNET_STATE_DATA,
    TELNET_STATE_COMMAND,
    TELNET_STATE_OPTION,
    TELNET_STATE_SUBNEG,
    TELNET_STATE_SUBNEG_COMMAND,
} telnet_state_t;

// Everything settled with one peer. Negotiation is per connection, so a
// session that faces several peers at once - the socket server does - needs one
// of these each, or one peer's negotiation would decide what the others get.
// Which end of the link this context sits on. The two see the same option
// negotiation but opposite subnegotiations: a client receives the settings a
// server applied, a server receives the settings a client is asking for.
typedef enum
{
    TELNET_ROLE_CLIENT,
    TELNET_ROLE_SERVER,
} telnet_role_t;

typedef struct
{
    telnet_role_t role;
    telnet_state_t state;
    unsigned char pending_command;
    bool engaged;

    // Whether any data byte has arrived from the peer yet. A peer that sends
    // data before it ever negotiates is not speaking Telnet, so a command
    // marker after that point is a data byte rather than the start of a
    // command. Without this a single 0xff of device output turns a raw peer
    // into a Telnet one for the rest of the session.
    bool saw_data;

    // What has been settled for each option, and whether it has been answered
    // at all. Both are needed: an unanswered request must be answered even when
    // the answer matches the current state, and an answered one must not be
    // answered again, which is what keeps the peer and us from trading the same
    // pair of commands forever.
    bool remote_enabled[TELNET_OPTION_COUNT];
    bool remote_answered[TELNET_OPTION_COUNT];
    bool local_enabled[TELNET_OPTION_COUNT];
    bool local_answered[TELNET_OPTION_COUNT];

    unsigned char subneg[TELNET_SUBNEG_MAX];
    size_t subneg_length;
    bool subneg_overflow;

    // What was last asked of the remote port, kept so that the server's answer
    // can be compared against it. A server is entitled to answer with a
    // different value, and that difference is the whole point: it means the
    // port could not take the setting, which the user has to be told rather
    // than shown as done.
    int requested_baudrate;
    int requested_databits;
    int requested_stopbits;
    int requested_parity;
} telnet_t;

// The modem control lines a client can drive. The remaining lines on a serial
// port are inputs, which no protocol makes writable.
typedef enum
{
    TELNET_LINE_DTR,
    TELNET_LINE_RTS,
} telnet_line_t;

// The context for the peer this session connects to as a client. It has
// exactly one, which is why it can be reached without being passed around.
telnet_t *telnet_client(void);

// Forget any negotiation state. Called per connection, since a reconnect faces
// a peer that has to negotiate again from nothing.
void telnet_reset(telnet_t *telnet, telnet_role_t role);

// Offer the options a socket client may want, which is what starts negotiation
// with a client that would otherwise wait to be spoken to. Only called when
// the user asked the server to make the offer, since a client that does not
// speak Telnet would read the offer as device output.
void telnet_server_offer(telnet_t *telnet, int fd);

// True once the peer has completed a negotiation - a command marker followed by
// a request and its option, or the start of a subnegotiation - which is what
// identifies it as speaking Telnet. Until then the stream is carried through
// untouched, so a raw peer - tio's own socket server among them - is not
// escaped at.
//
// A lone command marker is deliberately not enough. It is a legal data byte,
// and treating the first one as proof of Telnet meant a single 0xff of device
// output from a raw peer engaged the session permanently and doubled every
// marker sent back to a peer that would never un-double it.
bool telnet_engaged(const telnet_t *telnet);

// True when the peer took the serial-port option, which is what decides
// whether an operation needing a serial line can be carried to it at all
bool telnet_serial_control(const telnet_t *telnet);

// Consume the protocol bytes in a block read from the peer, answering any
// negotiation on the way, and compact the remaining data down to the front of
// the same buffer. Returns how many data bytes are left.
size_t telnet_filter_input(telnet_t *telnet, int fd, char *buffer, size_t count);

// Ask the remote port for a break. Returns false when the peer never took the
// serial-port option, leaving the caller to report the operation unavailable.
bool telnet_send_break(telnet_t *telnet, int fd);

// Drive a modem control line on the remote port. Returns false on a peer that
// did not take the serial-port option.
bool telnet_set_line(telnet_t *telnet, int fd, telnet_line_t line, bool assert_line);

// Carry the serial settings to the remote port. Does nothing when the peer
// declined the serial-port option, which leaves the session a plain byte
// stream rather than a failure.
void telnet_send_port_settings(telnet_t *telnet, int fd);

// Write to a peer that speaks Telnet, doubling the command marker so that a
// data byte equal to it is not read as the start of a command. Returns the
// number of bytes of the caller's buffer that were sent, not the number of
// bytes that went out on the wire.
ssize_t telnet_send(int fd, const void *buffer, size_t count);
