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
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "telnet.h"
#include "net.h"
#include "options.h"
#include "tty.h"
#include "print.h"

/* RFC 854 */
#define IAC  255
#define SE   240
#define SB   250
#define WILL 251
#define WONT 252
#define DO   253
#define DONT 254

/* Options we take a position on */
#define OPT_BINARY            0   /* RFC 856 */
#define OPT_SUPPRESS_GO_AHEAD 3   /* RFC 858 */
#define OPT_COM_PORT          44  /* RFC 2217 */

/* RFC 2217 client commands. A server answers each with the same value plus
 * SERVER_OFFSET, carrying the setting it actually applied. */
#define COM_SET_BAUDRATE 1
#define COM_SET_DATASIZE 2
#define COM_SET_PARITY   3
#define COM_SET_STOPSIZE 4
#define COM_SET_CONTROL  5
#define COM_PURGE_DATA   12
#define SERVER_OFFSET    100

/* PURGE-DATA values */
#define PURGE_RX   1
#define PURGE_TX   2
#define PURGE_BOTH 3

/* RFC 2217 SET-CONTROL values, in the order the protocol assigns them. Each of
 * the three things that can be driven has its own triple: ask what it is, set it
 * on, set it off - and the ask comes FIRST in each triple. That is the trap. An
 * earlier version of this table began at break-on, so every value from four up
 * was read as the command one above it: asking what the break state was sent a
 * real break, asking about DTR dropped it, and setting DTR drove RTS. */
#define CONTROL_FLOW_REQUEST     0
#define CONTROL_FLOW_NONE        1
#define CONTROL_FLOW_SOFT        2
#define CONTROL_FLOW_HARD        3
#define CONTROL_BREAK_REQUEST    4
#define CONTROL_BREAK_ON         5
#define CONTROL_BREAK_OFF        6
#define CONTROL_DTR_REQUEST      7
#define CONTROL_DTR_ON           8
#define CONTROL_DTR_OFF          9
#define CONTROL_RTS_REQUEST     10
#define CONTROL_RTS_ON          11
#define CONTROL_RTS_OFF         12
#define CONTROL_FLOW_IN_REQUEST 13
#define CONTROL_FLOW_IN_NONE    14
#define CONTROL_FLOW_IN_SOFT    15
#define CONTROL_FLOW_IN_HARD    16

/* Flow control keyed off a modem line. Not offered, and answered as no flow
 * control rather than echoed, so a client is not told a mode was adopted. */
#define CONTROL_DCD_FLOW        17
#define CONTROL_DTR_FLOW        18
#define CONTROL_DSR_FLOW        19

/* Matches what tcsendbreak sends for a zero duration, so a break behaves the
 * same whether the port is local or at the far end of a socket */
#define BREAK_DURATION_US 250000

/* RFC 2217 parity values */
#define PARITY_VALUE_NONE  1
#define PARITY_VALUE_ODD   2
#define PARITY_VALUE_EVEN  3
#define PARITY_VALUE_MARK  4
#define PARITY_VALUE_SPACE 5

/* This session connects to exactly one peer as a client, so its context can
 * live here rather than being handed in at every call site */
static telnet_t client_context;

telnet_t *telnet_client(void)
{
    return &client_context;
}

void telnet_reset(telnet_t *telnet, telnet_role_t role, bool enabled)
{
    memset(telnet, 0, sizeof(*telnet));
    telnet->state = TELNET_STATE_DATA;
    telnet->role = role;
    telnet->enabled = enabled;
}

bool telnet_engaged(const telnet_t *telnet)
{
    return telnet->engaged;
}

/* Binary keeps the high bit of a serial byte intact and suppress-go-ahead puts
 * the link in the character-at-a-time mode a console needs. Anything else is
 * declined: this is a serial session, not a terminal login. */
static bool option_wanted(unsigned char opt)
{
    return (opt == OPT_BINARY) || (opt == OPT_SUPPRESS_GO_AHEAD) ||
           (opt == OPT_COM_PORT);
}

