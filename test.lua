local L = require 'bestline'

-- Standard ANSI color codes.
local colors = { red = 31, green = 32, yellow = 33, blue = 34, magenta = 35, cyan = 36, white = 37 }

local prompt, history = '? ', 'history.txt'

local test_mode = os.getenv("BESTLINE_TEST") or os.getenv("LINENOISE_TEST")

if not test_mode then
    print('--- lua-bestline full example ---')
    print('Commands: /mask, /balance, /llama, /emacs, /xlat, /multiline, /clear, /history <len>')
    print('Try typing "h" for completion/hints or use arrow keys for history.')
end

-- 1. L.setmultiline(multiline)
local multiline = false
if arg[1] == "--multiline" then
    multiline = true
    L.setmultiline(true)
end

-- 2. L.historyload(filename)
L.historyload(history)

-- 3. L.historysetmaxlen(length)
L.historysetmaxlen(100)

if not test_mode then
    -- Normal mode.
    L.setcompletion(function(c, s)
        if s == 'h' then
            c:add('help')
            L.addcompletion(c, 'halt')
        elseif s == '/' then
            c:add('/mask')
            c:add('/balance')
            c:add('/llama')
            c:add('/emacs')
            c:add('/xlat')
            c:add('/multiline')
            c:add('/clear')
            c:add('/history')
        end
    end)

    L.sethints(function(s)
        if s == 'h' then
            return ' (help or halt)', { color = colors.cyan, bold = true }
        elseif s == '/mask' then
            return ' [toggle password mode]', { color = colors.red }
        end
    end)
else
    -- Automated test mode (test.c).
    -- Provide deterministic behavior for C assertions.
    L.setcompletion(function(c, s)
        if s == 't' then
            c:add('test-completion')
        end
    end)

    L.sethints(function(s)
        if s == 't' then
            return ' hint-text', { color = 31 } -- Red.
        end
    end)
end

-- 6. L.linenoise(prompt) and main loop.
local line, err = L.linenoise(prompt)
local mask = false
local balance = false
local llama = false
local emacs = false
local xlat = false

while line do
    if #line > 0 then
        if line == '/mask' then
            mask = not mask
            -- 7. L.setmaskmode(mask)
            L.setmaskmode(mask)
            print("Mask mode: " .. (mask and "on" or "off"))
        elseif line == '/balance' then
            balance = not balance
            L.setbalancemode(balance)
            print("Balance mode: " .. (balance and "on" or "off"))
        elseif line == '/llama' then
            llama = not llama
            L.setllamamode(llama)
            print("Llama mode: " .. (llama and "on" or "off"))
        elseif line == '/emacs' then
            emacs = not emacs
            L.setemacsmode(emacs)
            print("Emacs mode: " .. (emacs and "on" or "off"))
        elseif line == '/xlat' then
            xlat = not xlat
            if xlat then
                L.setxlat(function(codepoint)
                    if codepoint >= 97 and codepoint <= 122 then
                        return codepoint - 32
                    end
                    return codepoint
                end)
            else
                L.setxlat(nil)
            end
            print("Xlat mode: " .. (xlat and "on" or "off"))
        elseif line == '/multiline' then
            multiline = not multiline
            L.setmultiline(multiline)
            print("Multiline mode: " .. (multiline and "on" or "off"))
        elseif line == '/clear' then
            -- 8. L.clearscreen()
            L.clearscreen()
        elseif line:match('^/history') then
            local len = tonumber(line:match('%d+'))
            if len then 
                L.historysetmaxlen(len) 
                print("History max len: " .. len)
            end
        elseif line == '/keycodes' then
            -- 9. L.printkeycodes()
            print("Press keys to see codes (ESC to stop):")
            L.printkeycodes()
        else
            if not test_mode then
                print("Echo: " .. line:upper())
            end
            -- 10. L.historyadd(line)
            L.historyadd(line)
            -- 11. L.historysave(filename)
            L.historysave(history)
        end
    end
    line, err = L.linenoise(prompt)
end

if err then
    print('An error occurred: ' .. err)
end
