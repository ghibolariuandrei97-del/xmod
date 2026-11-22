-- ModCreator test plugin for the real-game mock (tests/mock_mc.cpp). Not part of the package.
local MC = require("modcreator")
local N, E, P = xmod.mc, xmod.ent, xmod.player
local pass, fail = 0, 0
local function ok(c, name, extra) if c then pass = pass + 1 else fail = fail + 1; xmod.log("MCT FAIL " .. name .. (extra and (" -> " .. tostring(extra)) or "")) end end
local function near(a, b, eps) return a and b and math.abs(a - b) <= (eps or 0.01) end

-- ---- load time
ok(N ~= nil and MC.version == "1.0.0", "native + version")
local thinks, used, dmg_seen, killed_cls = 0, 0, 0, 0
MC.ent.class("mct_box", { think_interval = 0.5,
  on_think = function(e) thinks = thinks + 1 end,
  on_use = function(e, act) used = used + 1 end,
  on_damage = function(e, inf, att, dmg) dmg_seen = dmg; return dmg * 2 end,
  on_killed = function(e) killed_cls = killed_cls + 1 end })
MC.npc.register("grunt", { model = "models/mc_test.mdl", health = 60, speed = 250, attack = { damage = 4, range = 70, rate = 0.5, delay = 0.1 },
  reward = { frags = 1, money = 100 }, corpse = 1, sounds = { death = { "mct/die.wav" } } })
MC.weapons.register("plasma", { base = "weapon_ak47", name = "Plasma", v_model = "models/v_plasma.mdl", p_model = "models/p_plasma.mdl", w_model = "models/w_plasma.mdl",
  damage = 2.0, clip = 50, ammo = 120, rate = 0.2, sound = "weapons/plasma.wav", cost = 100 })
MC.weapons.register("laser", { base = "weapon_m4a1", name = "Laser", clip = 30, ammo = 60, rate = 0.15, v_model = "models/v_plasma.mdl",
  fire = { damage = 25, spread = 0, shots = 1, tracer = true }, anims = { shoot = { 3, 4 } } })
MC.npc.register("archer", { model = "models/mc_test.mdl", health = 80, speed = 300, ai = "idle", aggressive = false, attack = { ranged = true, damage = 7, range = 900, rate = 0.4 } })
MC.teams.define { { key = "humans", name = "Humans", cs = 2, color = { 0, 128, 255 } }, { key = "raiders", name = "Raiders", cs = 1, color = { 255, 0, 0 } } }
local ev = { new = 0, start = 0, ["end"] = 0, winner = nil, npc_killed = 0, obj = nil }
MC.round.on("new", function(n) ev.new = ev.new + 1 end)
MC.round.on("start", function(n) ev.start = ev.start + 1 end)
MC.round.on("end", function(w, cs, reason) ev["end"] = ev["end"] + 1; ev.winner = w; ev.reason = reason end)
xmod.on("mc:npc_killed", function(e, name, att) ev.npc_killed = ev.npc_killed + 1; ev.npc_killer = att end)
xmod.on("mc:objective_complete", function(id, team) ev.obj = id .. ":" .. team end)

