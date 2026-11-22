# ModCreator 1.0 — AI skills file (write whole Counter-Strike 1.6 mods with xmod + ModCreator)

Give this file to an AI together with `xmod_skills.md`. It describes **everything** ModCreator offers, the rules that keep a
server alive, and ready-made recipes. ModCreator = an xmod extension (C++, namespace `xmod.mc`) + a Lua framework
(`lib/modcreator.lua`, loaded with `local MC = require("modcreator")`). Works with **xmod 0.4.0 – 0.4.4+ without any core
update**, on **Windows and Linux**, with **ReGameDLL** (recommended) and the original game DLL.

---------------------------------------------------------------------------------------------------------------------------
## 0. The 12 rules (read first)

1. Install = `modcreator_mm_i386.so` / `modcreator_mm.dll` listed in `addons/metamod/plugins.ini` **after** xmod, plus
   `configs/extensions-modcreator.ini` (one line: `mc`). Check the server log for `[modcreator] ModCreator 1.0.0 ready`.
2. **Never call `xmod.mc.*` at the top level of a script** (xmod loads scripts before Metamod attaches the extension).
   The Lua library already respects this: `MC.npc.register`, `MC.weapons.register`, `MC.ent.class`, `MC.teams.define`,
   `MC.round.on`, `MC.objectives.add` are safe at load time. Everything that touches the game (spawn, give, damage,
   round.finish …) goes into events, commands, timers. `MC.ready()` tells whether the extension is up.
3. **Models/sounds must be precached.** Register NPCs/weapons/objectives at plugin load (they precache their files every
   map), or call `MC.precache.model/sound(path)` at load. `MC.ent.create` refuses a model ModCreator did not precache
   (that would be a Host_Error crash); use `MC.precache.mark(path)` for files the game precaches itself, or `unsafe_model=true`.
4. CS team numbers: **1 = T, 2 = CT, 3 = SPEC**. Win status numbers: **1 = CT won, 2 = T won, 3 = draw**. ModCreator APIs take
   team **keys** (`"t"`, `"ct"`, `"draw"`, or your own keys from `MC.teams.define`) so you rarely need the numbers.
5. Custom entities: create them on `info_target` (default base). Their `think`/`touch` come per classname, their
   `use`/`damage`/`killed` through Ham hooks on the base class, dispatched by classname.
6. An entity only thinks when `nextthink` is set; `MC.ent.class{think_interval=…}` keeps it going for you.
7. Every user callback runs protected (`MC.safe`): an error is logged, the framework keeps going.
8. Handlers of one hook all see the **original** arguments. Returning a number from `on_damage` changes the damage.
9. `xmod.mc.ham(...)` = AMXX `ExecuteHamB` (hooks fire). There is no "skip hooks" variant.
10. Keep per-frame work low: NPC AI runs at 10 Hz (`think=0.1`); 20–40 NPCs are fine, hundreds are not (edict limit too).
11. `MC.round.block_end(true)` (stop the GAME from ending rounds) needs ReGameDLL (`mp_round_infinite`); `MC.round.finish`
    works everywhere game rules were found (`MC.round.rules_ok()`).
12. Private data names (`m_iClip`, `m_pActiveItem` …) replace raw offsets: `xmod.mc.pdata_get(ent, "m_iClip")` is right on
    Windows/Linux, vanilla/ReGameDLL.

---------------------------------------------------------------------------------------------------------------------------
## 1. Skeleton of a complete mod

```lua
-- plugins/mymod/mymod.lua   (list "mymod/mymod.lua" in configs/plugins.ini or configs/plugins-mymod.ini)
plugin = { name = "mymod", version = "1.0", author = "me" }
local MC = require("modcreator")
local P, E = xmod.player, xmod.ent

MC.teams.define {                                   -- custom teams (map onto CS teams)
  { key = "humans", name = "Humans", cs = 2, color = { 60, 160, 255 } },
  { key = "aliens", name = "Aliens", cs = 1, color = { 120, 255, 60 } },
}
MC.teams.friendly_fire(false)

MC.npc.register("drone", { model = "models/mymod/drone.mdl", team = "aliens", health = 80, speed = 260,
  attack = { damage = 10, range = 60, rate = 1.0 }, reward = { frags = 1, money = 200 } })

MC.weapons.register("blaster", { base = "weapon_famas", name = "Blaster", v_model = "models/mymod/v_blaster.mdl",
  p_model = "models/mymod/p_blaster.mdl", w_model = "models/mymod/w_blaster.mdl", damage = 1.4, clip = 40, ammo = 160,
  rate = 0.09, sound = "weapons/mymod/blaster.wav", anims = { shoot = { 3, 4 } }, cost = 3000 })

MC.round.block_end(true)
MC.round.on("new", function(n)
  MC.npc.remove_all()
  MC.objectives.remove("core")
  MC.objectives.add { id = "core", name = "Alien Core", type = "destroy", model = "models/mymod/core.mdl",
    origin = { 0, 0, 0 }, health = 2000, team = "humans", round_win = true }
end)
MC.round.on("start", function()
  for i = 1, 6 do local x, y, z, yaw = MC.spawns.random("t"); if x then MC.npc.spawn("drone", x, y, z, yaw) end end
end)
MC.round.win_check(function() if MC.teams.alive("humans") == 0 then return "aliens" end end)
xmod.cmd.say("/guns", "", function(id) MC.weapons.menu(id) end)
```