static void send_command(int fd, unsigned char command, unsigned char opt)
{
    unsigned char message[3] = { IAC, command, opt };

    net_send_raw(fd, message, sizeof(message));
}

static void handle_remote_offer(telnet_t *telnet, int fd, unsigned char opt, bool offered)
{
    bool wanted = offered && option_wanted(opt);

    if (telnet->remote_answered[opt] && (telnet->remote_enabled[opt] == wanted))
    {
        /* Already settled this way - answering again would only invite the
         * peer to repeat itself */
        return;
    }

    if (!(wanted && (opt == OPT_COM_PORT) && telnet->com_port_do_sent))
    {
        /* Skipped only when this is the peer agreeing to a DO already sent from
         * here: repeating it would be a second request for the same option.
         * Mirrors the same test on the WILL side. */
        send_command(fd, wanted ? DO : DONT, opt);
    }
    telnet->remote_enabled[opt] = wanted;
    telnet->remote_answered[opt] = true;
}

static void handle_local_request(telnet_t *telnet, int fd, unsigned char opt, bool requested)
{
    bool wanted = requested && option_wanted(opt);

    if (telnet->local_answered[opt] && (telnet->local_enabled[opt] == wanted))
    {
        return;
    }

    if (!(wanted && (opt == OPT_COM_PORT) && telnet->com_port_will_sent))
    {
        /* Skipped only when this is the peer agreeing to a WILL already sent from
         * here: repeating it would be a second offer of the same option */
        send_command(fd, wanted ? WILL : WONT, opt);
    }
    telnet->local_enabled[opt] = wanted;
    telnet->local_answered[opt] = true;

    if (wanted && (opt == OPT_COM_PORT) && (telnet->role == TELNET_ROLE_CLIENT))
    {
        /* The option is live now, so the settings the user asked for on the
         * command line can be carried to the remote port.
         *
         * Only from the client. These are requests - the command numbers without
         * the server offset - and a server has no remote port to configure. This
         * function serves both roles, so without the test a client sending the
         * legal IAC DO COM-PORT made the server answer correctly and then send it
         * a client's requests, which is a direction the protocol has no meaning
         * in. A peer that negotiates both directions of an option sends exactly
         * that; the harness's own test server opens with it. */
        telnet_send_port_settings(telnet, fd);
    }
}

/* A value byte equal to the command marker has to be doubled inside a
 * subnegotiation too, or the peer reads it as the end of one */
static size_t append_value(unsigned char *out, size_t at, unsigned char value)
{
    out[at++] = value;
    if (value == IAC)
    {
        out[at++] = IAC;
    }

    return at;
}

static void send_subneg(int fd, unsigned char command, const unsigned char *values, size_t count)
{
    unsigned char message[8 + 2 * 4];
    size_t at = 0;

    message[at++] = IAC;
    message[at++] = SB;
    message[at++] = OPT_COM_PORT;
    message[at++] = command;

    for (size_t i = 0; i < count; i++)
    {
        at = append_value(message, at, values[i]);
    }

    message[at++] = IAC;
    message[at++] = SE;

    net_send_raw(fd, message, at);
}

static unsigned char parity_value(parity_t parity)
{
    switch (parity)
    {
        case PARITY_ODD:
            return PARITY_VALUE_ODD;
        case PARITY_EVEN:
            return PARITY_VALUE_EVEN;
        case PARITY_MARK:
            return PARITY_VALUE_MARK;
        case PARITY_SPACE:
            return PARITY_VALUE_SPACE;
        case PARITY_NONE:
        default:
            return PARITY_VALUE_NONE;
    }
}

