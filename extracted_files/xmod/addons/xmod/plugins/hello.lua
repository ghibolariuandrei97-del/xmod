xmod.register("Hello xmod", "0.1", "Andrew")

xmod.cvar.create("xmod_hello_msg", "Welcome to the server, {t}%s{y}!", xmod.cvar.SERVER)

xmod.on("client_putinserver", function(id)
  xmod.timer.after(3.0, function(pid)
    if xmod.player.connected(pid) then
      local fmt = xmod.cvar.get("xmod_hello_msg")
      xmod.msg.chat(pid, "{g}[xmod] {y}" .. fmt:format(xmod.player.name(pid)))
    end
  end, id)
end)

-- chat command:  say /me
xmod.cmd.say("/me", "", function(id)
  local x, y, z = xmod.player.origin(id)
  xmod.msg.chat(id, string.format("{g}[xmod] {y}hp=%d  money=%d  pos=%.0f %.0f %.0f",
    xmod.player.health(id), xmod.player.money(id), x, y, z))
end, "show my info")

-- per-player data + cmd_start: count jumps
xmod.on("cmd_start", function(id, buttons)
  local d = xmod.player.data(id)
  local jumping = (buttons & xmod.IN_JUMP) ~= 0
  if jumping and not d.was_jumping then d.jumps = (d.jumps or 0) + 1 end
  d.was_jumping = jumping
end)
xmod.cmd.say("/jumps", "", function(id) xmod.msg.chat(id, "{g}[xmod] {y}jumps: " .. (xmod.player.data(id).jumps or 0)) end)

-- message hook on DeathMsg: killer, victim, headshot, weapon
xmod.on("message:DeathMsg", function(name, dest, ent, a)
  local killer, victim = a[1], a[2]
  if killer ~= victim and killer > 0 and xmod.player.connected(killer) and xmod.player.connected(victim) then
    xmod.msg.hud(0, xmod.player.name(killer) .. " killed " .. xmod.player.name(victim),
      { y = 0.15, hold = 3, r = 255, g = 120, b = 0, channel = 3 })
  end
end)
