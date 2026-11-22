-- Server ads: rotates lines from configs/ads.ini in chat (or HUD).  Placeholders: %map% %timeleft% %players% %max% %nextmap% %time%
xmod.register("Server Messages", "0.1", "xmod")
local U = require("util")
xmod.cvar.create("xmod_ads_interval", "90")   -- seconds between messages (0 = off)
xmod.cvar.create("xmod_ads_mode", "0")        -- 0 = chat, 1 = HUD

local ads, idx = {}, 0
local function load_ads()
  ads = {}
  for line in ((xmod.fs.read("configs/ads.ini") or "") .. "\n"):gmatch("(.-)\r?\n") do
    line = line:gsub("^%s+", "")
    if line ~= "" and line:sub(1, 1) ~= ";" and line:sub(1, 2) ~= "//" then ads[#ads + 1] = line end
  end
end
local function expand(s)
  local tl = U.timeleft()
  return (s:gsub("%%(%w+)%%", function(k)
    if k == "map" then return xmod.map() elseif k == "timeleft" then return tl and U.fmt_time(tl) or "no limit"
    elseif k == "players" then return tostring(#xmod.player.all()) elseif k == "max" then return tostring(xmod.maxplayers())
    elseif k == "nextmap" then return xmod.cvar.get("xmod_nextmap") elseif k == "time" then return os.date("%H:%M") end
  end))
end
xmod.on("config", load_ads)
local acc = 0
xmod.timer.every(1.0, function()
  local iv = xmod.cvar.int("xmod_ads_interval")
  if iv <= 0 or #ads == 0 then acc = 0; return end
  acc = acc + 1
  if acc < iv then return end
  acc = 0; idx = idx % #ads + 1
  local m = expand(ads[idx])
  if xmod.cvar.int("xmod_ads_mode") == 1 then xmod.msg.hud(0, m, { y = 0.9, hold = 5, r = 100, g = 200, b = 255, channel = 2 })
  else xmod.msg.chat(0, "{g}[info] {y}" .. m) end
end)