void telnet_send_port_settings(telnet_t *telnet, int fd)
{
    unsigned char values[4];

    if (!telnet->local_enabled[OPT_COM_PORT])
    {
        /* Peer declined the option, so the session carries bytes only and the
         * serial settings stay a local matter */
        return;
    }

    /* Only what was actually asked for, and only when it has changed since it
     * was last sent. A remote port is somebody's console and reconfiguring one
     * nobody asked to change is not harmless: applying a rate to a live line
     * can glitch it into a break, and a break on a Linux console arms SysRq,
     * whose next character is the command. Attaching to a console must not be
     * able to reboot the far end, and neither must pressing a mapping key.
     *
     * Each setting carries its own comparison. A single guard over all four
     * could not work: a setting the user never named keeps the zero it was
     * reset to while the option holds its default, so the guard never matched
     * and every reconfigure resent the ones that were named.
     *
     * Zero doubles as "not sent yet" because it is not a value any of the four
     * is ever sent with, which is a narrower claim than it being invalid. A
     * zero baud rate is real - it is in the probed rate list, B0 hangs a local
     * line up, and -b 0 is accepted - but it never reaches the wire: the
     * inbound check rejects it, and RFC 2217 reserves zero on all four setting
     * commands for "report the current value", so sending it would ask a
     * question rather than hang the far end up. */
    if (option.baudrate_set && (telnet->requested_baudrate != option.baudrate))
    {
        telnet->requested_baudrate = option.baudrate;
        values[0] = (unsigned char) ((unsigned int) option.baudrate >> 24);
        values[1] = (unsigned char) ((unsigned int) option.baudrate >> 16);
        values[2] = (unsigned char) ((unsigned int) option.baudrate >> 8);
        values[3] = (unsigned char) ((unsigned int) option.baudrate);
        send_subneg(fd, COM_SET_BAUDRATE, values, 4);
    }

    if (option.databits_set && (telnet->requested_databits != option.databits))
    {
        telnet->requested_databits = option.databits;
        values[0] = (unsigned char) option.databits;
        send_subneg(fd, COM_SET_DATASIZE, values, 1);
    }

    if (option.parity_set && (telnet->requested_parity != parity_value(option.parity)))
    {
        telnet->requested_parity = parity_value(option.parity);
        values[0] = (unsigned char) telnet->requested_parity;
        send_subneg(fd, COM_SET_PARITY, values, 1);
    }

    if (option.stopbits_set && (telnet->requested_stopbits != option.stopbits))
    {
        telnet->requested_stopbits = option.stopbits;
        values[0] = (unsigned char) option.stopbits;
        send_subneg(fd, COM_SET_STOPSIZE, values, 1);
    }
}

bool telnet_serial_control(const telnet_t *telnet)
{
    return telnet->local_enabled[OPT_COM_PORT];
}

bool telnet_send_break(telnet_t *telnet, int fd)
{
    unsigned char value;

    if (!telnet_serial_control(telnet))
    {
        return false;
    }

    value = CONTROL_BREAK_ON;
    send_subneg(fd, COM_SET_CONTROL, &value, 1);

    usleep(BREAK_DURATION_US);

    value = CONTROL_BREAK_OFF;
    send_subneg(fd, COM_SET_CONTROL, &value, 1);

    return true;
}

bool telnet_set_line(telnet_t *telnet, int fd, telnet_line_t line, bool assert_line)
{
    unsigned char value;

    if (!telnet_serial_control(telnet))
    {
        return false;
    }

    if (line == TELNET_LINE_DTR)
    {
        value = assert_line ? CONTROL_DTR_ON : CONTROL_DTR_OFF;
    }
    else
    {
        value = assert_line ? CONTROL_RTS_ON : CONTROL_RTS_OFF;
    }

    send_subneg(fd, COM_SET_CONTROL, &value, 1);

    return true;
}

static void report_setting(const char *name, long applied, long wanted)
{
    if (applied == wanted)
    {
        tio_printf("Remote port %s set to %ld", name, applied);
    }
    else
    {
        /* Saying "set" here would be a lie the user acts on later */
        tio_warning_printf("Remote port %s %ld not applied, port is at %ld", name, wanted, applied);
    }
}

static const char *parity_name(long value)
{
    switch (value)
    {
        case PARITY_VALUE_NONE:
            return "none";
        case PARITY_VALUE_ODD:
            return "odd";
        case PARITY_VALUE_EVEN:
            return "even";
        case PARITY_VALUE_MARK:
            return "mark";
        case PARITY_VALUE_SPACE:
            return "space";
        default:
            return "unknown";
    }
}

