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

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "telnet.h"
#include "net.h"
#include "options.h"
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
#define SERVER_OFFSET    100

/* RFC 2217 SET-CONTROL values. A break is a pair rather than a duration: the
 * line is held and then released, so the length is the client's to decide. */
#define CONTROL_BREAK_ON  4
#define CONTROL_BREAK_OFF 5
#define CONTROL_DTR_ON    6
#define CONTROL_DTR_OFF   7
#define CONTROL_RTS_ON    8
#define CONTROL_RTS_OFF   9

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

void telnet_reset(telnet_t *telnet)
{
    memset(telnet, 0, sizeof(*telnet));
    telnet->state = TELNET_STATE_DATA;
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

    send_command(fd, wanted ? DO : DONT, opt);
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

    send_command(fd, wanted ? WILL : WONT, opt);
    telnet->local_enabled[opt] = wanted;
    telnet->local_answered[opt] = true;

    if (wanted && (opt == OPT_COM_PORT))
    {
        /* The option is live now, so the settings the user asked for on the
         * command line can be carried to the remote port */
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

    if (telnet->settings_sent &&
        (telnet->requested_baudrate == option.baudrate) &&
        (telnet->requested_databits == option.databits) &&
        (telnet->requested_stopbits == option.stopbits) &&
        (telnet->requested_parity == parity_value(option.parity)))
    {
        /* Reached from a reconfigure that changed something else - resending
         * would put four lines of settings on screen for a mapping keypress */
        return;
    }

    telnet->settings_sent = true;

    telnet->requested_baudrate = option.baudrate;
    values[0] = (unsigned char) ((unsigned int) option.baudrate >> 24);
    values[1] = (unsigned char) ((unsigned int) option.baudrate >> 16);
    values[2] = (unsigned char) ((unsigned int) option.baudrate >> 8);
    values[3] = (unsigned char) ((unsigned int) option.baudrate);
    send_subneg(fd, COM_SET_BAUDRATE, values, 4);

    telnet->requested_databits = option.databits;
    values[0] = (unsigned char) option.databits;
    send_subneg(fd, COM_SET_DATASIZE, values, 1);

    telnet->requested_parity = parity_value(option.parity);
    values[0] = (unsigned char) telnet->requested_parity;
    send_subneg(fd, COM_SET_PARITY, values, 1);

    telnet->requested_stopbits = option.stopbits;
    values[0] = (unsigned char) option.stopbits;
    send_subneg(fd, COM_SET_STOPSIZE, values, 1);
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

static void handle_subneg(const telnet_t *telnet)
{
    if (telnet->subneg_overflow)
    {
        /* A payload longer than anything this speaks; acting on a truncated
         * one would be worse than ignoring it */
        return;
    }

    if ((telnet->subneg_length >= 1) && (telnet->subneg[0] == OPT_COM_PORT))
    {
        handle_com_port_response(telnet);
    }
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
}

size_t telnet_filter_input(telnet_t *telnet, int fd, char *buffer, size_t count)
{
    unsigned char *input = (unsigned char *) buffer;
    size_t kept = 0;

    for (size_t i = 0; i < count; i++)
    {
        unsigned char byte = input[i];

        switch (telnet->state)
        {
            case TELNET_STATE_DATA:
                if (byte == IAC)
                {
                    if (telnet->engaged || !telnet->saw_data)
                    {
                        /* Either the peer is known to speak Telnet, or it has
                         * sent nothing but this, so the marker may still be the
                         * start of a negotiation. Parse it and let the bytes
                         * that follow decide. */
                        telnet->state = TELNET_STATE_COMMAND;
                    }
                    else
                    {
                        /* A peer that sent data before it ever negotiated is
                         * not speaking Telnet, so this is one of its data
                         * bytes. Reading it as a command is how a raw peer -
                         * tio's own socket server among them - used to lose two
                         * bytes and have every marker doubled back at it for
                         * the rest of the session. */
                        input[kept++] = byte;
                    }
                }
                else
                {
                    telnet->saw_data = true;
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
                    if (!telnet->engaged && ((kept + 1) <= i))
                    {
                        /* Not a negotiation after all, so the marker was a data
                         * byte and so is this one. Put both back.
                         *
                         * The room test is what makes writing them safe: two
                         * bytes were consumed to get here and none of them was
                         * kept, so kept has fallen at least two behind i and
                         * both slots sit in territory already read. It fails
                         * only when the marker ended one read and this byte
                         * began the next, where there is no slot to expand
                         * into; that costs those two bytes and nothing after
                         * them. */
                        input[kept++] = IAC;
                        input[kept++] = byte;
                        telnet->saw_data = true;
                    }
                    /* Otherwise: a command that carries no option, which a
                     * serial session has nothing to do with. */
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
                    handle_subneg(telnet);
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

ssize_t telnet_send(int fd, const void *buffer, size_t count)
{
    const unsigned char *input = (const unsigned char *) buffer;
    unsigned char escaped[2 * BUFSIZ];
    size_t consumed = 0;
    size_t produced = 0;
    size_t sent = 0;

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

        if (status <= 0)
        {
            return status;
        }

        sent += status;
    }

    return (ssize_t) consumed;
}
