-- Kill announcements: first blood, headshots, kill streaks (HUD).  Built on the DeathMsg user message (works without Ham/ReGameDLL).
xmod.register("Kill Streaks", "0.1", "xmod")
xmod.cvar.create("xmod_killstreaks", "1")
local streak, first_blood = {}, false
local TEXT = { [3] = "is on a KILLING SPREE", [5] = "is DOMINATING", [7] = "is UNSTOPPABLE", [10] = "is GODLIKE" }

xmod.on("message:DeathMsg", function(_, _, _, a)
  if xmod.cvar.int("xmod_killstreaks") == 0 then return end
  local killer, victim, hs = a[1], a[2], a[3] == 1
  streak[victim] = 0
  if killer < 1 or killer == victim or not xmod.player.connected(killer) then return end
  streak[killer] = (streak[killer] or 0) + 1
  local kn = xmod.player.name(killer)
  if not first_blood then first_blood = true; xmod.msg.hud(0, kn .. " drew FIRST BLOOD!", { y = 0.2, r = 255, g = 40, b = 40, hold = 3, channel = 3 })
  elseif TEXT[streak[killer]] then xmod.msg.hud(0, kn .. " " .. TEXT[streak[killer]] .. " (" .. streak[killer] .. ")", { y = 0.2, r = 255, g = 180, b = 0, hold = 3, channel = 3 })
  elseif hs then xmod.msg.hud(killer, "HEADSHOT!", { y = 0.6, r = 255, g = 255, b = 0, hold = 1.2, channel = 3 }) end
end)
xmod.on("client_disconnect", function(id) streak[id] = nil end)
xmod.on("map_end", function() streak = {}; first_blood = false end)
xmod.on("init", function() first_blood = false end)