/* Parity is the one setting whose wire value means nothing to a reader, so it
 * is reported by name rather than by the number that crossed the link */
static void report_parity(long applied, long wanted)
{
    if (applied == wanted)
    {
        tio_printf("Remote port parity set to %s", parity_name(applied));
    }
    else
    {
        tio_warning_printf("Remote port parity %s not applied, port is at %s",
                           parity_name(wanted), parity_name(applied));
    }
}

static void handle_com_port_response(const telnet_t *telnet)
{
    unsigned char command;

    if (telnet->subneg_length < 2)
    {
        return;
    }

    command = telnet->subneg[1];

    switch (command)
    {
        case COM_SET_BAUDRATE + SERVER_OFFSET:
            if (telnet->subneg_length >= 6)
            {
                long applied = ((long) telnet->subneg[2] << 24) | ((long) telnet->subneg[3] << 16) |
                               ((long) telnet->subneg[4] << 8) | (long) telnet->subneg[5];
                report_setting("baud rate", applied, telnet->requested_baudrate);
            }
            break;

        case COM_SET_DATASIZE + SERVER_OFFSET:
            if (telnet->subneg_length >= 3)
            {
                report_setting("data bits", telnet->subneg[2], telnet->requested_databits);
            }
            break;

        case COM_SET_PARITY + SERVER_OFFSET:
            if (telnet->subneg_length >= 3)
            {
                report_parity(telnet->subneg[2], telnet->requested_parity);
            }
            break;

        case COM_SET_STOPSIZE + SERVER_OFFSET:
            if (telnet->subneg_length >= 3)
            {
                report_setting("stop bits", telnet->subneg[2], telnet->requested_stopbits);
            }
            break;

        default:
            break;
    }
}

/* The reverse of parity_value(): what the client asked for, in the terms the
 * device is configured in */
static parity_t parity_from_value(unsigned char value)
{
    switch (value)
    {
        case PARITY_VALUE_ODD:
            return PARITY_ODD;
        case PARITY_VALUE_EVEN:
            return PARITY_EVEN;
        case PARITY_VALUE_MARK:
            return PARITY_MARK;
        case PARITY_VALUE_SPACE:
            return PARITY_SPACE;
        case PARITY_VALUE_NONE:
        default:
            return PARITY_NONE;
    }
}

/* A server answers every request with the setting that ended up in effect,
 * which is the requested one only when the device could take it. Answering
 * with the request instead would tell the client a device it cannot see is
 * configured in a way it is not. */
/* Which of the things behind SET-CONTROL a request is about. One command covers
 * flow control in each direction, the break, and the two modem lines, and each
 * has its own values.
 *
 * Deciding the group once is the point. Acting on a request and answering it used
 * to switch on the value separately, and when the value table was wrong both
 * switches were wrong in the same way - so the reply agreed with the action and a
 * client asking for DTR was told DTR while RTS moved. One classification cannot
 * disagree with itself. */
typedef enum
{
    CONTROL_GROUP_FLOW_OUT,
    CONTROL_GROUP_FLOW_IN,
    CONTROL_GROUP_BREAK,
    CONTROL_GROUP_DTR,
    CONTROL_GROUP_RTS,
    CONTROL_GROUP_UNSUPPORTED,
} control_group_t;

