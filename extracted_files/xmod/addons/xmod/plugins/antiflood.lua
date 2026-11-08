-- Anti-flood: blocks chat spam. Admins with immunity (flag a) are exempt.
xmod.register("Anti Flood", "0.1", "xmod")
xmod.cvar.create("xmod_flood_time", "0.75")
xmod.on("chat", function(id)
  if xmod.player.has_access(id, "a") then return end
  local d, now = xmod.player.data(id), xmod.gametime()
  local min = xmod.cvar.num("xmod_flood_time")
  if min > 0 and d.last_chat and now - d.last_chat < min then
    d.last_chat = now
    xmod.msg.chat(id, "{g}[xmod] {y}Stop flooding the server!")
    return true
  end
  d.last_chat = now
end, 90)
