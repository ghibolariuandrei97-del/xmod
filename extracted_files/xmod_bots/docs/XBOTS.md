# xmod bots 1.0 — bots for CS 1.6 on xmod

Two parts, like a body and a brain:

| part | file | does |
|---|---|---|
| **body** (C++ extension) | `extensions/xmod_bots/` → `xmod_bots_mm_i386.so` / `xmod_bots_mm.dll` | creates fake clients; **every server frame**: human-like aim, walking the navigation graph (jump / crouch / ladder), strafing, stuck recovery, the fire controller; vision traces (FOV, smoke, walls); the navigation graph (A*, automatic map analyzer, learning from players, `.nav` files); fake client commands (`jointeam`, buy, radio, `say`) |
| **brain** (Lua) | `plugins/xbots/xbots.lua` | ~10 decisions per second per bot: team plans, objectives (plant / defuse / hostages), combat tactics, buying and economy, hearing, obeying human orders, long-term memory, chat (templates or an LLM), cvars, admin commands, API for other plugins |
| editor (Lua) | `plugins/xbots/xbots_editor.lua` | draws and edits the navigation graph in game (`say /botnav`) |

The rule from xmod_skills.md ("decide in Lua at low frequency, apply natively") is followed: nothing per frame runs in Lua.

## Install

Ready-built binaries are in `addons/xmod/dlls/`:
- `xmod_bots_mm_i386.so` — Linux, 32-bit, needs only glibc 2.17+ (works on old and new distros, libstdc++ built in)
- `xmod_bots_mm.dll` — Windows, 32-bit (MinGW, static, exports like xmod_mm.dll)

1. Copy `addons/xmod/dlls/xmod_bots_mm_i386.so` (or the .dll) next to `xmod_mm_i386.so`.
2. `addons/metamod/plugins.ini` — **after** xmod:
   ```
   linux addons/xmod/dlls/xmod_mm_i386.so
   linux addons/xmod/dlls/xmod_bots_mm_i386.so
   ```
   Windows: `win32 addons/xmod/dlls/xmod_bots_mm.dll` after `xmod_mm.dll`.
3. `addons/xmod/configs/extensions.ini` — add a line: `bot`
4. Copy `addons/xmod/plugins/xbots/` and the three files in `addons/xmod/configs/` (`xbots.cfg`, `xbots.ini`, `xbots_chat.ini`).
5. `addons/xmod/configs/plugins.ini`:
   ```
   xbots/xbots.lua          ; xmod bots - the brain
   xbots/xbots_editor.lua   ; say /botnav - navigation editor
   ```
6. Restart. Set `xbots_quota` in `xbots.cfg` (default 10, mode `fill`).