static control_group_t control_group(unsigned char request)
{
    switch (request)
    {
        case CONTROL_FLOW_REQUEST:
        case CONTROL_FLOW_NONE:
        case CONTROL_FLOW_SOFT:
        case CONTROL_FLOW_HARD:
            return CONTROL_GROUP_FLOW_OUT;

        case CONTROL_FLOW_IN_REQUEST:
        case CONTROL_FLOW_IN_NONE:
        case CONTROL_FLOW_IN_SOFT:
        case CONTROL_FLOW_IN_HARD:
            return CONTROL_GROUP_FLOW_IN;

        case CONTROL_BREAK_REQUEST:
        case CONTROL_BREAK_ON:
        case CONTROL_BREAK_OFF:
            return CONTROL_GROUP_BREAK;

        case CONTROL_DTR_REQUEST:
        case CONTROL_DTR_ON:
        case CONTROL_DTR_OFF:
            return CONTROL_GROUP_DTR;

        case CONTROL_RTS_REQUEST:
        case CONTROL_RTS_ON:
        case CONTROL_RTS_OFF:
            return CONTROL_GROUP_RTS;

        default:
            return CONTROL_GROUP_UNSUPPORTED;
    }
}

/* Carry out a request. Only the set values do anything: a request value asks
 * what the state is and must leave it alone, which is what makes reading the
 * table correctly load-bearing rather than cosmetic. */
static void control_apply(unsigned char request)
{
    switch (request)
    {
        case CONTROL_BREAK_ON:
            tty_apply_break();
            break;

        case CONTROL_DTR_ON:
        case CONTROL_DTR_OFF:
            tty_apply_dtr(request == CONTROL_DTR_ON);
            break;

        case CONTROL_RTS_ON:
        case CONTROL_RTS_OFF:
            tty_apply_rts(request == CONTROL_RTS_ON);
            break;

        default:
            /* Break-off needs nothing, since the break already released the
             * line; flow control is not offered; and every request value is a
             * question rather than an instruction. */
            break;
    }
}

/* What to answer with: the state the port is in, not an echo of the request. */
static unsigned char control_state(unsigned char request)
{
    switch (control_group(request))
    {
        case CONTROL_GROUP_FLOW_OUT:
            /* A client cannot change it here, so the answer is what the port was
             * configured with rather than what was asked for. */
            return (unsigned char) tty_flow_control_value();

        case CONTROL_GROUP_FLOW_IN:
            /* The same three answers, in the inbound range */
            return (unsigned char) (CONTROL_FLOW_IN_REQUEST + tty_flow_control_value());

        case CONTROL_GROUP_BREAK:
            /* The break is a pulse rather than a state that is held, so by the
             * time this answers the line has already been released */
            return CONTROL_BREAK_OFF;

        case CONTROL_GROUP_DTR:
            return tty_dtr_asserted() ? CONTROL_DTR_ON : CONTROL_DTR_OFF;

        case CONTROL_GROUP_RTS:
            return tty_rts_asserted() ? CONTROL_RTS_ON : CONTROL_RTS_OFF;

        case CONTROL_GROUP_UNSUPPORTED:
        default:
            /* Flow control keyed off a modem line, which this does not do.
             * Echoing the request back would tell the client the mode was
             * adopted; saying no flow control is the truth. */
            return CONTROL_FLOW_NONE;
    }
}

