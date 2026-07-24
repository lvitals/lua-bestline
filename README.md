# lua-bestline

Lua binding for [Bestline](https://github.com/jart/bestline), a small
single-file C library for interactive command prompts using ANSI X3.64 / VT100
control sequences.

Repository: <https://github.com/lvitals/lua-bestline.git>

Bestline is a maintained fork of
[linenoise](https://github.com/antirez/linenoise/) with UTF-8 editing, history
search, completions, hints, multiline editing, kill ring support and many GNU
Readline-style shortcuts. `lua-bestline` follows the structure and compatibility
style of `lua-linenoise`, but installs a separate Lua module:

```lua
local bestline = require "bestline"
```

## Requirements

You need:

* Lua 5.1 or newer
* A C compiler
* LuaRocks, or Lua headers available for `make`
* An ANSI UTF-8 terminal with VT100-compatible control sequences for interactive
  use

Bestline itself is written in portable C99 and has no runtime dependency beyond
the platform C/POSIX APIs used by the vendored `bestline.c`.

## Install

From the Git repository:

```sh
git clone https://github.com/lvitals/lua-bestline.git
cd lua-bestline
luarocks make bestline-scm-1.rockspec
```

For local development without installing:

```sh
make LUA_INCDIR=/usr/include/lua5.4
```

Adjust `LUA_INCDIR` if your Lua headers are installed elsewhere.

## Test

Run the full VT100-emulator test harness:

```sh
make test
```

This builds `bestline-test` and runs an interactive test suite derived from the
modernized `lua-linenoise` tests.

Run the lightweight Lua-only API checks:

```sh
make test-unit
```

Expected results:

```text
Tests failed: 0
ok 38 tests
```

## Example

```lua
local bestline = require "bestline"

local history = "history.txt"
bestline.historyload(history)

bestline.setcompletion(function(completions, line)
  if line == "h" then
    completions:add("help")
    bestline.addcompletion(completions, "halt")
  end
end)

bestline.sethints(function(line)
  if line == "h" then
    return " help", { ansi1 = "\027[90m", ansi2 = "\027[39m" }
  end
end)

for line in bestline.lines("> ") do
  if #line > 0 then
    print(line)
    bestline.historyadd(line)
    bestline.historysave(history)
  end
end
```

Bestline also provides a convenience history helper:

```lua
local bestline = require "bestline"

while true do
  local line, err = bestline.withhistory("IN> ", "foo")
  if not line then
    if err then io.stderr:write(err, "\n") end
    break
  end
  print("OUT> " .. line)
end
```

Passing `"foo"` stores history in `~/.foo_history`. Passing a path-like string,
such as `"history.txt"` or `"/tmp/my_history"`, uses that exact file.

## API

### Input

* `bestline.line(prompt[, initial])`
* `bestline.bestline(prompt[, initial])`
* `bestline.linenoise(prompt[, initial])`
* `bestline.lines(prompt)`
* `bestline.withhistory(prompt, program_or_history_file)`
* `bestline.raw(prompt[, infd[, outfd[, initial]]])`

`line`, `bestline` and `linenoise` are aliases. They return the input string, or
`nil[, err]` on EOF or error.

`lines(prompt)` returns an iterator.

### History

* `bestline.historyadd(line)`
* `bestline.historysave(filename)`
* `bestline.historyload(filename)`
* `bestline.historyfree()`
* `bestline.addhistory(line)`
* `bestline.savehistory(filename)`
* `bestline.loadhistory(filename)`

`historysetmaxlen(length)` and `sethistorymaxlen(length)` exist for
`lua-linenoise` source compatibility, but return `nil, err`, because Bestline
uses a fixed compile-time history limit.

### Completion

```lua
bestline.setcompletion(function(completions, line, pos)
  if line == "h" then
    completions:add("help")
  end
end)
```

The callback receives:

* `completions`: userdata with `:add(value)`
* `line`: current input buffer
* `pos`: current cursor position from Bestline

Existing `lua-linenoise` callbacks that accept only `(completions, line)` still
work.

Clear completion with:

```lua
bestline.setcompletion(nil)
```

### Hints

```lua
bestline.sethints(function(line)
  if line == "h" then
    return " help", { ansi1 = "\027[90m", ansi2 = "\027[39m" }
  end
end)
```

For `lua-linenoise` compatibility, legacy style tables are accepted too:

```lua
return " help", { color = 36, bold = true }
```

Clear hints with:

```lua
bestline.sethints(nil)
```

### Modes

* `bestline.setmaskmode(enabled)`
* `bestline.setbalancemode(enabled)`
* `bestline.setllamamode(enabled)`
* `bestline.setemacsmode(enabled)`
* `bestline.setmultiline(enabled)`
* `bestline.disablerawmode()`

`setmaskmode(true)` displays typed input as asterisks.

`setbalancemode(true)` keeps reading until parentheses are balanced.

`setllamamode(true)` enables triple-quote multiline input, compatible with
Bestline's Ollama-style mode.

`setemacsmode(true)` enables additional Emacs editing commands.

`setmultiline(enabled)` is kept as a compatibility no-op: Bestline is
multiline-only.

### Character Helpers

* `bestline.setxlat(callback_or_nil)`
* `bestline.characterwidth(codepoint_or_string)`
* `bestline.isseparator(codepoint_or_string)`
* `bestline.notseparator(codepoint_or_string)`
* `bestline.isxeparator(codepoint_or_string)`
* `bestline.uppercase(codepoint_or_string)`
* `bestline.lowercase(codepoint_or_string)`
* `bestline.toupper(codepoint_or_string)`
* `bestline.tolower(codepoint_or_string)`

`setxlat` lets Lua translate typed characters before insertion:

```lua
bestline.setxlat(function(codepoint)
  if codepoint >= string.byte("a") and codepoint <= string.byte("z") then
    return codepoint - 32
  end
  return codepoint
end)
```

The callback may return a Unicode codepoint number, a UTF-8 string, or `nil` to
keep the original character.

## Shortcuts

Bestline implements many Readline-style shortcuts:

```text
CTRL-E         END
CTRL-A         START
CTRL-B         BACK
CTRL-F         FORWARD
CTRL-L         CLEAR
CTRL-H         BACKSPACE
CTRL-D         DELETE
CTRL-Y         YANK
CTRL-D         EOF, if empty
CTRL-N         NEXT HISTORY
CTRL-P         PREVIOUS HISTORY
CTRL-R         SEARCH HISTORY
CTRL-G         CANCEL SEARCH
ALT-<          BEGINNING OF HISTORY
ALT->          END OF HISTORY
ALT-F          FORWARD WORD
ALT-B          BACKWARD WORD
CTRL-ALT-F     FORWARD EXPR
CTRL-ALT-B     BACKWARD EXPR
ALT-RIGHT      FORWARD EXPR
ALT-LEFT       BACKWARD EXPR
CTRL-K         KILL LINE FORWARDS
CTRL-U         KILL LINE BACKWARDS
ALT-H          KILL WORD BACKWARDS
CTRL-W         KILL WORD BACKWARDS
CTRL-ALT-H     KILL WORD BACKWARDS
ALT-D          KILL WORD FORWARDS
ALT-Y          ROTATE KILL RING AND YANK AGAIN
ALT-\          SQUEEZE ADJACENT WHITESPACE
CTRL-T         TRANSPOSE
ALT-T          TRANSPOSE WORD
ALT-U          UPPERCASE WORD
ALT-L          LOWERCASE WORD
ALT-C          CAPITALIZE WORD
CTRL-C         INTERRUPT PROCESS
CTRL-Z         SUSPEND PROCESS
CTRL-\         QUIT PROCESS
CTRL-S         PAUSE OUTPUT
CTRL-Q         UNPAUSE OUTPUT, if paused
CTRL-Q         ESCAPED INSERT
CTRL-SPACE     SET MARK
CTRL-X CTRL-X  GOTO MARK
```

## Why Bestline Instead Of Linenoise?

Compared to the original linenoise, Bestline adds or improves:

* UTF-8 editing
* History search with `CTRL-R`
* Multiline editing as the default mode
* Long input lines
* Kill ring commands
* Terminal resize handling
* O_NONBLOCK file descriptor handling
* Raw mode restoration when the process returns to the foreground
* Many GNU Readline-style editing shortcuts
* ANSI / VT100 parsing to avoid corrupted input state

## Compatibility Notes

This module intentionally does not replace `require "linenoise"`. Use:

```lua
local bestline = require "bestline"
```

The binding includes compatibility aliases for common `lua-linenoise` function
names to make migration straightforward. The project can therefore coexist with
an existing `lua-linenoise` installation in the same Lua environment.

## License

`bestlinelib.c` is MIT licensed and based on the original `lua-linenoise`
binding.

The vendored `bestline.c` and `bestline.h` are BSD-2-Clause licensed. See
[LICENSE](LICENSE) for the combined license text.
