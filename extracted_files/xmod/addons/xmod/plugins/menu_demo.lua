xmod.register("Menu demo", "0.1", "Andrew")
local menu = require("menu")

xmod.cmd.say("/menu", "", function(id)
  menu.show(id, {
    title = "xmod demo",
    items = { {"Heal me", "heal"}, {"Give 1000$", "money"}, {"Launch up", "launch"}, {"Show map name", "map"} },
    on_select = function(pid, label, value)
      if value == "heal" then xmod.player.set_health(pid, 100)
      elseif value == "money" then xmod.player.set_money(pid, xmod.player.money(pid) + 1000)
      elseif value == "launch" then local vx, vy, vz = xmod.player.velocity(pid); xmod.player.set_velocity(pid, vx, vy, vz + 300)
      else xmod.msg.chat(pid, "{g}[xmod] {y}map: " .. xmod.map()) end
    end,
  })
end, "open demo menu")