---------------------------------------------------------------------------------------------------------------------------
## 2. `MC.ent` — entities

| function | what it does |
|---|---|
| `MC.ent.create(def)` → ent | creates (CREATE_NAMED_ENTITY on `def.base`, default `info_target`), keyvalues, spawns, then applies fields. def: `classname, base, model, origin={x,y,z}, angles, mins, maxs, solid, movetype, health, max_health, takedamage (true/false/0/1/2), gravity, friction, scale, skin, body, sequence, framerate, effects, flags, spawnflags, team, impulse, render={mode,amt,fx,r,g,b}, owner, velocity, kv={key=value}, nextthink, spawn=true, drop=false, unsafe_model=false` |
| `MC.ent.class(name, def)` | behaviour for a classname: `base="info_target", think_interval, on_spawn(e,def), on_think(e)` (return seconds = next think, `false` = stop), `on_touch(e, other)` (true blocks), `on_use(e, activator, caller, type, value)` (true blocks), `on_damage(e, inflictor, attacker, dmg, bits)` (number = new dmg, false = no damage), `on_killed(e, attacker, gib)` (true = keep it, the game does not remove it) |
| `MC.ent.class_of(e)` | its class def or nil |
| `MC.ent.data(e)` | per-entity Lua table (cleared by `MC.ent.remove`, when the class is killed, and on map change) |
| `MC.ent.remove(e)` | removes (not players) + clears data |
| `MC.ent.keyvalue(e, k, v)` | game DispatchKeyValue (map keys: `targetname`, `target`, `wait`, `speed`, `rendermode`, `spawnflags` …). Do it **before** spawn for keys read in Spawn (use `def.kv`) |
| `MC.ent.use(e, activator, caller, type, value)` | calls Use (buttons, doors, triggers, `multi_manager` …) |
| `MC.ent.touch(e, other)` / `MC.ent.think(e)` | call Touch / Think |
| `MC.ent.damage(victim, inflictor, attacker, dmg, bits, hitgroup)` | TakeDamage (players get m_LastHitGroup for headshot/armor) |
| `MC.ent.kill(e, attacker, gib)` / `MC.ent.heal(e, amount)` | Killed / TakeHealth |
| `MC.ent.find_all(classname)` / `MC.ent.in_sphere(x,y,z,r[,classname])` | lists |
| `MC.ent.set_think(e, delay)` | next think in `delay` s |
| `MC.ent.glow(e, r,g,b[,amount])` / `MC.ent.unglow(e)` | glow shell |
| `MC.ent.visible(e, target)` / `MC.ent.face(e, x,y,z[,instant])` | line of sight / turn |

Examples:
```lua
MC.ent.class("mymod_mine", { think_interval = 0.2,
  on_think = function(e) for _, id in ipairs(MC.util.alive_players()) do
      if MC.util.dist(e, id) < 90 then local x, y, z = E.get(e, "origin"); MC.fx.explosion(x, y, z)
        xmod.mc.radius_damage(x, y, z, 250, 120, e, E.get(e, "owner")); MC.ent.remove(e); return false end end end })
local mine = MC.ent.create { classname = "mymod_mine", model = "models/mymod/mine.mdl", origin = { x, y, z },
  mins = { -6, -6, 0 }, maxs = { 6, 6, 6 }, solid = MC.SOLID.BBOX, movetype = MC.MOVETYPE.TOSS, owner = id }
-- a breakable crate that drops money
MC.ent.class("crate", { on_killed = function(e, att) if MC.util.is_player(att) then xmod.player.set_money(att, xmod.player.money(att) + 500) end end })
MC.ent.create { classname = "crate", model = "models/mymod/crate.mdl", origin = { x, y, z }, mins = { -16, -16, 0 }, maxs = { 16, 16, 32 },
  solid = MC.SOLID.BBOX, health = 100, takedamage = true }
```
Constants: `MC.SOLID.NOT/TRIGGER/BBOX/SLIDEBOX/BSP`, `MC.MOVETYPE.NONE/WALK/STEP/FLY/TOSS/PUSH/NOCLIP/FLYMISSILE/BOUNCE/BOUNCEMISSILE/FOLLOW/PUSHSTEP`,
`MC.FL.*` (CLIENT MONSTER GODMODE NOTARGET ONGROUND FAKECLIENT FROZEN …), `MC.DMG.*` (BULLET SLASH BURN BLAST FALL SHOCK NERVEGAS …),
`MC.HIT.HEAD/CHEST/STOMACH/LEFTARM/RIGHTARM/LEFTLEG/RIGHTLEG`, `MC.USE.OFF/ON/SET/TOGGLE`, `MC.WIN.CT/T/DRAW`.

