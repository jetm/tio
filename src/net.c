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

#include <stddef.h>
#include <string.h>
#include "net.h"

// Target prefixes which select socket mode instead of tty mode
static const char *socket_prefix[] =
{
    "unix:",
    "inet:",
    "inet6:",
};

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
