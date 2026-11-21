-- metalog demo: how a plugin uses lib/metalog.lua            plugins.ini:  examples/metalog_demo.lua
-- Console: metalog status | metalog tail metalog_demo | metalog level metalog_demo trace
xmod.register("metalog demo", "1.0", "xmod")
local log = require("metalog").new("metalog_demo")          -- file: data/logs/metalog_demo_<date>.md

-- a debug-only helper: when metalog is switched off (lib/metalog.lua: enabled = false, or level above "debug")
-- this becomes an EMPTY function and the code inside never runs
local show_debug_hud = log:fn(function(id, text)
  xmod.msg.hud(id, "[debug] " .. text, { x = 0.02, y = 0.25, hold = 3, channel = 4, r = 255, g = 160, b = 0 })
end)

xmod.on("client_putinserver", function(id)
  log:info("%s joined", require("metalog").who(id))
  show_debug_hud(id, "welcome, you are #" .. id)
end)

-- errors inside this handler are logged with the arguments and the handler keeps working
xmod.on("message:DeathMsg", log:wrap(function(_, _, _, a)
  local killer, victim = a[1], a[2]
  log:count("kills")
  log:debug("DeathMsg killer=%d victim=%d hs=%d weapon=%s", killer, victim, a[3], a[4])
  if killer == victim then log:every(5, "info", "suicide by #%d", victim) end
end, "DeathMsg handler"))

xmod.cmd.say("/mlog", "", function(id)
  log:section("/mlog by " .. xmod.player.name(id))
  log:player(id, "asked for the demo")
  log:dump("player data", { name = xmod.player.name(id), team = xmod.player.team(id), origin = { xmod.player.origin(id) } })
  local rows = {}
  for _, p in ipairs(xmod.player.all()) do rows[#rows + 1] = { p, xmod.player.name(p), xmod.player.frags(p) } end
  log:table("scoreboard", { "id", "name", "frags" }, rows)
  log:counts()
  log:time("loop"); local s = 0; for i = 1, 100000 do s = s + i end; log:time_end("loop")
  local ok = log:try(function() error("demo error - this is expected") end)
  log:assert(ok, "the try() above failed as planned")
  xmod.msg.chat(id, "{g}[metalog] {y}written to {t}" .. log:path())
end)
