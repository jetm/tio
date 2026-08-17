/*
 * tio - a serial device I/O tool
 *
 * Copyright (c) 2014-2024  Martin Lund
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
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <lauxlib.h>
#include <lualib.h>
#include <sys/ioctl.h>
#include <ctype.h>
#include "misc.h"
#include "print.h"
#include "options.h"
#include "tty.h"
#include "telnet.h"
#include "xymodem.h"
#include "log.h"
#include "script.h"
#include "fs.h"
#include "timestamp.h"
#include "termios.h"

#define MAX_BUFFER_SIZE 2000 // Maximum size of circular buffer
#define READ_LINE_SIZE 4096 // read_line buffer length

static int device_fd;

static char script_init[] =
"tio.set = function(arg)\n"
"    local dtr = arg.DTR or -1\n"
"    local rts = arg.RTS or -1\n"
"    local cts = arg.CTS or -1\n"
"    local dsr = arg.DSR or -1\n"
"    local cd = arg.CD or -1\n"
"    local ri = arg.RI or -1\n"
"    tio.line_set(dtr, rts, cts, dsr, cd, ri)\n"
"end\n"
"tio.expect = function(pattern, timeout)\n"
"    local str = ''\n"
"    while true do\n"
"        local c = tio.read(1, timeout)\n"
"        if c then\n"
"            str = str .. c\n"
"            if string.match(str, pattern) then\n"
"                return string.match(str, pattern)\n"
"            end\n"
"        else\n"
"            return nil, str\n"
"        end\n"
"    end\n"
"end\n"
"tio.alwaysecho = true\n"
"setmetatable(tio, tio)\n";

static bool alwaysecho(lua_State *L)
{
    bool b;

    lua_getglobal(L, "tio");
    lua_getfield(L, -1, "alwaysecho");
    b = lua_toboolean(L, -1);
    lua_pop(L, 2);

    return b;
}

static int api_echo(lua_State *L)
{
    size_t len = 0;
    const char *str = luaL_checklstring(L, 1, &len);

    if (option.timestamp)
    {
        char *pTimeStampNow = timestamp_current_time();
        if (pTimeStampNow)
        {
            tio_printf("%s", str);
            if (option.log)
            {
                log_printf("\n[%s] %s", pTimeStampNow, str);
            }
        }
    } else {
        for (size_t i=0; i<len; i++)
        {
            putchar(str[i]);

            if (option.log)
                log_putc(str[i]);
        }
    }

    return 0;
}

static void maybe_echo(lua_State *L)
{
    if (alwaysecho(L))
    {
        lua_pushcfunction(L, api_echo);
        lua_pushvalue(L, -2);
        lua_call(L, 1, 0);
    }
}

// lua: tio.sleep(seconds)
static int api_sleep(lua_State *L)
{
    long seconds = lua_tointeger(L, 1);

    if (seconds < 0)
    {
        return 0;
    }

    tio_printf("Sleeping %ld seconds", seconds);

    sleep(seconds);

    return 0;
}

// lua: tio.msleep(miliseconds)
static int api_msleep(lua_State *L)
{
    long mseconds = lua_tointeger(L, 1);
    long useconds = mseconds * 1000;

    if (useconds < 0)
    {
        return 0;
    }

    tio_printf("Sleeping %ld ms", mseconds);
    usleep(useconds);

    return 0;
}

// lua: tio.line_set(dtr,rts,cts,dsr,cd,ri)
static int line_set(lua_State *L)
{
    tty_line_config_t line_config[6] = { };

    int dtr = lua_tointeger(L, 1);
    int rts = lua_tointeger(L, 2);
    int cts = lua_tointeger(L, 3);
    int dsr = lua_tointeger(L, 4);
    int cd = lua_tointeger(L, 5);
    int ri = lua_tointeger(L, 6);

    if (dtr != -1)
    {
        line_config[0].mask = TIOCM_DTR;
        line_config[0].value = dtr;
        line_config[0].reserved = true;
    }
    if (rts != -1)
    {
        line_config[1].mask = TIOCM_RTS;
        line_config[1].value = rts;
        line_config[1].reserved = true;
    }
    if (cts != -1)
    {
        line_config[2].mask = TIOCM_CTS;
        line_config[2].value = cts;
        line_config[2].reserved = true;
    }
    if (dsr != -1)
    {
        line_config[3].mask = TIOCM_DSR;
        line_config[3].value = dsr;
        line_config[3].reserved = true;
    }
    if (cd != -1)
    {
        line_config[4].mask = TIOCM_CD;
        line_config[4].value = cd;
        line_config[4].reserved = true;
    }
    if (ri != -1)
    {
        line_config[5].mask = TIOCM_RI;
        line_config[5].value = ri;
        line_config[5].reserved = true;
    }

    if (device_serial_only("tio.line_set()"))
    {
        return 0;
    }

    tty_line_set(device_fd, line_config);

    return 0;
}

// lua: tio.send(file, protocol)
static int api_send(lua_State *L)
{
    const char *file = luaL_checkstring(L, 1);
    int protocol = luaL_checkinteger(L, 2);
    int ret;

    if (file == NULL)
    {
        return 0;
    }

    if (device_serial_only("tio.send()"))
    {
        return 0;
    }

    switch (protocol)
    {
        case XMODEM_1K:
            tio_printf("Sending file '%s' using XMODEM-1K", file);
            ret = xymodem_send(device_fd, file, XMODEM_1K);
            tio_printf("%s", ret < 0 ? "Aborted" : "Done");
            break;

        case XMODEM_CRC:
            tio_printf("Sending file '%s' using XMODEM-CRC", file);
            ret = xymodem_send(device_fd, file, XMODEM_CRC);
            tio_printf("%s", ret < 0 ? "Aborted" : "Done");
            break;

        case YMODEM:
            tio_printf("Sending file '%s' using YMODEM", file);
            ret = xymodem_send(device_fd, file, YMODEM);
            tio_printf("%s", ret < 0 ? "Aborted" : "Done");
            break;
    }

    return 0;
}

// lua: tio.write(string)
static int api_write(lua_State *L)
{
    size_t len = 0;
    const char *string = luaL_checklstring(L, 1, &len);

    /* Goes out the same way typed input does, so that a socket target gets the
     * command marker escaped and the output mapping is applied. Writing the
     * descriptor directly put a bare marker on the wire and the peer read it
     * as the start of a command.
     *
     * Copied a chunk at a time because the write path maps characters in
     * place, and a Lua string is interned and shared - mapping it where it
     * lies would alter every other reference to it. */
    while (len > 0)
    {
        char chunk[BUFSIZ];
        size_t count = (len < sizeof(chunk)) ? len : sizeof(chunk);
        ssize_t ret;

        memcpy(chunk, string, count);

        ret = device_write(device_fd, chunk, count);
        if (ret < 0)
            return luaL_error(L, "%s", strerror(errno));
        if (ret == 0)
            return luaL_error(L, "partial write");

        string += ret;
        len -= ret;
    }

    device_sync(device_fd);

    lua_getglobal(L, "tio");

    return 1;
}