---------------------------------------------------------------------------------------------------------------------------
## 3. `MC.anim` — studio models

`play(e, seq_or_name[, rate=1[, restart=true]])` → duration (s) · `advance(e[, interval])` → finished (HLSDK StudioFrameAdvance, call
in think) · `lookup(e, name)` → index/-1 (case-insensitive) · `list(e)` → names · `info(e, seq)` → `{index,name,fps,frames,duration,loop,activity,actweight,motion,events}`
· `duration(e, seq)` · `by_activity(e, act)` (weighted random like LookupActivity) · `first(e, {names})` / `pick(e, {names})` → first / random existing
· `controller(e, 0..3, degrees)` (head turning, turrets) · `blending(e, 0..1, value)` (aim up/down) · `body(e, group[, value])` (sub-models)
· `attachment(e, n)` → x,y,z (muzzle) · `bone(e, index_or_name)` → x,y,z · `bones(e)` · `model(e)` → `{name, sequences, bones, attachments, bodyparts, controllers, eye}`.
The entity needs its model set (`modelindex`), else these return nil/-1.

---------------------------------------------------------------------------------------------------------------------------
## 4. `MC.npc` — NPCs and AI

`MC.npc.register(name, def)` (at load) · `MC.npc.spawn(name, x, y, z[, yaw[, overrides]])` → **npc object** · `MC.npc.get(ent)` → npc or nil
· `MC.npc.def(name)` · `MC.npc.all([name])` · `MC.npc.count([name])` · `MC.npc.remove_all([name])` · `MC.npc.relation(npc, other)`.

def fields (defaults): `model` (required) · `health=100` · `speed=200` u/s (walking = half) · `mins={-16,-16,0} maxs={16,16,72}` · `eye=64`
· `turn=25` deg/think · `think=0.1` · `gravity=1` · `scale, skin, body, render` · `team="monsters"` · `ai="hunter"` (`"hunter"` search & chase,
`"guard"` stay home within `leash`, `"wander"` random walk around home, `"follow"` follow `npc:follow(ent)`, `"idle"`, or `function(npc, dt)`)
· `sight=1500` · `fov=140` (≤220 units it notices you from any side) · `memory=6` s · `leash=400` · `aggressive=true` (false = only fights back)
· `movement="step"` (HL monster walking: stairs, slopes, MOVETYPE_STEP) or `"fly"` · `jump=260` (unstick jump) · `blood=247` · `corpse=5` s (`-1` keep, `0` remove now)
· `pain_chance=0.35` · `follow_dist=110` · `path_run`, `goal_run` (run instead of walk) · `reward={frags=1, money=300}` · `classname="npc_<name>"`
· `attack={damage=10, range=60, rate=1.2, delay=0.35 (hit moment), bits=DMG_SLASH, knockback=0, sound=…|{…}, ranged=false, spread=0.05, shots=1, attachment=nil}`
  (ranged: `range` default 800, shoots real bullets with `xmod.mc.fire_bullets` from the attachment or the eyes)
· `anims={idle, walk, run, attack, rattack, pain, death}` each a name, an index or a list (lists = random); missing ones fall back to common
  names (`idle`, `idle1`, `walk`, `run`, `attack1`, `flinch`, `die` …) and then to HL activities.
· `sounds={idle, alert, attack, pain, death}` (paths or lists, precached automatically)
· callbacks: `on_spawn(npc)` · `on_think(npc)` (true = skip built-in AI this tick) · `on_damage(npc, attacker, dmg, bits, inflictor)` (number/false)
  · `on_death(npc, attacker)` · `on_attack(npc, target)` (false cancels) · `on_enemy(npc, enemy)` · `relation(npc, other)` → `"enemy"|"ally"|"neutral"`.

