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
#define SERVER_OFFSET    100

/* RFC 2217 parity values */
#define PARITY_VALUE_NONE  1
#define PARITY_VALUE_ODD   2
#define PARITY_VALUE_EVEN  3
#define PARITY_VALUE_MARK  4
#define PARITY_VALUE_SPACE 5

#define OPTION_COUNT 256
#define SUBNEG_MAX   64

typedef enum
{
    STATE_DATA,
    STATE_COMMAND,
    STATE_OPTION,
    STATE_SUBNEG,
    STATE_SUBNEG_COMMAND,
} telnet_state_t;

static telnet_state_t state = STATE_DATA;
static unsigned char pending_command = 0;
static bool engaged = false;

/* What has been settled for each option, and whether it has been answered at
 * all. Both are needed: an unanswered request must be answered even when the
 * answer matches the current state, and an answered one must not be answered
 * again, which is what keeps the peer and us from trading the same pair of
 * commands forever. */
static bool remote_enabled[OPTION_COUNT];
static bool remote_answered[OPTION_COUNT];
static bool local_enabled[OPTION_COUNT];
static bool local_answered[OPTION_COUNT];

static unsigned char subneg[SUBNEG_MAX];
static size_t subneg_length = 0;
static bool subneg_overflow = false;

/* What was last asked of the remote port, kept so that the server's answer can
 * be compared against it. A server is entitled to answer with a different
 * value, and that difference is the whole point: it means the port could not
 * take the setting, which the user has to be told rather than shown as done. */
static int requested_baudrate = 0;
static int requested_databits = 0;
static int requested_stopbits = 0;
static int requested_parity = 0;
static bool settings_sent = false;

void telnet_reset(void)
{
    state = STATE_DATA;
    pending_command = 0;
    engaged = false;

    memset(remote_enabled, 0, sizeof(remote_enabled));
    memset(remote_answered, 0, sizeof(remote_answered));
    memset(local_enabled, 0, sizeof(local_enabled));
    memset(local_answered, 0, sizeof(local_answered));

    subneg_length = 0;
    subneg_overflow = false;

    requested_baudrate = 0;
    requested_databits = 0;
    requested_stopbits = 0;
    requested_parity = 0;
    settings_sent = false;
}

