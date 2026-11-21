-- translator demo: lang/translator_demo.txt + lib/translator.lua     plugins.ini:  examples/translator_demo.lua
-- say /lang (menu), console: xmod_lang ro | xmod_lang en | xmod_lang reset ; server: xmod_lang_list, xmod_lang_check
xmod.register("translator demo", "1.0", "xmod")
local T = require("translator")
local tr = T.load("translator_demo")

tr:add("de", { DEMO_HP = "{g}[demo] {y}Du hast {t}%d{y} HP." })   -- texts can also be added from Lua

xmod.on("client_putinserver", function(id)
  xmod.timer.after(3, function(pid)
    if xmod.player.connected(pid) then tr:chat(pid, "DEMO_WELCOME", xmod.player.name(pid)) end
  end, id)
end)

xmod.on("lang_changed", function(id, old, new)            -- fired by the translator when a player changes language
  xmod.log(("player #%d: %s -> %s"):format(id, old, new))
end)

xmod.cmd.say("/hp", "", function(id) tr:chat(id, "DEMO_HP", math.floor(xmod.player.health(id))) end)
xmod.cmd.say("/kills", "", function(id) xmod.msg.chat(id, tr:plural(id, "DEMO_KILLS", xmod.player.frags(id))) end)
xmod.cmd.say("/team", "", function(id)
  xmod.msg.chat(id, tr(id, "DEMO_NAMED", { name = xmod.player.name(id), team = xmod.player.team_name(xmod.player.team(id)) }))
end)
xmod.cmd.say("/rhud", "", function(id) tr:hud(id, { y = 0.3, hold = 3 }, "DEMO_HUD") end)