npc object: fields `ent name def team ai state enemy home data dead`; methods `npc:origin()` `npc:alive()` `npc:health()` `npc:anim(kind_or_names[, rate[, restart]])`
`npc:move_to(x,y,z)` `npc:path({{x,y,z},…}[, loop])` `npc:follow(ent)` `npc:stop()` `npc:set_enemy(e)` `npc:sound(kind)` `npc:hurt(dmg, attacker)` `npc:kill(attacker)` `npc:remove()`.
Change behaviour at runtime: `npc.ai = "guard"`, `npc.team = "humans"`, `npc.def` is shared — use spawn overrides for per-NPC values.
Events: `mc:npc_spawned(ent, name)`, `mc:npc_killed(ent, name, attacker)`, `mc:npc_path_done(ent, name)`, `mc:npc_goal_reached(ent, name)`.
NPCs are FL_MONSTER + SOLID_SLIDEBOX → bullets use hitboxes (headshots), knockback works, CS bots ignore them.

```lua
MC.npc.register("boss", { model = "models/mymod/boss.mdl", health = 3000, speed = 160, scale = 1.4,
  attack = { damage = 40, range = 90, rate = 1.6, delay = 0.5, knockback = 400 },
  on_damage = function(npc, att, dmg) if npc:health() < 1000 and not npc.data.rage then npc.data.rage = true
      npc.def.speed = 260; MC.ent.glow(npc.ent, 255, 0, 0, 20) end return dmg end,
  on_death = function(npc, killer) MC.round.finish("humans", { message = "The boss is dead!" }) end })
-- escort: a friendly NPC walks a path, the round is won when it arrives
local vip = MC.npc.spawn("vip", x, y, z, 0, { team = "humans", ai = "idle", aggressive = false })
vip:path({ { 100, 200, 0 }, { 800, 200, 0 }, { 800, 900, 0 } })
xmod.on("mc:npc_path_done", function(e) if e == vip.ent then MC.round.finish("humans") end end)
```

---------------------------------------------------------------------------------------------------------------------------
## 5. `MC.round` — rounds

Events `MC.round.on(kind, fn)`: `"new"(n)` new round (players respawned, freeze begins; the first round of a map counts too) ·
`"start"(n)` freeze over · `"end"(winner_key, win_status, reason)` reason `"game"` (the game ended it) / `"mod"` / `"objective:<id>"` / `"win_check"` ·
`"second"(time_left)` each second while live. Same as xmod events `mc:round_new`, `mc:round_start`, `mc:round_end`.

| function | |
|---|---|
| `finish(winner[, opts])` → bool | ends the round NOW (TerminateRound). winner: `"ct"`, `"t"`, `"draw"`, 1/2/3 (win status) or a team key. opts: `delay=5, message=nil` (default "#CTs_Win"… or "<Team> win!"; a `#Key` is a CS localized TextMsg, anything else a HUD text; `false` = none), `sound` (default the radio win sound; path or `false`), `score=true`, `reason="mod"` |
| `draw(opts)` | finish("draw") |
| `block_end(true/false)` → method | the game stops ending rounds by itself (eliminations, time, bomb, hostages) — ReGameDLL `mp_round_infinite`, else the `reapi:RoundEnd` hook; returns nil if impossible |
| `win_check(fn[, interval=0.5])` | fn() → `winner[, opts]` or nil, checked while live |
| `time_left()` / `set_time(s)` / `add_time(s)` | round timer (HUD updated) |
| `scores()` → ct, t · `set_scores(ct, t)` | team round wins (TeamScore sent) |
| `number()` · `state()` (`warmup/freeze/live/ended`) · `is_live()` · `info()` (raw `xmod.mc.round_state()`) · `rules_ok()` | |
| `restart_game(delay)` | sv_restart |

---------------------------------------------------------------------------------------------------------------------------
## 6. `MC.teams` — teams

`define({ {key, name, cs=1|2|nil, color={r,g,b}}, … })` · `list()` · `get(key)` · `name(key)` · `color(key)` → r,g,b · `of(id_or_ent)` → key (explicit assignment, else by CS team: the first defined team
with that `cs`, else `"t"`/`"ct"`/`"spec"`; NPCs → their team) · `cs(key)` · `assign(id, key[, {respawn=true, silent=true}])` (moves the CS team when `cs` is set)
· `on_change(fn(id, old, new))` (+ event `mc:team_change`) · `players(key[, alive_only])` · `count(key[, alive])` · `alive(key)`
· `relation(a, b)` → `"ally"` (same) / `"enemy"` / `"neutral"` (spec) · `set_relation(a, b, rel[, one_way])` · `friendly_fire(false|true|nil)` (false: allies cannot hurt each other, NPCs included; nil: mp_friendlyfire)
· `score(key)` / `add_score(key, n)` (+ `mc:team_score`) / `set_score` / `reset_scores` · `balance([keys])` · `swap()`.
More than two teams: several keys can share a CS team (`cs=2` for "red" and "blue" squads); damage/relations follow the keys.

