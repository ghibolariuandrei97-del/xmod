-- test fixture: message safety + bots
xmod.register("msg test", "1", "test")
local seen = {}
xmod.on("message:DeathMsg", function(name, dest, ent, a)
  xmod.log("DeathMsg handler args=" .. table.concat(a, ","))
  xmod.msg.hud(0, "kill message from inside DeathMsg handler", { y = 0.2 })      -- used to crash the server
  xmod.msg.chat(0, "{g}chat from inside DeathMsg handler")
  xmod.msg.fade(1, 1, 1, 0, 255, 0, 0, 80)
  xmod.player.kill(2)                                                              -- game sends DeathMsg etc.: must be safe
  seen.death = true
end)
xmod.on("message:Blockme", function() xmod.log("Blockme handler -> blocking"); return true end)
xmod.on("message:Observe", function(n, d, e, a) xmod.log("Observe handler sees " .. table.concat(a, ",")) end)
xmod.on("client_connect", function(id, name, ip) xmod.log(("client_connect #%d %s %s"):format(id, name, ip)) end)
xmod.on("client_putinserver", function(id) xmod.log(("client_putinserver #%d bot=%s name=%s"):format(id, tostring(xmod.player.is_bot(id)), xmod.player.name(id))) end)
xmod.on("client_disconnect", function(id) xmod.log(("client_disconnect #%d"):format(id)) end)
xmod.cmd.server("t_chat", function() xmod.msg.chat(1, "{y}deferred chat while a game message is open") end)
xmod.cmd.server("t_builder_err", function()
  xmod.msg.begin(xmod.msg.ONE, 100, nil, nil, nil, 1); xmod.msg.byte(0); xmod.msg.string("half built")
  error("boom between begin and finish")                                           -- builder must be discarded, engine untouched
end)
xmod.cmd.server("t_builder_ok", function()
  xmod.msg.begin(xmod.msg.ONE, 100, nil, nil, nil, 1); xmod.msg.byte(0); xmod.msg.string("atomic message"); xmod.msg.finish()
end)