static void handle_com_port_request(const telnet_t *telnet, int fd)
{
    unsigned char command;
    unsigned char values[4];
    int applied;

    if (telnet->subneg_length < 2)
    {
        return;
    }

    command = telnet->subneg[1];

    switch (command)
    {
        case COM_SET_BAUDRATE:
            if (telnet->subneg_length >= 6)
            {
                int asked = (int) (((long) telnet->subneg[2] << 24) | ((long) telnet->subneg[3] << 16) |
                                   ((long) telnet->subneg[4] << 8) | (long) telnet->subneg[5]);

                applied = tty_apply_baudrate(asked);
                values[0] = (unsigned char) ((unsigned int) applied >> 24);
                values[1] = (unsigned char) ((unsigned int) applied >> 16);
                values[2] = (unsigned char) ((unsigned int) applied >> 8);
                values[3] = (unsigned char) applied;
                send_subneg(fd, COM_SET_BAUDRATE + SERVER_OFFSET, values, 4);
            }
            break;

        case COM_SET_DATASIZE:
            if (telnet->subneg_length >= 3)
            {
                values[0] = (unsigned char) tty_apply_databits(telnet->subneg[2]);
                send_subneg(fd, COM_SET_DATASIZE + SERVER_OFFSET, values, 1);
            }
            break;

        case COM_SET_PARITY:
            if (telnet->subneg_length >= 3)
            {
                parity_t got;

                if ((telnet->subneg[2] < PARITY_VALUE_NONE) ||
                    (telnet->subneg[2] > PARITY_VALUE_SPACE))
                {
                    /* Same rule as the other three settings: a value this does
                     * not recognise is answered with what the port is set to
                     * rather than acted on. Zero arrives here in normal use -
                     * it is how a client asks what the current setting is -
                     * and mapping it onto a parity would answer the question
                     * by changing the answer. */
                    got = option.parity;
                }
                else
                {
                    got = tty_apply_parity(parity_from_value(telnet->subneg[2]));
                }

                values[0] = parity_value(got);
                send_subneg(fd, COM_SET_PARITY + SERVER_OFFSET, values, 1);
            }
            break;

        case COM_SET_STOPSIZE:
            if (telnet->subneg_length >= 3)
            {
                values[0] = (unsigned char) tty_apply_stopbits(telnet->subneg[2]);
                send_subneg(fd, COM_SET_STOPSIZE + SERVER_OFFSET, values, 1);
            }
            break;

        case COM_SET_CONTROL:
            if (telnet->subneg_length >= 3)
            {
                unsigned char request = telnet->subneg[2];

                control_apply(request);

                /* Answer it. Every other setting command reports the value the
                 * port ended up at and this one reported nothing, which is not
                 * a missing nicety: a client that waits for the answer cannot
                 * finish opening the port. pyserial does exactly that and timed
                 * out here, so a served socket could not be opened by it at all
                 * - the one thing the option exists for. */
                values[0] = control_state(request);
                send_subneg(fd, COM_SET_CONTROL + SERVER_OFFSET, values, 1);
            }
            break;

        case COM_PURGE_DATA:
            if (telnet->subneg_length >= 3)
            {
                unsigned char what = telnet->subneg[2];

                /* Discarding buffered data is part of opening a port for a
                 * client that wants a known starting state, and pyserial asks
                 * for it during open - so leaving it unimplemented stopped the
                 * session getting established at all, the same way an
                 * unanswered control request did. */
                bool input = (what == PURGE_RX) || (what == PURGE_BOTH);
                bool output = (what == PURGE_TX) || (what == PURGE_BOTH);
                unsigned char done = 0;

                if (tty_apply_purge(input, output))
                {
                    done = what;
                }

                /* Answer with what was discarded, not with what was asked for.
                 * Every other command in this switch reports the value the port
                 * ended up at; echoing the request told a client that sent an
                 * undefined value - or one whose flush failed - that it had been
                 * carried out. */
                values[0] = done;
                send_subneg(fd, COM_PURGE_DATA + SERVER_OFFSET, values, 1);
            }
            break;

        default:
            break;
    }
}

static void handle_subneg(const telnet_t *telnet, int fd)
{
    if (telnet->subneg_overflow)
    {
        /* A payload longer than anything this speaks; acting on a truncated
         * one would be worse than ignoring it */
        return;
    }

    if ((telnet->subneg_length < 1) || (telnet->subneg[0] != OPT_COM_PORT))
    {
        return;
    }

    if (telnet->role == TELNET_ROLE_SERVER)
    {
        if (!telnet->remote_enabled[OPT_COM_PORT])
        {
            /* The client never took the option. Acting on its requests anyway
             * would let a peer that declined the protocol - or never answered
             * at all - configure the port the protocol governs.
             *
             * Say so rather than dropping it silently: a client that treats the
             * unsolicited offer as sufficient and never answers is the first
             * thing this rejects, and from the far end it looks like requests
             * vanishing for no reason. */
            tio_debug_printf("Ignoring serial port request from a client that has not taken the option");
            return;
        }

        handle_com_port_request(telnet, fd);
    }
    else
    {
        handle_com_port_response(telnet);
    }
}

