-- ModCreator example: say /npc spawns a knife grunt where you aim, /npcs lists them, /killnpcs removes all.
plugin = { name = "mc_ex_npc", version = "1.0", author = "Andrew & Claude" }
local MC = require("modcreator")
MC.npc.register("guard", {
  model = "models/player/gign/gign.mdl", health = 150, speed = 220, team = "monsters", ai = "guard", leash = 500,
  attack = { damage = 12, range = 64, rate = 1.0, delay = 0.3 },
  anims = { idle = { "idle1" }, walk = { "walk" }, run = { "run" }, attack = { "ref_shoot_knife" }, pain = { "gut_flinch" }, death = { "death1", "death2" } },
  reward = { frags = 1, money = 300 },
  on_death = function(npc, killer) if MC.util.is_player(killer) then xmod.msg.chat(0, "{g}" .. xmod.player.name(killer) .. "{y} killed a guard") end end,
})
xmod.cmd.say("/npc", "a", function(id)
  local _, tr = MC.player.aim_ent(id, 2000)
  local npc = MC.npc.spawn("guard", tr.x, tr.y, tr.z + 2, select(2, xmod.ent.get(id, "angles")) + 180)
  xmod.msg.chat(id, "{g}[MC] {y}npc #" .. npc.ent .. " spawned")
end, "spawn a guard NPC")
xmod.cmd.say("/npcs", "a", function(id) xmod.msg.chat(id, "{g}[MC] {y}" .. MC.npc.count() .. " NPCs alive") end)
xmod.cmd.say("/killnpcs", "a", function(id) MC.npc.remove_all() end)
