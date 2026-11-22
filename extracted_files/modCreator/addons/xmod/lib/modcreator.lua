-- ModCreator 1.0 - build whole new mods with xmod:   local MC = require("modcreator")
-- Needs the ModCreator extension (dlls/modcreator_mm*.so|dll, namespace xmod.mc). Works with xmod 0.4.0 - 0.4.4+.
--
--   MC.ent      entity classes (think/touch/use/damage/killed per classname), create, keyvalues, per-entity data
--   MC.anim     studio models: sequences, frame advance, controllers, body groups, attachments, bones
--   MC.npc      NPC definitions + spawn + AI (hunter / guard / wander / follow / path / custom), melee + ranged attacks
--   MC.round    round events (new/start/end), finish a round with a winner, timer, scores, block the game's own round end
--   MC.teams    custom teams on top of CS teams (relations, friendly fire, scores, balance)
--   MC.objectives  capture / destroy / reach / collect / kills / survive / custom objectives with progress + HUD
--   MC.weapons  custom weapons (WeaponMod-style) on a base CS weapon: models, damage, clip, rate, recoil, sounds, custom fire
--   MC.fx       temp entities (beams, explosions, sprites, sparks, light), MC.hud, MC.spawns, MC.player, MC.precache
-- Every user callback runs protected: an error is logged with its plugin/line and never breaks the framework.
local M = { version = "1.0.0" }
local N = xmod.mc
if not N then
  error("ModCreator: the extension is not loaded. Add 'modcreator' to addons/metamod/plugins.ini AFTER xmod "
     .. "(linux addons/xmod/dlls/modcreator_mm_i386.so, windows addons/xmod/dlls/modcreator_mm.dll) and keep configs/extensions-modcreator.ini", 2)
