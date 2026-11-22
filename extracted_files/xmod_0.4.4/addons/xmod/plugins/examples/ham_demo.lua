-- HAM EXAMPLE: virtual-function hooks on CS entities. Works on stock CS 1.6 and ReGameDLL.
-- Event name:  ham:<function>:<classname>[:post]     (function list: docs/HAM_FUNCTIONS.md)
xmod.register("Ham demo", "0.1", "xmod")
xmod.cvar.create("xmod_demo_dmg", "1.0")           -- damage multiplier

-- PRE hook: args after the entity are (inflictor, attacker, damage, damage_bits).  set_arg(n, v): n counts from 1 after 'this'.
xmod.on("ham:takedamage:player", function(victim, inflictor, attacker, damage, bits)
  local m = xmod.cvar.num("xmod_demo_dmg")
  if m ~= 1.0 then xmod.hook.set_arg(3, damage * m) end        -- 3 = damage
  -- To block the damage completely:  return xmod.HC_SUPERCEDE, 0
end)

-- POST hook: same args + the original return value as the last arg
xmod.on("ham:killed:player:post", function(victim, attacker, gib)
  if attacker >= 1 and attacker <= xmod.maxplayers() and attacker ~= victim then
    xmod.msg.chat(attacker, "{g}[demo] {y}You killed {t}" .. xmod.player.name(victim))
  end
end)

-- Spawn: give armor to everyone on spawn
xmod.on("ham:spawn:player:post", function(id)
  if xmod.player.alive(id) then xmod.player.set_armor(id, 100) end
end)

-- Weapon hook: log every AK-47 reload
xmod.on("ham:weapon_reload:weapon_ak47", function(wpn) xmod.log("ak47 reload, weapon ent " .. wpn) end)