To build from source instead: copy `extensions/xmod_bots/` into the xmod tree under `extensions/` and run the normal
`./build_linux.sh` / `./build_windows.sh`. (A build made on a very new distro needs that distro's glibc; the shipped .so was
built with zig for glibc 2.17: `python3 -m ziglang c++ -target x86-linux-gnu.2.17 ...`, flags as in xmod's CMakeLists.)

## Cvars (`configs/xbots.cfg`)

| cvar | default | |
|---|---|---|
| `xbots_quota` | 10 | number of bots |
| `xbots_quota_mode` | fill | `fill` bots + humans = quota · `fixed` · `match` quota bots per human |
| `xbots_keep_slot` | 1 | slots always left free for humans |
| `xbots_join_team` | any | `any` / `t` / `ct` |
| `xbots_difficulty` | 2 | 0 newbie · 1 easy · 2 normal · 3 hard · 4 expert |
| `xbots_skill_min/max` | -1 | exact skill range 0..100 (overrides difficulty) |
| `xbots_reaction` | 1.0 | reaction time multiplier |
| `xbots_aim_head` | 1.0 | headshot preference multiplier |
| `xbots_fov` / `xbots_vision` | 100 / 4000 | field of view, sight distance |
| `xbots_think` | 0.1 | seconds between decisions |
| `xbots_buy` / `xbots_eco` / `xbots_grenades` | 1 | buying, team economy (save rounds together), HE grenades |
| `xbots_restrict` | "" | weapons never bought, e.g. `awp,g3sg1,sg550` |
| `xbots_knife_only` / `xbots_peaceful` / `xbots_frozen` / `xbots_ffa` | 0 | knife only · never attack · don't move · attack everybody |
| `xbots_radio` / `xbots_obey` | 1 | radio · obey human radio/chat orders |
| `xbots_chat` / `xbots_chat_rate` / `xbots_chat_lang` | 1 / 1.0 / ro | chat on, frequency, language section in xbots_chat.ini |
| `xbots_ai` | 0 | 1 = replies through an LLM (`configs/xbots.ini [ai]`, needs `xmod_query`) |
| `xbots_memory` | 1 | long-term memory (`data/vaults/xbots_memory.vault`) |
| `xbots_tag` | "" | name prefix, e.g. `[BOT] ` |
| `xbots_learn` / `xbots_auto_nav` / `xbots_nav_max` | 1 / 1 / 6000 | navigation learning · auto analyze · node limit |
| `xbots_admin_flag` | h | access letter for the commands and menus |
| `xbots_debug` | 0 | decisions in the log |

## Commands
- `xbots add [t|ct|any] [skill] [name]` · `kick <name|all|t|ct>` · `quota <n> [mode]` · `difficulty <0-4>` · `skill <0-100> [name|all]`
- `xbots list` · `info <name>` (stats, nemesis, favourite victim, best weapon) · `forget <name|all>`
- `xbots say <name> <text>` · `order <name|all> <follow [player]|hold|rush|free>`
- `xbots nav <info|analyze [clear]|save|load|clear|learn 0/1>` · `xbots reload`
- `say /bots` — admin menu · `say /botnav` — navigation editor menu
- Editor console: `xbots_nav show | add [flags] | del | from | to [jump|crouch|ladder] [both] | unlink | flag <name> | radius <r> | info | path | go | bots_here`

## What the bots do (why they are better than classic bots)

**Aim like a person, not a robot.** The view is a critically damped spring (no snapping), with a skill-based max turn speed.
On first sight there is a reaction time (shorter when already alert). The aim point has a deliberate error that is bigger on
moving targets and shrinks while tracking (aim "settles"), and the bot aims slightly behind fast targets (human lag).
Recoil is compensated by skill (`v_angle + punchangle`). Head or body is chosen per engagement, only visible parts are aimed at.

**Shoot like a player.** Pistols tap, rifles spray close / burst at mid range / tap far, SMGs spray, shotguns only close,
AWP/Scout zoom first and wait for the bolt. It fires only when the crosshair is on target (tolerance from distance and
skill) and **never shoots through a teammate**. Empty magazine → pistol switch when close, otherwise reload; reload in calm
moments. Blinded by a flashbang → stops aiming, sprays only if the enemy was close.

**See and hear fairly.** FOV cone + 3 traces (head, body, feet), smoke clouds block vision, invisible players are ignored.
Hearing: running footsteps (~900 units) and gunshots (~2200) of enemies, deaths of teammates; bots turn towards an
attacker they cannot see, watch the spot where an enemy disappeared, and pre-aim when they go hunting.

**Move like a player.** A* over a graph with learned danger (deaths) and a bit of randomness (bots take different routes);
jump, crouch and ladder links; corner cutting, look-ahead; stuck recovery (jump, crouch, strafe, use doors) and re-planning.
Strafing while shooting, crouch-spraying at long range, pushing (aggressive) or holding (careful), retreating at low HP.
Long runs with the knife out (faster), switching back before the fight.

**Play as a team.** Each round: Ts pick one site and go together (with a lurker), the C4 carrier is escorted, a dropped bomb is
picked up by the nearest T, bots plant in the bomb zone, guard the planted bomb; CTs split over the sites and hold good
spots, defuse when the bomb is down. cs_ maps: CTs lead hostages to the rescue zone, Ts guard them. Radio: enemy spotted,
enemy down, need backup, in position, cover me, fire in the hole.

**Obey humans.** Radio `followme / coverme / holdpos / regroup / stormfront / go / fallback / needbackup` → the nearest bots
follow / hold / regroup / rush / fall back and answer "roger". Chat works too: *"boti dupa mine"*, *"bots hold"*,
*"Vulpea follow me"*, *"boti liberi"*.

**Memory.** Each bot name has a persistent memory: kills, deaths, headshots, rounds, wins, kills per weapon (it buys the
weapon it does best with), and a relation with every player (by SteamID): who killed it how many times. The player who
killed it most becomes its **nemesis**: higher target priority, special chat when they meet, a greeting when they join
("iar tu {name}?"). Mood: tilts after deaths (more aggressive, grumpy chat), happy after kills. The navigation remembers
dangerous corridors and good positions per map; this experience fades slowly.

**Talk.** Event chat in RO/EN (kill, headshot, knife, revenge, ace, death, nemesis, team kill, tilt, round won/lost, bomb planted,
greetings, short banter between two bots), with typing delay and cooldowns, dead chat rules respected (real `say`).
Replies to players who mention a bot's name or "bot(i)". With `xbots_ai 1`, replies come from an LLM with the bot's name,
personality, the recent chat and its memory of that player (OpenAI-compatible endpoints incl. a local Ollama, or Anthropic).

## Lua API of the extension — `xmod.bot.*` (synchronous, game thread)

Node indices are 1-based (0 = none). Teams: 1 T, 2 CT. Raises an error if `id` is not a bot of this extension.

| function | |
|---|---|
| `add(name)` → id (number) or error text | create a bot (then send `jointeam`/`joinclass` with `cmd`) |
| `remove(id)` · `is_bot(id)` · `list()` · `count()` | |
| `cmd(id, "cmd args; cmd2 ...")` | fake client command(s): `jointeam 2`, `ak47;vesthelm`, `weapon_knife`, `roger`, `drop`... |
| `say(id, text [, team])` | real `say` / `say_team` |
| `set(id, {aim_speed, max_turn, aim_error, recoil, lag, fov, vision, frozen, speed})` | aim/sense profile |
| `go(id, x, y, z [, {speed=1, walk=false, direct=false, fallback=true}])` → nodes \| -1 (direct) \| false | walk there (path or straight) |
| `stop(id)` · `strafe(id, -1\|0\|1, seconds)` | |
| `look(id, x, y, z)` · `look_ent(id, ent, "head"\|"body"\|"feet")` · `look_path(id)` · `look_none(id)` | where to aim |
| `press(id, mask)` · `hold(id, mask [, seconds])` · `release(id [, mask])` | buttons (`IN_*` masks) |
| `fire(id, mode [, hold, pause, max_angle])` | 0 off · 1 tap · 2 burst · 3 spray — fires only when aimed within `max_angle` degrees |
| `state(id)` → `{move, moving, path_left, gx, gy, gz, aim_error, look, look_ent, fire, stuck, pitch, yaw, frozen}` | |
| `scan(id [, all_teams])` → `{{id, dist, part}, ...}` closest first | visible enemies |
| `visible(viewer, ent [, ignore_fov])` → "head"/"body"/"feet" or false · `see_point(viewer, x, y, z)` | any player as viewer |
| `weapon(player)` → `{name, clip, wid, ent}` or nil | active weapon of any player |
| `config({autolearn, max_nodes, danger_weight, path_random})` | navigation settings |
| `nav_info()` → `{nodes, links, analyzing, progress, dirty, autolearn, max_nodes}` | |
| `nav_load()` · `nav_save()` · `nav_clear()` · `nav_analyze([clear, max_nodes])` | event `bot_nav_done(nodes, links)` |
| `nav_add(x, y, z [, flags])` → i · `nav_remove(i)` · `nav_link(a, b [, flags, both])` · `nav_unlink(a, b [, both=true])` | |
| `nav_flags(i [, flags])` · `nav_radius(i [, r])` · `nav_node(i)` → `{x,y,z,flags,radius,danger_t,danger_ct,value_t,value_ct,links={{to,flags}}}` | |
| `nav_nearest(x, y, z [, maxdist=400, visible])` · `nav_near(x, y, z, r)` · `nav_find(flags)` · `nav_random([flags])` | |
| `nav_path(a, b [, team])` → list or nil · `nav_best(x, y, z, r, team, "kill"\|"safe" [, need_flags])` | best camp / safest node |
| `nav_learn(x, y, z, team, "death"\|"kill" [, amount])` | experience |

Node flags: `CROUCH 1, LADDER 2, CAMP 4, GOAL 8, RESCUE 16, TSPAWN 32, CTSPAWN 64, SNIPER 128, HOSTAGE 256, AVOID 512, DOOR 1024, LEARNED 2048, JUMP 4096, USER1..4 = 65536..524288`.
Link flags: `JUMP 1, CROUCH 2, LADDER 4, DROP 8`.
Events from the extension: `bot_arrived(id)`, `bot_stuck(id)`, `bot_nav_done(nodes, links)`.

## API of the brain (for other plugins)

```lua
local id = xmod.call("xbots.add", "Name"|nil, "t"|"ct"|nil, skill|nil, "aggressive"|"normal"|"careful"|"sniper"|"joker"|nil)
xmod.call("xbots.kick", id | "all")          xmod.call("xbots.list")          xmod.call("xbots.is_bot", id)
xmod.call("xbots.skill", id, 0..100)
xmod.call("xbots.order", id | 0, "follow", leader_id, nil, nil, seconds)   -- also "hold"/"goto" with x, y, z; "rush"; "free"
xmod.call("xbots.task", id, { kind = "go"|"camp"|"seek"|"wait", x = , y = , z = , time = 30 })
xmod.call("xbots.say", id, "text", team_only)       xmod.call("xbots.chat_event", id, "kill", { name = "Andrew" })
xmod.call("xbots.brain", id)  -- the live brain table (skill, pers, mood, task, order, enemy, last_enemy, heard, mem ...)
xmod.call("xbots.memory", id) -- a copy of the long-term memory
```
Events: `xbots_added(id)`, `xbots_removed(id)`, `xbots_spawn(id)`, `xbots_kill(bot, victim)`, `xbots_death(bot, killer)`,
`xbots_order(bot, kind, leader)`; hooks you can answer: `xbots_think(id, brain)` → `true` = your plugin drives this bot
this tick (use `xmod.bot.*` yourself), `xbots_target(id, enemy)` → `true` = ignore that enemy (friendly NPC, VIP, a
"tame" mode for new players...), `xbots_chat(id, text)` → `true` = block the message.

Example — a "bodyguard" bot for VIP players:
```lua
xmod.on("xbots_target", function(bot, enemy) if xmod.player.has_access(enemy, "t") then return true end end)
xmod.cmd.say("/guard", "t", function(id)
  local b = xmod.call("xbots.add", "Bodyguard", xmod.player.team(id) == 1 and "t" or "ct", 90, "careful")
  if b then xmod.timer.after(3, function() xmod.call("xbots.order", b, "follow", id, nil, nil, 600) end) end
end)
```

## Test status
- **Builds**: the shipped .so/.dll are built from this source against xmod's own `include/` (mm_sdk.h, xmod_ext.h) and
  the Valve HLSDK; exports checked (only the Metamod entry points, `GiveFnptrsToDll@8` stdcall as in xmod).
- **Loaded by the real xmod** in xmod's `test/mock_engine.cpp`: the extension connects, registers `xmod.bot.*`, and
  `tests/bot_ext_test.lua` passes 18/18 (values in/out, 1-based nodes, errors, nav save/load on disk) with xbots.lua running.
- **C++ behaviour** (32-bit, xmod's SDK) against a fake engine (room, wall with a gap, step, crate): analyzer (546 nodes,
  jump links onto the crate, no node inside walls), A* around the wall, `.nav` save/load, a bot walking around the wall and
  jumping onto the crate, aim convergence (90° in ~0.4 s), vision blocked by a wall, burst timing, learning from a human.
- **Lua brain**: ~2 minutes of simulated rounds in a mock of xmod (quota, team join, buying, plant / follow / camp, kills,
  memory, chat + replies, radio orders, flashbang, admin commands, editor, exports): 0 errors.
- **Not yet on a real HLDS / ReGameDLL.** Watch in the first test: team joining (`jointeam` / `joinclass`, falls back to
  `menuselect`), buy aliases, ladder climbing, CPU with 20+ bots (`xmod_dbg`).

## Limits (and how they are covered)
- The analyzer does not create ladder links by itself — they are learned when a human climbs a ladder, or placed with the editor.
- Doors / breakables: handled by stuck recovery (USE, jump, strafe) and re-planning; mark problem spots `avoid` in the editor.
- as_ (VIP) maps have no special logic (bots roam / hunt).
- Footsteps are estimated from speed (CS plays them client-side, there is no server event).
- Smoke detection is a heuristic (a landed smoke grenade = a 115-unit cloud).
