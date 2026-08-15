/*
 * tio - a serial device I/O tool
 *
 * Copyright (c) 2014-2022  Martin Lund
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
#include <glib.h>

#define LINE_HIGH true
#define LINE_LOW false

#define TOPOLOGY_ID_SIZE 4

typedef enum
{
    FLOW_NONE,
    FLOW_HARD,
    FLOW_SOFT,
} flow_t;

typedef enum
{
    PARITY_NONE,
    PARITY_ODD,
    PARITY_EVEN,
    PARITY_MARK,
    PARITY_SPACE,
} parity_t;

typedef enum
{
    AUTO_CONNECT_DIRECT,
    AUTO_CONNECT_NEW,
    AUTO_CONNECT_LATEST,
    AUTO_CONNECT_END,
} auto_connect_t;

typedef enum
{
    DEVICE_MODE_TTY,
    DEVICE_MODE_SOCKET,
} device_mode_t;

typedef struct
{
    char *tid;
    double uptime;
    char *path;
    char *driver;
    char *description;
} device_t;

typedef struct
{
    int mask;
    int value;
    bool reserved;
} tty_line_config_t;

extern const char *device_name;
extern device_mode_t device_mode;
extern bool interactive_mode;

void stdout_configure(void);
void stdin_configure(void);
void tty_configure(void);
void tty_reconfigure(void);
int device_connect(void);
void device_wait(void);
void list_serial_devices(void);
void tty_input_thread_create(void);
void tty_input_thread_wait_ready(void);
void tty_line_set(int fd, tty_line_config_t line_config[]);

// Write to the connected device, applying the output character mapping and
// whatever the negotiated protocol requires on the way out. Buffers; call
// device_sync() to put it on the wire. Maps in place, so the caller's buffer
// must be writable and must not be shared.
ssize_t device_write(int fd, const void *buffer, size_t count);
void device_sync(int fd);

// Report an operation that only a serial line can carry out, when the session
// is connected to a socket. Returns true when the caller must not proceed.
bool device_serial_only(const char *operation);

// Apply a setting a socket client asked for to the device being served, and
// return what is in effect afterwards. That differs from the request when the
// device could not take it, which is what the caller has to answer with.
int tty_apply_baudrate(int baudrate);
int tty_apply_databits(int databits);
int tty_apply_stopbits(int stopbits);
parity_t tty_apply_parity(parity_t parity);

// Send a break on the device being served, at a client's request
void tty_apply_break(void);

// Drive a modem control line on the device being served to the state a client
// asked for
void tty_apply_dtr(bool assert_line);
void tty_apply_rts(bool assert_line);

void tty_search(void);
GList *tty_search_for_serial_devices(void);