void telnet_server_offer(telnet_t *telnet, int fd)
{
    if (!telnet->enabled)
    {
        return;
    }

    /* Speaking first is the commitment. Waiting for the client to send a
     * command marker before escaping would leave every device byte equal to
     * one going out bare in the meantime, and a client that only ever reads
     * never closes that window at all. */
    telnet->engaged = true;
    /* Binary in both directions so the high bit of a serial byte survives,
     * and the serial-port option so a client can configure the device */
    send_command(fd, WILL, OPT_BINARY);
    telnet->local_enabled[OPT_BINARY] = true;
    telnet->local_answered[OPT_BINARY] = true;

    send_command(fd, DO, OPT_BINARY);
    telnet->remote_enabled[OPT_BINARY] = true;
    telnet->remote_answered[OPT_BINARY] = true;

    send_command(fd, DO, OPT_COM_PORT);

    /* Record that the offer was made, so the client's answer is read as an answer
     * rather than as an unsolicited offer needing one - which is what drew a
     * second DO for the same option on every negotiated session.
     *
     * Deliberately NOT remote_enabled/remote_answered, which the two lines above
     * do set for binary. remote_enabled[OPT_COM_PORT] is the test that refuses
     * serial requests from a client that never took the option; asserting it here
     * would mean a client that ignores the offer and sends requests anyway gets
     * them honoured. Asking is not being answered. */
    telnet->com_port_do_sent = true;
}

static void handle_option(telnet_t *telnet, int fd, unsigned char command, unsigned char opt)
{
    switch (command)
    {
        case WILL:
            handle_remote_offer(telnet, fd, opt, true);
            break;

        case WONT:
            handle_remote_offer(telnet, fd, opt, false);
            break;

        case DO:
            handle_local_request(telnet, fd, opt, true);
            break;

        case DONT:
            handle_local_request(telnet, fd, opt, false);
            break;

        default:
            break;
    }

    /* RFC 2217 has the client ask for the serial-port option and the server
     * agree, but this client only ever answered, so a server that waits to be
     * asked was met with silence: the option never came up, and a named baud
     * rate, a break and the modem lines all quietly did nothing.
     *
     * Ask, but only after the peer has negotiated something - which is what
     * this function handling an option means. Offering on connect instead would
     * put three protocol bytes in front of a peer that speaks no protocol, and
     * tio's own socket server forwards whatever it is sent straight to the
     * serial device, so those bytes would land on somebody's console. */
    if ((telnet->role == TELNET_ROLE_CLIENT) && !telnet->com_port_will_sent
            && !telnet->local_answered[OPT_COM_PORT])
    {
        telnet->com_port_will_sent = true;
        send_command(fd, WILL, OPT_COM_PORT);
    }
}