---------------------------------------------------------------------------------------------------------------------------
## 7. `MC.objectives`

`add(def)` · `get(id)` · `list()` · `remove(id)` · `reset([id])` · `complete(id[, team])` · `progress(id[, team])` · `status_text()` · `hud_style={r,g,b,x,y,channel}`.
Common fields: `id`, `name`, `type`, `team` (who completes it; nil = any), `round_win` (default true when `team` is set), `reset_each_round=true`, `hud=true`
(1 Hz HUD list on channel 4), `always=false` (also tick outside live rounds), `active=true`, `on_progress(obj, team, p)`, `on_complete(obj, team)`.
Types:
- `capture` — `origin, radius=128, time=10, contested=true, decay=0.5, recapture=true, initial_owner, hold_points` (team score/s while owned), `beacon=true`
  (ring effect in team colour), `single` (complete on first capture even without team). Several capturers = faster (up to x4).
- `destroy` — `ent` (existing, e.g. a func_breakable) or `model, origin, health=500, mins, maxs` (created as `mc_objective`).
- `reach` — `origin+radius` or `mins/maxs`; `count=1|"all"` players of the team inside.
- `collect` — `items={{x,y,z},…}, model, glow={r,g,b}, count=#items`.
- `kills` — `count`, `npc="<name>"` (only that NPC), `count_npcs=true` (NPC kills count too when no `npc`).
- `survive` — `time` (the team must keep living players).
- `custom` — `check(obj)` → progress 0..1 or true (+ optional team).
Events: `mc:objective_progress(id, team, p)`, `mc:objective_complete(id, team)`, `mc:objective_captured(id, team)`, `mc:objective_collect(id, player)`.

---------------------------------------------------------------------------------------------------------------------------
## 8. `MC.weapons` — custom weapons (WeaponMod style)

`register(key, def)` (at load) · `give(id, key[, switch])` → wpn (replaces a weapon of the same base) · `switch(id, wpn)` · `strip(id, wpn_or_classname)`
· `get(wpn)` → def · `of(id)` → def, wpn (in hands) · `is(wpn, key)` · `list()` · `def(key)` · `shoot(id, {damage, spread, shots, distance, bits, tracer})` → rows
· `menu(id[, filter])` (buy menu, `cost`, `team`) · `zoom(id, fov)`.
def: `base` (required, a CS weapon) · `name` · `v_model p_model w_model` · `damage` (multiplier) or `damage_fixed` · `clip` (magazine weapons, not m3/xm1014)
· `ammo` (backpack given) · `rate` (s between shots) · `recoil` (punch multiplier) · `speed` (max speed held) · `knockback` · `anim_ext` · `sound` (fire sound, path/list)
· `anims={shoot={…}, reload=n, draw=n}` · `reload_time` · `cost` · `team` · `block_events` (auto when `sound`, `anims.shoot` or `fire`: hides the base weapon's own client
shot sound/animation and turns off client weapon prediction for the holder) · `fire` (function(id, wpn, def) or a `shoot` table: replaces the shot — clip, rate,
recoil, muzzle flash, sound, anim are handled for you) · `on_attack(id, wpn)` (true blocks) · `on_secondary(id, wpn)` (true blocks the base secondary)
· `on_deploy(id, wpn)` · `on_reload(id, wpn)` (false blocks) · `on_hit(attacker, victim, dmg, wpn)` (number/false; works on players and NPCs).
How it works: the weapon entity's `pev->impulse` holds the weapon's key number (it survives drop/pickup), Ham hooks on the base class do the rest,
the dropped weaponbox gets `w_model` (engine SetModel hook). Damage multipliers apply to bullets of the holder (inflictor == attacker, DMG_BULLET).
Event: `mc:weapon_given(id, key, wpn)`.

```lua
MC.weapons.register("freeze_gun", { base = "weapon_ump45", name = "Freeze Gun", damage = 0.5, cost = 4000,
  on_hit = function(att, victim, dmg) if MC.util.is_player(victim) then MC.player.freeze(victim, true); MC.ent.glow(victim, 0, 128, 255)
      xmod.timer.after(1.5, function() if xmod.player.ingame(victim) then MC.player.freeze(victim, false); MC.ent.unglow(victim) end end) end
    return dmg end })
MC.weapons.register("railgun", { base = "weapon_scout", name = "Railgun", clip = 4, ammo = 20, rate = 1.2, sound = "weapons/mymod/rail.wav",
  fire = function(id, wpn, def) local sx, sy, sz, ex, ey, ez, dx, dy, dz = MC.util.aim(id, 8192)
    local rows = xmod.mc.fire_bullets(id, id, sx, sy, sz, dx, dy, dz, 0, 8192, 120, 1)
    MC.fx.beam(sx, sy, sz - 4, rows[1][2], rows[1][3], rows[1][4], 0, 200, 255, { width = 20, life = 0.4 }) end })
```

