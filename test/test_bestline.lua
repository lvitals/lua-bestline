local bestline = require "bestline"

local total = 0

local function ok(condition, message)
  total = total + 1
  if not condition then
    error(message or "assertion failed", 2)
  end
end

local function eq(actual, expected, message)
  ok(actual == expected, (message or "values differ") ..
    string.format(": expected %q, got %q", tostring(expected), tostring(actual)))
end

local function truthy(value, message)
  ok(value == true, message)
end

eq(type(bestline), "table", "module returns a table")
eq(bestline._NAME, "lua-bestline", "module name")
eq(type(bestline.line), "function", "line function exists")
eq(bestline.line, bestline.bestline, "line aliases bestline")
eq(bestline.linenoise, bestline.bestline, "linenoise aliases bestline")

eq(type(bestline.lines("> ")), "function", "lines returns an iterator")
truthy(bestline.enableutf8() == nil, "enableutf8 is a no-op")

truthy(bestline.setcompletion(function() end), "setcompletion accepts a callback")
truthy(bestline.setcompletion(nil), "setcompletion clears callback")
truthy(bestline.sethints(function()
  return " hint", { color = 90, bold = false }
end), "sethints accepts legacy color style")
truthy(bestline.sethints(function()
  return " hint", { ansi1 = "\027[90m", ansi2 = "\027[39m" }
end), "sethints accepts ANSI style")
truthy(bestline.sethints(nil), "sethints clears callback")

truthy(bestline.setmaskmode(true), "enable mask mode")
truthy(bestline.setmaskmode(false), "disable mask mode")
truthy(bestline.setbalancemode(true), "enable balance mode")
truthy(bestline.setbalancemode(false), "disable balance mode")
truthy(bestline.setllamamode(true), "enable llama mode")
truthy(bestline.setllamamode(false), "disable llama mode")
truthy(bestline.setemacsmode(true), "enable emacs mode")
truthy(bestline.setemacsmode(false), "disable emacs mode")
truthy(bestline.setxlat(function(c) return c end), "set xlat callback")
truthy(bestline.setxlat(nil), "clear xlat callback")

eq(bestline.characterwidth(("a"):byte()), 1, "ASCII width")
eq(bestline.characterwidth("🎉"), 2, "emoji width")
eq(bestline.isseparator((" "):byte()), true, "space is a separator")
eq(bestline.notseparator(("a"):byte()), true, "letter is not a separator")
eq(bestline.isxeparator((" "):byte()), true, "space is an expression separator")
eq(bestline.uppercase(("a"):byte()), ("A"):byte(), "uppercase ASCII")
eq(bestline.lowercase(("Z"):byte()), ("z"):byte(), "lowercase ASCII")

truthy(bestline.historyfree(), "historyfree succeeds")
truthy(bestline.historyadd("first"), "historyadd first entry")
truthy(bestline.historyadd("second"), "historyadd second entry")

local tmp = os.tmpname()
truthy(bestline.historysave(tmp), "historysave succeeds")
truthy(bestline.historyfree(), "historyfree after save")
truthy(bestline.historyload(tmp), "historyload succeeds")

local missing = tmp .. ".missing"
truthy(bestline.historyload(missing), "loading a missing history file is ok")

local ok_setmax, err = bestline.historysetmaxlen(10)
eq(ok_setmax, nil, "historysetmaxlen is unsupported")
eq(type(err), "string", "historysetmaxlen returns an error message")

os.remove(tmp)

print(string.format("ok %d tests", total))