local box, npc, w, npc2
xmod.cmd.server("mct_phase1", function()
  local i = N.pdata_info("m_iClip"); ok(i and i.offset == 220 and i.windows == 204 and i.offset4 == 51 and i.linux_diff4 == 4, "pdata_info m_iClip", i and i.offset)
  ok(N.pdata_info("CBasePlayer.m_iTeam").offset == 476, "m_iTeam linux offset")
  ok(N.pdata_info("m_iNumCTWins", true).offset == 120, "rules m_iNumCTWins linux ReGameDLL (+8)", N.pdata_info("m_iNumCTWins", true).offset)
  ok(N.ham_list():find("takedamage") ~= nil, "ham_list")
  ok(#N.pdata_list("CWeaponBox") >= 5, "pdata_list")
  ok(not pcall(N.pdata_get, 0, "nope"), "pdata_get bad member errors")
  ok(MC.round.block_end(true) == "mp_round_infinite" and xmod.cvar.get("mp_round_infinite") == "1", "round.block_end (ReGameDLL cvar)")
  local r = N.rules_available(); ok(r[1], "rules available", r[2]); xmod.log("MCT rules via " .. tostring(r[2]) .. " regamedll=" .. tostring(r[3]))
  ok(N.rules_get("m_iRoundTime") == math.floor(xmod.cvar.num("mp_roundtime") * 60), "m_iRoundTime == mp_roundtime*60 (layout)", N.rules_get("m_iRoundTime"))
  ok(N.rules_get("m_iIntroRoundTime") == math.floor(xmod.cvar.num("mp_freezetime")), "m_iIntroRoundTime == mp_freezetime", N.rules_get("m_iIntroRoundTime"))
  local st = MC.round.info(); ok(st and st.round_time > 0, "round_state")
  ok(P.ingame(1) and P.ingame(2), "players in game")
  xmod.log(string.format("MCT players: 1 team=%d alive=%s hp=%s | 2 team=%d alive=%s", P.team(1), tostring(P.alive(1)), tostring(P.health(1)), P.team(2), tostring(P.alive(2))))
  xmod.log(string.format("MCT p1 joining=%d menu=%d spawns=%d deadflag=%d flags=%d", N.pdata_get(1, "m_iJoiningState"), N.pdata_get(1, "m_iMenu"), N.pdata_get(1, "m_iNumSpawns"), E.get(1, "deadflag"), E.get(1, "flags")))
  ok(N.pdata_get(1, "m_iTeam") == P.team(1), "pdata m_iTeam == player.team", N.pdata_get(1, "m_iTeam"))
  ok(MC.teams.of(1) == "humans" and MC.teams.of(2) == "raiders", "team keys from CS teams", MC.teams.of(1))
  ok(#N.weapon_list(1) >= 1, "weapon_list", #N.weapon_list(1))
  ok(N.weapon_find(1, "weapon_knife") ~= 0, "weapon_find knife")
  local act = N.weapon_active(1); ok(act ~= 0, "weapon_active"); if act ~= 0 then local inf = N.weapon_info(act); ok(inf.owner == 1, "weapon_info.owner", inf.owner) end
  -- entity class
  box = MC.ent.create { classname = "mct_box", model = "models/mc_test.mdl", origin = { 100, 200, 0 }, mins = { -8, -8, 0 }, maxs = { 8, 8, 16 }, solid = 2, health = 50, takedamage = true }
  ok(E.valid(box) and E.classname(box) == "mct_box", "MC.ent.create")
  ok(N.keyvalue(box, "targetname", "boxy") and E.get(box, "targetname") == "boxy", "keyvalue (game DispatchKeyValue)")
  MC.ent.damage(box, 1, 1, 10); ok(dmg_seen == 10 and near(E.get(box, "health"), 30), "class on_damage doubles (ham takedamage, real vtable)", E.get(box, "health"))
  MC.ent.use(box, 1); ok(used == 1, "class on_use (ham use)")
  ok(N.ham("classify", box) ~= nil, "ham classify returns")
  ok(N.ham("isplayer", 1) == 1 and N.ham("isplayer", box) == 0, "ham isplayer player/box")
  -- studio
  local mi = MC.anim.model(box); ok(mi and mi.sequences == 5 and mi.bones == 3, "model_info")
  ok(MC.anim.lookup(box, "run") == 1 and MC.anim.lookup(box, "RUN") == 1 and MC.anim.lookup(box, "nope") == -1, "seq_lookup")
  ok(near(MC.anim.play(box, "attack1"), 0.5), "anim duration", MC.anim.play(box, "attack1"))
  ok(E.get(box, "sequence") == 2 and near(E.get(box, "framerate"), 1), "anim sets sequence")
  ok(N.frame_advance(box, 0.25) == false and near(E.get(box, "frame"), 128, 0.6), "frame_advance half", E.get(box, "frame"))
  ok(N.frame_advance(box, 0.3) == true and near(E.get(box, "frame"), 255), "frame_advance finished (no loop)")
  MC.anim.play(box, "idle"); ok(N.frame_advance(box, 1.5) == true and E.get(box, "frame") < 256, "loop wraps")
  ok(near(MC.anim.controller(box, 0, 45), 45, 0.5) and N.ev(box, "controller0") == 191, "set_controller", N.ev(box, "controller0"))
  ok(MC.anim.body(box, 1, 2) and MC.anim.body(box, 1) == 2, "body groups")
  local ax, ay, az = MC.anim.attachment(box, 0); ok(near(ax, 110) and near(az, 50), "attachment")
  local bx, by, bz = MC.anim.bone(box, "Bip01 Head"); ok(near(bz, 40), "bone by name", bz)
  ok(N.seq_by_activity(box, 4) == 1, "seq_by_activity ACT_RUN")
  MC.anim.play(box, "attack1"); ok(near(MC.anim.blending(box, 0, 0), 0, 0.5) and N.ev(box, "blending0") == 127, "set_blending", N.ev(box, "blending0"))
  -- weapons
  w = MC.weapons.give(1, "plasma", true); ok(w ~= 0 and N.weapon_active(1) == w, "weapons.give + switch", N.weapon_active(1))
  if w ~= 0 then
    ok(E.get(w, "impulse") == MC.weapons.def("plasma").impulse and MC.weapons.is(w, "plasma"), "custom impulse")
    ok(N.weapon_clip(w) == 50 and N.bpammo(1, w) == 120, "custom clip + ammo", N.weapon_clip(w) .. "/" .. N.bpammo(1, w))
    ok(N.pdata_get(w, "m_iId") == 28, "pdata m_iId AK47", N.pdata_get(w, "m_iId"))
    ok(N.weapon_owner(w) == 1, "weapon_owner")
  end
  -- npc
  npc = MC.npc.spawn("grunt", 300, 0, 0, 180)
  ok(npc and E.valid(npc.ent) and MC.npc.get(npc.ent) == npc, "npc.spawn")
  ok(E.get(npc.ent, "flags") & MC.FL.MONSTER ~= 0 and E.get(npc.ent, "movetype") == 4, "npc flags/movetype")
  ok(npc.seq.run == 1 and npc.seq.attack == 2 and npc.seq.death == 3, "npc sequences resolved")
end)
xmod.cmd.server("mct_phase2", function()
  ok(thinks >= 4, "class think_interval", thinks)
  local x = E.get(npc.ent, "origin"); ok(math.abs(x - 300) > 50, "npc moved (hunter)", x)
  xmod.log(string.format("MCT npc enemy=%s state=%s x=%.0f  p1 hp=%s p2 hp=%s", tostring(npc.enemy), npc.state, x, tostring(P.health(1)), tostring(P.health(2))))
  ok(npc.enemy == 1 or npc.enemy == 2, "npc found an enemy", npc.enemy)
  ok((P.health(1) < 100) or (P.health(2) < 100), "npc hurt a player")
  -- bullets from player 1 into the npc
  local nx, ny, nz = E.get(npc.ent, "origin"); local px, py, pz = P.origin(1)
  local money0, frags0 = P.money(1), P.frags(1)
  local rows = N.fire_bullets(1, 1, px, py, pz + 10, nx - px, ny - py, (nz + 30) - (pz + 10), 0, 4096, 200, 1)
  ok(rows[1][1] == npc.ent, "fire_bullets hit the npc", rows[1][1])
  ok(npc.dead == true and ev.npc_killed == 1 and ev.npc_killer == 1, "npc killed + event", ev.npc_killed)
  ok(P.frags(1) == frags0 + 1 and P.money(1) == math.min(16000, money0 + 100), "npc reward", P.money(1) - money0)
  ok(E.valid(npc.ent) and E.get(npc.ent, "sequence") == 3, "corpse plays death anim")
  npc2 = MC.npc.spawn("grunt", -100, 300, 0, 0, { health = 500, ai = "idle" })
  local hits = N.radius_damage(-100, 300, 20, 200, 100, 1, 1)
  ok(#hits >= 1 and E.get(npc2.ent, "health") < 500 and E.get(npc2.ent, "health") > 400, "radius_damage falloff", E.get(npc2.ent, "health"))
end)
xmod.cmd.server("mct_phase3", function()
  ok(not E.valid(npc.ent) or E.classname(npc.ent) ~= "npc_grunt", "corpse removed after corpse time")
  ok(killed_cls == 0, "box still alive")
  MC.ent.damage(box, 1, 1, 100); ok(killed_cls == 1, "class on_killed")
  local e0 = ev["end"]
  ok(MC.round.finish("humans", { delay = 2 }), "round.finish")
  xmod.log(string.format("MCT rules after finish: win=%s term=%s ct=%s t=%s", tostring(N.rules_get("m_iRoundWinStatus")), tostring(N.rules_get("m_bRoundTerminating")), tostring(N.rules_get("m_iNumCTWins")), tostring(N.rules_get("m_iNumTerroristWins"))))
  ok(N.rules_get("m_iRoundWinStatus") == 1 and N.rules_get("m_bRoundTerminating") == true, "round_terminate wrote the rules")
  local ct = MC.round.scores(); ok(ct >= 1, "CT score added", ct)
  ok(ev["end"] == e0 + 1 and ev.winner == "humans" and ev.reason == "mod", "round end event", ev.winner)
  ok(not MC.round.finish("raiders"), "second finish refused")
  ev.new0, ev.start0 = ev.new, ev.start
end)
xmod.cmd.server("mct_phase4", function()
  ok(ev.new > ev.new0, "new round event after terminate (HLTV)", ev.new .. " vs " .. ev.new0)
  ok(ev.start > ev.start0, "round start event (freeze over)", ev.start)
  ok(MC.round.state() == "live", "round live", MC.round.state())
  ok(MC.round.set_time(90) and math.abs(MC.round.time_left() - 90) < 1, "round.set_time", MC.round.time_left())
  local px, py, pz = P.origin(1)
  MC.objectives.add { id = "hill", name = "Hill", type = "capture", origin = { px, py, pz }, radius = 100, time = 1, team = "humans", round_win = true }
  MC.objectives.add { id = "kills", name = "Kill 1", type = "kills", count = 1, team = "raiders", round_win = false }
end)
xmod.cmd.server("mct_phase5", function()
  local o = MC.objectives.get("hill")
  ok(o.done and o.winner == "humans" and ev.obj == "hill:humans", "capture objective completed", ev.obj)
  ok(ev.winner == "humans" and ev.reason == "objective:hill", "objective ended the round", ev.reason)
  ok(MC.objectives.status_text():find("Hill") ~= nil, "status_text")
  ok(MC.teams.relation("humans", "raiders") == "enemy" and MC.teams.relation("humans", "humans") == "ally", "relations")
  MC.teams.friendly_fire(false)
  MC.teams.assign(2, "humans"); ok(MC.teams.of(2) == "humans" and P.team(2) == 2, "teams.assign moves CS team", P.team(2))
  local h = P.health(1); N.damage(1, 2, 2, 10, 2); ok(P.health(1) == h, "friendly fire blocked between allies", P.health(1))
  MC.teams.assign(2, "raiders")
end)
xmod.cmd.server("mct_phase6", function()
  if not P.alive(1) then MC.player.respawn(1) end
  ok(P.alive(1), "respawn")
  w = N.weapon_find(1, "weapon_ak47"); if w == 0 then w = MC.weapons.give(1, "plasma", true) else MC.weapons.switch(1, w) end
  ok(w ~= 0 and MC.weapons.is(w, "plasma"), "plasma kept/given")
  xmod.log("MCT phase6 active=" .. N.weapon_active(1) .. " w=" .. w)
  N.ham("item_deploy", w)
  ok(E.get(1, "viewmodel") == "models/v_plasma.mdl" and E.get(1, "weaponmodel") == "models/p_plasma.mdl", "deploy post sets models", E.get(1, "viewmodel"))
  local t = MC.npc.spawn("grunt", -300, -300, 0, 0, { health = 1000, ai = "idle", aggressive = false })
  local px, py, pz = P.origin(1); local nx, ny, nz = E.get(t.ent, "origin")
  if N.weapon_active(1) ~= w then xmod.log("MCT active weapon is not the plasma: " .. N.weapon_active(1)) end
  N.fire_bullets(1, 1, px, py, pz + 10, nx - px, ny - py, (nz + 30) - (pz + 10), 0, 4096, 10, 1)
  ok(near(E.get(t.ent, "health"), 980), "custom weapon damage x2 through ham takedamage", E.get(t.ent, "health"))
  t:remove()
end)
xmod.cmd.server("mct_phase7", function()
  local wb, found = 0, false
  repeat wb = E.find(wb, "weaponbox"); if wb ~= 0 then xmod.log("MCT weaponbox " .. wb .. " model " .. E.get(wb, "model")) end
    if wb ~= 0 and E.get(wb, "model") == "models/w_plasma.mdl" then found = true end until wb == 0
  xmod.log("MCT p1 alive=" .. tostring(P.alive(1)) .. " active=" .. N.weapon_active(1))
  ok(found, "dropped weaponbox got the custom world model (SetModel hook)")
end)
local path_done = 0
xmod.on("mc:npc_path_done", function() path_done = path_done + 1 end)
local col
xmod.cmd.server("mct_phase8", function()
  -- fx / hud: every temp entity must be a valid engine message (the mock checks begin/end pairs)
  local x, y, z = P.origin(1)
  MC.fx.beam(x, y, z, x + 100, y, z, 255, 0, 0); MC.fx.ring(x, y, z, 100, 0, 255, 0, 10); MC.fx.explosion(x, y, z); MC.fx.sprite(x, y, z, "sprites/laserbeam.spr")
  MC.fx.smoke(x, y, z); MC.fx.sparks(x, y, z); MC.fx.tracer(x, y, z, x, y + 50, z); MC.fx.light(x, y, z, 20, 255, 255, 255); MC.fx.implosion(x, y, z)
  MC.fx.trail(1, 255, 255, 255); MC.fx.beam_ents(1, 2, 255, 255, 0); MC.fx.blood(x, y, z); MC.fx.decal(x, y, 0); MC.fx.teleport(x, y, z)
  MC.hud.bar(1, 3); MC.hud.status_icon(1, "dmg_rad", true); MC.hud.timer(30); MC.hud.line(1, "test"); MC.hud.center(0, "hi"); MC.hud.hide(1, 0)
  ok(#MC.spawns.get("t") == 1 and #MC.spawns.get("ct") == 1 and #MC.spawns.get("any") == 2, "spawns from map entities")
  local sx = MC.spawns.random("ct"); ok(sx == 500, "spawns.random", sx)
  -- custom fire mode weapon
  local lw = MC.weapons.give(1, "laser", true); ok(lw ~= 0 and N.weapon_active(1) == lw, "give laser")
  local target = MC.npc.spawn("archer", x - 200, y, 0, 0)
  local _, yaw = N.vec_to_angles(-200, 0, 0)
  E.set(1, "v_angle", 0, 180, 0)
  local hp0 = E.get(target.ent, "health"); local clip0 = N.weapon_clip(lw)
  N.weapon_times(lw, 0, 0, 0); N.ham("weapon_primaryattack", lw)
  ok(N.weapon_clip(lw) == clip0 - 1, "custom fire uses one round", N.weapon_clip(lw))
  ok(near(E.get(target.ent, "health"), hp0 - 25), "custom fire bullets hit (no double multiplier)", E.get(target.ent, "health"))
  ok(near(N.weapon_times(lw)[1], 0.15), "custom fire rate", N.weapon_times(lw)[1])
  -- custom clip reload: 10 -> 30 with backpack 60
  N.weapon_clip(lw, 10); N.next_attack(1, 0); N.ham("weapon_reload", lw)
  ok(N.weapon_clip(lw) == 10 and N.pdata_get(lw, "m_fInReload") ~= 0, "reload started, clip kept", N.weapon_clip(lw))
  -- ranged npc + path
  -- objectives: collect / reach / destroy / kills / survive / custom
  col = MC.objectives.add { id = "coins", type = "collect", model = "models/mc_test.mdl", items = { { x, y, 0 } }, team = "humans", round_win = false, always = true, reset_each_round = false }
  MC.objectives.add { id = "zone", type = "reach", origin = { x, y, z }, radius = 80, count = 1, team = "humans", round_win = false, always = true, reset_each_round = false }
  MC.objectives.add { id = "tower", type = "destroy", model = "models/mc_test.mdl", origin = { x + 300, y + 300, 0 }, health = 50, team = "humans", round_win = false, always = true, reset_each_round = false }
  MC.objectives.add { id = "hold", type = "survive", time = 1, team = "humans", round_win = false, always = true, reset_each_round = false }
  MC.objectives.add { id = "calc", type = "custom", team = "raiders", round_win = false, always = true, reset_each_round = false, check = function() return 1 end }
  local tw = MC.objectives.get("tower").ent
  xmod.log("MCT tower ent=" .. tostring(tw) .. " cls=" .. (tw and E.valid(tw) and E.classname(tw) or "?") .. " hp=" .. (tw and E.valid(tw) and E.get(tw, "health") or -1) .. " td=" .. (tw and E.valid(tw) and E.get(tw, "takedamage") or -1))
  N.damage(tw, 1, 1, 100)
  xmod.log("MCT tower after: valid=" .. tostring(E.valid(tw)) .. " done=" .. tostring(MC.objectives.get("tower").done))
  local pn = MC.npc.spawn("archer", x - 400, y, 0, 0)
  pn:path({ { x - 400, y + 150, 0 }, { x - 400, y + 300, 0 } })
end)
xmod.cmd.server("mct_phase9", function()
  local lw = N.weapon_find(1, "weapon_m4a1")
  ok(N.weapon_clip(lw) == 30 and N.bpammo(1, lw) == 40, "custom clip reload finished 30 (bp 60->40)", N.weapon_clip(lw) .. "/" .. N.bpammo(1, lw))
  ok(path_done == 1, "npc path followed", path_done)
  for _, id in ipairs({ "coins", "zone", "tower", "hold", "calc" }) do local o = MC.objectives.get(id); ok(o.done, "objective " .. id, o.progress and next(o.progress)) end
  ok(MC.objectives.get("calc").winner == "raiders", "custom objective team")
  ok(MC.info().rules and MC.info().weapons == 2, "MC.info")
end)
xmod.cmd.server("mct_report", function() xmod.log(string.format("MC TEST DONE: %d passed, %d failed", pass, fail)) end)