---------------------------------------------------------------------------------------------------------------------------
## 9. `MC.fx`, `MC.hud`, `MC.spawns`, `MC.player`, `MC.util`, `MC.precache`

- `MC.fx`: `beam(x1,y1,z1, x2,y2,z2, r,g,b[, {sprite,width,life,noise,alpha,speed}])` · `beam_ents(e1,e2, r,g,b[,opts])` · `trail(e, r,g,b[,opts])` ·
  `ring(x,y,z, radius, r,g,b[, life10])` · `explosion(x,y,z[, {sprite,scale,framerate,silent}])` · `sprite(x,y,z, sprite[, scale, alpha])` · `smoke` · `sparks`
  · `tracer(x1,y1,z1,x2,y2,z2)` · `light(x,y,z[, radius, r,g,b, life, decay])` · `implosion` · `lava_splash` · `teleport` · `blood(x,y,z[, dx,dy,dz, color, amount])`
  · `decal(x,y,z[, name, ent])` · `shake(id|0, amp, secs, freq)` · `fade(id|0, secs, hold, r,g,b,a)`. Sprites: path (precached via MC.precache) or index.
  Built in: `sprites/laserbeam.spr`, `sprites/shockwave.spr`, `sprites/zerogxplode.spr`, `sprites/steam1.spr`.
- `MC.hud`: `text(id|0, text, opts)` · `center(id|0, text)` · `line(id, text[, opts])` (status line, channel 2, refresh each second) · `bar(id|0, seconds)` (CS progress bar)
  · `status_icon(id|0, name, on|2=flash, r,g,b)` · `timer(seconds)` · `hide(id, flags)`.
- `MC.spawns`: `get(kind)` (`"t"`, `"ct"`, `"any"`, your kinds) → `{{x,y,z,yaw},…}` · `random(kind[, free_radius=48])` → x,y,z,yaw (a free one when possible)
  · `teleport(id, kind)` · `add(kind, x,y,z,yaw)` · `save()` (→ `data/mc_spawns_<map>.txt`, loaded automatically) · `clear(kind)`.
- `MC.player`: `respawn(id)` (RoundRespawn) · `freeze(id, on)` · `god(id, on)` · `glow/unglow` · `speed(id, v|nil)` · `next_attack(id[, s])` · `give(id, item)`
  · `strip(id[, keep_knife=true])` · `aim(id[, dist])` → sx,sy,sz, ex,ey,ez, dx,dy,dz · `aim_ent(id[, dist])` → ent, trace · `team_key(id)` · `alive_list()`.
- `MC.util`: `dist3 dist2 dist(a,b) origin(e) center(e) aim(id) is_player(e) valid(e) alive_players() shuffle(t) safe(where, fn, ...)`.
- `MC.precache`: `model/sound/generic(path)` → index (at load: registered for every map; in `precache`: now) · `index(path)` · `ok(path)` · `mark(path[, index])`.
- `MC.info()` → `{version, rules, rules_source, regamedll, npcs, weapons}` · `MC.ready()` · `MC.version` · `MC.native` (= xmod.mc).

---------------------------------------------------------------------------------------------------------------------------
## 10. Native layer `xmod.mc.*` (C++)

Vectors come back as arrays `{x,y,z}`; rows as arrays. Entity arguments are indexes (0 = world). Errors are Lua errors with a clear message.

