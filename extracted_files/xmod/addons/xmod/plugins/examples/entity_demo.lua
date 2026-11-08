-- ENTITY EXAMPLE: say /look  -> tells you what you are aiming at (trace from your eyes along your view angles).
xmod.register("Entity demo", "0.1", "xmod")

xmod.cmd.say("/look", "", function(id)
  local ox, oy, oz = xmod.player.origin(id)
  local _, _, vz = xmod.ent.get(id, "view_ofs")
  local pitch, yaw = xmod.player.view_angles(id)
  local p, y = math.rad(pitch), math.rad(yaw)
  local dx, dy, dz = math.cos(p) * math.cos(y), math.cos(p) * math.sin(y), -math.sin(p)
  local sx, sy, sz = ox, oy, oz + vz
  local tr = xmod.trace.line(sx, sy, sz, sx + dx * 4096, sy + dy * 4096, sz + dz * 4096, 0, id)
  if tr.ent > 0 then
    xmod.msg.chat(id, ("{g}[look] {y}entity {t}%d {y}class {t}%s{y}, distance %.0f"):format(tr.ent, xmod.ent.classname(tr.ent), tr.fraction * 4096))
  else
    xmod.msg.chat(id, ("{g}[look] {y}world surface at %.0f units"):format(tr.fraction * 4096))
  end
end)

-- touch hook by classname: log when a player touches a button (event: touch:<classname>(entity, other))
xmod.on("touch:func_button", function(button, other)
  if other >= 1 and other <= xmod.maxplayers() then xmod.log(xmod.player.name(other) .. " touched button " .. button) end
end)
