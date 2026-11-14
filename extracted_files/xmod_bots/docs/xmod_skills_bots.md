# Part IV — xmod bots (append this to xmod_skills.md)

> Extension `xmod_bots` (namespace `xmod.bot`, line `bot` in configs/extensions.ini) + brain plugin `plugins/xbots/xbots.lua`.
> Full reference: docs/XBOTS.md. When Andrew asks for something with bots, prefer the brain's exports/events; use
> `xmod.bot.*` directly only to drive a bot yourself (answer `xbots_think` with `true` while you do).

## B1. Rules
1. Extension functions exist from `config`/`init`/gameplay events on, not at the top level. Check `xmod.ext.available("bot")`.
2. Bots are real players: `xmod.player.*` works on them; `xmod.bot.is_bot(id)` = created by xmod_bots; `xmod.call("xbots.is_bot", id)` = has a brain.
3. Never call `xmod.bot.go` every tick: call it when the target changes (the body walks the path by itself every frame).
4. Node indices are 1-based, 0 = none. Teams 1 = T, 2 = CT. `fire` modes 0 off, 1 tap, 2 burst, 3 spray.

## B2. Body — `xmod.bot.*`
`add(name)`→id|errtext · `remove(id)` · `is_bot(id)` · `list()` · `count()` · `cmd(id, "a;b")` (fake client commands) · `say(id, text [, team])`
· `set(id, {aim_speed, max_turn, aim_error, recoil, lag, fov, vision, frozen, speed})` · `go(id, x,y,z [, {speed, walk, direct, fallback}])`→nodes|-1|false
· `stop(id)` · `strafe(id, dir, secs)` · `look(id, x,y,z)` · `look_ent(id, ent, "head"|"body"|"feet")` · `look_path(id)` · `look_none(id)`
· `press(id, mask)` · `hold(id, mask [, secs])` · `release(id [, mask])` · `fire(id, mode [, hold, pause, max_angle])` · `state(id)`→table
· `scan(id [, all_teams])`→`{{id, dist, part}}` · `visible(viewer, ent [, ignore_fov])` · `see_point(viewer, x,y,z)` · `weapon(player)`→`{name, clip, wid, ent}`
· `config({autolearn, max_nodes, danger_weight, path_random})`
· nav: `nav_info nav_load nav_save nav_clear nav_analyze([clear, max]) nav_add(x,y,z[,flags]) nav_remove(i) nav_link(a,b[,flags,both]) nav_unlink(a,b[,both])
  nav_flags(i[,f]) nav_radius(i[,r]) nav_node(i) nav_nearest(x,y,z[,max,visible]) nav_near(x,y,z,r) nav_find(flags) nav_random([flags])
  nav_path(a,b[,team]) nav_best(x,y,z,r,team,"kill"|"safe"[,need]) nav_learn(x,y,z,team,"death"|"kill"[,amount])`
· events `bot_arrived(id)` `bot_stuck(id)` `bot_nav_done(nodes, links)`.
Node flags CROUCH 1 LADDER 2 CAMP 4 GOAL 8 RESCUE 16 TSPAWN 32 CTSPAWN 64 SNIPER 128 HOSTAGE 256 AVOID 512 DOOR 1024 LEARNED 2048 JUMP 4096 USER1-4 65536..524288 · link flags JUMP 1 CROUCH 2 LADDER 4 DROP 8.

## B3. Brain — exports and events
`xmod.call("xbots.add", name|nil, "t"|"ct"|nil, skill|nil, pers|nil)` · `"xbots.kick"(id|"all")` · `"xbots.list"()` · `"xbots.is_bot"(id)` · `"xbots.skill"(id, s)`
· `"xbots.order"(id|0, "follow"|"hold"|"goto"|"rush"|"free", leader_or_x, y, z, secs)` · `"xbots.task"(id, {kind="go"|"camp"|"seek"|"wait", x,y,z, time})`
· `"xbots.say"(id, text, team)` · `"xbots.chat_event"(id, ev, vars)` · `"xbots.brain"(id)` (live table) · `"xbots.memory"(id)` (copy).
Events: `xbots_added xbots_removed xbots_spawn(id)` · `xbots_kill(bot, victim)` · `xbots_death(bot, killer)` · `xbots_order(bot, kind, leader)` ·
answerable: `xbots_think(id, brain)`→true = you drive it this tick · `xbots_target(id, enemy)`→true = ignore enemy · `xbots_chat(id, text)`→true = block.
Personalities: aggressive normal careful sniper joker. Cvars: `xbots_*` (docs/XBOTS.md). Chat lines: configs/xbots_chat.ini `[lang.event]`.