**Virtual calls (ExecuteHamB):** `ham(name, ent, ...)` — any function of `ham_list()` (same names as xmod's `ham:*` events: `takedamage`
`(inflictor, attacker, dmg, bits)`, `killed(attacker, gib)`, `takehealth(amount, bits)`, `use(activator, caller, type, value)`, `touch(other)`, `think`,
`blocked(other)`, `classify`, `bloodcolor`, `isalive`, `isplayer`, `objectcaps`, `respawn`, `addplayeritem(item)`, `removeplayeritem(item)`,
`giveammo(amount, name, max)`, `item_deploy`, `item_holster(skiplocal)`, `item_kill`, `weapon_primaryattack`, `weapon_secondaryattack`,
`weapon_reload`, `weapon_retireweapon`, `cstrike_roundrespawn`, `cstrike_player_resetmaxspeed`, `cstrike_item_getmaxspeed` …). Returns int/float/string/ent or nil.
**Entities:** `create(classname)` (not spawned) · `keyvalue(ent, key, value[, classname])` → handled · `ev(ent, field[, value])` extra entvars:
`ideal_yaw yaw_speed pitch_speed idealpitch weaponanim dmg_take dmg_save light_level air_finished pain_finished radsuit_finished teleport_time gamestate
impacttime starttime flFallVelocity bInDuck flTimeStepSound flSwimTime flDuckTime iStepLeft pushmsec armortype fixangle maxspeed deadflag controller0..3 blending0..1`
· `alloc_string(s)` / `string(i)` · `entity_count()` → `{used, max}`.
**Movement / sight:** `walk_move(ent, yaw, dist[, mode])` → moved · `move_to_origin(ent, x,y,z, dist[, type])` · `drop_to_floor(ent)` → 1/0/-1 · `on_floor(ent)`
· `change_yaw(ent)` · `face(ent, x,y,z[, instant])` · `vec_to_yaw(x,y,z)` · `vec_to_angles(x,y,z)` · `angle_vectors(p,y,r)` → 9 numbers (forward,right,up)
· `visible(ent, target)` · `visible_point(ent, x,y,z)` · `in_view_cone(ent, x,y,z[, fov=90])` · `illum(ent)`.
**Studio:** `model_info seq_lookup seq_list seq_info seq_by_activity anim frame_advance seq_duration seq_fps set_controller set_blending set_body get_body attachment bone bone_list` (see §3).
**Combat:** `fire_bullets(attacker, inflictor, sx,sy,sz, dx,dy,dz, spread, distance, damage[, shots=1[, bits=BULLET|NEVERGIB[, fx=true]]])` →
`{{ent,x,y,z,hitgroup,damage},…}` (CS hit group multipliers head x4 / stomach x1.25 / legs x0.75, blood, wall decals) ·
`radius_damage(x,y,z, radius, damage, inflictor, attacker[, bits=BLAST[, falloff=true[, ignore_ent[, los=true]]]])` → `{{ent, damage},…}` ·
`damage(victim, inflictor, attacker, dmg[, bits[, hitgroup]])` · `blood(x,y,z, dx,dy,dz[, color, amount])` · `decal(x,y,z[, name[, ent]])` · `blood_color(ent)`.
**Private data by name:** `pdata_get(ent, member[, index])` · `pdata_set(ent, member, value[, index])` (vectors: `pdata_set(e, m, x, y, z)`) ·
`pdata_info(member[, rules])` → `{class, member, type, count, offset, windows, linux, offset4, linux_diff4}` · `pdata_list([class_or_prefix[, rules]])` ·
`raw_get(ent, byte_offset, type)` / `raw_set(ent, byte_offset, type, value)` (types `int float bool short char entity ehandle vector string_t`).
Member names = AMX Mod X gamedata (`m_iClip`, `m_pActiveItem`, `m_rgAmmo`, `m_iTeam`, `m_iAccount`, `m_flNextAttack`, `m_iFOV`, `m_bHasDefuser`,
`m_iKevlar`, `m_LastHitGroup`, `m_szAnimExtention`, `m_pPlayer`, `m_iId`, `m_flNextPrimaryAttack`, `m_fInReload`, `m_iShotsFired`, …). Ambiguous
names: `"Class.member"` (e.g. `"CWeaponBox.m_rgpPlayerItems"`); bare names prefer CBasePlayer, CBasePlayerWeapon, CBasePlayerItem, CBaseMonster … .
Entity pointers/EHANDLEs read/write as entity indexes.
**Game rules:** `rules_available()` → `{ok, source, regamedll}` · `rules_get(member[, index])` / `rules_set(member, value[, index])` (76 CHalfLifeMultiplay /
CGameRules members: `m_iRoundTime m_iRoundTimeSecs m_iIntroRoundTime m_fRoundCount m_bFreezePeriod m_iNumCTWins m_iNumTerroristWins m_iRoundWinStatus
m_bRoundTerminating m_fTeamCount (restart time) m_bCompleteReset m_iAccountCT m_iAccountTerrorist m_bCTCantBuy m_bTCantBuy m_iMaxRounds
m_iTotalRoundsPlayed m_bMapHasBuyZone m_bMapHasBombTarget m_iC4Timer m_iLimitTeams m_bFirstConnected …`; ReGameDLL's extra 8 bytes are handled)
· `round_terminate(delay, win_status)` · `round_state()` → `{terminating, freeze, win_status, restart_at, round_start, round_time, time_left, ct_wins, t_wins, rounds_played, game_started}`.
**Weapons:** `weapon_active(id)` · `weapon_owner(wpn)` · `weapon_list(id)` · `weapon_find(id, classname_or_impulse)` · `weapon_info(wpn)` →
`{id, classname, owner, clip, ammo_type, bpammo, in_reload, next_primary, next_secondary, idle, impulse}` · `weapon_clip(wpn[, n])` · `bpammo(id, ammo_type_or_wpn[, n])`
· `weapon_times(wpn[, primary, secondary, idle])` (relative seconds) · `next_attack(id[, s])` · `weapon_anim(id, anim[, body])` · `anim_ext(id[, ext])`
· `custom_weapon(impulse, {wmodel=, block_events=} | nil)`.

---------------------------------------------------------------------------------------------------------------------------
## 11. AMXX → ModCreator

| AMXX | ModCreator |
|---|---|
| `create_entity("info_target")` + `entity_set_*` | `MC.ent.create{…}` |
| `register_think("cls", "fn")` / `register_touch` | `MC.ent.class("cls", {on_think=, on_touch=})` |
| `RegisterHam(Ham_TakeDamage, "info_target", …)` filtered by classname | `MC.ent.class("cls", {on_damage=})` |
| `ExecuteHamB(Ham_Use, ent, id, id, 1, 1.0)` | `MC.ent.use(ent, id)` / `xmod.mc.ham("use", ent, id, id, 1, 1)` |
| `ExecuteHamB(Ham_TakeDamage, …)` / `ExecuteHamB(Ham_CS_RoundRespawn, id)` | `MC.ent.damage(…)` / `MC.player.respawn(id)` |
| `DispatchKeyValue(ent, "k", "v")` | `MC.ent.keyvalue(ent, "k", "v")` / `def.kv` |
| `engfunc(EngFunc_WalkMove / MoveToOrigin / DropToFloor)` | `xmod.mc.walk_move / move_to_origin / drop_to_floor` |
| `lookup_sequence` / `set_controller` / `engfunc(EngFunc_GetAttachment / GetBonePosition)` | `MC.anim.lookup / controller / attachment / bone` |
| `get_pdata_int(wpn, 51, 4)` / `set_pdata_float(id, 83, t, 5)` | `xmod.mc.pdata_get(wpn, "m_iClip")` / `xmod.mc.pdata_set(id, "m_flNextAttack", t)` |
| `cs_get_weapon_ammo` / `cs_set_user_bpammo` | `xmod.mc.weapon_clip(wpn[, n])` / `xmod.mc.bpammo(id, wpn, n)` |
| `ham_strip_weapon` / `fm_give_item` + clip | `MC.weapons.strip(id, "weapon_x")` / `MC.weapons.give(id, key)` |
| `rg_round_end(5.0, WINSTATUS_CTS, ROUND_CTS_WIN)` | `MC.round.finish("ct", {delay = 5})` |
| `rg_update_teamscores(1, 0)` | `MC.round.set_scores(ct + 1, t)` |
| `get_member_game(m_bFreezePeriod)` / `set_member_game(m_iRoundTimeSecs, 300)` | `xmod.mc.rules_get("m_bFreezePeriod")` / `MC.round.set_time(300)` |
| `register_logevent("Round_Start", 2, …)` / `register_event("HLTV", …, "a", "1=0", "2=0")` | `MC.round.on("start" / "new", fn)` |
| WeaponMod `wpn_register_weapon` | `MC.weapons.register(key, def)` |
| `fakemeta_util fm_radius_damage` | `xmod.mc.radius_damage(...)` |

---------------------------------------------------------------------------------------------------------------------------
## 12. Pitfalls and answers

- "attempt to call … not available: the extension providing 'mc' is not loaded" at load → you called a native at the top level (rule 2), or the
  extension line is missing / before xmod in plugins.ini.
- NPC does not move: it needs ground (`drop_to_floor` is done at spawn; spawning inside a wall/solid fails), `movement="step"` only climbs 18-unit steps;
  use `movement="fly"` for flyers. Check `npc.state`/`npc.enemy`.
- NPC animations wrong: check names with `MC.anim.list(npc.ent)`; give `anims={…}` explicitly. Player models (`models/player/x/x.mdl`) have
  `idle1 walk run ref_shoot_knife ref_shoot_ak47 gut_flinch head_flinch death1 death2 death3 head gutshot back`.
- Custom weapon shows the base weapon's fire sound for the shooter: give it a `sound` (or `block_events=true`).
- `MC.round.finish` returns false: the round already ended (`MC.round.state() == "ended"`).
- Several plugins changing the same damage: each handler sees the original damage; coordinate in one place (an `on_damage` / `on_hit`).
- Entity limit: `xmod.mc.entity_count()`; NPC corpses are removed after `corpse` seconds.
- Windows vs Linux: nothing to do — offsets, calling conventions and vtables are handled natively.