// lua: tio.read(size, timeout)
/* Read for the script API, taking the protocol out of what the script sees and
 * answering it, exactly as the main loop does. Without this a script on an
 * RFC 2217 target reads negotiation as data - a server's opening offer alone
 * is around twenty bytes of it - and, because the negotiation is never
 * answered, escaping on the write side never engages either.
 *
 * A read that was entirely protocol waits again rather than reporting a
  * timeout that did not happen, but it waits for what is LEFT of the caller's
  * timeout rather than for the whole of it again. Restarting the clock made the
  * timeout an upper bound on nothing: a peer sending protocol every 200 ms held a
  * one-second read open for as long as it kept talking, measured at 6.2 s. That
  * was excused as a burst at the start of a connection, which is true of
  * negotiation and not true in general - and a script's timeout is what a script
  * uses to decide a device is not answering, so stretching it disables exactly
  * the caller's own error handling.
  *
  * One deadline for the whole call, monotonic so that a clock adjustment mid-read
  * cannot move it. A negative timeout means wait forever and has no deadline to
  * compute. */
static ssize_t script_read_poll(void *data, size_t len, int timeout)
{
    struct timespec start;
    int remaining = timeout;

    clock_gettime(CLOCK_MONOTONIC, &start);

    while (true)
    {
        ssize_t ret = read_poll(device_fd, data, len, remaining);

        if ((ret <= 0) || (device_mode != DEVICE_MODE_SOCKET))
        {
            return ret;
        }

        ret = (ssize_t) telnet_filter_input(telnet_client(), device_fd, data,
                                            (size_t) ret);
        if (ret > 0)
        {
            return ret;
        }

        if (timeout >= 0)
        {
            struct timespec now;
            long elapsed;

            clock_gettime(CLOCK_MONOTONIC, &now);
            elapsed = ((now.tv_sec - start.tv_sec) * 1000)
                    + ((now.tv_nsec - start.tv_nsec) / 1000000);

            if (elapsed >= timeout)
            {
                /* Report the timeout the caller asked for. read_poll says 0 for a
                 * timeout, which is what this returns rather than an error: nothing
                 * went wrong, the data just did not come. */
                return 0;
            }
            remaining = timeout - (int) elapsed;
        }
    }
}

