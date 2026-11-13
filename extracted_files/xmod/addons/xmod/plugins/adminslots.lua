-- Reserved slots: keep `xmod_reserved_slots` slots free for admins with flag b (reservation).
-- Non-reserved players who would take a reserved slot are kicked.
xmod.register("Admin Slots", "0.1", "xmod")
local U = require("util")
xmod.cvar.create("xmod_reserved_slots", "0")
xmod.cvar.create("xmod_slots_kick_msg", "Server full - slot reserved for admins")

xmod.on("config", function()
  local r = xmod.cvar.int("xmod_reserved_slots")
  if r > 0 and xmod.cvar.exists("sv_visiblemaxplayers") then xmod.cvar.set("sv_visiblemaxplayers", tostring(xmod.maxplayers() - r)) end
end)
xmod.on("client_putinserver", function(id)
  local r = xmod.cvar.int("xmod_reserved_slots")
  if r <= 0 or xmod.player.is_bot(id) then return end
  xmod.timer.after(1.0, function()          -- give admin.lua a moment to assign access
    if not xmod.player.connected(id) or xmod.player.has_access(id, "b") then return end
    if U.count_humans() > xmod.maxplayers() - r then xmod.player.kick(id, xmod.cvar.get("xmod_slots_kick_msg")) end
  end)
end)
