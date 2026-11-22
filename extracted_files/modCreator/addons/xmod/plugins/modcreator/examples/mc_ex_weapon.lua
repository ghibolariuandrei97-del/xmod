-- ModCreator example: two custom weapons. /laser and /shotty give them (admins with flag "a").
plugin = { name = "mc_ex_weapon", version = "1.0", author = "Andrew & Claude" }
local MC = require("modcreator")
-- 1) "WeaponMod" style: the base weapon shoots, ModCreator changes damage / clip / rate / recoil / speed
MC.weapons.register("sniper_pro", { base = "weapon_awp", name = "AWP Pro", damage = 1.5, clip = 7, ammo = 40, speed = 230, cost = 6000 })
-- 2) custom fire: the shot is replaced completely (laser bullets with a tracer, own rate, no spread)
MC.weapons.register("laser", { base = "weapon_m4a1", name = "Laser Rifle", clip = 40, ammo = 160, rate = 0.12, recoil = 0.4,
  fire = { damage = 34, spread = 0.004, shots = 1, tracer = true }, anims = { shoot = { 3, 4, 5 } }, sound = "weapons/m4a1_unsil-1.wav",
  on_hit = function(attacker, victim, dmg) if MC.util.is_player(victim) then MC.fx.light(xmod.player.origin(victim)) end return dmg end })
-- 3) shotgun on the deagle: 8 pellets
MC.weapons.register("shotty", { base = "weapon_deagle", name = "Hand Cannon", clip = 6, ammo = 30, rate = 0.6, recoil = 3,
  fire = { damage = 14, spread = 0.09, shots = 8 }, sound = "weapons/m3-1.wav", anims = { shoot = { 2 } } })
xmod.cmd.say("/laser", "a", function(id) MC.weapons.give(id, "laser", true) end)
xmod.cmd.say("/shotty", "a", function(id) MC.weapons.give(id, "shotty", true) end)
xmod.cmd.say("/wmenu", "", function(id) MC.weapons.menu(id) end)