static int api_read(lua_State *L)
{
    int size = luaL_checkinteger(L, 1);
    int timeout = lua_tointeger(L, 2);

    if (timeout == 0)
    {
        timeout = -1; // Wait forever
    }

    luaL_Buffer buffer;
    luaL_buffinit(L, &buffer);

#if LUA_VERSION_NUM >= 502
    char *p = luaL_prepbuffsize(&buffer, size);
#else
    if (size > LUAL_BUFFERSIZE)
        return luaL_error(L, "buffer overflow, max size is: %d", LUAL_BUFFERSIZE);
    char *p = luaL_prepbuffer(&buffer);
#endif

    ssize_t ret = script_read_poll(p, size, timeout);
    if (ret < 0)
        return luaL_error(L, "%s", strerror(errno));

    luaL_addsize(&buffer, ret);
    luaL_pushresult(&buffer);

    if (ret == 0)
    {
        // On timeout return nil instead of an empty string
        lua_pop(L, 1);
        lua_pushnil(L);
    }
    else
    {
        maybe_echo(L);
    }

    return 1;
}

// lua: string = tio.readline(timeout)
static int api_readline(lua_State *L) {
    int timeout = lua_tointeger(L, 1); //ms
    luaL_Buffer b;
    char ch;

    if (timeout == 0)
    {
        timeout = -1; // Wait forever
    }

    luaL_buffinit(L, &b);
    luaL_prepbuffer(&b);
    while (true) {
        int ret = (int) script_read_poll(&ch, 1, timeout);

        if (ret < 0)
            return luaL_error(L, "%s", strerror(errno));

        if (ret == 0)
        {
            luaL_pushresult(&b);
            maybe_echo(L);
            lua_pushnil(L);
            lua_insert(L, -2);
            return 2;
        }

        if (ch == '\n')
        {
            luaL_pushresult(&b);
            maybe_echo(L);
            return 1;
        }

        luaL_addchar(&b, ch);
    }
}

// lua: table = tio.ttysearch()
static int api_ttysearch(lua_State *L)
{
    UNUSED(L);
    GList *iter;
    int i = 1;

    GList *device_list = tty_search_for_serial_devices();

    if (device_list == NULL)
    {
        return 0;
    }

    // Create a new table
    lua_newtable(L);

    // Iterate through found devices
    for (iter = device_list; iter != NULL; iter = g_list_next(iter))
    {
        device_t *device = (device_t *) iter->data;

        // Create a new sub-table for each serial device
        lua_newtable(L);

        // Add elements to the table
        lua_pushstring(L, "path");
        lua_pushstring(L, device->path);
        lua_settable(L, -3);

        lua_pushstring(L, "tid");
        lua_pushstring(L, device->tid);
        lua_settable(L, -3);

        lua_pushstring(L, "uptime");
        lua_pushnumber(L, device->uptime);
        lua_settable(L, -3);

        lua_pushstring(L, "driver");
        lua_pushstring(L, device->driver);
        lua_settable(L, -3);

        lua_pushstring(L, "description");
        lua_pushstring(L, device->description);
        lua_settable(L, -3);

        // Set the sub-table as a row in the main table
        lua_rawseti(L, -2, i++);
    }

    // Return table
    return 1;
}

