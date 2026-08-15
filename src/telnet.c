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

#define OPTION_COUNT 256

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

void telnet_reset(void)
{
    state = STATE_DATA;
    pending_command = 0;
    engaged = false;

    memset(remote_enabled, 0, sizeof(remote_enabled));
    memset(remote_answered, 0, sizeof(remote_answered));
    memset(local_enabled, 0, sizeof(local_enabled));
    memset(local_answered, 0, sizeof(local_answered));
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
    return (opt == OPT_BINARY) || (opt == OPT_SUPPRESS_GO_AHEAD);
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
                /* Payload is discarded until the option that needs it is
                 * implemented; only its end marker matters here */
                if (byte == IAC)
                {
                    state = STATE_SUBNEG_COMMAND;
                }
                break;

            case STATE_SUBNEG_COMMAND:
                state = (byte == SE) ? STATE_DATA : STATE_SUBNEG;
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
