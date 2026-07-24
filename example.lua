local bestline = require "bestline"

local prompt = "> "
local history = "history.txt"

bestline.historyload(history)

bestline.setcompletion(function(completions, line)
  if line == "h" then
    completions:add("help")
    completions:add("halt")
  end
end)

bestline.sethints(function(line)
  if line == "h" then
    return " help", { ansi1 = "\027[90m", ansi2 = "\027[39m" }
  end
end)

for line in bestline.lines(prompt) do
  if #line > 0 then
    print(line)
    bestline.historyadd(line)
    bestline.historysave(history)
  end
end