size_t telnet_filter_input(telnet_t *telnet, int fd, char *buffer, size_t count)
{
    unsigned char *input = (unsigned char *) buffer;
    size_t kept = 0;

    if (!telnet->enabled)
    {
        /* Not a Telnet link, so there is no protocol in here to find. Every
         * byte is data, including the ones that look like a command. */
        return count;
    }

    for (size_t i = 0; i < count; i++)
    {
        unsigned char byte = input[i];

        switch (telnet->state)
        {
            case TELNET_STATE_DATA:
                if (byte == IAC)
                {
                    telnet->state = TELNET_STATE_COMMAND;
                }
                else
                {
                    input[kept++] = byte;
                }
                break;

            case TELNET_STATE_COMMAND:
                if (byte == IAC)
                {
                    /* Doubled marker - the peer means the byte itself */
                    input[kept++] = byte;
                    telnet->state = TELNET_STATE_DATA;
                }
                else if ((byte == WILL) || (byte == WONT) || (byte == DO) || (byte == DONT))
                {
                    telnet->pending_command = byte;
                    telnet->state = TELNET_STATE_OPTION;
                }
                else if (byte == SB)
                {
                    /* A subnegotiation is structure no raw byte stream produces
                     * by accident, so this settles that the peer speaks Telnet */
                    telnet->engaged = true;
                    telnet->subneg_length = 0;
                    telnet->subneg_overflow = false;
                    telnet->state = TELNET_STATE_SUBNEG;
                }
                else
                {
                    /* A command that carries no option, which a serial session
                     * has nothing to do with */
                    telnet->state = TELNET_STATE_DATA;
                }
                break;

            case TELNET_STATE_OPTION:
                /* A marker, a request and its option: a complete negotiation,
                 * which is the point the peer is known to speak Telnet */
                telnet->engaged = true;
                handle_option(telnet, fd, telnet->pending_command, byte);
                telnet->state = TELNET_STATE_DATA;
                break;

            case TELNET_STATE_SUBNEG:
                if (byte == IAC)
                {
                    telnet->state = TELNET_STATE_SUBNEG_COMMAND;
                }
                else if (telnet->subneg_length < sizeof(telnet->subneg))
                {
                    telnet->subneg[telnet->subneg_length++] = byte;
                }
                else
                {
                    telnet->subneg_overflow = true;
                }
                break;

            case TELNET_STATE_SUBNEG_COMMAND:
                if (byte == SE)
                {
                    handle_subneg(telnet, fd);
                    telnet->state = TELNET_STATE_DATA;
                }
                else
                {
                    /* A doubled marker inside the payload stands for the byte
                     * itself, as it does in the data stream */
                    if (telnet->subneg_length < sizeof(telnet->subneg))
                    {
                        telnet->subneg[telnet->subneg_length++] = byte;
                    }
                    else
                    {
                        telnet->subneg_overflow = true;
                    }
                    telnet->state = TELNET_STATE_SUBNEG;
                }
                break;
        }
    }

    return kept;
}

ssize_t telnet_send(const telnet_t *telnet, int fd, const void *buffer, size_t count)
{
    const unsigned char *input = (const unsigned char *) buffer;
    unsigned char escaped[2 * BUFSIZ];
    size_t consumed = 0;
    size_t produced = 0;
    size_t sent = 0;

    if (!telnet_engaged(telnet))
    {
        /* Not a Telnet peer, so a byte equal to the command marker is a byte.
         * Deciding that here rather than at the call sites is the point: the rule
         * was implemented twice, once here and once inline for the server, and
         * two encodings of one rule are what let the read and write sides drift
         * apart before. */
        return net_send_raw(fd, buffer, count);
    }

    /* Stop short of the caller's buffer rather than overrun ours; the caller
     * writes what is left on the next call */
    while ((consumed < count) && (produced + 2 <= sizeof(escaped)))
    {
        unsigned char byte = input[consumed++];

        escaped[produced++] = byte;
        if (byte == IAC)
        {
            escaped[produced++] = IAC;
        }
    }

    while (sent < produced)
    {
        ssize_t status = net_send_raw(fd, escaped + sent, produced - sent);

        if (status < 0)
        {
            if ((errno == EINTR) || (errno == EAGAIN) || (errno == EWOULDBLOCK))
            {
                /* Nothing has gone wrong with the stream, so carry on from
                 * where this stopped. Giving up here is what left a doubled
                 * marker split down the middle. */
                continue;
            }
        }

        if (status <= 0)
        {
            /* Report the caller's bytes whose escaped form went out in full,
             * and let it re-supply the rest.
             *
             * Returning the error instead threw away how much had already been
             * sent, and the caller - which resumes from what it was told went
             * out - resent those bytes on top of the ones already on the wire.
             * That is the mirror of losing them: the wire gained bytes because
             * the accounting fell behind it. */
            size_t escaped_len = 0;
            size_t whole = 0;

            while (whole < consumed)
            {
                size_t width = (input[whole] == IAC) ? 2u : 1u;

                if ((escaped_len + width) > sent)
                {
                    break;
                }
                escaped_len += width;
                whole++;
            }

            if (whole > 0)
            {
                return (ssize_t) whole;
            }
            return status;
        }

        sent += status;
    }

    return (ssize_t) consumed;
}
