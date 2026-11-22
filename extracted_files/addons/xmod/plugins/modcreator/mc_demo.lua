-- ModCreator demo mod: "Bot Siege" - everything in ~150 lines.
--   * every player is a Human (CT); waves of NPC Invaders (knife grunts + rifle soldiers) attack from the T spawns
--   * hold out until the timer runs down (survive objective) -> Humans win; everybody dead -> Invaders win
--   * custom weapon "Thunder AK" (say /guns), money for kills, HUD line with wave / enemies / time
-- Enable: remove the ";" in front of modcreator/mc_demo.lua in configs/plugins-modcreator.ini (and restart the map).
plugin = { name = "mc_demo", version = "1.0", author = "Andrew & Claude" }
local MC = require("modcreator")
local P, E = xmod.player, xmod.ent

local HOLD_TIME = 150             -- seconds the humans must survive
local WAVE_EVERY = 25             -- seconds between waves
local MAX_ALIVE = 24              -- NPC cap (entity budget)

MC.teams.define {
  { key = "humans", name = "Humans", cs = 2, color = { 60, 160, 255 } },
  { key = "invaders", name = "Invaders", cs = 1, color = { 255, 70, 40 } },
}
MC.teams.friendly_fire(false)

-- NPCs on the CS player models (the game precaches those itself)
local anims = { idle = { "idle1", "idle" }, walk = { "walk" }, run = { "run" }, pain = { "gut_flinch", "head_flinch" },
  death = { "death1", "death2", "death3", "head", "gutshot", "back" } }
MC.npc.register("grunt", { model = "models/player/terror/terror.mdl", team = "invaders", health = 90, speed = 230,
  attack = { damage = 14, range = 64, rate = 1.1, delay = 0.3, sound = { "weapons/knife_hit1.wav", "weapons/knife_hit2.wav" } },
  anims = setmetatable({ attack = { "ref_shoot_knife" } }, { __index = anims }), sounds = { death = { "player/die1.wav", "player/die2.wav", "player/die3.wav" } },
  reward = { frags = 1, money = 150 }, corpse = 4 })
MC.npc.register("soldier", { model = "models/player/leet/leet.mdl", team = "invaders", health = 120, speed = 200, sight = 1800,
  attack = { ranged = true, damage = 9, range = 900, rate = 0.9, delay = 0.15, spread = 0.06, sound = "weapons/ak47-1.wav" },
  anims = setmetatable({ rattack = { "ref_shoot_ak47" } }, { __index = anims }), sounds = { death = { "player/die1.wav", "player/die3.wav" } },
  reward = { frags = 2, money = 250 }, corpse = 4 })

-- the custom weapon (no new files needed: the AK47 models + a stronger, faster gun)
MC.weapons.register("thunder", { base = "weapon_ak47", name = "Thunder AK", damage = 1.6, clip = 45, ammo = 180, rate = 0.085,
  recoil = 0.6, knockback = 110, cost = 2500 })

local wave, next_wave, hold = 0, 0, nil
local function alive_invaders() return MC.npc.count() end
local function spawn_wave()
  wave = wave + 1
  local n = math.min(3 + wave * 2, MAX_ALIVE - alive_invaders())
  for i = 1, n do
    local x, y, z, yaw = MC.spawns.random("t", 40)
    if not x then break end
    local kind = (i % 3 == 0) and "soldier" or "grunt"
    MC.npc.spawn(kind, x + math.random(-40, 40), y + math.random(-40, 40), z, yaw, { health = MC.npc.def(kind).health + wave * 10 })
  end
  MC.hud.text(0, "WAVE " .. wave, { r = 255, g = 80, b = 40, y = 0.25, hold = 2.5, channel = 3 })
  MC.fx.shake(0, 6, 1.5, 4)
end

MC.round.block_end(true)          -- the game must not end rounds by itself (there are no T players)
MC.round.on("new", function()
  MC.npc.remove_all(); wave = 0
  for _, id in ipairs(P.all()) do MC.teams.assign(id, "humans") end
  MC.objectives.remove("hold")
  hold = MC.objectives.add { id = "hold", name = "Hold out", type = "survive", time = HOLD_TIME, team = "humans", round_win = true }
end)
MC.round.on("start", function() next_wave = xmod.gametime() + 3 end)
MC.round.win_check(function()
  if MC.teams.alive("humans") == 0 and #P.all() > 0 then return "invaders", { message = "The Invaders overran the base!" } end
end)
MC.round.on("end", function(winner) if winner == "humans" then MC.npc.remove_all() end end)

xmod.timer.every(1.0, function()
  if not MC.round.is_live() then return end
  local t = xmod.gametime()
  if t >= next_wave then next_wave = t + WAVE_EVERY; spawn_wave() end
  local left = hold and math.max(0, math.floor(HOLD_TIME - (t - hold.started))) or 0
  for _, id in ipairs(P.all()) do
    MC.hud.line(id, string.format("Wave %d  |  Invaders: %d  |  Hold: %d s  |  $%d", wave, alive_invaders(), left, P.money(id)), { r = 120, g = 200, b = 255 })
  end
end)

-- players: always Humans, a Thunder AK at every spawn while they can afford nothing better
xmod.on("client_putinserver", function(id) xmod.timer.after(1, function() if P.ingame(id) then MC.teams.assign(id, "humans") end end) end)
xmod.on("ham:spawn:player:post", function(id)
  xmod.timer.after(0.2, function() if P.alive(id) and MC.teams.of(id) == "humans" then MC.weapons.give(id, "thunder", true) end end)
end)
xmod.cmd.say("/guns", "", function(id) MC.weapons.menu(id) end, "buy ModCreator weapons")
xmod.on("mc:npc_killed", function(e, name, killer)
  if MC.util.is_player(killer) then xmod.msg.chat(killer, "{g}[Siege] {y}+" .. (MC.npc.def(name).reward.money) .. "$ for a " .. name) end
end)
