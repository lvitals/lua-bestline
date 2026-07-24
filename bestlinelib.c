/* vim:sts=4 sw=4 expandtab
 */

/*
 * Copyright (c) 2011-2015 Rob Hoelz <rob@hoelz.ro>
 * Copyright (c) 2026 lua-bestline contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <errno.h>
#include <lauxlib.h>
#include <lua.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bestline.h"

#define BL_COMPLETION_TYPE "bestlineCompletions*"

#ifdef _WIN32
#define BL_EXPORT __declspec(dllexport)
#else
#define BL_EXPORT extern
#endif

#ifndef LUA_OK
#define LUA_OK 0
#endif

#if LUA_VERSION_NUM < 502
#define lua_rawlen lua_objlen
#endif

static int completion_func_ref = LUA_NOREF;
static int hints_func_ref = LUA_NOREF;
static int xlat_func_ref = LUA_NOREF;
static int callback_error_ref = LUA_NOREF;
static lua_State *callback_state;

static char hint_ansi_start[64];
static char hint_ansi_end[64];

static int push_error(lua_State *L, const char *message)
{
    lua_pushnil(L);
    if (message) {
        lua_pushstring(L, message);
        return 2;
    }
    return 1;
}

static int push_errno_error(lua_State *L)
{
    return push_error(L, strerror(errno));
}

static int push_ok(lua_State *L)
{
    lua_pushboolean(L, 1);
    return 1;
}

static void clear_callback_error(lua_State *L)
{
    lua_pushliteral(L, "");
    lua_rawseti(L, LUA_REGISTRYINDEX, callback_error_ref);
}

static int has_callback_error(lua_State *L)
{
    int has_error;
    lua_rawgeti(L, LUA_REGISTRYINDEX, callback_error_ref);
    has_error = lua_isstring(L, -1) && strlen(lua_tostring(L, -1)) != 0;
    return has_error;
}

static char *copy_string(const char *s)
{
    size_t len;
    char *copy;

    if (!s) {
        return NULL;
    }

    len = strlen(s);
    copy = (char *)malloc(len + 1);
    if (copy) {
        memcpy(copy, s, len + 1);
    }
    return copy;
}

static void remember_callback_error(lua_State *L)
{
    if (!lua_isstring(L, -1)) {
        lua_pop(L, 1);
        lua_pushliteral(L, "callback failed");
    }
    lua_rawseti(L, LUA_REGISTRYINDEX, callback_error_ref);
}

static void completion_callback_wrapper(const char *line, int pos,
                                        bestlineCompletions *completions)
{
    lua_State *L = callback_state;
    int status;

    if (!L || completion_func_ref == LUA_NOREF) {
        return;
    }

    lua_rawgeti(L, LUA_REGISTRYINDEX, completion_func_ref);
    *((bestlineCompletions **)lua_newuserdata(L, sizeof(bestlineCompletions *))) = completions;
    luaL_getmetatable(L, BL_COMPLETION_TYPE);
    lua_setmetatable(L, -2);
    lua_pushstring(L, line);
    lua_pushinteger(L, pos);

    status = lua_pcall(L, 3, 0, 0);
    if (status != LUA_OK) {
        remember_callback_error(L);
    }
}

static int copy_table_string(lua_State *L, const char *field, char *buffer,
                             size_t buffer_len)
{
    const char *value;

    lua_getfield(L, -1, field);
    if (lua_isnoneornil(L, -1)) {
        lua_pop(L, 1);
        return 0;
    }
    if (!lua_isstring(L, -1)) {
        lua_pushfstring(L, "Invalid %s value of type '%s' from hints callback - string or nil required",
                        field, lua_typename(L, lua_type(L, -1)));
        lua_rawseti(L, LUA_REGISTRYINDEX, callback_error_ref);
        lua_pop(L, 1);
        return -1;
    }

    value = lua_tostring(L, -1);
    snprintf(buffer, buffer_len, "%s", value);
    lua_pop(L, 1);
    return 1;
}

static int configure_hint_style(lua_State *L)
{
    int saw_style;

    snprintf(hint_ansi_start, sizeof(hint_ansi_start), "\033[90m");
    snprintf(hint_ansi_end, sizeof(hint_ansi_end), "\033[39m");

    if (lua_isnoneornil(L, -1)) {
        return 0;
    }
    if (!lua_istable(L, -1)) {
        lua_pushfstring(L, "Invalid second value of type '%s' from hints callback - table or nil required",
                        lua_typename(L, lua_type(L, -1)));
        lua_rawseti(L, LUA_REGISTRYINDEX, callback_error_ref);
        return -1;
    }

    saw_style = copy_table_string(L, "ansi1", hint_ansi_start, sizeof(hint_ansi_start));
    if (saw_style < 0) {
        return -1;
    }
    if (copy_table_string(L, "ansi2", hint_ansi_end, sizeof(hint_ansi_end)) < 0) {
        return -1;
    }

    lua_getfield(L, -1, "color");
    if (lua_isnumber(L, -1)) {
        int color = (int)lua_tointeger(L, -1);
        int bold = 0;
        lua_pop(L, 1);

        lua_getfield(L, -1, "bold");
        bold = lua_toboolean(L, -1);
        lua_pop(L, 1);

        snprintf(hint_ansi_start, sizeof(hint_ansi_start), "\033[%d;%dm",
                 bold ? 1 : 22, color);
        snprintf(hint_ansi_end, sizeof(hint_ansi_end), "\033[0m");
    } else if (!lua_isnoneornil(L, -1)) {
        lua_pushfstring(L, "Invalid color value of type '%s' from hints callback - number or nil required",
                        lua_typename(L, lua_type(L, -1)));
        lua_rawseti(L, LUA_REGISTRYINDEX, callback_error_ref);
        lua_pop(L, 1);
        return -1;
    } else {
        lua_pop(L, 1);
    }

    (void)saw_style;
    return 0;
}

static char *hints_callback_wrapper(const char *line, const char **ansi1,
                                    const char **ansi2)
{
    lua_State *L = callback_state;
    char *result = NULL;
    int status;

    if (!L || hints_func_ref == LUA_NOREF) {
        return NULL;
    }

    lua_rawgeti(L, LUA_REGISTRYINDEX, hints_func_ref);
    lua_pushstring(L, line);

    status = lua_pcall(L, 1, 2, 0);
    if (status != LUA_OK) {
        remember_callback_error(L);
        return NULL;
    }

    if (!lua_isnoneornil(L, -2)) {
        if (!lua_isstring(L, -2)) {
            lua_pushfstring(L, "Invalid first value of type '%s' from hints callback - string or nil required",
                            lua_typename(L, lua_type(L, -2)));
            lua_rawseti(L, LUA_REGISTRYINDEX, callback_error_ref);
            lua_pop(L, 2);
            return NULL;
        }

        result = copy_string(lua_tostring(L, -2));
        if (!result) {
            lua_pushliteral(L, "out of memory while copying hint");
            lua_rawseti(L, LUA_REGISTRYINDEX, callback_error_ref);
            lua_pop(L, 2);
            return NULL;
        }

        if (configure_hint_style(L) < 0) {
            free(result);
            lua_pop(L, 2);
            return NULL;
        }
        *ansi1 = hint_ansi_start;
        *ansi2 = hint_ansi_end;
    }

    lua_pop(L, 2);
    return result;
}

static void free_hints_callback(void *p)
{
    free(p);
}

static unsigned decode_utf8_codepoint(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;

    if ((p[0] & 0x80) == 0) {
        return p[0];
    }
    if ((p[0] & 0xe0) == 0xc0 && p[1]) {
        return ((unsigned)(p[0] & 0x1f) << 6) |
               (unsigned)(p[1] & 0x3f);
    }
    if ((p[0] & 0xf0) == 0xe0 && p[1] && p[2]) {
        return ((unsigned)(p[0] & 0x0f) << 12) |
               ((unsigned)(p[1] & 0x3f) << 6) |
               (unsigned)(p[2] & 0x3f);
    }
    if ((p[0] & 0xf8) == 0xf0 && p[1] && p[2] && p[3]) {
        return ((unsigned)(p[0] & 0x07) << 18) |
               ((unsigned)(p[1] & 0x3f) << 12) |
               ((unsigned)(p[2] & 0x3f) << 6) |
               (unsigned)(p[3] & 0x3f);
    }
    return p[0];
}

static unsigned xlat_callback_wrapper(unsigned c)
{
    lua_State *L = callback_state;
    unsigned result = c;
    int status;

    if (!L || xlat_func_ref == LUA_NOREF) {
        return c;
    }

    lua_rawgeti(L, LUA_REGISTRYINDEX, xlat_func_ref);
    lua_pushinteger(L, c);
    status = lua_pcall(L, 1, 1, 0);
    if (status != LUA_OK) {
        remember_callback_error(L);
        return c;
    }

    if (lua_isnumber(L, -1)) {
        result = (unsigned)lua_tointeger(L, -1);
    } else if (lua_isstring(L, -1)) {
        result = decode_utf8_codepoint(lua_tostring(L, -1));
    } else if (!lua_isnoneornil(L, -1)) {
        lua_pushfstring(L, "Invalid return value of type '%s' from xlat callback - number, string or nil required",
                        lua_typename(L, lua_type(L, -1)));
        lua_rawseti(L, LUA_REGISTRYINDEX, callback_error_ref);
    }

    lua_pop(L, 1);
    return result;
}

static int push_bestline_result(lua_State *L, char *line)
{
    if (has_callback_error(L)) {
        lua_pushnil(L);
        lua_insert(L, -2);
        if (line) {
            bestlineFree(line);
        }
        return 2;
    }
    lua_pop(L, 1);

    if (!line && errno) {
        return push_errno_error(L);
    }
    if (!line) {
        return push_error(L, NULL);
    }

    lua_pushstring(L, line);
    bestlineFree(line);
    return 1;
}

static int l_bestline(lua_State *L)
{
    const char *prompt = luaL_checkstring(L, 1);
    const char *init = luaL_optstring(L, 2, "");
    char *line;

    callback_state = L;
    clear_callback_error(L);
    errno = 0;
    line = bestlineInit(prompt, init);
    callback_state = NULL;

    return push_bestline_result(L, line);
}

static int l_withhistory(lua_State *L)
{
    const char *prompt = luaL_checkstring(L, 1);
    const char *prog = luaL_optstring(L, 2, NULL);
    char *line;

    callback_state = L;
    clear_callback_error(L);
    errno = 0;
    line = bestlineWithHistory(prompt, prog);
    callback_state = NULL;

    return push_bestline_result(L, line);
}

static int l_raw(lua_State *L)
{
    const char *prompt = luaL_checkstring(L, 1);
    int infd = (int)luaL_optinteger(L, 2, STDIN_FILENO);
    int outfd = (int)luaL_optinteger(L, 3, STDOUT_FILENO);
    const char *init = luaL_optstring(L, 4, "");
    char *line;

    callback_state = L;
    clear_callback_error(L);
    errno = 0;
    line = bestlineRawInit(prompt, init, infd, outfd);
    callback_state = NULL;

    return push_bestline_result(L, line);
}

static int lines_next(lua_State *L)
{
    lua_pushcfunction(L, l_bestline);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_call(L, 1, 1);
    return 1;
}

static int l_lines(lua_State *L)
{
    luaL_checkstring(L, 1);
    lua_pushcclosure(L, lines_next, 1);
    return 1;
}

static int l_historyadd(lua_State *L)
{
    const char *line = luaL_checkstring(L, 1);

    if (!bestlineHistoryAdd(line)) {
        return push_error(L, "history entry was not added");
    }

    return push_ok(L);
}

static int l_historysetmaxlen(lua_State *L)
{
    luaL_checkinteger(L, 1);
    return push_error(L, "bestline uses fixed compile-time history length");
}

static int l_historysave(lua_State *L)
{
    const char *filename = luaL_checkstring(L, 1);

    if (bestlineHistorySave(filename) < 0) {
        return push_errno_error(L);
    }
    return push_ok(L);
}

static int l_historyload(lua_State *L)
{
    const char *filename = luaL_checkstring(L, 1);

    if (bestlineHistoryLoad(filename) < 0) {
        return push_errno_error(L);
    }
    return push_ok(L);
}

static int l_historyfree(lua_State *L)
{
    bestlineHistoryFree();
    return push_ok(L);
}

static int l_clearscreen(lua_State *L)
{
    int fd = (int)luaL_optinteger(L, 1, STDOUT_FILENO);
    bestlineClearScreen(fd);
    return push_ok(L);
}

static int l_setcompletion(lua_State *L)
{
    if (lua_isnoneornil(L, 1)) {
        luaL_unref(L, LUA_REGISTRYINDEX, completion_func_ref);
        completion_func_ref = LUA_NOREF;
        bestlineSetCompletionCallback(NULL);
    } else {
        luaL_checktype(L, 1, LUA_TFUNCTION);

        lua_pushvalue(L, 1);
        if (completion_func_ref == LUA_NOREF) {
            completion_func_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        } else {
            lua_rawseti(L, LUA_REGISTRYINDEX, completion_func_ref);
        }
        bestlineSetCompletionCallback(completion_callback_wrapper);
    }

    return push_ok(L);
}

static int l_addcompletion(lua_State *L)
{
    bestlineCompletions *completions =
        *((bestlineCompletions **)luaL_checkudata(L, 1, BL_COMPLETION_TYPE));
    const char *entry = luaL_checkstring(L, 2);

    bestlineAddCompletion(completions, entry);
    return push_ok(L);
}

static int l_sethints(lua_State *L)
{
    if (lua_isnoneornil(L, 1)) {
        luaL_unref(L, LUA_REGISTRYINDEX, hints_func_ref);
        hints_func_ref = LUA_NOREF;
        bestlineSetHintsCallback(NULL);
        bestlineSetFreeHintsCallback(NULL);
    } else {
        luaL_checktype(L, 1, LUA_TFUNCTION);

        lua_pushvalue(L, 1);
        if (hints_func_ref == LUA_NOREF) {
            hints_func_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        } else {
            lua_rawseti(L, LUA_REGISTRYINDEX, hints_func_ref);
        }
        bestlineSetHintsCallback(hints_callback_wrapper);
        bestlineSetFreeHintsCallback(free_hints_callback);
    }

    return push_ok(L);
}

static int l_setmaskmode(lua_State *L)
{
    if (lua_toboolean(L, 1)) {
        bestlineMaskModeEnable();
    } else {
        bestlineMaskModeDisable();
    }
    return push_ok(L);
}

static int l_setbalancemode(lua_State *L)
{
    if (lua_toboolean(L, 1)) {
        bestlineBalanceModeEnable();
    } else {
        bestlineBalanceModeDisable();
    }
    return push_ok(L);
}

static int l_setllamamode(lua_State *L)
{
    bestlineLlamaMode(lua_toboolean(L, 1) ? 1 : 0);
    return push_ok(L);
}

static int l_setmultiline(lua_State *L)
{
    (void)L;
    return push_ok(L);
}

static int l_setemacsmode(lua_State *L)
{
    bestlineEmacsMode(lua_toboolean(L, 1) ? 1 : 0);
    return push_ok(L);
}

static int l_setxlat(lua_State *L)
{
    if (lua_isnoneornil(L, 1)) {
        luaL_unref(L, LUA_REGISTRYINDEX, xlat_func_ref);
        xlat_func_ref = LUA_NOREF;
        bestlineSetXlatCallback(NULL);
    } else {
        luaL_checktype(L, 1, LUA_TFUNCTION);

        lua_pushvalue(L, 1);
        if (xlat_func_ref == LUA_NOREF) {
            xlat_func_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        } else {
            lua_rawseti(L, LUA_REGISTRYINDEX, xlat_func_ref);
        }
        bestlineSetXlatCallback(xlat_callback_wrapper);
    }

    return push_ok(L);
}

static unsigned check_codepoint(lua_State *L, int index)
{
    if (lua_isnumber(L, index)) {
        return (unsigned)lua_tointeger(L, index);
    }
    return decode_utf8_codepoint(luaL_checkstring(L, index));
}

static int l_characterwidth(lua_State *L)
{
    lua_pushinteger(L, bestlineCharacterWidth((int)check_codepoint(L, 1)));
    return 1;
}

static int l_isseparator(lua_State *L)
{
    lua_pushboolean(L, bestlineIsSeparator(check_codepoint(L, 1)));
    return 1;
}

static int l_notseparator(lua_State *L)
{
    lua_pushboolean(L, bestlineNotSeparator(check_codepoint(L, 1)));
    return 1;
}

static int l_isxeparator(lua_State *L)
{
    lua_pushboolean(L, bestlineIsXeparator(check_codepoint(L, 1)));
    return 1;
}

static int l_uppercase(lua_State *L)
{
    lua_pushinteger(L, bestlineUppercase(check_codepoint(L, 1)));
    return 1;
}

static int l_lowercase(lua_State *L)
{
    lua_pushinteger(L, bestlineLowercase(check_codepoint(L, 1)));
    return 1;
}

static int l_enableutf8(lua_State *L)
{
    (void)L;
    return 0;
}

static int l_disable_raw_mode(lua_State *L)
{
    bestlineDisableRawMode();
    return push_ok(L);
}

static const luaL_Reg bestline_funcs[] = {
    { "bestline", l_bestline },
    { "line", l_bestline },
    { "lines", l_lines },
    { "withhistory", l_withhistory },
    { "raw", l_raw },

    { "historyadd", l_historyadd },
    { "historysetmaxlen", l_historysetmaxlen },
    { "historysave", l_historysave },
    { "historyload", l_historyload },
    { "historyfree", l_historyfree },

    { "clearscreen", l_clearscreen },
    { "setcompletion", l_setcompletion },
    { "addcompletion", l_addcompletion },
    { "sethints", l_sethints },

    { "setmaskmode", l_setmaskmode },
    { "setbalancemode", l_setbalancemode },
    { "setllamamode", l_setllamamode },
    { "setemacsmode", l_setemacsmode },
    { "setxlat", l_setxlat },
    { "disablerawmode", l_disable_raw_mode },
    { "characterwidth", l_characterwidth },
    { "isseparator", l_isseparator },
    { "notseparator", l_notseparator },
    { "isxeparator", l_isxeparator },
    { "uppercase", l_uppercase },
    { "lowercase", l_lowercase },
    { "toupper", l_uppercase },
    { "tolower", l_lowercase },

    /* Compatibility aliases from lua-linenoise. */
    { "linenoise", l_bestline },
    { "addhistory", l_historyadd },
    { "sethistorymaxlen", l_historysetmaxlen },
    { "savehistory", l_historysave },
    { "loadhistory", l_historyload },
    { "setmultiline", l_setmultiline },
    { "enableutf8", l_enableutf8 },
    { "printkeycodes", l_enableutf8 },

    { NULL, NULL }
};

static const luaL_Reg bestline_methods[] = {
    { "add", l_addcompletion },
    { NULL, NULL }
};

BL_EXPORT int luaopen_bestline(lua_State *L)
{
    lua_pushliteral(L, "");
    callback_error_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    lua_newtable(L);

    luaL_newmetatable(L, BL_COMPLETION_TYPE);
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "__metatable");
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");

#if LUA_VERSION_NUM > 501
    luaL_setfuncs(L, bestline_methods, 0);
    lua_pop(L, 1);
    luaL_setfuncs(L, bestline_funcs, 0);
#else
    luaL_register(L, NULL, bestline_methods);
    lua_pop(L, 1);
    luaL_register(L, NULL, bestline_funcs);
#endif

    lua_pushliteral(L, "lua-bestline");
    lua_setfield(L, -2, "_NAME");
    lua_pushliteral(L, "0.1.0");
    lua_setfield(L, -2, "_VERSION");

    return 1;
}