static void script_buffer_run(lua_State *L, const char *script_buffer)
{
    int error;

    error = luaL_loadbuffer(L, script_buffer, strlen(script_buffer), "tio") ||
        lua_pcall(L, 0, 0, 0);
    if (error)
    {
        tio_warning_printf("lua: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);  /* Pop error message from the stack */
    }
}

static void script_file_run(lua_State *L, const char *filename)
{
    if (strlen(filename) == 0)
    {
        tio_warning_printf("Missing script filename\n");
        return;
    }

    if (luaL_dofile(L, filename))
    {
        tio_warning_printf("lua: %s", lua_tostring(L, -1));
        lua_pop(L, 1);  /* pop error message from the stack */
        return;
    }
}

static const struct luaL_Reg tio_lib[] =
{
    { "echo", api_echo},
    { "sleep", api_sleep},
    { "msleep", api_msleep},
    { "line_set", line_set},
    { "send", api_send},
    { "write", api_write},
    { "read", api_read},
    { "readline", api_readline},
    { "ttysearch", api_ttysearch},
    {NULL, NULL}
};

static void script_load(lua_State *L)
{
    int error;

    error = luaL_loadbuffer(L, script_init, strlen(script_init), "tio") || lua_pcall(L, 0, 0, 0);
    if (error)
    {
        tio_error_print("%s\n", lua_tostring(L, -1));
        lua_pop(L, 1); // Pop error message from the stack
    }
}

static void script_set_global(lua_State *L, const char *name, long value)
{
    lua_pushnumber(L, value);
    lua_setglobal(L, name);
}

static void script_set_globals(lua_State *L)
{
    script_set_global(L, "toggle", 2);
    script_set_global(L, "high", 1);
    script_set_global(L, "low", 0);
    script_set_global(L, "XMODEM_CRC", XMODEM_CRC);
    script_set_global(L, "XMODEM_1K", XMODEM_1K);
    script_set_global(L, "YMODEM", YMODEM);
}

#if LUA_VERSION_NUM >= 502
static int luaopen_tio(lua_State *L)
{
    luaL_newlib(L, tio_lib);
    return 1;
}
#endif

void script_run(int fd, const char *script_filename)
{
    lua_State *L;

    device_fd = fd;

    L = luaL_newstate();
    luaL_openlibs(L);

#if LUA_VERSION_NUM >= 502
    luaL_requiref(L, "tio", luaopen_tio, 1);
#else
    luaL_register(L, "tio", tio_lib);
#endif
    lua_pop(L, 1);

    // Load lua init script
    script_load(L);

    // Initialize globals
    script_set_globals(L);

    if (script_filename != NULL)
    {
        tio_printf("Running script %s", script_filename);
        script_file_run(L, script_filename);
    }
    else if (option.script_filename != NULL)
    {
        tio_printf("Running script %s", option.script_filename);
        script_file_run(L, option.script_filename);
    }
    else if (option.script != NULL)
    {
        tio_printf("Running script");
        script_buffer_run(L, option.script);
    }

    lua_close(L);
}

const char *script_run_state_to_string(script_run_t state)
{
    switch (state)
    {
        case SCRIPT_RUN_ONCE:
            return "once";
        case SCRIPT_RUN_ALWAYS:
            return "always";
        case SCRIPT_RUN_NEVER:
            return "never";
        default:
            return "Unknown";
    }
}