end
M.native = N
-- xmod loads scripts BEFORE Metamod attaches the extensions, so nothing may call xmod.mc.* while the scripts load.
-- when_ready(fn) runs fn now if the extension is up, else at the first precache/init (the extension is attached by then).
local native_ok, pending = false, {}
local function ready() if not native_ok then native_ok = pcall(function() return N.ham_list() end) end return native_ok end
local function flush_pending() if not ready() then return end local p = pending; pending = {} for _, f in ipairs(p) do local ok, e = pcall(f); if not ok then xmod.error("[modcreator] " .. tostring(e)) end end end
local function when_ready(fn) if ready() then fn() else pending[#pending + 1] = fn end end
M.ready = ready
xmod.on("precache", flush_pending, 1000); xmod.on("init", flush_pending, 1000)
local X, E, P = xmod, xmod.ent, xmod.player
local unpack = table.unpack
local DMG_GENERIC, DMG_BULLET, DMG_SLASH, DMG_BURN, DMG_BLAST, DMG_NEVERGIB = 0, 2, 4, 8, 64, 4096
M.DMG = { GENERIC = 0, CRUSH = 1, BULLET = 2, SLASH = 4, BURN = 8, FREEZE = 16, FALL = 32, BLAST = 64, CLUB = 128, SHOCK = 256,
  SONIC = 512, ENERGYBEAM = 1024, NEVERGIB = 4096, ALWAYSGIB = 8192, DROWN = 16384, PARALYZE = 32768, NERVEGAS = 65536,
  POISON = 131072, RADIATION = 262144, ACID = 1048576, SLOWBURN = 2097152, HEADSHOT = 1 << 30, GRENADE = 1 << 24 }
M.MOVETYPE = { NONE = 0, WALK = 3, STEP = 4, FLY = 5, TOSS = 6, PUSH = 7, NOCLIP = 8, FLYMISSILE = 9, BOUNCE = 10, BOUNCEMISSILE = 11, FOLLOW = 12, PUSHSTEP = 13 }
M.SOLID = { NOT = 0, TRIGGER = 1, BBOX = 2, SLIDEBOX = 3, BSP = 4 }
M.FL = { FLY = 1, SWIM = 2, CONVEYOR = 4, CLIENT = 8, INWATER = 16, MONSTER = 32, GODMODE = 64, NOTARGET = 128, ONGROUND = 512,
  FAKECLIENT = 8192, DUCKING = 16384, FROZEN = 4096, KILLME = 1 << 30, DORMANT = 1 << 31 }
M.HIT = { GENERIC = 0, HEAD = 1, CHEST = 2, STOMACH = 3, LEFTARM = 4, RIGHTARM = 5, LEFTLEG = 6, RIGHTLEG = 7, SHIELD = 8 }
M.USE = { OFF = 0, ON = 1, SET = 2, TOGGLE = 3 }
M.WIN = { NONE = 0, CT = 1, T = 2, DRAW = 3 }

-- ------------------------------------------------------------------------------------------------ helpers
local function warn(fmt, ...) X.log("[modcreator] " .. string.format(fmt, ...)) end
M.warn = warn
local function safe(where, fn, ...)
  if not fn then return end
  local r = table.pack(xpcall(fn, function(e) return tostring(e) end, ...))
  if not r[1] then X.error("[modcreator] error in " .. where .. ": " .. tostring(r[2])); return end
  return unpack(r, 2, r.n)
end
M.safe = safe
local function now() return X.gametime() end
local function dist3(ax, ay, az, bx, by, bz) local dx, dy, dz = ax - bx, ay - by, az - bz; return math.sqrt(dx * dx + dy * dy + dz * dz) end
local function dist2(ax, ay, bx, by) local dx, dy = ax - bx, ay - by; return math.sqrt(dx * dx + dy * dy) end
local function maxp() return X.maxplayers() end
local function is_player(e) return e and e >= 1 and e <= maxp() end
local function valid(e) return e and e > 0 and E.valid(e) end
M.util = { dist3 = dist3, dist2 = dist2, is_player = is_player, valid = valid, safe = safe }
function M.util.origin(e) if is_player(e) then return P.origin(e) end return E.get(e, "origin") end
function M.util.center(e)
  local ax, ay, az = E.get(e, "absmin"); local bx, by, bz = E.get(e, "absmax")
  if ax == 0 and bx == 0 then return E.get(e, "origin") end
  return (ax + bx) / 2, (ay + by) / 2, (az + bz) / 2
end
function M.util.dist(a, b) local ax, ay, az = M.util.origin(a); local bx, by, bz = M.util.origin(b); return dist3(ax, ay, az, bx, by, bz) end
function M.util.aim(id, dist)   -- eye position, aim end point, direction of a player
  local ox, oy, oz = P.origin(id); local vx, vy, vz = E.get(id, "view_ofs"); local p, y, r = E.get(id, "v_angle")
  local v = N.angle_vectors(p, y, r); local sx, sy, sz = ox + vx, oy + vy, oz + vz; dist = dist or 8192
  return sx, sy, sz, sx + v[1] * dist, sy + v[2] * dist, sz + v[3] * dist, v[1], v[2], v[3]
end
function M.util.alive_players() local t = {} for _, id in ipairs(P.all()) do if P.alive(id) then t[#t + 1] = id end end return t end
function M.util.shuffle(t) for i = #t, 2, -1 do local j = math.random(i); t[i], t[j] = t[j], t[i] end return t end

-- ------------------------------------------------------------------------------------------------ precache
-- Register files at plugin load (or call inside the precache event). ModCreator precaches them every map and remembers
-- what was precached, so MC.ent.create never sets a model the engine does not know (that would crash the server).
M.precache = {}
local want = { model = {}, sound = {}, generic = {} }
local cached = {}            -- path -> engine index (this map)
local precache_open = false
local function do_precache(kind, path)
  if cached[path] then return cached[path] end
  local f = X.precache[kind]; local ok, idx = pcall(f, path)
  if ok and idx and idx > 0 then cached[path] = idx; return idx end
  warn("precache %s '%s' failed (file missing?)", kind, path); return 0
end
function M.precache.model(path) want.model[path] = true; if precache_open then return do_precache("model", path) end return cached[path] or 0 end
function M.precache.sound(path) want.sound[path] = true; if precache_open then return do_precache("sound", path) end return cached[path] or 0 end
function M.precache.generic(path) want.generic[path] = true; if precache_open then return do_precache("generic", path) end return cached[path] or 0 end
function M.precache.index(path) return cached[path] or 0 end
function M.precache.ok(path) return cached[path] ~= nil end
function M.precache.mark(path, index) cached[path] = index or 1 end      -- "the game precaches this one itself"
for _, m in ipairs({ "models/player.mdl", "models/w_ak47.mdl", "models/v_knife.mdl", "models/p_knife.mdl", "sprites/laserbeam.spr" }) do M.precache.mark(m) end
X.on("precache", function()
  precache_open = true
  for _, k in ipairs({ "model", "sound", "generic" }) do for path in pairs(want[k]) do do_precache(k, path) end end
  for path, _ in pairs({ ["sprites/laserbeam.spr"] = 1, ["sprites/zerogxplode.spr"] = 1, ["sprites/steam1.spr"] = 1, ["sprites/shockwave.spr"] = 1 }) do
    local ok, idx = pcall(X.precache.model, path); if ok and idx and idx > 0 then cached[path] = idx end
  end
  precache_open = false
end, 100)
X.on("map_end", function() cached = {}; for _, m in ipairs({ "models/player.mdl", "models/w_ak47.mdl", "models/v_knife.mdl", "models/p_knife.mdl" }) do cached[m] = 1 end end)
local function model_ok(path, unsafe)
  if not path or path == "" then return true end
  if cached[path] or unsafe then return true end
  if path:sub(1, 1) == "*" then return true end            -- brush model of the map
  return false
end

-- ------------------------------------------------------------------------------------------------ MC.ent
M.ent = {}
local edata = {}               -- ent -> per-entity Lua table
local classes = {}             -- classname -> def
local hooked_base = {}         -- base classname -> true (ham hooks installed)
local function cls_of(e) return E.classname(e) end
function M.ent.data(e) local d = edata[e]; if not d then d = {}; edata[e] = d end return d end
function M.ent.clear_data(e) edata[e] = nil end
X.on("map_end", function() edata = {} end)

local function vt3(v, a, b, c) if type(v) == "table" then return v[1] or v.x or 0, v[2] or v.y or 0, v[3] or v.z or 0 end return a or 0, b or 0, c or 0 end
-- MC.ent.create{ classname="my_box", base="info_target", model="models/x.mdl", origin={x,y,z}, angles={p,y,r},
--   mins={..}, maxs={..}, solid=2, movetype=0, health=100, takedamage=true, gravity, friction, scale, skin, body,
--   render={mode, amt, fx, r,g,b}, kv={key=value} (before spawn), owner, nextthink (seconds from now), spawn=true, drop=false, unsafe_model=false }
function M.ent.create(def)
  def = def or {}
  local base = def.base or "info_target"
  local e = N.create(base)
  if e == 0 then error("MC.ent.create: the game could not create '" .. base .. "'", 2) end
  if def.kv then for k, v in pairs(def.kv) do N.keyvalue(e, k, tostring(v)) end end
  if def.origin then E.set_origin(e, vt3(def.origin)) end
  if def.spawn ~= false then E.spawn(e) end
  if def.classname then E.set(e, "classname", def.classname) end
  if def.model then
    if not model_ok(def.model, def.unsafe_model) then E.remove(e); error("MC.ent.create: model '" .. def.model .. "' is not precached (MC.precache.model it at plugin load)", 2) end
    E.set_model(e, def.model)
  end
  if def.mins and def.maxs then local a, b, c = vt3(def.mins); local x, y, z = vt3(def.maxs); E.set_size(e, a, b, c, x, y, z) end
  if def.origin then E.set_origin(e, vt3(def.origin)) end
  if def.angles then E.set(e, "angles", vt3(def.angles)) end
  if def.solid then E.set(e, "solid", def.solid) end
  if def.movetype then E.set(e, "movetype", def.movetype) end
  if def.health then E.set(e, "health", def.health); E.set(e, "max_health", def.max_health or def.health) end
  if def.takedamage ~= nil then E.set(e, "takedamage", (def.takedamage == true and 2) or (def.takedamage == false and 0) or def.takedamage) end
  for _, f in ipairs({ "gravity", "friction", "scale", "skin", "body", "sequence", "framerate", "effects", "flags", "spawnflags", "team", "impulse" }) do
    if def[f] then E.set(e, f, def[f]) end
  end
  if def.render then local r = def.render
    E.set(e, "rendermode", r.mode or r[1] or 0); E.set(e, "renderamt", r.amt or r[2] or 255); E.set(e, "renderfx", r.fx or r[3] or 0)
    E.set(e, "rendercolor", r.r or r[4] or 255, r.g or r[5] or 255, r.b or r[6] or 255)
  end
  if def.owner then E.set(e, "owner", def.owner) end
  if def.velocity then E.set(e, "velocity", vt3(def.velocity)) end
  if def.drop then N.drop_to_floor(e) end
  local c = def.classname and classes[def.classname]
  if c and c.think_interval and c.think_interval > 0 then E.set(e, "nextthink", now() + (def.nextthink or c.think_interval))
  elseif def.nextthink then E.set(e, "nextthink", now() + def.nextthink) end
  if c then safe(def.classname .. ".on_spawn", c.on_spawn, e, def) end
  return e
end
function M.ent.remove(e) if valid(e) and not is_player(e) then edata[e] = nil; E.remove(e) end end
function M.ent.keyvalue(e, k, v) return N.keyvalue(e, k, tostring(v)) end
function M.ent.use(e, activator, caller, use_type, value) return N.ham("use", e, activator or 0, caller or activator or 0, use_type or 1, value or 0) end
function M.ent.touch(e, other) return N.ham("touch", e, other) end
function M.ent.think(e) return N.ham("think", e) end
function M.ent.damage(victim, inflictor, attacker, dmg, bits, hitgroup) return N.damage(victim, inflictor or 0, attacker or 0, dmg, bits or 0, hitgroup) end
function M.ent.kill(e, attacker, gib) return N.ham("killed", e, attacker or 0, gib or 0) end
function M.ent.heal(e, amount, bits) return N.ham("takehealth", e, amount, bits or 0) end
function M.ent.find_all(classname) local t, e = {}, 0 repeat e = E.find(e, classname); if e ~= 0 then t[#t + 1] = e end until e == 0 return t end
function M.ent.in_sphere(x, y, z, r, classname)
  local out = {} for _, e in ipairs(E.find_sphere(x, y, z, r)) do if not classname or E.classname(e) == classname then out[#out + 1] = e end end return out
end
function M.ent.set_think(e, delay) E.set(e, "nextthink", now() + (delay or 0.01)) end
function M.ent.glow(e, r, g, b, amount) E.set(e, "renderfx", 19); E.set(e, "rendercolor", r, g, b); E.set(e, "rendermode", 0); E.set(e, "renderamt", amount or 16) end
function M.ent.unglow(e) E.set(e, "renderfx", 0); E.set(e, "rendermode", 0); E.set(e, "renderamt", 255) end
function M.ent.visible(e, target) return N.visible(e, target) end
function M.ent.face(e, x, y, z, instant) return N.face(e, x, y, z, instant) end

-- ham dispatch for custom classes (every custom class lives on a base class vtable, usually info_target)
local function dispatch_base(base)
  if hooked_base[base] then return end
  hooked_base[base] = true
  X.on("ham:use:" .. base, function(e, act, caller, ut, val)
    local c = classes[cls_of(e)]; if c and c.on_use then if safe(c.name .. ".on_use", c.on_use, e, act, caller, ut, val) == true then return X.HC_SUPERCEDE end end
  end)
  X.on("ham:takedamage:" .. base, function(e, inf, att, dmg, bits)
    local c = classes[cls_of(e)]; if not c or not c.on_damage then return end
    local r = safe(c.name .. ".on_damage", c.on_damage, e, inf, att, dmg, bits)
    if r == false then return X.HC_SUPERCEDE, 0 elseif type(r) == "number" and r ~= dmg then X.hook.set_arg(3, r) end
  end)
  X.on("ham:killed:" .. base, function(e, att, gib)
    local c = classes[cls_of(e)]; if not c or not c.on_killed then return end
    if safe(c.name .. ".on_killed", c.on_killed, e, att, gib) == true then return X.HC_SUPERCEDE end
    edata[e] = nil
  end)
end
-- MC.ent.class(name, { base="info_target", think_interval=0.1, on_spawn(e,def), on_think(e) [return seconds = next think, false = stop],
--   on_touch(e, other) [true = block], on_use(e, activator, caller, type, value) [true = block], on_damage(e, inflictor, attacker, dmg, bits)
--   [number = new damage, false = no damage], on_killed(e, attacker, gib) [true = keep the entity alive/not removed] })
function M.ent.class(name, def)
  def.name = name; def.base = def.base or "info_target"; classes[name] = def
  if not def._hooked then
    def._hooked = true
    X.on("think:" .. name, function(e)
      local c = classes[name]; if not c then return end
      local r = safe(name .. ".on_think", c.on_think, e)
      if not valid(e) or E.classname(e) ~= name then return end
      if r == false then return end
      local d = type(r) == "number" and r or c.think_interval
      if d and d > 0 then E.set(e, "nextthink", now() + d) end
    end)
    X.on("touch:" .. name, function(e, other)
      local c = classes[name]; if c and c.on_touch then return safe(name .. ".on_touch", c.on_touch, e, other) == true end
    end)
  end
  if def.on_use or def.on_damage or def.on_killed then dispatch_base(def.base) end
  return def
end
function M.ent.class_of(e) return classes[cls_of(e)] end

-- ------------------------------------------------------------------------------------------------ MC.anim (studio models)
M.anim = {}
function M.anim.play(e, seq, rate, restart) return N.anim(e, seq, rate or 1.0, restart ~= false) end
function M.anim.advance(e, interval) return N.frame_advance(e, interval) end
function M.anim.lookup(e, name) return N.seq_lookup(e, name) end
function M.anim.list(e) return N.seq_list(e) end
function M.anim.info(e, seq) return N.seq_info(e, seq) end
function M.anim.duration(e, seq) return N.seq_duration(e, seq) end
function M.anim.by_activity(e, act) return N.seq_by_activity(e, act) end
function M.anim.controller(e, ctl, deg) return N.set_controller(e, ctl, deg) end
function M.anim.blending(e, b, v) return N.set_blending(e, b, v) end
function M.anim.body(e, group, value) if value == nil then return N.get_body(e, group) end return N.set_body(e, group, value) end
function M.anim.attachment(e, n) local v = N.attachment(e, n); if v then return v[1], v[2], v[3] end end
function M.anim.bone(e, b) local v = N.bone(e, b); if v then return v[1], v[2], v[3] end end
function M.anim.bones(e) return N.bone_list(e) end
function M.anim.model(e) return N.model_info(e) end
function M.anim.first(e, names)       -- first existing sequence of a list of names (or index)
  if type(names) ~= "table" then names = { names } end
  for _, n in ipairs(names) do local i = type(n) == "number" and n or N.seq_lookup(e, n); if i and i >= 0 then return i end end
  return -1
end
function M.anim.pick(e, names)        -- random existing sequence of a list
  if type(names) ~= "table" then return M.anim.first(e, names) end
  local ok = {} for _, n in ipairs(names) do local i = type(n) == "number" and n or N.seq_lookup(e, n); if i and i >= 0 then ok[#ok + 1] = i end end
  if #ok == 0 then return -1 end return ok[math.random(#ok)]
end

-- ------------------------------------------------------------------------------------------------ MC.npc
-- MC.npc.register(name, def) - def fields (all optional except model):
--   model, health=100, speed=200 (units/s), mins={-16,-16,0}, maxs={16,16,72}, eye=64, turn=25 (deg per think), think=0.1, gravity=1, scale,
--   team="monsters" (a MC.teams key, or "t"/"ct"), ai="hunter"|"guard"|"wander"|"follow"|"path"|"idle"|function(npc) end,
--   sight=1500, fov=140, memory=6 (s to keep chasing an enemy it cannot see), leash=400 (guard/wander), aggressive=true,
--   movement="step"|"fly", jump=260 (unstick jump speed, 0 = off), blood=247 (-1 none), corpse=5 (s, -1 keep, 0 remove at once),
--   attack={ damage=10, range=60, rate=1.2, delay=0.35 (hit moment after the anim starts), bits=DMG_SLASH, ranged=false,
--            spread=0.05, shots=1, attachment=nil (muzzle), knockback=0, sound=nil|{...} },
--   anims={ idle={"idle","idle1"}, walk={"walk"}, run={"run"}, attack={"attack1","attack"}, pain={"flinch","pain"}, death={"die","die1"} },
--   sounds={ idle={...}, alert={...}, attack={...}, pain={...}, death={...}, step=nil }  (precached automatically),
--   reward={ frags=1, money=300 }, classname="npc_<name>", render={...}, skin, body,
--   on_spawn(npc), on_think(npc) [true = skip the built-in AI this tick], on_damage(npc, attacker, dmg, bits, inflictor) [number = new dmg, false = none],
--   on_death(npc, attacker), on_attack(npc, target) [false = cancel], on_enemy(npc, enemy), relation(npc, other) ["enemy"|"ally"|"neutral"]
-- npc object (MC.npc.get(ent)): fields ent name def team enemy state data(table for you) home{x,y,z}
--   methods: npc:origin() npc:anim(names [,rate]) npc:move_to(x,y,z) npc:path(points [,loop]) npc:follow(ent) npc:stop()
--            npc:set_enemy(e) npc:attack() npc:sound(kind) npc:hurt(dmg, attacker) npc:kill(attacker) npc:remove() npc:alive() npc:health()
M.npc = {}
local npc_defs, npcs = {}, {}           -- name -> def ; ent -> npc
local NPC = {}; NPC.__index = NPC
local ACT = { idle = 1, walk = 3, run = 4, attack = 30, rattack = 28, pain = 26, death = 36, fly = 5 }
local function npc_of(e) return npcs[e] end
M.npc.get = npc_of
function NPC:origin() return E.get(self.ent, "origin") end
function NPC:alive() return not self.dead and valid(self.ent) end
function NPC:health() return E.get(self.ent, "health") end
function NPC:sound(kind)
  local s = self.def.sounds and self.def.sounds[kind]; if not s then return end
  if type(s) == "table" then s = s[math.random(#s)] end
  X.sound.emit(self.ent, kind == "step" and X.sound.CHAN_AUTO or X.sound.CHAN_VOICE, s, 1.0, 0.8, 0, 95 + math.random(0, 10))
end
function NPC:anim(names, rate, restart)
  local seq
  if type(names) == "number" then seq = names
  elseif type(names) == "string" and (self.seq[names] or self.def.anims[names]) then seq = self.seq[names] or M.anim.pick(self.ent, self.def.anims[names])
  else seq = M.anim.pick(self.ent, names) end
  if not seq or seq < 0 then return -1 end
  if seq == self.cur_seq and not restart then return seq end
  self.cur_seq = seq; N.anim(self.ent, seq, rate or 1.0, true); self.anim_done = false
  return seq
end
function NPC:move_to(x, y, z) self.goal = { x, y, z }; self.path_pts = nil end
function NPC:path(points, loop) self.path_pts = points; self.path_i = 1; self.path_loop = loop; self.goal = nil end
function NPC:follow(e) self.leader = e end
function NPC:stop() self.goal = nil; self.path_pts = nil; self.leader = nil end
function NPC:set_enemy(e)
  if e == self.enemy then return end
  self.enemy = e; self.seen_at = now()
  if e and e ~= 0 then self:sound("alert"); safe(self.name .. ".on_enemy", self.def.on_enemy, self, e) end
end
function NPC:hurt(dmg, attacker) return N.damage(self.ent, attacker or 0, attacker or 0, dmg, 0) end
function NPC:kill(attacker) if self:alive() then N.ham("killed", self.ent, attacker or 0, 0) end end
function NPC:remove() npcs[self.ent] = nil; M.ent.remove(self.ent) end

local function team_of_ent(e)
  if is_player(e) then return M.teams.of(e) end
  local n = npcs[e]; if n then return n.team end
  return nil
end
M.team_of_ent = team_of_ent
local function relation(n, other)
  if n.def.relation then local r = safe(n.name .. ".relation", n.def.relation, n, other); if r then return r end end
  local ot = team_of_ent(other); if not ot then return "neutral" end
  return M.teams.relation(n.team, ot)
end
M.npc.relation = relation
local function target_ok(n, t)
  if not valid(t) or t == n.ent then return false end
  if is_player(t) then
    if not P.alive(t) then return false end
    local tm = P.team(t); if tm ~= 1 and tm ~= 2 then return false end
    if E.get(t, "flags") & M.FL.NOTARGET ~= 0 then return false end
  else
    local o = npcs[t]; if not o or o.dead then return false end
  end
  return true
end
local function find_enemy(n)
  local e = n.ent; local ex, ey, ez = E.get(e, "origin"); local best, bd = nil, n.def.sight
  local function consider(t)
    if not target_ok(n, t) or relation(n, t) ~= "enemy" then return end
    local tx, ty, tz = M.util.origin(t); local d = dist3(ex, ey, ez, tx, ty, tz)
    if d >= bd then return end
    if d > 220 and not N.in_view_cone(e, tx, ty, tz, n.def.fov) then return end
    if not N.visible(e, t) then return end
    best, bd = t, d
  end
  for _, id in ipairs(P.all()) do consider(id) end
  for t, o in pairs(npcs) do if o ~= n then consider(t) end end
  return best, bd
end
local function step_to(n, gx, gy, gz, dt, run)
  local e = n.ent; local spd = n.def.speed * (run and 1 or 0.5)
  if n.def.movement == "fly" then
    local x, y, z = E.get(e, "origin"); local dx, dy, dz = gx - x, gy - y, gz - z; local l = math.sqrt(dx * dx + dy * dy + dz * dz)
    if l < 1 then E.set(e, "velocity", 0, 0, 0) return end
    E.set(e, "velocity", dx / l * spd, dy / l * spd, dz / l * spd); N.face(e, gx, gy, gz, true)
  else
    N.move_to_origin(e, gx, gy, gz, spd * dt, 0)
  end
  n:anim(run and "run" or "walk")
end
local function unstick(n, dt)
  local e = n.ent; local x, y, z = E.get(e, "origin"); local t = now()
  if not n.lp then n.lp = { x, y, z, t } return end
  if dist2(x, y, n.lp[1], n.lp[2]) > 8 then n.lp = { x, y, z, t } return end
  if t - n.lp[4] < 0.8 then return end
  n.lp = { x, y, z, t }
  local yaw = select(2, E.get(e, "angles"))
  if n.def.jump and n.def.jump > 0 and (E.get(e, "flags") & M.FL.ONGROUND) ~= 0 then
    local fx, fy = math.cos(math.rad(yaw)), math.sin(math.rad(yaw))
    E.set(e, "flags", E.get(e, "flags") & ~M.FL.ONGROUND); E.set(e, "velocity", fx * 150, fy * 150, n.def.jump)
  else
    N.walk_move(e, yaw + (math.random(2) == 1 and 90 or -90), n.def.speed * dt * 3, 0)
  end
end
local function do_hit(n)
  local a = n.def.attack; local t = n.enemy; if not target_ok(n, t) then return end
  local e = n.ent
  if a.ranged then
    local sx, sy, sz
    if a.attachment then sx, sy, sz = M.anim.attachment(e, a.attachment) end
    if not sx then local x, y, z = E.get(e, "origin"); local _, _, vz = E.get(e, "view_ofs"); sx, sy, sz = x, y, z + vz end
    local tx, ty, tz = M.util.center(t)
    N.fire_bullets(e, e, sx, sy, sz, tx - sx, ty - sy, tz - sz, a.spread or 0.05, n.def.sight + 200, a.damage, a.shots or 1, a.bits or DMG_BULLET)
  else
    if M.util.dist(e, t) > a.range * 1.3 then return end
    N.damage(t, e, e, a.damage, a.bits or DMG_SLASH)
    if a.knockback and a.knockback > 0 and is_player(t) then
      local x, y = E.get(e, "origin"); local tx, ty = P.origin(t); local dx, dy = tx - x, ty - y; local l = math.max(1, math.sqrt(dx * dx + dy * dy))
      local vx, vy, vz = P.velocity(t); P.set_velocity(t, vx + dx / l * a.knockback, vy + dy / l * a.knockback, vz + a.knockback * 0.3)
    end
  end
  if a.sound then local s = type(a.sound) == "table" and a.sound[math.random(#a.sound)] or a.sound; X.sound.emit(e, X.sound.CHAN_WEAPON, s, 1.0, 0.8, 0, 100) end
end
local function start_attack(n)
  if safe(n.name .. ".on_attack", n.def.on_attack, n, n.enemy) == false then return false end
  local a = n.def.attack; local t = now()
  local ex, ey, ez = M.util.origin(n.enemy); N.face(n.ent, ex, ey, ez, true)
  n.state = "attack"; n.hit_at = t + (a.delay or 0.35); n.next_attack = t + (a.rate or 1.2)
  n:anim(a.ranged and "rattack" or "attack", 1.0, true); n:sound("attack")
  n.attack_end = t + math.max((a.delay or 0.35) + 0.15, N.seq_duration(n.ent, n.cur_seq) or 0.5)
  if not a.ranged and n.def.movement ~= "fly" then E.set(n.ent, "velocity", 0, 0, 0) end
  return true
end
local function ai_tick(n, dt)
  local e, d, t = n.ent, n.def, now()
  local ex, ey, ez = E.get(e, "origin")
  -- attack in progress
  if n.state == "attack" then
    if n.hit_at and t >= n.hit_at then n.hit_at = nil; do_hit(n) end
    if t < n.attack_end then return end
    n.state = "idle"
  end
  if n.pain_until and t < n.pain_until then return end
  -- enemy search (2x / s), memory of the last seen position
  if t >= (n.next_search or 0) then
    n.next_search = t + 0.5
    if d.aggressive ~= false or n.enemy then
      local best = find_enemy(n)
      if best then n:set_enemy(best); n.seen_at = t; n.last_x, n.last_y, n.last_z = M.util.origin(best)
      elseif n.enemy and (not target_ok(n, n.enemy) or t - (n.seen_at or 0) > d.memory) then n:set_enemy(nil) end
    end
  end
  local mode = n.ai
  if n.enemy and target_ok(n, n.enemy) then
    local tx, ty, tz = M.util.origin(n.enemy); local dd = dist3(ex, ey, ez, tx, ty, tz)
    if N.visible(e, n.enemy) then n.seen_at = t; n.last_x, n.last_y, n.last_z = tx, ty, tz end
    local leash_ok = true
    if (mode == "guard" or mode == "wander") and n.home then leash_ok = dist2(tx, ty, n.home[1], n.home[2]) <= d.leash end
    if dd <= d.attack.range and t >= (n.next_attack or 0) and (not d.attack.ranged or N.visible(e, n.enemy)) then
      if start_attack(n) then return end
    end
    if d.attack.ranged and dd <= d.attack.range and N.visible(e, n.enemy) then
      N.face(e, tx, ty, tz, false); N.change_yaw(e); n:anim("idle"); return
    end
    if leash_ok and mode ~= "idle" then step_to(n, n.last_x or tx, n.last_y or ty, n.last_z or tz, dt, true); unstick(n, dt); return end
  elseif n.enemy then n:set_enemy(nil) end
  -- no enemy: the movement mode
  if mode == "follow" and valid(n.leader) then
    local lx, ly, lz = M.util.origin(n.leader); if dist3(ex, ey, ez, lx, ly, lz) > (d.follow_dist or 110) then step_to(n, lx, ly, lz, dt, dist3(ex, ey, ez, lx, ly, lz) > 300); unstick(n, dt); return end
  elseif n.path_pts and #n.path_pts > 0 then
    local p = n.path_pts[n.path_i]; local px, py, pz = vt3(p)
    if dist2(ex, ey, px, py) < 24 then n.path_i = n.path_i + 1
      if n.path_i > #n.path_pts then if n.path_loop then n.path_i = 1 else n.path_pts = nil; X.emit("mc:npc_path_done", n.ent, n.name) end end
    else step_to(n, px, py, pz, dt, d.path_run); unstick(n, dt); return end
  elseif n.goal then
    local gx, gy, gz = vt3(n.goal)
    if dist2(ex, ey, gx, gy) < 24 then n.goal = nil; X.emit("mc:npc_goal_reached", n.ent, n.name) else step_to(n, gx, gy, gz, dt, d.goal_run); unstick(n, dt); return end
  elseif mode == "wander" or (mode == "guard" and n.home and dist2(ex, ey, n.home[1], n.home[2]) > 48) then
    if mode == "guard" then n.goal = { n.home[1], n.home[2], n.home[3] }
    elseif t >= (n.next_wander or 0) then
      n.next_wander = t + math.random(3, 7)
      local a = math.random() * math.pi * 2; local r = math.random(80, d.leash)
      n.goal = { n.home[1] + math.cos(a) * r, n.home[2] + math.sin(a) * r, n.home[3] }
    end
  end
  if d.movement == "fly" then E.set(e, "velocity", 0, 0, 0) end
  n:anim("idle")
  if t >= (n.next_idle_sound or 0) then n.next_idle_sound = t + math.random(6, 14); if math.random(3) == 1 then n:sound("idle") end end
end
local function npc_think(e)
  local n = npcs[e]; if not n then return false end
  local d = n.def; local dt = d.think
  local finished = N.frame_advance(e)
  if finished then n.anim_done = true end
  if n.dead then
    if d.corpse >= 0 and now() >= n.remove_at then n:remove() return false end
    return dt
  end
  if safe(n.name .. ".on_think", d.on_think, n) == true then return dt end
  if type(n.ai) == "function" then safe(n.name .. ".ai", n.ai, n, dt) else ai_tick(n, dt) end
  return dt
end
local function npc_damage(e, inf, att, dmg, bits)
  local n = npcs[e]; if not n or n.dead then return false end
  if att and att ~= e and valid(att) and relation(n, att) == "ally" and not M.teams.friendly_fire_on() then return false end
  local r = safe(n.name .. ".on_damage", n.def.on_damage, n, att, dmg, bits, inf)
  if r == false then return false end
  if type(r) == "number" then dmg = r end
  if att and att ~= e and target_ok(n, att) and relation(n, att) == "enemy" and (not n.enemy or math.random(3) == 1) then n:set_enemy(att); n.next_search = now() + 1 end
  if n.state ~= "attack" and math.random() < (n.def.pain_chance or 0.35) and now() >= (n.next_pain or 0) then
    n.next_pain = now() + 1.5; n:anim("pain", 1.0, true); n.pain_until = now() + math.min(0.6, N.seq_duration(e, n.cur_seq) or 0.4); n:sound("pain")
  end
  return dmg
end
local function npc_killed(e, att)
  local n = npcs[e]; if not n then return end
  if n.dead then return true end
  n.dead = true; n.state = "dead"; n.enemy = nil
  E.set(e, "takedamage", 0); E.set(e, "deadflag", 2); E.set(e, "solid", 0); E.set(e, "velocity", 0, 0, 0)
  n:anim("death", 1.0, true); n:sound("death")
  local corpse = n.def.corpse; n.remove_at = now() + math.max(0, corpse)
  local rw = n.def.reward
  if rw and is_player(att) and P.ingame(att) then
    if rw.frags then P.set_frags(att, P.frags(att) + rw.frags) end
    if rw.money then P.set_money(att, math.min(16000, P.money(att) + rw.money)) end
  end
  safe(n.name .. ".on_death", n.def.on_death, n, att)
  X.emit("mc:npc_killed", e, n.name, att or 0)
  if corpse == 0 then X.timer.after(0, function() if npcs[e] == n then n:remove() end end) end
  return true
end
local DEF = { health = 100, speed = 200, eye = 64, turn = 25, think = 0.1, gravity = 1.0, team = "monsters", ai = "hunter", sight = 1500, fov = 140,
  memory = 6, leash = 400, movement = "step", jump = 260, blood = 247, corpse = 5 }
local ADEF = { damage = 10, range = 60, rate = 1.2, delay = 0.35, bits = DMG_SLASH }
function M.npc.register(name, def)
  assert(type(def) == "table" and def.model, "MC.npc.register(name, {model=...})")
  for k, v in pairs(DEF) do if def[k] == nil then def[k] = v end end
  def.attack = def.attack or {}; for k, v in pairs(ADEF) do if def.attack[k] == nil then def.attack[k] = v end end
  if def.attack.ranged and def.attack.range == 60 then def.attack.range = 800 end
  def.anims = def.anims or {}; def.name = name; def.classname = def.classname or ("npc_" .. name)
  M.precache.model(def.model)
  for _, list in pairs(def.sounds or {}) do for _, s in ipairs(type(list) == "table" and list or { list }) do M.precache.sound(s) end end
  if def.attack.sound then for _, s in ipairs(type(def.attack.sound) == "table" and def.attack.sound or { def.attack.sound }) do M.precache.sound(s) end end
  npc_defs[name] = def
  M.ent.class(def.classname, { think_interval = def.think, on_think = npc_think, on_damage = npc_damage, on_killed = npc_killed })
  return def
end
function M.npc.def(name) return npc_defs[name] end
local function resolve_seqs(n)
  local e, a = n.ent, n.def.anims
  local function pick(kind, defaults, act)
    local s = M.anim.first(e, a[kind] or defaults); if s < 0 and act then s = N.seq_by_activity(e, act) or -1 end
    return s
  end
  n.seq = {
    idle = pick("idle", { "idle", "idle1", "idle_1", "stand" }, ACT.idle),
    walk = pick("walk", { "walk", "walk1" }, ACT.walk),
    run = pick("run", { "run", "run1" }, ACT.run),
    attack = pick("attack", { "attack1", "attack", "attack2" }, ACT.attack),
    rattack = pick("rattack", { "shoot", "shootgun", "attack1" }, ACT.rattack),
    pain = pick("pain", { "flinch", "pain", "flinchsmall", "smflinch" }, ACT.pain),
    death = pick("death", { "die", "die1", "dieforward", "diesimple", "death1" }, ACT.death),
  }
  if n.seq.run < 0 then n.seq.run = n.seq.walk end
  if n.seq.walk < 0 then n.seq.walk = n.seq.run end
  if n.seq.rattack < 0 then n.seq.rattack = n.seq.attack end
  if n.seq.pain < 0 then n.seq.pain = n.seq.idle end
  for k, v in pairs(n.seq) do if v < 0 then n.seq[k] = 0 end end
  -- lists: random death / attack / pain animations
  if type(a.death) == "table" and #a.death > 1 then n.seq.death = nil end
  if type(a.attack) == "table" and #a.attack > 1 then n.seq.attack = nil end
  if type(a.pain) == "table" and #a.pain > 1 then n.seq.pain = nil end
end
-- MC.npc.spawn(name, x, y, z [, yaw=0 [, overrides]]) -> npc object (nil if the entity could not be created)
function M.npc.spawn(name, x, y, z, yaw, over)
  local base = npc_defs[name]; if not base then error("MC.npc.spawn: unknown npc '" .. tostring(name) .. "'", 2) end
  local d = base
  if over then d = setmetatable({}, { __index = base }); for k, v in pairs(over) do d[k] = v end; if over.attack then setmetatable(d.attack, { __index = base.attack }) end end
  local mins, maxs = d.mins or { -16, -16, 0 }, d.maxs or { 16, 16, 72 }
  local e = M.ent.create { classname = d.classname, model = d.model, origin = { x, y, z }, angles = { 0, yaw or 0, 0 }, mins = mins, maxs = maxs,
    solid = d.movement == "fly" and M.SOLID.BBOX or M.SOLID.SLIDEBOX, movetype = d.movement == "fly" and M.MOVETYPE.FLY or M.MOVETYPE.STEP,
    health = d.health, takedamage = 2, gravity = d.gravity, scale = d.scale, skin = d.skin, body = d.body, render = d.render, nextthink = 0.1 + math.random() * 0.1 }
  E.set(e, "flags", E.get(e, "flags") | M.FL.MONSTER); E.set(e, "view_ofs", 0, 0, d.eye); E.set(e, "deadflag", 0)
  N.ev(e, "yaw_speed", d.turn); N.ev(e, "ideal_yaw", yaw or 0)
  if d.movement ~= "fly" then N.drop_to_floor(e) end
  local n = setmetatable({ ent = e, name = name, def = d, team = d.team, ai = d.ai, state = "idle", data = {}, cur_seq = -1 }, NPC)
  n.home = { E.get(e, "origin") }
  npcs[e] = n
  resolve_seqs(n); n:anim("idle")
  safe(name .. ".on_spawn", d.on_spawn, n)
  X.emit("mc:npc_spawned", e, name)
  return n
end
function M.npc.all(name) local t = {} for _, n in pairs(npcs) do if not n.dead and (not name or n.name == name) then t[#t + 1] = n end end return t end
function M.npc.count(name) return #M.npc.all(name) end
function M.npc.remove_all(name) for e, n in pairs(npcs) do if not name or n.name == name then n:remove() end end end
X.on("map_end", function() npcs = {} end)

-- ------------------------------------------------------------------------------------------------ MC.teams
-- Custom teams on top of the two CS teams. A team: { key, name, cs = 1 (T) | 2 (CT) | nil, color = {r,g,b} }.
-- Without MC.teams.define the keys are "t" and "ct" (and "spec"). NPCs have a team key too (default "monsters").
M.teams = {}
local T = { defs = {}, order = {}, of = {}, score = {}, rel = {}, ff = nil, listeners = {}, ff_hooked = false }
local function tdef(key) return T.defs[key] end
function M.teams.define(list)
  T.defs, T.order = {}, {}
  for _, d in ipairs(list) do assert(d.key, "MC.teams.define: every team needs a key"); d.name = d.name or d.key; T.defs[d.key] = d; T.order[#T.order + 1] = d.key; T.score[d.key] = T.score[d.key] or 0 end
end
function M.teams.list() if #T.order > 0 then return T.order end return { "t", "ct" } end
function M.teams.get(key) return T.defs[key] end
function M.teams.name(key) local d = T.defs[key]; if d then return d.name end return ({ t = "Terrorists", ct = "Counter-Terrorists", spec = "Spectators", monsters = "Monsters" })[key] or tostring(key) end
function M.teams.color(key) local d = T.defs[key]; if d and d.color then return unpack(d.color) end if key == "t" then return 255, 64, 64 elseif key == "ct" then return 64, 128, 255 end return 220, 220, 220 end
function M.teams.of(id)
  if not is_player(id) then return team_of_ent(id) end
  if T.of[id] then return T.of[id] end
  local cs = P.ingame(id) and P.team(id) or 0
  for _, k in ipairs(T.order) do if T.defs[k].cs == cs then return k end end
  return cs == 1 and "t" or cs == 2 and "ct" or cs == 3 and "spec" or nil
end
function M.teams.cs(key) local d = T.defs[key]; if d then return d.cs end return key == "t" and 1 or key == "ct" and 2 or nil end
-- MC.teams.assign(id, key [, {respawn=false, silent=false}])
function M.teams.assign(id, key, opts)
  opts = opts or {}; local old = M.teams.of(id)
  T.of[id] = key
  local cs = M.teams.cs(key)
  if cs and P.ingame(id) and P.team(id) ~= cs then P.set_team(id, cs) end
  if opts.respawn then M.player.respawn(id) end
  if old ~= key and not opts.silent then
    for _, f in ipairs(T.listeners) do safe("teams.on_change", f, id, old, key) end
    X.emit("mc:team_change", id, old or "", key)
  end
end
function M.teams.on_change(fn) T.listeners[#T.listeners + 1] = fn end
function M.teams.players(key, alive_only)
  local t = {} for _, id in ipairs(P.all()) do if M.teams.of(id) == key and (not alive_only or P.alive(id)) then t[#t + 1] = id end end return t
end
function M.teams.count(key, alive_only) return #M.teams.players(key, alive_only) end
function M.teams.alive(key) return #M.teams.players(key, true) end
function M.teams.relation(a, b)
  if a == nil or b == nil then return "neutral" end
  local r = T.rel[a .. ">" .. b]; if r then return r end
  if a == b then return "ally" end
  if a == "spec" or b == "spec" then return "neutral" end
  return "enemy"
end
function M.teams.set_relation(a, b, rel, one_way) T.rel[a .. ">" .. b] = rel; if not one_way then T.rel[b .. ">" .. a] = rel end end
function M.teams.score(key) return T.score[key] or 0 end
function M.teams.add_score(key, n) T.score[key] = (T.score[key] or 0) + (n or 1); X.emit("mc:team_score", key, T.score[key]); return T.score[key] end
function M.teams.set_score(key, n) T.score[key] = n end
function M.teams.reset_scores() for k in pairs(T.score) do T.score[k] = 0 end end
function M.teams.friendly_fire_on() if T.ff ~= nil then return T.ff end local ok, v = pcall(X.cvar.int, "mp_friendlyfire"); return ok and v == 1 end
-- MC.teams.friendly_fire(false): players / NPCs of allied teams cannot hurt each other (custom teams too); true: they can; nil: mp_friendlyfire decides
function M.teams.friendly_fire(on)
  T.ff = on
  if T.ff_hooked then return end
  T.ff_hooked = true
  X.on("ham:takedamage:player", function(v, inf, att, dmg, bits)
    if T.ff ~= false or not valid(att) or att == v then return end
    local at = team_of_ent(att); if not at and not is_player(att) then return end
    if M.teams.relation(at, M.teams.of(v)) == "ally" then return X.HC_SUPERCEDE, 0 end
  end, 50)
end
-- MC.teams.balance([keys]) - moves players from the biggest to the smallest team (alive players last). Returns moved count.
function M.teams.balance(keys)
  keys = keys or M.teams.list(); local moved = 0
  for _ = 1, 32 do
    local big, small
    for _, k in ipairs(keys) do local c = M.teams.count(k); if not big or c > M.teams.count(big) then big = k end; if not small or c < M.teams.count(small) then small = k end end
    if not big or M.teams.count(big) - M.teams.count(small) < 2 then break end
    local list = M.teams.players(big); table.sort(list, function(a, b) return (P.alive(a) and 1 or 0) < (P.alive(b) and 1 or 0) end)
    M.teams.assign(list[1], small); moved = moved + 1
  end
  return moved
end
function M.teams.swap() for _, id in ipairs(P.all()) do local k = M.teams.of(id); local cs = P.team(id)
  if #T.order >= 2 then local o = (k == T.order[1]) and T.order[2] or (k == T.order[2]) and T.order[1] or k; M.teams.assign(id, o)
  elseif cs == 1 or cs == 2 then P.set_team(id, 3 - cs) end end end
X.on("client_disconnect", function(id) T.of[id] = nil end)

-- ------------------------------------------------------------------------------------------------ MC.round
-- Events: MC.round.on("new" | "start" | "end" | "second", fn)
--   new(number)            new round (players respawned, freeze time begins)
--   start(number)          freeze time over, players can move
--   end(winner, cs_status, reason)   winner = team key ("t","ct", custom) or "draw"; reason = "game" | "mod"
--   second(time_left)      every second while the round is live
M.round = {}
local R = { n = 0, state = "warmup", ended = false, ls = { new = {}, start = {}, ["end"] = {}, second = {} }, block = false, freeze_was = false, checks = {} }
local function fire(kind, ...) for _, f in ipairs(R.ls[kind]) do safe("round." .. kind, f, ...) end end
function M.round.on(kind, fn) assert(R.ls[kind], "MC.round.on: kind must be new|start|end|second"); R.ls[kind][#R.ls[kind] + 1] = fn end
local function rules_ok() if not ready() then return false end local r = N.rules_available(); return r[1] end
M.round.rules_ok = rules_ok
function M.round.number() return R.n end
function M.round.state() return R.state end            -- "warmup" | "freeze" | "live" | "ended"
function M.round.is_live() return R.state == "live" end
function M.round.info() if rules_ok() then return N.round_state() end return nil end
function M.round.time_left()
  if rules_ok() then local s = N.round_state(); return math.max(0, s.time_left) end
  return 0
end
local function send_roundtime(secs) X.msg.begin(X.msg.ALL, "RoundTime"); X.msg.short(math.max(0, math.floor(secs))); X.msg.finish() end
-- MC.round.set_time(seconds): the round ends (game rules) after `seconds` from now; the HUD timer is updated
function M.round.set_time(secs)
  if not rules_ok() then warn("round.set_time: game rules not available"); return false end
  local s = N.round_state(); local elapsed = X.gametime() - s.round_start
  N.rules_set("m_iRoundTimeSecs", math.floor(elapsed + secs)); N.rules_set("m_iRoundTime", math.floor(elapsed + secs))
  send_roundtime(secs); return true
end
function M.round.add_time(secs) return M.round.set_time(M.round.time_left() + secs) end
function M.round.scores() if rules_ok() then local s = N.round_state(); return s.ct_wins, s.t_wins end return 0, 0 end
local function send_teamscore() local ct, t = M.round.scores()
  X.msg.begin(X.msg.ALL, "TeamScore"); X.msg.string("TERRORIST"); X.msg.short(t); X.msg.finish()
  X.msg.begin(X.msg.ALL, "TeamScore"); X.msg.string("CT"); X.msg.short(ct); X.msg.finish() end
function M.round.set_scores(ct, t) if not rules_ok() then return false end N.rules_set("m_iNumCTWins", ct); N.rules_set("m_iNumTerroristWins", t); send_teamscore(); return true end
-- winner: 1|"ct" CT, 2|"t" T, 3|"draw", or a custom team key (its cs team gets the CS win status)
-- CS team numbers: 1 = T, 2 = CT.  Win status numbers: 1 = CT won, 2 = T won, 3 = draw.
local function key_of_cs(cs) for _, k in ipairs(T.order) do if T.defs[k].cs == cs then return k end end return cs == 2 and "ct" or "t" end
local function resolve_winner(w)
  if w == nil or w == 3 or w == "draw" then return "draw", 3 end
  if w == 1 or w == "ct" then return (w == 1 and key_of_cs(2) or "ct"), 1 end
  if w == 2 or w == "t" then return (w == 2 and key_of_cs(1) or "t"), 2 end
  local cs = M.teams.cs(w); return w, cs == 2 and 1 or cs == 1 and 2 or 3
end
M.round.winner_key = resolve_winner
-- MC.round.finish(winner [, {delay=5, message=..., sound=..., score=true, reason="mod"}]) -> true if the round was ended now
function M.round.finish(winner, opts)
  opts = opts or {}
  if R.ended then return false end
  local key, cs = resolve_winner(winner)
  local delay = opts.delay or 5
  if rules_ok() then N.round_terminate(delay, cs)
  else warn("round.finish without game rules: restarting the round with sv_restartround"); X.exec("sv_restartround " .. math.max(1, math.floor(delay))) end
  if opts.score ~= false and rules_ok() and (cs == 1 or cs == 2) then
    local ct, t = M.round.scores(); if cs == 1 then ct = ct + 1 else t = t + 1 end; M.round.set_scores(ct, t)
  end
  local snd = opts.sound; if snd == nil then snd = cs == 1 and "%!MRAD_ctwin" or cs == 2 and "%!MRAD_terwin" or "%!MRAD_rounddraw" end
  if snd then X.msg.begin(X.msg.ALL, "SendAudio"); X.msg.byte(0); X.msg.string(snd); X.msg.short(100); X.msg.finish() end
  local msg = opts.message
  if msg == nil then
    if T.defs[key] then msg = M.teams.name(key) .. " win!"
    else msg = cs == 1 and "#CTs_Win" or cs == 2 and "#Terrorists_Win" or "#Round_Draw" end
  end
  if msg then
    if msg:sub(1, 1) == "#" then X.msg.begin(X.msg.ALL, "TextMsg"); X.msg.byte(4); X.msg.string(msg); X.msg.finish()
    else local r, g, b = M.teams.color(key); X.msg.hud(0, msg, { r = r, g = g, b = b, x = -1, y = 0.3, hold = delay, channel = 3 }) end
  end
  R.ended = true; R.state = "ended"
  fire("end", key, cs, opts.reason or "mod"); X.emit("mc:round_end", key, cs, opts.reason or "mod")
  return true
end
function M.round.draw(opts) return M.round.finish("draw", opts) end
-- MC.round.block_end(true): the GAME does not end rounds by itself any more (eliminations, time, bomb ...); MC.round.finish still works
function M.round.block_end(on)
  R.block = on and true or false
  if X.cvar.exists("mp_round_infinite") then X.cvar.set("mp_round_infinite", R.block and "1" or "0"); return "mp_round_infinite" end
  if X.reapi.available() and not R.reapi_hooked then
    R.reapi_hooked = pcall(X.on, "reapi:RoundEnd", function() if R.block then return X.HC_SUPERCEDE, false end end)
    if R.reapi_hooked then return "reapi:RoundEnd" end
  elseif R.reapi_hooked then return "reapi:RoundEnd" end
  warn("round.block_end needs ReGameDLL (mp_round_infinite / RoundEnd hook) - not available on this server"); return nil
end
-- MC.round.win_check(fn [, interval=0.5]): fn() -> winner [, opts] or nil; checked while the round is live
function M.round.win_check(fn, interval) R.checks[#R.checks + 1] = { fn = fn, every = interval or 0.5, next = 0 } end
function M.round.restart_game(delay) X.exec("sv_restart " .. math.max(1, math.floor(delay or 1))) end
local function new_round()
  R.n = R.n + 1; R.ended = false
  local fz = 0; local ok, v = pcall(X.cvar.num, "mp_freezetime"); if ok and v then fz = v end
  R.state = fz > 0 and "freeze" or "live"; R.freeze_was = fz > 0; R.started_at = X.gametime()
  fire("new", R.n); X.emit("mc:round_new", R.n)
  if R.state == "live" then fire("start", R.n); X.emit("mc:round_start", R.n) end
end
X.on("message:HLTV", function(_, _, _, args) if args[1] == 0 and args[2] == 0 then new_round() end end)
X.on("message:SendAudio", function(_, _, _, args)
  if R.ended or R.state == "warmup" then return end
  local s = args[2]; local cs = s == "%!MRAD_ctwin" and 1 or s == "%!MRAD_terwin" and 2 or s == "%!MRAD_rounddraw" and 3 or nil
  if cs and not rules_ok() then R.ended = true; R.state = "ended"; local key = resolve_winner(cs); fire("end", key, cs, "game"); X.emit("mc:round_end", key, cs, "game") end
end)
local last_sec = -1
X.timer.every(0.1, function()
  if not rules_ok() then
    if R.state == "warmup" and ready() and #M.util.alive_players() > 0 then new_round() end
    if R.state == "freeze" then local ok, fz = pcall(X.cvar.num, "mp_freezetime"); if X.gametime() - (R.started_at or 0) >= (ok and fz or 0) then R.state = "live"; fire("start", R.n); X.emit("mc:round_start", R.n) end end
  else
    local s = N.round_state()
    if R.state == "warmup" then new_round(); if R.state == "freeze" and not s.freeze then R.state = "live"; fire("start", R.n); X.emit("mc:round_start", R.n) end end
    if R.state == "freeze" and not s.freeze then R.state = "live"; fire("start", R.n); X.emit("mc:round_start", R.n) end
    if s.terminating and not R.ended and R.state ~= "warmup" then
      R.ended = true; R.state = "ended"; local key = resolve_winner(s.win_status == 0 and 3 or s.win_status)
      fire("end", key, s.win_status, "game"); X.emit("mc:round_end", key, s.win_status, "game")
    end
    if R.state == "live" then local tl = math.floor(s.time_left); if tl ~= last_sec then last_sec = tl; fire("second", tl) end end
  end
  if R.state == "live" and not R.ended then
    local t = X.gametime()
    for _, c in ipairs(R.checks) do if t >= c.next then c.next = t + c.every
      local w, o = safe("round.win_check", c.fn); if w ~= nil and w ~= false then M.round.finish(w, o or { reason = "win_check" }) break end end end
  end
end)
X.on("map_end", function() R.n = 0; R.state = "warmup"; R.ended = false end)

-- ------------------------------------------------------------------------------------------------ MC.objectives
-- MC.objectives.add{ id="flag_a", name="Point A", type=..., team="ct" (who must complete it; nil = any team), round_win=true,
--   reset_each_round=true, hud=true, on_progress(obj, team, progress 0..1), on_complete(obj, team), ...type fields }
-- types:
--   "capture" origin={x,y,z} radius=128 time=10 (s) contested=true (stops while enemies are inside) decay=0.5 (progress lost/s when empty)
--             owner=nil, hold_points=0 (team score per second while owned), recapture=true, beacon=true (ring effect)
--   "destroy" ent=<existing ent> or model=..., origin, health=500, mins/maxs   (complete when the entity dies)
--   "reach"   origin + radius, or mins={..}, maxs={..}; count=1 | "all" (alive players of the team inside)
--   "collect" items={{x,y,z},...} model="models/..." count=#items (touched by the team)
--   "kills"   count=20 (kills by the team: players + NPCs; `npc="zombie"` counts only that NPC kind)
--   "survive" time=120 (the team must have living players when the time is over)
--   "custom"  check=function(obj) -> progress 0..1 | true (done) [, team]
-- Events: xmod.emit("mc:objective_progress", id, team, p) / ("mc:objective_complete", id, team)
M.objectives = {}
local OBJ, OBJ_ORDER = {}, {}
local function team_ok(o, key) return o.team == nil or o.team == key end
local function ob_progress(o, key, p)
  p = math.max(0, math.min(1, p))
  if math.abs((o.progress[key] or 0) - p) < 1e-4 then return end
  o.progress[key] = p
  safe(o.id .. ".on_progress", o.on_progress, o, key, p); X.emit("mc:objective_progress", o.id, key or "", p)
end
local function ob_complete(o, key)
  if o.done then return end
  o.done = true; o.winner = key; o.progress[key or "any"] = 1
  safe(o.id .. ".on_complete", o.on_complete, o, key); X.emit("mc:objective_complete", o.id, key or "")
  if o.round_win and key then M.round.finish(key, { reason = "objective:" .. o.id }) end
end
local function players_in(o)          -- team key -> count of alive players inside the zone
  local res = {}
  for _, id in ipairs(P.all()) do if P.alive(id) then
    local x, y, z = P.origin(id); local inside
    if o.mins then inside = x >= o.mins[1] and x <= o.maxs[1] and y >= o.mins[2] and y <= o.maxs[2] and z >= o.mins[3] and z <= o.maxs[3]
    else inside = dist3(x, y, z, o.origin[1], o.origin[2], o.origin[3]) <= o.radius end
    if inside then local k = M.teams.of(id); if k then res[k] = (res[k] or 0) + 1 end end
  end end
  return res
end
local TYPES = {}
TYPES.capture = function(o, dt)
  local inside = players_in(o); local teams_in = {} for k in pairs(inside) do teams_in[#teams_in + 1] = k end
  local capper = nil
  if #teams_in == 1 and team_ok(o, teams_in[1]) and (o.recapture or not o.owner) and teams_in[1] ~= o.owner then capper = teams_in[1] end
  if #teams_in > 1 and o.contested then o.state = "contested" return end
  if capper then
    if o.capturing ~= capper then o.capturing = capper; o.progress = {} end
    local rate = 1 + math.min(3, (inside[capper] - 1) * 0.5)
    ob_progress(o, capper, (o.progress[capper] or 0) + dt * rate / o.time); o.state = "capturing"
    if o.progress[capper] >= 1 then
      o.owner = capper; o.capturing = nil; o.state = "owned"; o.progress = { [capper] = 1 }
      X.emit("mc:objective_captured", o.id, capper)
      if o.team or o.round_win or o.single then ob_complete(o, capper) else safe(o.id .. ".on_complete", o.on_complete, o, capper) end
    end
  elseif o.capturing then
    local p = (o.progress[o.capturing] or 0) - dt * (o.decay or 0.5) / o.time
    if p <= 0 then o.progress = {}; o.capturing = nil; o.state = o.owner and "owned" or "neutral" else ob_progress(o, o.capturing, p) end
  end
  if o.owner and o.hold_points and o.hold_points > 0 then o.acc = (o.acc or 0) + dt * o.hold_points; if o.acc >= 1 then M.teams.add_score(o.owner, math.floor(o.acc)); o.acc = o.acc - math.floor(o.acc) end end
  if o.beacon ~= false and X.gametime() >= (o.next_fx or 0) then o.next_fx = X.gametime() + 1.0
    local r, g, b = 255, 255, 255; local k = o.capturing or o.owner; if k then r, g, b = M.teams.color(k) end
    M.fx.ring(o.origin[1], o.origin[2], o.origin[3] + 8, o.radius, r, g, b, 10) end
end
TYPES.reach = function(o)
  local inside = players_in(o)
  for _, key in ipairs(o.team and { o.team } or M.teams.list()) do
    local c = inside[key] or 0; local need = o.count == "all" and math.max(1, M.teams.alive(key)) or (o.count or 1)
    ob_progress(o, key, c / need); if c >= need then ob_complete(o, key) return end
  end
end
TYPES.survive = function(o)
  local key = o.team; if not key then return end
  if M.teams.alive(key) == 0 then o.failed = true; return end
  local p = (X.gametime() - o.started) / o.time; ob_progress(o, key, p); if p >= 1 then ob_complete(o, key) end
end
TYPES.custom = function(o)
  local p, key = safe(o.id .. ".check", o.check, o); key = key or o.team
  if p == true then ob_complete(o, key) elseif type(p) == "number" then ob_progress(o, key or "any", p); if p >= 1 then ob_complete(o, key) end end
end
local function setup(o)
  o.done, o.winner, o.failed, o.progress, o.capturing, o.started = false, nil, false, {}, nil, X.gametime()
  o.kills = {}
  if o.type == "capture" then o.owner = o.initial_owner; o.state = o.owner and "owned" or "neutral" end
  if o.type == "destroy" then
    if o.model and not (o.ent and valid(o.ent)) then
      o.ent = M.ent.create { classname = "mc_objective", model = o.model, origin = o.origin, mins = o.mins or { -24, -24, 0 }, maxs = o.maxs or { 24, 24, 64 },
        solid = M.SOLID.BBOX, movetype = M.MOVETYPE.TOSS, health = o.health or 500, takedamage = 2, unsafe_model = o.unsafe_model }
      M.ent.data(o.ent).objective = o.id
    elseif o.ent and valid(o.ent) then
      M.ent.data(o.ent).objective = o.id
      if o.model then E.set(o.ent, "health", o.health or 500); E.set(o.ent, "takedamage", 2); E.set(o.ent, "solid", M.SOLID.BBOX); E.set_origin(o.ent, vt3(o.origin)) end
    end
    o.max_health = o.ent and E.get(o.ent, "health") or 1
  end
  if o.type == "collect" then
    for _, it in ipairs(o.items_ents or {}) do M.ent.remove(it) end
    o.items_ents, o.collected = {}, {}
    for _, p in ipairs(o.items or {}) do
      local e = M.ent.create { classname = "mc_collect", model = o.model, origin = p, mins = { -12, -12, 0 }, maxs = { 12, 12, 24 }, solid = M.SOLID.TRIGGER,
        movetype = M.MOVETYPE.TOSS, unsafe_model = o.unsafe_model }
      if o.glow then M.ent.glow(e, unpack(o.glow)) end
      M.ent.data(e).objective = o.id; o.items_ents[#o.items_ents + 1] = e
    end
    o.need = o.count or #o.items_ents
  end
end
function M.objectives.add(def)
  assert(def.id and def.type, "MC.objectives.add{id=, type=}")
  assert(def.type == "kills" or def.type == "destroy" or def.type == "collect" or TYPES[def.type], "unknown objective type " .. tostring(def.type))
  def.name = def.name or def.id; def.radius = def.radius or 128; def.time = def.time or 10; def.active = def.active ~= false
  if def.contested == nil then def.contested = true end
  if def.recapture == nil then def.recapture = true end
  if def.round_win == nil then def.round_win = def.team ~= nil end
  if def.reset_each_round == nil then def.reset_each_round = true end
  if def.hud == nil then def.hud = true end
  if def.model then M.precache.model(def.model) end
  if not OBJ[def.id] then OBJ_ORDER[#OBJ_ORDER + 1] = def.id end
  OBJ[def.id] = def
  if def.ready ~= false and X.gametime() > 0 then pcall(setup, def) end
  return def
end
function M.objectives.get(id) return OBJ[id] end
function M.objectives.list() local t = {} for _, id in ipairs(OBJ_ORDER) do if OBJ[id] then t[#t + 1] = OBJ[id] end end return t end
function M.objectives.remove(id) local o = OBJ[id]; if not o then return end
  if o.type == "collect" then for _, e in ipairs(o.items_ents or {}) do M.ent.remove(e) end end
  if o.type == "destroy" and o.model and valid(o.ent) then M.ent.remove(o.ent) end
  OBJ[id] = nil end
function M.objectives.reset(id) if id then local o = OBJ[id]; if o then setup(o) end else for _, o in pairs(OBJ) do setup(o) end end end
function M.objectives.complete(id, team) local o = OBJ[id]; if o then ob_complete(o, team or o.team) end end
function M.objectives.progress(id, team) local o = OBJ[id]; if not o then return 0 end
  if team then return o.progress[team] or 0 end local best = 0 for _, p in pairs(o.progress) do best = math.max(best, p) end return best end
function M.objectives.status_text()
  local lines = {}
  for _, o in ipairs(M.objectives.list()) do if o.active and o.hud then
    local p = math.floor(M.objectives.progress(o.id) * 100 + 0.5); local s
    if o.done then s = string.format("[x] %s - %s", o.name, M.teams.name(o.winner))
    elseif o.type == "capture" then s = string.format("[ ] %s - %s %d%%", o.name, o.state == "contested" and "CONTESTED" or (o.capturing and M.teams.name(o.capturing)) or (o.owner and M.teams.name(o.owner) or "neutral"), p)
    elseif o.type == "destroy" then s = string.format("[ ] %s - %d%%", o.name, valid(o.ent) and math.floor(E.get(o.ent, "health") / o.max_health * 100) or 0)
    elseif o.type == "collect" then s = string.format("[ ] %s - %d/%d", o.name, #o.collected, o.need)
    elseif o.type == "kills" then local k = o.team and (o.kills[o.team] or 0) or 0; s = string.format("[ ] %s - %d/%d", o.name, k, o.count or 1)
    elseif o.type == "survive" then s = string.format("[ ] %s - %ds", o.name, math.max(0, math.floor(o.time - (X.gametime() - o.started))))
    else s = string.format("[ ] %s - %d%%", o.name, p) end
    lines[#lines + 1] = s
  end end
  return table.concat(lines, "\n")
end
M.objectives.hud_style = { r = 255, g = 200, b = 60, x = 0.02, y = 0.25, channel = 4 }
-- collect: touching an item
M.ent.class("mc_collect", { on_touch = function(e, other)
  if not is_player(other) or not P.alive(other) then return end
  local o = OBJ[M.ent.data(e).objective]; if not o or o.done then return end
  local key = M.teams.of(other); if not team_ok(o, key) then return end
  o.collected[#o.collected + 1] = other; M.ent.remove(e)
  ob_progress(o, key, #o.collected / o.need); if #o.collected >= o.need then ob_complete(o, key) end
  X.emit("mc:objective_collect", o.id, other)
end })
-- destroy: the target dies (attacker team completes it)
M.ent.class("mc_objective", { on_killed = function(e, att)
  local o = OBJ[M.ent.data(e).objective]; if not o then return end
  local key = att and team_of_ent(att) or o.team; if o.team and key ~= o.team then key = o.team end
  ob_complete(o, key)
end })
X.on("ham:killed:info_target", function(e, att)
  for _, o in pairs(OBJ) do if o.type == "destroy" and o.ent == e and not o.model and not o.done then ob_complete(o, att and team_of_ent(att) or o.team) end end
end)
-- kills: players (DeathMsg) and NPCs
local function count_kill(killer, victim_npc)
  local key = team_of_ent(killer); if not key then return end
  for _, o in pairs(OBJ) do if o.type == "kills" and o.active and not o.done and team_ok(o, key) and (not o.npc or o.npc == victim_npc) and (o.npc or not victim_npc or o.count_npcs) then
    o.kills[key] = (o.kills[key] or 0) + 1; ob_progress(o, key, o.kills[key] / (o.count or 1)); if o.kills[key] >= (o.count or 1) then ob_complete(o, key) end
  end end
end
X.on("message:DeathMsg", function(_, _, _, args) local k, v = args[1], args[2]; if is_player(k) and k ~= v then count_kill(k, nil) end end)
X.on("mc:npc_killed", function(_, name, att) if valid(att) and att ~= 0 then count_kill(att, name) end end)
local function ob_tick()
  local dt = 0.25; local any_hud = false
  for _, o in pairs(OBJ) do if o.active and not o.done and not o.failed and (M.round.state() == "live" or o.always) then
    local f = TYPES[o.type]; if f then safe("objective " .. o.id, f, o, dt) end
    if o.type == "destroy" and o.ent and valid(o.ent) then local h = E.get(o.ent, "health"); ob_progress(o, o.team or "any", 1 - h / math.max(1, o.max_health)) end
  end if o.active and o.hud then any_hud = true end end
  if any_hud and X.gametime() >= (M.objectives._next_hud or 0) then
    M.objectives._next_hud = X.gametime() + 1.0
    local t = M.objectives.status_text(); if t ~= "" then local st = M.objectives.hud_style
      X.msg.hud(0, t, { r = st.r, g = st.g, b = st.b, x = st.x, y = st.y, hold = 1.1, fadein = 0, fadeout = 0, channel = st.channel }) end
  end
end
X.timer.every(0.25, function() ob_tick() end)
M.round.on("new", function() for _, o in pairs(OBJ) do if o.reset_each_round then pcall(setup, o) end end end)
X.on("map_end", function() for _, o in pairs(OBJ) do o.ent = nil; o.items_ents = {} end end)

-- ------------------------------------------------------------------------------------------------ MC.weapons (WeaponMod style)
-- MC.weapons.register(key, def) - a custom weapon built on a real CS weapon (def.base, e.g. "weapon_ak47"):
--   name, base, v_model, p_model, w_model (dropped weaponbox), damage=1.0 (multiplier) | damage_fixed, clip (custom clip size),
--   ammo (backpack ammo given), rate (seconds between shots), recoil=1.0 (punch multiplier), speed (max speed while held),
--   knockback=0, anim_ext ("rifle", "knife" ...), sound="weapons/x.wav" | {...} (fire sound), anims={shoot={..}, reload=n, draw=n},
--   reload_time (custom reload duration), cost, team (MC.teams key allowed to buy), block_events (auto when sound/anims.shoot),
--   fire=function(id, wpn, def) | {damage=, spread=, shots=, distance=, bits=, tracer=true}  -> replaces the base weapon's shot,
--   on_attack(id, wpn) [true = block the shot], on_secondary(id, wpn) [true = block], on_deploy(id, wpn), on_reload(id, wpn) [false = block],
--   on_hit(attacker, victim, dmg, wpn) [number = new damage, false = none]
-- Custom clip sizes are for magazine weapons (not m3 / xm1014).
M.weapons = {}
local W, W_imp, W_order, W_hooked = {}, {}, {}, {}
local IMP0 = 7713000
local wst = {}                  -- wpn -> per-shot state
local in_fire = false
local function wdef(wpn) local i = valid(wpn) and E.get(wpn, "impulse") or 0; return i ~= 0 and W_imp[i] or nil end
M.weapons.get = wdef
function M.weapons.of(id) local w = N.weapon_active(id); if w == 0 then return nil, 0 end return wdef(w), w end
function M.weapons.is(wpn, key) local d = wdef(wpn); return d ~= nil and d.key == key end
local function pickv(v) if type(v) == "table" then return v[math.random(#v)] end return v end
local function apply_models(id, w, d)
  if d.v_model then E.set(id, "viewmodel", d.v_model) end
  if d.p_model then E.set(id, "weaponmodel", d.p_model) end
  if d.anim_ext then N.anim_ext(id, d.anim_ext) end
  if d.anims and d.anims.draw then N.weapon_anim(id, d.anims.draw) end
end
function M.weapons.shoot(id, b)
  local sx, sy, sz, _, _, _, dx, dy, dz = M.util.aim(id, 10)
  local rows = N.fire_bullets(id, id, sx, sy, sz, dx, dy, dz, b.spread or 0.02, b.distance or 8192, b.damage or 30, b.shots or 1, b.bits or (DMG_BULLET | DMG_NEVERGIB))
  if b.tracer then for _, r in ipairs(rows) do M.fx.tracer(sx + dx * 20, sy + dy * 20, sz + dz * 20, r[2], r[3], r[4]) end end
  return rows
end
local function install(base)
  if W_hooked[base] then return end
  W_hooked[base] = true
  X.on("ham:item_deploy:" .. base .. ":post", function(w)
    local d = wdef(w); if not d then return end
    local id = N.weapon_owner(w); if not is_player(id) then return end
    apply_models(id, w, d); safe(d.key .. ".on_deploy", d.on_deploy, id, w)
  end)
  X.on("ham:weapon_primaryattack:" .. base, function(w)
    local d = wdef(w); if not d then return end
    local id = N.weapon_owner(w); if not is_player(id) then return end
    local px, py, pz = E.get(id, "punchangle")
    wst[w] = { clip = N.weapon_clip(w), px = px, py = py, pz = pz }
    if d.on_attack and safe(d.key .. ".on_attack", d.on_attack, id, w) == true then return X.HC_SUPERCEDE end
    if not d.fire then return end
    local clip = N.weapon_clip(w); if clip <= 0 then return end          -- empty click: the base weapon handles it
    N.weapon_clip(w, clip - 1)
    in_fire = true
    if type(d.fire) == "function" then safe(d.key .. ".fire", d.fire, id, w, d) else M.weapons.shoot(id, d.fire) end
    in_fire = false
    local rate = d.rate or 0.1
    N.weapon_times(w, rate, rate, rate + 1.0)
    local kick = d.recoil or 1.0
    E.set(id, "punchangle", px - kick * (0.8 + math.random() * 0.6), py + kick * (math.random() - 0.5), pz)
    E.set(id, "effects", E.get(id, "effects") | 2)        -- EF_MUZZLEFLASH (3rd person)
    if d.sound then X.sound.emit(id, X.sound.CHAN_WEAPON, pickv(d.sound), 1.0, 0.8, 0, 94 + math.random(0, 12)) end
    if d.anims and d.anims.shoot then N.weapon_anim(id, pickv(d.anims.shoot)) end
    return X.HC_SUPERCEDE
  end)
  X.on("ham:weapon_primaryattack:" .. base .. ":post", function(w)
    local d = wdef(w); local s = wst[w]; if not d or not s or d.fire then return end
    local id = N.weapon_owner(w); if not is_player(id) then return end
    if N.weapon_clip(w) >= s.clip then return end                          -- did not fire
    if d.rate then N.weapon_times(w, d.rate) end
    if d.recoil and d.recoil ~= 1 then local x, y, z = E.get(id, "punchangle")
      E.set(id, "punchangle", s.px + (x - s.px) * d.recoil, s.py + (y - s.py) * d.recoil, s.pz + (z - s.pz) * d.recoil) end
    if d.sound then X.sound.emit(id, X.sound.CHAN_WEAPON, pickv(d.sound), 1.0, 0.8, 0, 94 + math.random(0, 12)) end
    if d.anims and d.anims.shoot then N.weapon_anim(id, pickv(d.anims.shoot)) end
  end)
  X.on("ham:weapon_secondaryattack:" .. base, function(w)
    local d = wdef(w); if not d or not d.on_secondary then return end
    local id = N.weapon_owner(w); if is_player(id) and safe(d.key .. ".on_secondary", d.on_secondary, id, w) == true then return X.HC_SUPERCEDE end
  end)
  X.on("ham:weapon_reload:" .. base, function(w)
    local d = wdef(w); if not d then return end
    local id = N.weapon_owner(w); if not is_player(id) then return end
    if d.on_reload and safe(d.key .. ".on_reload", d.on_reload, id, w) == false then return X.HC_SUPERCEDE end
    if not d.clip then return end
    local clip, bp = N.weapon_clip(w), N.bpammo(id, w)
    if bp <= 0 or clip >= d.clip then return X.HC_SUPERCEDE end
    wst[w] = wst[w] or {}; wst[w].reload_clip = clip; N.weapon_clip(w, 0)
  end)
  X.on("ham:weapon_reload:" .. base .. ":post", function(w)
    local s = wst[w]; if not s or not s.reload_clip then return end
    N.weapon_clip(w, s.reload_clip); s.reload_clip = nil
    local d = wdef(w); local id = N.weapon_owner(w); if not d or not is_player(id) then return end
    if N.pdata_get(w, "m_fInReload") ~= 0 then
      if d.reload_time then N.next_attack(id, d.reload_time); N.weapon_times(w, nil, nil, d.reload_time + 0.5) end
      if d.anims and d.anims.reload then N.weapon_anim(id, d.anims.reload) end
    end
  end)
  X.on("ham:item_postframe:" .. base, function(w)
    local d = wdef(w); if not d or not d.clip then return end
    if N.pdata_get(w, "m_fInReload") == 0 then return end
    local id = N.weapon_owner(w); if not is_player(id) or N.next_attack(id) > 0 then return end
    local clip, bp = N.weapon_clip(w), N.bpammo(id, w); local j = math.min(d.clip - clip, bp)
    if j > 0 then N.weapon_clip(w, clip + j); N.bpammo(id, w, bp - j) end
    N.pdata_set(w, "m_fInReload", 0)
  end)
  pcall(X.on, "ham:cstrike_item_getmaxspeed:" .. base, function(w) local d = wdef(w); if d and d.speed then return X.HC_SUPERCEDE, d.speed end end)
end
local function wdamage(v, inf, att, dmg, bits)
  if not is_player(att) or inf ~= att or (bits & DMG_BULLET) == 0 then return end
  local w = N.weapon_active(att); if w == 0 then return end
  local d = wdef(w); if not d then return end
  local nd = dmg
  if not in_fire then nd = d.damage_fixed or (d.damage and dmg * d.damage) or dmg end
  if d.on_hit then local r = safe(d.key .. ".on_hit", d.on_hit, att, v, nd, w); if r == false then return X.HC_SUPERCEDE, 0 elseif type(r) == "number" then nd = r end end
  if d.knockback and d.knockback > 0 then
    local ax, ay = P.origin(att); local vx, vy = E.get(v, "origin"); local dx, dy = vx - ax, vy - ay; local l = math.max(1, math.sqrt(dx * dx + dy * dy))
    local x, y, z = E.get(v, "velocity"); E.set(v, "velocity", x + dx / l * d.knockback, y + dy / l * d.knockback, z + d.knockback * 0.25)
  end
  if nd ~= dmg then X.hook.set_arg(3, nd) end
end
local dmg_hooked = false
function M.weapons.register(key, def)
  assert(type(def) == "table" and def.base, "MC.weapons.register(key, {base='weapon_ak47', ...})")
  if not W[key] then W_order[#W_order + 1] = key; def.impulse = IMP0 + #W_order else def.impulse = W[key].impulse end
  def.key = key; def.name = def.name or key; W[key] = def; W_imp[def.impulse] = def
  for _, m in ipairs({ def.v_model, def.p_model, def.w_model }) do M.precache.model(m) end
  for _, s in ipairs(type(def.sound) == "table" and def.sound or { def.sound }) do M.precache.sound(s) end
  if def.block_events == nil then def.block_events = (def.sound ~= nil) or (def.anims ~= nil and def.anims.shoot ~= nil) or (def.fire ~= nil) end
  when_ready(function() N.custom_weapon(def.impulse, { wmodel = def.w_model or "", block_events = def.block_events }) end)
  install(def.base)
  if not dmg_hooked then dmg_hooked = true; X.on("ham:takedamage:player", wdamage, 40); X.on("ham:takedamage:info_target", wdamage, 40) end
  return def
end
function M.weapons.list() local t = {} for _, k in ipairs(W_order) do t[#t + 1] = W[k] end return t end
function M.weapons.def(key) return W[key] end
-- MC.weapons.strip(id, wpn_or_classname) - removes one weapon from the player (ham_strip_weapon)
function M.weapons.strip(id, x)
  local w = type(x) == "string" and N.weapon_find(id, x) or x
  if not w or w == 0 then return false end
  local info = N.weapon_info(w)
  if N.weapon_active(id) == w then pcall(N.ham, "weapon_retireweapon", w) end
  if N.ham("removeplayeritem", id, w) == 0 then return false end
  N.ham("item_kill", w)
  E.set(id, "weapons", E.get(id, "weapons") & ~(1 << info.id))
  return true
end
-- MC.weapons.switch(id, wpn) - puts a weapon the player carries in his hands (Holster old -> Deploy new)
function M.weapons.switch(id, w)
  local cur = N.weapon_active(id); if cur == w or w == 0 then return cur == w end
  if N.weapon_owner(w) ~= id then return false end
  if cur ~= 0 then pcall(N.ham, "item_holster", cur, 0) end
  N.pdata_set(id, "m_pLastItem", cur); N.pdata_set(id, "m_pActiveItem", w)
  N.ham("item_deploy", w)
  return true
end
-- MC.weapons.give(id, key [, switch=false]) -> weapon entity (0 = failed). An existing weapon of the same base is replaced.
function M.weapons.give(id, key, switch)
  local d = W[key]; if not d then error("MC.weapons.give: unknown weapon '" .. tostring(key) .. "'", 2) end
  if not P.alive(id) then return 0 end
  local old = N.weapon_find(id, d.base); if old ~= 0 then M.weapons.strip(id, old) end
  if not P.give_item(id, d.base) then return 0 end
  local w = N.weapon_find(id, d.base); if w == 0 then return 0 end
  E.set(w, "impulse", d.impulse)
  if d.clip then N.weapon_clip(w, d.clip) end
  if d.ammo then N.bpammo(id, w, d.ammo) end
  if switch then M.weapons.switch(id, w) elseif N.weapon_active(id) == w then apply_models(id, w, d) end
  X.emit("mc:weapon_given", id, key, w)
  return w
end
-- MC.weapons.menu(id [, filter(def) -> bool]) - buy menu with every registered weapon (cost, team)
function M.weapons.menu(id, filter)
  local menu = require("menu"); local items = {}
  for _, d in ipairs(M.weapons.list()) do
    if (not d.team or d.team == M.teams.of(id)) and (not filter or filter(d)) then
      items[#items + 1] = { d.cost and string.format("%s \\y$%d", d.name, d.cost) or d.name, d.key } end
  end
  menu.show(id, { title = "Weapons", items = items, on_select = function(pid, _, key)
    local d = W[key]; if not d or not P.alive(pid) then return end
    if d.cost and P.money(pid) < d.cost then X.msg.chat(pid, "{g}[MC] {y}Not enough money ({g}$" .. d.cost .. "{y})"); return end
    if M.weapons.give(pid, key, true) ~= 0 and d.cost then P.set_money(pid, P.money(pid) - d.cost) end
  end })
end
function M.weapons.zoom(id, fov) N.pdata_set(id, "m_iFOV", fov or 90); E.set(id, "fov", fov or 90) end

-- ------------------------------------------------------------------------------------------------ MC.fx (temp entities)
-- Sprites/models are paths (precached through MC.precache) or engine indexes. Colours 0..255. Everything goes to the PVS of the point.
M.fx = {}
local TE = xmod.msg.TEMPENTITY or 23
local function spr(s) if type(s) == "number" then return s end local i = M.precache.index(s or "sprites/laserbeam.spr"); return i end
local function te_begin(x, y, z, kind) X.msg.begin(X.msg.PVS, TE, x, y, z); X.msg.byte(kind) end
local function te_coord3(x, y, z) X.msg.coord(x); X.msg.coord(y); X.msg.coord(z) end
function M.fx.beam(x1, y1, z1, x2, y2, z2, r, g, b, opts)
  opts = opts or {}; te_begin(x1, y1, z1, 0); te_coord3(x1, y1, z1); te_coord3(x2, y2, z2)
  X.msg.short(spr(opts.sprite)); X.msg.byte(opts.frame or 0); X.msg.byte(opts.rate or 10); X.msg.byte(math.floor((opts.life or 0.2) * 10))
  X.msg.byte(opts.width or 10); X.msg.byte(opts.noise or 0); X.msg.byte(r or 255); X.msg.byte(g or 255); X.msg.byte(b or 255)
  X.msg.byte(opts.alpha or 200); X.msg.byte(opts.speed or 0); X.msg.finish()
end
function M.fx.beam_ents(e1, e2, r, g, b, opts)
  opts = opts or {}; local x, y, z = M.util.origin(e1); te_begin(x, y, z, 8)
  X.msg.short(e1); X.msg.short(e2); X.msg.short(spr(opts.sprite)); X.msg.byte(0); X.msg.byte(10); X.msg.byte(math.floor((opts.life or 0.2) * 10))
  X.msg.byte(opts.width or 10); X.msg.byte(opts.noise or 0); X.msg.byte(r or 255); X.msg.byte(g or 255); X.msg.byte(b or 255); X.msg.byte(opts.alpha or 200); X.msg.byte(0); X.msg.finish()
end
function M.fx.trail(e, r, g, b, opts)   -- TE_BEAMFOLLOW: a trail behind a moving entity
  opts = opts or {}; local x, y, z = M.util.origin(e); te_begin(x, y, z, 22)
  X.msg.short(e); X.msg.short(spr(opts.sprite)); X.msg.byte(math.floor((opts.life or 1) * 10)); X.msg.byte(opts.width or 6)
  X.msg.byte(r or 255); X.msg.byte(g or 255); X.msg.byte(b or 255); X.msg.byte(opts.alpha or 180); X.msg.finish()
end
function M.fx.ring(x, y, z, radius, r, g, b, life, opts)   -- TE_BEAMCYLINDER
  opts = opts or {}; te_begin(x, y, z, 21); te_coord3(x, y, z); te_coord3(x, y, z + radius)
  X.msg.short(spr(opts.sprite or "sprites/shockwave.spr")); X.msg.byte(0); X.msg.byte(0); X.msg.byte(life or 10); X.msg.byte(opts.width or 12); X.msg.byte(0)
  X.msg.byte(r or 255); X.msg.byte(g or 255); X.msg.byte(b or 255); X.msg.byte(opts.alpha or 180); X.msg.byte(0); X.msg.finish()
end
function M.fx.explosion(x, y, z, opts)   -- TE_EXPLOSION (+ sound unless opts.silent)
  opts = opts or {}; te_begin(x, y, z, 3); te_coord3(x, y, z + 20); X.msg.short(spr(opts.sprite or "sprites/zerogxplode.spr"))
  X.msg.byte(opts.scale or 30); X.msg.byte(opts.framerate or 15); X.msg.byte(opts.silent and 4 or 0); X.msg.finish()
end
function M.fx.sprite(x, y, z, sprite, scale, alpha) te_begin(x, y, z, 17); te_coord3(x, y, z); X.msg.short(spr(sprite)); X.msg.byte(scale or 10); X.msg.byte(alpha or 200); X.msg.finish() end
function M.fx.smoke(x, y, z, scale, framerate) te_begin(x, y, z, 5); te_coord3(x, y, z); X.msg.short(spr("sprites/steam1.spr")); X.msg.byte(scale or 20); X.msg.byte(framerate or 10); X.msg.finish() end
function M.fx.sparks(x, y, z) te_begin(x, y, z, 9); te_coord3(x, y, z); X.msg.finish() end
function M.fx.tracer(x1, y1, z1, x2, y2, z2) te_begin(x1, y1, z1, 6); te_coord3(x1, y1, z1); te_coord3(x2, y2, z2); X.msg.finish() end
function M.fx.light(x, y, z, radius, r, g, b, life, decay)   -- TE_DLIGHT
  te_begin(x, y, z, 27); te_coord3(x, y, z); X.msg.byte(radius or 20); X.msg.byte(r or 255); X.msg.byte(g or 255); X.msg.byte(b or 255)
  X.msg.byte(math.floor((life or 0.5) * 10)); X.msg.byte(decay or 30); X.msg.finish()
end
function M.fx.implosion(x, y, z, radius, count, life) te_begin(x, y, z, 14); te_coord3(x, y, z); X.msg.byte(radius or 64); X.msg.byte(count or 20); X.msg.byte(math.floor((life or 0.5) * 10)); X.msg.finish() end
function M.fx.lava_splash(x, y, z) te_begin(x, y, z, 10); te_coord3(x, y, z); X.msg.finish() end
function M.fx.teleport(x, y, z) te_begin(x, y, z, 11); te_coord3(x, y, z); X.msg.finish() end
function M.fx.blood(x, y, z, dx, dy, dz, color, amount) return N.blood(x, y, z, dx or 0, dy or 0, dz or 1, color or 247, amount or 50) end
function M.fx.decal(x, y, z, name, ent) return N.decal(x, y, z, name, ent) end
function M.fx.shake(id, amp, secs, freq) X.msg.shake(id or 0, amp or 8, secs or 1, freq or 5) end
function M.fx.fade(id, secs, hold, r, g, b, a) X.msg.fade(id or 0, secs or 1, hold or 0, 0, r or 0, g or 0, b or 0, a or 150) end

-- ------------------------------------------------------------------------------------------------ MC.hud
M.hud = {}
function M.hud.text(id, text, opts) X.msg.hud(id or 0, text, opts or {}) end
function M.hud.center(id, text) X.msg.print(id or 0, "center", text) end
function M.hud.bar(id, seconds)        -- the CS progress bar (BarTime), 0 hides it
  local function one(p) X.msg.begin(X.msg.ONE, "BarTime", nil, nil, nil, p); X.msg.short(math.floor(seconds or 0)); X.msg.finish() end
  if id and id > 0 then one(id) else for _, p in ipairs(P.all()) do if not P.is_bot(p) then one(p) end end end
end
function M.hud.status_icon(id, sprite_name, on, r, g, b)   -- StatusIcon: e.g. "dmg_rad", "dmg_shock", "c4", "defuser", "buyzone"
  local function one(p) X.msg.begin(X.msg.ONE, "StatusIcon", nil, nil, nil, p); X.msg.byte(on == false and 0 or (on == 2 and 2 or 1)); X.msg.string(sprite_name)
    if on ~= false then X.msg.byte(r or 0); X.msg.byte(g or 160); X.msg.byte(b or 0) end X.msg.finish() end
  if id and id > 0 then one(id) else for _, p in ipairs(P.all()) do if not P.is_bot(p) then one(p) end end end
end
function M.hud.timer(seconds) X.msg.begin(X.msg.ALL, "RoundTime"); X.msg.short(math.max(0, math.floor(seconds))); X.msg.finish() end
function M.hud.hide(id, flags)          -- HideWeapon: 1 crosshair+ammo, 8 flashlight, 16 timer, 32 money, 64 crosshair ...
  X.msg.begin(X.msg.ONE, "HideWeapon", nil, nil, nil, id); X.msg.byte(flags or 0); X.msg.finish()
end
-- a per-player text line on a fixed channel, refreshed by you (e.g. each second): MC.hud.line(id, "HP 100 | Kills 3", {y=0.9})
function M.hud.line(id, text, opts) opts = opts or {}; X.msg.hud(id, text, { r = opts.r or 200, g = opts.g or 200, b = opts.b or 200, x = opts.x or -1, y = opts.y or 0.9,
  hold = opts.hold or 1.2, fadein = 0, fadeout = 0, channel = opts.channel or 2 }) end

-- ------------------------------------------------------------------------------------------------ MC.spawns
-- Spawn points by kind: "t" (info_player_deathmatch), "ct" (info_player_start), "any", or your own kinds saved in data/mc_spawns_<map>.txt
M.spawns = {}
local SP = { list = {}, loaded_for = nil }
local function load_spawns()
  local map = X.map(); if SP.loaded_for == map then return end
  SP.loaded_for = map; SP.list = { t = {}, ct = {}, any = {} }
  for _, pair in ipairs({ { "info_player_deathmatch", "t" }, { "info_player_start", "ct" } }) do
    for _, e in ipairs(M.ent.find_all(pair[1])) do local x, y, z = E.get(e, "origin"); local _, yaw = E.get(e, "angles")
      local p = { x, y, z, yaw }; table.insert(SP.list[pair[2]], p); table.insert(SP.list.any, p) end
  end
  local txt = X.fs.exists("data/mc_spawns_" .. map .. ".txt") and X.fs.read("data/mc_spawns_" .. map .. ".txt") or ""
  for line in txt:gmatch("[^\n]+") do
    local kind, x, y, z, yaw = line:match("^(%S+)%s+(%-?[%d%.]+)%s+(%-?[%d%.]+)%s+(%-?[%d%.]+)%s*(%-?[%d%.]*)")
    if kind then SP.list[kind] = SP.list[kind] or {}; table.insert(SP.list[kind], { tonumber(x), tonumber(y), tonumber(z), tonumber(yaw) or 0, custom = true }) end
  end
end
function M.spawns.get(kind) load_spawns(); return SP.list[kind or "any"] or {} end
function M.spawns.add(kind, x, y, z, yaw) load_spawns(); SP.list[kind] = SP.list[kind] or {}; table.insert(SP.list[kind], { x, y, z, yaw or 0, custom = true }) end
function M.spawns.save()
  load_spawns(); local out = {}
  for kind, l in pairs(SP.list) do for _, p in ipairs(l) do if p.custom then out[#out + 1] = string.format("%s %.1f %.1f %.1f %.1f", kind, p[1], p[2], p[3], p[4] or 0) end end end
  X.fs.write("data/mc_spawns_" .. X.map() .. ".txt", table.concat(out, "\n") .. "\n"); return #out
end
function M.spawns.clear(kind) load_spawns(); SP.list[kind] = {} end
-- MC.spawns.random(kind [, free_radius=48]) -> x, y, z, yaw  (a point with no living player/NPC within free_radius when possible)
function M.spawns.random(kind, free)
  local l = M.spawns.get(kind); if #l == 0 then return nil end
  free = free or 48; local order = {} for i = 1, #l do order[i] = i end; M.util.shuffle(order)
  for _, i in ipairs(order) do local p = l[i]; local busy = false
    for _, e in ipairs(E.find_sphere(p[1], p[2], p[3], free)) do if (is_player(e) and P.alive(e)) or npcs[e] then busy = true break end end
    if not busy then return p[1], p[2], p[3], p[4] or 0 end end
  local p = l[order[1]]; return p[1], p[2], p[3], p[4] or 0
end
function M.spawns.teleport(id, kind) local x, y, z, yaw = M.spawns.random(kind); if not x then return false end
  P.set_origin(id, x, y, z + 1); E.set(id, "angles", 0, yaw, 0); E.set(id, "v_angle", 0, yaw, 0); N.ev(id, "fixangle", 1); return true end
X.on("map_end", function() SP.loaded_for = nil end)

-- ------------------------------------------------------------------------------------------------ MC.player
M.player = {}
function M.player.respawn(id) if P.ingame(id) and (P.team(id) == 1 or P.team(id) == 2) then N.ham("cstrike_roundrespawn", id); return true end return false end
function M.player.freeze(id, on) local f = E.get(id, "flags"); E.set(id, "flags", on and (f | M.FL.FROZEN) or (f & ~M.FL.FROZEN)) end
function M.player.god(id, on) E.set(id, "takedamage", on and 0 or 2) end
function M.player.glow(id, r, g, b, amount) M.ent.glow(id, r, g, b, amount) end
function M.player.unglow(id) M.ent.unglow(id) end
function M.player.speed(id, v) E.set(id, "maxspeed", v); if v == nil then N.ham("cstrike_player_resetmaxspeed", id) end end
function M.player.next_attack(id, secs) return N.next_attack(id, secs) end
function M.player.give(id, item) return P.give_item(id, item) end
function M.player.strip(id, keep_knife) P.strip_weapons(id); if keep_knife ~= false then P.give_item(id, "weapon_knife") end end
function M.player.aim(id, dist) return M.util.aim(id, dist) end
function M.player.aim_ent(id, dist) local sx, sy, sz, ex, ey, ez = M.util.aim(id, dist or 8192); local tr = X.trace.line(sx, sy, sz, ex, ey, ez, 0, id); return tr.ent, tr end
function M.player.team_key(id) return M.teams.of(id) end
function M.player.alive_list() return M.util.alive_players() end

-- ------------------------------------------------------------------------------------------------ misc
function M.info()
  local r = N.rules_available()
  return { version = M.version, native = N.ham_list and true or false, rules = r[1], rules_source = r[2], regamedll = r[3], npcs = M.npc.count(), weapons = #W_order }
end
X.log(string.format("[modcreator] library %s loaded", M.version))
return M