bool telnet_engaged(void)
{
    return engaged;
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

static void handle_remote_offer(int fd, unsigned char opt, bool offered)
{
    bool wanted = offered && option_wanted(opt);

    if (remote_answered[opt] && (remote_enabled[opt] == wanted))
    {
        /* Already settled this way - answering again would only invite the
         * peer to repeat itself */
        return;
    }

    send_command(fd, wanted ? DO : DONT, opt);
    remote_enabled[opt] = wanted;
    remote_answered[opt] = true;
}

static void handle_local_request(int fd, unsigned char opt, bool requested)
{
    bool wanted = requested && option_wanted(opt);

    if (local_answered[opt] && (local_enabled[opt] == wanted))
    {
        return;
    }

    send_command(fd, wanted ? WILL : WONT, opt);
    local_enabled[opt] = wanted;
    local_answered[opt] = true;

    if (wanted && (opt == OPT_COM_PORT))
    {
        /* The option is live now, so the settings the user asked for on the
         * command line can be carried to the remote port */
        telnet_send_port_settings(fd);
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

void telnet_send_port_settings(int fd)
{
    unsigned char values[4];

    if (!local_enabled[OPT_COM_PORT])
    {
        /* Peer declined the option, so the session carries bytes only and the
         * serial settings stay a local matter */
        return;
    }

    if (settings_sent &&
        (requested_baudrate == option.baudrate) &&
        (requested_databits == option.databits) &&
        (requested_stopbits == option.stopbits) &&
        (requested_parity == parity_value(option.parity)))
    {
        /* Reached from a reconfigure that changed something else - resending
         * would put four lines of settings on screen for a mapping keypress */
        return;
    }

    settings_sent = true;

    requested_baudrate = option.baudrate;
    values[0] = (unsigned char) ((unsigned int) option.baudrate >> 24);
    values[1] = (unsigned char) ((unsigned int) option.baudrate >> 16);
    values[2] = (unsigned char) ((unsigned int) option.baudrate >> 8);
    values[3] = (unsigned char) ((unsigned int) option.baudrate);
    send_subneg(fd, COM_SET_BAUDRATE, values, 4);

    requested_databits = option.databits;
    values[0] = (unsigned char) option.databits;
    send_subneg(fd, COM_SET_DATASIZE, values, 1);

    requested_parity = parity_value(option.parity);
    values[0] = (unsigned char) requested_parity;
    send_subneg(fd, COM_SET_PARITY, values, 1);

    requested_stopbits = option.stopbits;
    values[0] = (unsigned char) option.stopbits;
    send_subneg(fd, COM_SET_STOPSIZE, values, 1);
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

static void handle_com_port_response(void)
{
    unsigned char command;

    if (subneg_length < 2)
    {
        return;
    }

    command = subneg[1];

    switch (command)
    {
        case COM_SET_BAUDRATE + SERVER_OFFSET:
            if (subneg_length >= 6)
            {
                long applied = ((long) subneg[2] << 24) | ((long) subneg[3] << 16) |
                               ((long) subneg[4] << 8) | (long) subneg[5];
                report_setting("baud rate", applied, requested_baudrate);
            }
            break;

        case COM_SET_DATASIZE + SERVER_OFFSET:
            if (subneg_length >= 3)
            {
                report_setting("data bits", subneg[2], requested_databits);
            }
            break;

        case COM_SET_PARITY + SERVER_OFFSET:
            if (subneg_length >= 3)
            {
                report_parity(subneg[2], requested_parity);
            }
            break;

        case COM_SET_STOPSIZE + SERVER_OFFSET:
            if (subneg_length >= 3)
            {
                report_setting("stop bits", subneg[2], requested_stopbits);
            }
            break;

        default:
            break;
    }
}

static void handle_subneg(void)
{
    if (subneg_overflow)
    {
        /* A payload longer than anything this speaks; acting on a truncated
         * one would be worse than ignoring it */
        return;
    }

    if ((subneg_length >= 1) && (subneg[0] == OPT_COM_PORT))
    {
        handle_com_port_response();
    }
}

static void handle_option(int fd, unsigned char command, unsigned char opt)
{
    switch (command)
    {
        case WILL:
            handle_remote_offer(fd, opt, true);
            break;

        case WONT:
            handle_remote_offer(fd, opt, false);
            break;

        case DO:
            handle_local_request(fd, opt, true);
            break;

        case DONT:
            handle_local_request(fd, opt, false);
            break;

        default:
            break;
    }
}

size_t telnet_filter_input(int fd, char *buffer, size_t count)
{
    unsigned char *input = (unsigned char *) buffer;
    size_t kept = 0;

    for (size_t i = 0; i < count; i++)
    {
        unsigned char byte = input[i];

        switch (state)
        {
            case STATE_DATA:
                if (byte == IAC)
                {
                    /* The first command marker is what tells us the peer is
                     * speaking Telnet rather than sending raw bytes */
                    engaged = true;
                    state = STATE_COMMAND;
                }
                else
                {
                    input[kept++] = byte;
                }
                break;

            case STATE_COMMAND:
                if (byte == IAC)
                {
                    /* Doubled marker - the peer means the byte itself */
                    input[kept++] = byte;
                    state = STATE_DATA;
                }
                else if ((byte == WILL) || (byte == WONT) || (byte == DO) || (byte == DONT))
                {
                    pending_command = byte;
                    state = STATE_OPTION;
                }
                else if (byte == SB)
                {
                    subneg_length = 0;
                    subneg_overflow = false;
                    state = STATE_SUBNEG;
                }
                else
                {
                    /* A command that carries no option and that a serial
                     * session has nothing to do with */
                    state = STATE_DATA;
                }
                break;

            case STATE_OPTION:
                handle_option(fd, pending_command, byte);
                state = STATE_DATA;
                break;

            case STATE_SUBNEG:
                if (byte == IAC)
                {
                    state = STATE_SUBNEG_COMMAND;
                }
                else if (subneg_length < sizeof(subneg))
                {
                    subneg[subneg_length++] = byte;
                }
                else
                {
                    subneg_overflow = true;
                }
                break;

            case STATE_SUBNEG_COMMAND:
                if (byte == SE)
                {
                    handle_subneg();
                    state = STATE_DATA;
                }
                else
                {
                    /* A doubled marker inside the payload stands for the byte
                     * itself, as it does in the data stream */
                    if (subneg_length < sizeof(subneg))
                    {
                        subneg[subneg_length++] = byte;
                    }
                    else
                    {
                        subneg_overflow = true;
                    }
                    state = STATE_SUBNEG;
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
