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

// Which end of the link this context sits on. The two see the same option
// negotiation but opposite subnegotiations: a client receives the settings a
// server applied, a server receives the settings a client is asking for.
typedef enum
{
    TELNET_ROLE_CLIENT,
    TELNET_ROLE_SERVER,
} telnet_role_t;

// Everything settled with one peer. Negotiation is per connection, so a
// session that faces several peers at once - the socket server does - needs one
// of these each, or one peer's negotiation would decide what the others get.
typedef struct
{
    telnet_role_t role;

    // Whether this context speaks Telnet at all. Both ends of the link are
    // opt-in and the user says so per side: --rfc2217 for the peer this session
    // connects to, --socket-rfc2217 for the peers it serves. Off, the stream is
    // carried through byte for byte and nothing is ever written to the peer
    // that the user did not type.
    //
    // It lives here rather than at the call sites so that the decision is made
    // once. A caller that forgets to ask cannot reach the protocol, which is
    // what an earlier per-call-site gate could not promise.
    bool enabled;

    telnet_state_t state;
    unsigned char pending_command;
    bool engaged;

    // A write to this peer could not be delivered. Recorded rather than returned because the
    // negotiation and answer paths have no local remedy - the useful response to a peer that
    // will not take its own answers is to stop having it, and only the code that owns the
    // connection can do that. Set by send_command and send_subneg, read by the owner after
    // telnet_filter_input returns; see telnet_write_failed().
    bool write_failed;

    // Whether this side has spoken first about an option, per option and per
    // direction: local_sent for a WILL we offered, remote_sent for a DO we asked.
    // Two arrays rather than one, because they are opposite commitments - WILL
    // offers to provide the option, DO asks the peer to - and sharing a flag made a
    // server that had sent DO suppress the WILL it owed a client that asked.
    //
    // Per option rather than per named option. These were two bools for the
    // serial-port option alone, which meant every other option's first word had no
    // record and the guards that use these carried an `opt == OPT_COM_PORT` test to
    // say so. That left binary asserting agreement it had not received, because the
    // only way to avoid re-answering a peer was to claim the option was already
    // live. One array removes both the special case and the claim.
    //
    // A client is otherwise purely reactive, which leaves it silent against a
    // server that waits to be asked; its WILL is only sent once the peer has shown
    // it speaks Telnet, so a raw peer is still never written to unasked.
    bool local_sent[TELNET_OPTION_COUNT];
    bool remote_sent[TELNET_OPTION_COUNT];

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
// a peer that has to negotiate again from nothing. Whether this side speaks
// Telnet is settled here too, because it is a property of the whole context and
// not of any one call into it.
void telnet_reset(telnet_t *telnet, telnet_role_t role, bool enabled);

// Offer the options a socket client may want, which is what starts negotiation
// with a client that would otherwise wait to be spoken to. Only called when
// the user asked the server to make the offer, since a client that does not
// speak Telnet would read the offer as device output.
void telnet_server_offer(telnet_t *telnet, int fd);

// True once the peer has completed a negotiation - a command marker followed by
// a request and its option, or the start of a subnegotiation. Never true on a
// context the user did not enable, so a session that was not asked to speak
// Telnet cannot be talked into it by the bytes that arrive.
bool telnet_engaged(const telnet_t *telnet);

// Whether a write to this peer failed to be delivered. Check it after
// telnet_filter_input(), beside the read-error handling: answering a peer happens from the
// read path, so a peer that cannot take its answers is discovered there and nowhere else. A
// caller that ignores this gets the send timeout's bound without its recovery, which lets
// such a peer stall the device loop once per message for as long as it keeps asking.
//
// All three callers check it, and that is deliberate rather than defensive on two of them.
// Only the server's accepted sockets carry a send timeout, so on the client this can fire
// only for a real error - but that is a fact about net_connect() in a different file, and a
// caller that skips the check is betting on it. The bet would be lost silently the day a
// timeout is added to the client socket, which is the kind of coupling this series has
// already paid for once.
bool telnet_write_failed(const telnet_t *telnet);

// True when the peer took the serial-port option, which is what decides
// whether an operation needing a serial line can be carried to it at all
bool telnet_serial_control(const telnet_t *telnet);

// Consume the protocol bytes in a block read from the peer, answering any
// negotiation on the way, and compact the remaining data down to the front of
// the same buffer. Returns how many data bytes are left.
size_t telnet_filter_input(telnet_t *telnet, int fd, char *buffer, size_t count);

// What became of a request to the remote port. Three outcomes rather than a bool,
// because "the session cannot carry this" and "the session could not deliver it" want
// different words to the user and, more importantly, different bookkeeping: a request
// that was never delivered must leave the caller's idea of the remote state alone.
// Collapsing them is what let a failed line request be recorded as a state change.
typedef enum
{
    TELNET_REQUEST_SENT,
    TELNET_REQUEST_UNAVAILABLE,
    TELNET_REQUEST_FAILED,
} telnet_request_t;

// Ask the remote port for a break. UNAVAILABLE when the peer never took the
// serial-port option, leaving the caller to report the operation unavailable.
telnet_request_t telnet_send_break(telnet_t *telnet, int fd);

// Drive a modem control line on the remote port. UNAVAILABLE on a peer that did not
// take the serial-port option; FAILED when the request could not be delivered, in which
// case the line did not move and no cached state may be advanced.
telnet_request_t telnet_set_line(telnet_t *telnet, int fd, telnet_line_t line, bool assert_line);

// Carry the serial settings to the remote port. Does nothing when the peer
// declined the serial-port option, which leaves the session a plain byte
// stream rather than a failure.
void telnet_send_port_settings(telnet_t *telnet, int fd);

// Write to a peer, doubling the command marker so that a data byte equal to it is
// not read as the start of a command. A peer that has not negotiated gets the
// bytes unaltered, so this is safe to call for any peer and is the only place the
// doubling rule is expressed. Returns the number of bytes of the caller's buffer
// that were sent, not the number of bytes that went out on the wire.
ssize_t telnet_send(const telnet_t *telnet, int fd, const void *buffer, size_t count);
