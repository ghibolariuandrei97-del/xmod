# xmod_skills.md — the complete xmod knowledge file (xmod 0.4.0 + Nexus 2.0 + extensions)

> **Instructions for Claude (read first).** The user (Andrew, AMXX/CS 1.6 developer, GitHub `ghibolariuandrei97`) runs **xmod**,
> a Metamod plugin that replaces AMX Mod X's Pawn VM with **Lua 5.4**, with **Nexus** (a client extension: HUDs, mouse menus, view /
> input control, per-player entity looks, 3D shapes, lights, effects) built in. This single file documents **everything**:
> Part I the Lua API of xmod, Part II Nexus (client-side features, `require("nexus")`), Part III writing C++ extensions
> (other Metamod plugins that add `xmod.<namespace>.*` functions), then appendices (ReAPI hooks, Ham functions, an index of every
> function and constant, the default config files).
>
> When he asks for a **plugin**: write **one `.lua` file** that follows this document exactly. Use only the API listed here — do not
> invent functions (Appendix C lists every name that exists). If something is not possible, say so and propose the closest
> alternative (or a small C++ addition / extension). Always add the `plugins.ini` line he must add. Prefer the libraries in `lib/`
> (`util`, `menu`, `vote`, `json`, `query`, `nexus`) over re-implementing helpers. When he asks for an **extension** (native
> module), follow Part III. Reply in the user's language (Romanian by default unless he writes in English).

# Part I — xmod core (Lua API)

## 1. What xmod is
- Metamod plugin (`xmod_mm_i386.so` on Linux, `xmod_mm.dll` on Windows), C++17 + embedded Lua 5.4. CS 1.6 / GoldSrc, 32-bit.
- Scripts are plain `.lua` text files: **no compiling**, `xmod reload` hot-reloads them.
- Each plugin runs in its **own sandbox environment** (globals you assign are private; reads fall back to the shared globals).
  No `io`, `os.execute`, `debug`, `package`, `dofile`, `loadfile`. Available: `base`, `table`, `string`, `math`, `utf8`, and `os.time/clock/date`.
  `print(...)` writes to the server log.
- Safety limits: a watchdog thread aborts any **single script call** that runs longer than `xmod_script_timeout` ms (default **100**, `0` = off; plugin loading gets 10x, min 1 s).
  It costs nothing on the normal path (no per-instruction hook; the abort hook is installed only after an overrun). It cannot interrupt time spent inside one C function (e.g. a huge `string.rep`).
  Lua memory cap **96 MB**; a plugin that raises **25 errors** is disabled automatically (others keep running). Errors are logged with a traceback: server console (also on a listen server), Metamod log, and `addons/xmod/data/xmod_errors.log`; `xmod plugins` shows why a plugin is DISABLED (0.3.1+).
  Ownership follows the code (0.3.1+): a function of your plugin that a library calls back (e.g. a Nexus `on_click`) still counts as your plugin for `xmod.player.data`, timers, `xmod.on`, commands and logs.
- Hooks: engine/DLL events, user-message interception, **Ham** (virtual function hooks, stock CS and ReGameDLL) and
  **ReAPI hookchains** (ReGameDLL required). All are just events on the same event bus (`xmod.on`).
- `x` is an alias for `xmod`. Player ids are `1..xmod.maxplayers()` and are also their entity indices. Entities are plain integers (edict index), never pointers.

## 2. Files and layout (`cstrike/addons/xmod/`)
```
dlls/xmod_mm_i386.so | xmod_mm.dll      the Metamod module
configs/plugins.ini     list of plugin files (one per line, ';' comments, subfolders allowed e.g. examples/foo.lua)
configs/users.ini       admins (AMXX format)        configs/xmod.cfg   cvars, exec'd every map start
configs/maps.ini        maps for votes/menus         configs/ads.ini    server ad lines
configs/extensions.ini  Lua namespaces of extensions configs/query.ini  xmod_query: DB profiles, allow-lists, limits
plugins/*.lua           plugins        lib/*.lua  shared libraries (require("name"))
data/                   writable: *.kv stores, logs (xmod.fs.write only allows data/ and configs/)
```
Install: add `linux addons/xmod/dlls/xmod_mm_i386.so` (Linux) or `win32 addons/xmod/dlls/xmod_mm.dll` (Windows) to `addons/metamod/plugins.ini`.
To add a plugin: put `myplugin.lua` in `plugins/`, add the line `myplugin.lua` to `configs/plugins.ini`, then `xmod reload` (or restart).
Server console: `xmod version | plugins | cmds | ext | reload`.

## 3. Lifecycle (order of events)
1. Metamod loads xmod → **scripts execute top-to-bottom once** (register commands/events/cvars here; this is the "plugin_init" for most things).
2. Per map: world spawn → `precache` → ServerActivate → `init(maxplayers)` → (2 frames later, after `xmod.cfg` was exec'd) `config`
   → gameplay events → `map_end` (ServerDeactivate). Timers survive map changes. `shutdown` fires when xmod unloads.
3. `xmod reload` re-creates the Lua state and re-runs all scripts, then fires `init` and `config` (not `precache`; precache calls fail after the map started).
- Top-level code runs *before* cvars from `xmod.cfg` are applied: read cvar values inside handlers / the `config` event, not at load time.
- Per-player state: `xmod.player.data(id)` returns a Lua table private to your plugin; it is cleared when the player connects/disconnects.

### Bots, message safety (important)
- **Bots are real players for xmod** (`xmod.player.is_bot(id)` is true). ReGameDLL/CS bots bypass Metamod's ClientConnect, so xmod registers them from the engine's `CreateFakeClient`: `client_connect` fires with ip `127.0.0.1`, `client_putinserver` fires at the bot's first think, and any client edict xmod missed (e.g. xmod loaded mid-game) is registered lazily. `xmod.player.all()` includes bots; `xmod.msg.chat/hud(0, ...)` skips them.
- **Ham on bots:** bots use their own class vtable, so a hook on `player` is also installed into the bot vtable at the bot's first think (the bot's very first spawn can be missed).
- **GoldSrc allows only ONE open message.** xmod makes this safe: a game message that has `message:<Name>` listeners is captured, your handlers run while the engine has *no* message open, then it is replayed (or dropped if a handler returns `true`). So inside any `message:*` handler you may freely call `xmod.msg.*`, `xmod.player.kill`, `give_item`, `set_team`, … `xmod.msg.begin()…finish()` is buffered and sent atomically at `finish()`; if the script errors in between, the half-built message is discarded. If Lua ever runs while a pass-through message is open, sending natives are queued and flushed right after that message ends (their return values are then assumed successful).

## 4. Writing a plugin — template
```lua
xmod.register("My Plugin", "1.0", "Andrew")          -- name, version, author (optional but recommended)
local U = require("util")                            -- optional libs: util, menu, vote, json

xmod.cvar.create("myp_enabled", "1")                 -- cvars: create at load, read with xmod.cvar.int/num/get inside handlers

xmod.on("client_putinserver", function(id)           -- events
  xmod.msg.chat(id, "{g}[myp] {y}Welcome {t}" .. xmod.player.name(id))
end)

U.cmd("hello", "", function(id, args)                -- registers xmod_hello + amx_hello, client AND server console (id==0 for server)
  U.reply(id, "hi " .. (args[1] or "world"))
end, "hello [name]")

xmod.cmd.say("/hi", "", function(id, args) xmod.msg.chat(id, "hi!") end)   -- chat command, flags "" = everyone
```
Conventions: prefix cvars with the plugin name; always re-check `xmod.player.connected(id)` inside timers/callbacks;
never loop/wait (use timers); keep `frame`, `prethink`, `postthink`, `cmd_start`, `touch`, `think` handlers tiny (they are hot);
chat text is cut at ~188 chars; use different HUD `channel` (1–4) for messages that must not overwrite each other.

## 5. Event bus
`xmod.on(event, fn [, priority=0])` (higher priority runs first) · `xmod.off(event, fn)` · `xmod.emit(event, ...)` → `nil|true|false`.
Handler return values: **`true` = consume/block** (stops other handlers and the default action where applicable),
**`false, "reason"` = reject** (client_connect), nothing = pass. Custom events: any other name works (`xmod.emit("my_event", a, b)`).

| Event | Args | Notes |
|---|---|---|
| `precache` | – | once per map, the only place `xmod.precache.*` works |
| `init` | maxplayers | map loaded |
| `config` | – | after xmod.cfg; read cvars here |
| `map_end` | – | |
| `shutdown` | – | |
| `frame` | – | every server frame (hot) |
| `client_connect` | id, name, ip | return `false, "reason"` to reject |
| `client_putinserver` | id | player fully in game |
| `client_disconnect` | id | |
| `client_infochanged` | id, oldname, newname | name/setinfo change |
| `client_command` | id, cmd, args | every client console command (say/menuselect/…); `true` blocks |
| `chat` | id, text, team_only | before say/say_team is processed; `true` blocks the message |
| `prethink` / `postthink` | id | per player per frame (hot) |
| `cmd_start` | id, buttons, impulse, forwardmove, sidemove, upmove | use `xmod.IN_*` masks |
| `touch` / `touch:<classname>` | ent, other | `true` blocks the touch |
| `think` / `think:<classname>` | ent | hot |
| `spawn` | ent, classname | entity spawned (post) |
| `emit_sound` | ent, channel, sample, volume, attn, flags, pitch | `true` blocks the sound |
| `add_to_full_pack` | ent, host, is_player (0/1) | **hottest hook** (every entity x every client x every frame). Return `true` = hide `ent` from client `host`. Must be registered at plugin top level while the server starts (restart after adding it) |
| `add_to_full_pack:post` | ent, host, is_player, ret | fires only for entities that were sent to the client; `xmod.fullpack.set_render/set_effects/set_origin` modify what that client sees; return `true` hides it |
| `message:<Name>` | name, dest, ent, args | user message observer; `args` = table of written values in order (numbers/strings); `true` blocks. e.g. `message:DeathMsg` args `{killer, victim, headshot, weapon}`; svc messages are `message:svc:<n>` (e.g. `message:svc:30` = intermission) |
| `reapi:<Hook>[:post]` | see appendix A | ReGameDLL hookchains |
| `ham:<func>:<classname>[:post]` | see appendix B | virtual hooks |

Native hooks (reapi/ham): pre-hook args = `(this_ent, args…)`; post-hook args = `(this_ent, args…, return_value)`.
Pre-hook may return `xmod.HC_SUPERCEDE, value` (skip the original and return `value`), `xmod.HC_BREAK` (stop other plugins, still run original),
or nothing (`xmod.HC_CONTINUE`). `xmod.hook.set_arg(n, value)` changes numeric arg `n` (1-based, **not counting `this`**) before the original runs.
`xmod.reapi.available()` → bool; `xmod.reapi.list()` / `xmod.ham.list()` → names. Registering an unknown/unavailable hook raises an error at `xmod.on` time
(wrap in `pcall` or check `xmod.reapi.available()` first). Ham needs a classname: `player`, `weapon_ak47`, `func_door`, `info_target`, …
Not supported in Ham: functions that pass/return `Vector` by value (traceattack, eyeposition, …) — use the matching ReAPI hook instead.

## 6. API reference
### Core
`xmod.register(name, version, author)` · `xmod.on/off/emit` · `xmod.export(name, fn)` · `xmod.call(name, ...)` (call another plugin's exported function; returns its results)
· `xmod.has_export(name)` · `xmod.plugins()` (list of `{name,version,author,file,enabled}`) · `xmod.time()`/`xmod.gametime()` (seconds since map start)
· `xmod.exec(cmd)` (server command) · `xmod.flags("abc")` → bitmask · `xmod.log(text)` · `xmod.error(text)` · `xmod.server_print(text)` · `xmod.map()` · `xmod.map_valid(name)`
· `xmod.maxplayers()` · `xmod.is_dedicated()` · `xmod.version` · `xmod.gamedir` · `require("lib")`.

### Players — `xmod.player.*` (id = 1..maxplayers; most functions raise an error for invalid/disconnected ids)
`connected(id)` (any connection state, safe for any int) · `ingame(id)` (fully joined) · `all()` → list of in-game ids · `name` · `ip` (`"a.b.c.d:port"`) · `steamid` · `userid` · `is_bot` · `alive`
· `team(id)` 0 unassigned, 1 T, 2 CT, 3 spectator · `set_team(id, 1|2|3)` · `team_name(n)`
· `health/set_health` · `armor/set_armor` · `frags/set_frags` · `money/set_money(id, v[, update_hud=1])` (0–16000)
· `origin(id)`→x,y,z · `set_origin(id,x,y,z)` · `velocity/set_velocity` · `view_angles(id)`→pitch,yaw,roll · `ping(id)`→ping,loss
· `kill(id)` · `kick(id[, reason])` · `give_item(id, "weapon_ak47"|"item_*"|"ammo_*")` (true/false)
· `access(id)` bitmask · `set_access(id, mask)` · `has_access(id, "abc"|mask)` (all flags required) · `infokey(id, key)` · `set_infokey(id, key, value)`
· `find(key)` → `id, hits` (`hits==1` means unique; key = partial name, `#userid`, or `STEAM_…`) · `data(id)` → per-plugin per-player table.

### Entities — `xmod.ent.*` (ent = edict index; players are ents too)
`valid(e)` · `create(classname)` → ent|0 · `spawn(e)` · `remove(e)` (not players) · `classname(e)` · `find(start_ent, value[, field="classname"])` → next ent|0 (iterate with start=0)
· `find_sphere(x,y,z,radius)` → list · `set_origin(e,x,y,z)` · `set_model(e, path)` · `set_size(e, minx,miny,minz, maxx,maxy,maxz)`
· `get(e, field)` / `set(e, field, value[, y, z])` — vectors take/return 3 numbers.
Fields — float: health max_health armorvalue takedamage dmg dmgtime speed maxspeed gravity friction frags fov nextthink ltime animtime framerate frame scale renderamt fuser1-4;
int: flags movetype solid effects deadflag sequence gaitsequence skin body rendermode renderfx button impulse weapons team playerclass spawnflags waterlevel watertype modelindex colormap iuser1-4 oldbuttons groupinfo;
vector: origin oldorigin velocity basevelocity angles avelocity v_angle punchangle mins maxs absmin absmax size view_ofs rendercolor movedir vuser1-4;
string: classname targetname target model globalname netname message noise noise1-3; entity (index, 0 = none): owner enemy aiment groundentity chain dmg_inflictor euser1-4.
Raw private data: `xmod.mem.get_int/set_int/get_float/set_float(ent, offset[, linux_diff])` — offset in **4-byte units** as in AMXX `get_pdata_int`; on Linux `linux_diff` is added (players: `xmod.mem.LINUX_PLAYER` = 5). Unsafe: wrong offsets crash the server.
Traces: `xmod.trace.line(x1,y1,z1, x2,y2,z2 [, flags=0 [, ignore_ent=0]])` → `{fraction, ent, x,y,z, nx,ny,nz, startsolid, allsolid}`;
`xmod.trace.hull(x1,y1,z1,x2,y2,z2, hull [, flags [, ignore]])`; `xmod.trace.contents(x,y,z)`.
Constants: `FL_ONGROUND FL_DUCKING FL_FROZEN FL_GODMODE FL_NOTARGET FL_FAKECLIENT FL_SPECTATOR FL_CLIENT MOVETYPE_NONE/WALK/NOCLIP/FLY/TOSS SOLID_NOT/BBOX/TRIGGER IN_ATTACK IN_ATTACK2 IN_JUMP IN_DUCK IN_FORWARD IN_BACK IN_MOVELEFT IN_MOVERIGHT IN_USE IN_RELOAD TRACE_NORMAL TRACE_IGNORE_MONSTERS TRACE_IGNORE_GLASS TEAM_UNASSIGNED TEAM_T TEAM_CT TEAM_SPEC RENDER_NORMAL RENDER_TRANSCOLOR RENDER_TRANSTEXTURE RENDER_GLOW RENDER_TRANSALPHA RENDER_TRANSADD FX_NONE FX_PULSE_SLOW FX_PULSE_FAST FX_DISTORT FX_GLOWSHELL EF_NODRAW EF_BRIGHTLIGHT EF_DIMLIGHT` (all directly on `xmod`; values in Appendix C).

### Messages — `xmod.msg.*`
High level (`id` 0 = all in-game human players):
- `chat(id|0, text [, sender_id])` coloured chat, max ~186 bytes. Colour codes (any style, mix freely):
  default `{y}` `{n}` `{default}` `^1` `!y` · team colour `{t}` `{team}` `^3` `!t` · green `{g}` `{green}` `^4` `!g` ·
  `{red}` `{blue}` `{grey}` = this message's team colour is red / blue / grey (one per message). `sender_id`: whose team colour `{t}` uses (default: the receiver's).
- `print(id|0, "console"|"center"|"chat", text)`
- `hud(id|0, text, {r=0,g=255,b=0, x=-1,y=0.35, hold=4, fadein=0.2, fadeout=0.5, channel=1, effect=0})` (x/y −1 = centred)
- `fade(id|0, seconds, hold, flags, r,g,b,a)` · `shake(id|0, amplitude, seconds, frequency)`
- `show_menu(id, keys_bitmask, seconds(-1 = until closed), text)` — raw VGUI-style menu text (`\y \r \w \d` colour codes), auto-chunked; selections arrive as client command `menuselect N` (use `require("menu")` instead).
- `block(name, true|false)` — drop a user message completely (e.g. `xmod.msg.block("HostagePos", true)`).
Low level: `xmod.msg.begin(dest, name_or_id [, x,y,z [, ent]])`, `byte char short long angle coord string entity`, `finish()`. Dest constants `xmod.msg.BROADCAST ONE ALL INIT PVS PAS ONE_UNRELIABLE TEMPENTITY`.

### Commands
- `xmod.cmd.client(name, flags, fn(id, args) [, desc])` — console command; `flags` = access letters required ("" = all).
- `xmod.cmd.say(trigger, flags, fn(id, args) [, desc])` — chat command; matches the first word of the chat line case-insensitively ("/menu", "nextmap"); `args` = words after the trigger.
- `xmod.cmd.server(name, fn(args) [, desc])` — server console command.
- `args` table: `args[1..n]`, `args.n`, `args.text` (raw rest of line). A command handler **consumes** the command unless it returns `false` (so `return false` in a say command lets the text appear as normal chat).
- Library shortcut: `U.cmd(name, flags, fn(id, args), desc)` registers `xmod_<name>` and `amx_<name>` for client + server console; `id == 0` for the server console.

### Cvars — `xmod.cvar.*`
`create(name, default [, flags])` (no-op if it exists; flags `xmod.cvar.SERVER|ARCHIVE|PROTECTED|SPONLY`) · `get(name)`→string · `num(name)`→float · `int(name)` · `set(name, value)` (no `"`, `;`, newline)
· `exists(name)` · `watch(name, fn(name, old, new))` (polled 2×/s) · `query_client(id, cvarname, fn(id, cvar, value))` (needs engine support).

### Timers — `xmod.timer.*` (return an id; callbacks receive the extra args; **return `false` to cancel**; survive map changes)
`after(seconds, fn, ...)` · `every(seconds, fn, ...)` · `times(count, seconds, fn, ...)` · `cancel(id)`.

### Storage and files
`xmod.kv.get(store, key [, default])` · `set(store, key, value)` (values are strings) · `del` · `save(store)` · `keys(store [, prefix])` — persisted in `data/<store>.kv` (flushed on map end/shutdown/save).
`xmod.fs.read(rel)` (relative to `addons/xmod/`) · `read_game(rel)` (relative to the mod dir, e.g. `mapcycle.txt`) · `write(rel, text [, append])` (only `data/…` and `configs/…`) · `exists(rel)` · `exists_game(rel)`. Paths with `..`, absolute paths and `:` are rejected. For tables use `xmod.vault`.

**`xmod.vault` — nVault + nVault-array replacement (use this for player data).** Values keep their type: numbers (integer / float),
strings, booleans and **tables** (arrays, nested). Every entry has a timestamp (unix seconds) for pruning. Files: `data/vaults/<name>.vault`,
written atomically with a `.bak` backup and a checksum (a damaged file is restored from the backup automatically). Saved on map end,
shutdown, `v:save()` and every 2 minutes when changed.
```lua
local v = xmod.vault.open("ranks")             -- name: 1..48 of [A-Za-z0-9_-]
v:set(key, value [, timestamp])                -- key: string/number (1..255 bytes); value nil = remove; functions cannot be stored
local value, ts = v:get(key [, default])
v:has(key) · v:remove(key) · v:touch(key [, ts]) · v:timestamp(key) · v:count() · v:clear() · v:save()
v:incr(key [, by=1]) -> new value             -- counters
v:keys([prefix]) -> {keys}                    -- sorted
v:each(function(key, value, ts) [return false to stop] end [, prefix])
v:prune(older_than_seconds) / v:prune(start_ts, end_ts) -> removed   -- entries with timestamp 0 are never pruned
xmod.vault.list() -> {names}
-- e.g.  v:set(xmod.player.steamid(id), { level = 7, xp = 1234, items = { "sword", "shield" } })
```

### AddToFullPack, performance and diagnostics
- `xmod.fullpack.set_render(mode, amount, fx, r, g, b)` (nil keeps a value) · `get_render()` · `set_effects(mask)` · `set_origin(x,y,z)` — only inside `add_to_full_pack:post` handlers. Constants: `RENDER_NORMAL/TRANSCOLOR/TRANSTEXTURE/GLOW/TRANSALPHA/TRANSADD`, `FX_NONE/GLOWSHELL/PULSE_SLOW/PULSE_FAST/DISTORT`, `EF_NODRAW/BRIGHTLIGHT/DIMLIGHT`.
- **Native hide table (fast path, no Lua per entity):** call `xmod.fullpack.native()` once at plugin load (installs the hook), then at a low rate (e.g. 1 Hz timer) call `xmod.fullpack.hide(host, ent, true|false)`, `xmod.fullpack.hide_all(ent, true|false)`, `xmod.fullpack.clear()`. C++ applies the table every frame (~14 ns per check). The table is cleared on map change. `xmod.maxentities()` gives the edict count.
- `xmod.stats.enable(bool)` · `reset()` · `get()` → list `{name, calls, total_us, avg_us, max_us, handlers}` sorted by CPU time (names: event names, `<timers>`, `<commands>`) · `info()` → `{frames, wall_s, fps, lua_kb, on, frame_ms_avg, frame_ms_max}`. Call counts are exact; timing is sampled (every call for the first 256, then 1 in 16), so it costs ~10 ns per event and `max_us` is the slowest *sampled* call (use `frame_ms_max` to spot hitches).
- `xmod.trace.line_n(...)` = `trace.line` without the result table (8 return values: fraction, ent, x, y, z, nx, ny, nz) — ~10x cheaper.
- **Real listen-server measurement (Windows build, 1 human + bots, 20 000 add_to_full_pack calls/s):** a counter-only Lua handler costs ~0.63 µs per call (about 1.3 % of a core at that rate); once-per-frame events (prethink, cmd_start, frame) show 3–30 µs per call because their code/data is cold in cache by the time the next frame comes. Mock-engine numbers below are 5–7x lower because they run in a tight loop with warm caches.
- Measured on a 3 GHz-class x86 core with the mock engine (not a live server!): empty Lua function call ~13 ns; `xmod.player.health(id)` ~18 ns; `xmod.ent.get(e,"health")` ~27 ns (`"origin"` ~50 ns); `xmod.cvar.int` ~200 ns; `string.format` ~450 ns; table creation ~105 ns; one hooked Lua call from the engine (dispatch + handler) ~85–90 ns idle, ~180 ns with two native calls and math. A full 20-player server can see ~1–3 million `add_to_full_pack` calls/s: Lua handlers there cost 10–50 % of a core, the native hide table ~2 %. **Rule: per-entity/per-frame hooks should do almost nothing in Lua; decide in Lua at low frequency, apply natively.**
- `plugins/xmod_debug.lua` (+ `configs/xmod_debug.cfg`) is the diagnostics plugin: logs everything to `data/xmod_debug.log`, writes PERFORMANCE REPORTS from `xmod.stats`, `xmod_dbg test` (API self-test, PASS/FAIL), `xmod_dbg bench` (micro-benchmarks), AddToFullPack stress modes (`xmod_dbg mode 0-4`).

### Extensions, async calls and `xmod.query` (SQL / HTTP / sockets)
- **Extensions** are separate Metamod plugins (C++) that add functions as `xmod.<namespace>.<fn>`. They are listed after xmod in metamod's `plugins.ini` and attach **after** the Lua scripts were loaded: use their functions from `config`/`init`/gameplay events and commands, **never at the top level of a script**. Namespaces listed in `configs/extensions.ini` (`query` is) exist as tables at load time, so `local q = xmod.query` at the top is fine. `xmod.ext.available("query" [, "http_get"])` → bool · `xmod.ext.list()` → `{name, version, functions, loaded}` · console `xmod ext`. Calling a function whose extension is not loaded raises a Lua error. To write a new extension, see Part III.
- **Async convention:** the **last argument is a callback `function(err, result)`**; `err` is `nil` on success, otherwise a string and `result` is `nil`. The call returns a job id at once; the callback runs later on the game thread (never while your current function is still running). `xmod.async.cancel(id)` drops the callback · `xmod.async.stats()`. Always re-check `xmod.player.connected(id)` inside callbacks. Callbacks are dropped after `xmod reload`.
- **`xmod.query`** (extension `xmod_query`; settings, credentials and allow-lists in `configs/query.ini` — scripts never see passwords):
  - Sync: `status()` → `{version, sqlite, mysql (bool), mysql_note, http, profiles, open_databases, open_sockets}` · `profiles()` → `{{name, driver}}` · `reload_config()`.
  - SQL: `open(profile, cb)` → `db` handle (integer; profile = `[db:NAME]` in query.ini, driver `sqlite` or `mysql`) · `exec(db, sql [, params], cb)` → `{rows = {{col = value, ...}, ...}, columns = {...}, affected, insert_id, truncated}` · `close(db, cb)`. `params` is an array for `?` placeholders (numbers, strings, booleans; SQL NULL = `require("query").NULL`) — **always use placeholders for player-supplied text**. NULL columns are absent from a row (`row.col == nil`). All `exec` calls on one handle run **in the order you call them**, one at a time, so you may fire several without nesting callbacks (`BEGIN`, statements, `COMMIT`). Results are capped at `max_rows` (then `truncated = true`). Open one handle in `config` and keep it; do not open per query. SQLite files live in `addons/xmod/data/`. SQLite accepts several `;`-separated statements when there are no params; MySQL takes one statement per `exec`.
  - HTTP: `http_get(url [, opts], cb)` · `http_post(url, body [, opts], cb)` · `http(method, url [, opts], cb)` → `{status, body, headers (lower-case keys), truncated, backend}`. `opts`: `headers = {["Content-Type"] = "application/json"}`, `body`, `timeout` (seconds), `max_bytes`, `follow` (default true). A 404/500 is a normal result (check `status`); `err` is only for network/TLS/timeout/not-allowed. HTTPS: built in on Windows; on Linux it needs the 32-bit libcurl installed (plain http works without). Use `require("json")` for JSON bodies.
  - Sockets: `tcp_request(host, port, data [, opts], cb)` (connect, send, read, close) → `{data, bytes, timed_out, closed}`; opts `timeout`, `max_bytes`, `read_until = "\n"` (stop when this text was received), `read = false` (send only) · `udp_request(host, port, data [, opts], cb)` → `{data, from}` · `resolve(host, cb)` → list of IPs · persistent: `tcp_open(host, port [, opts], cb)` → handle, `tcp_send(h, data [, opts], cb)` → bytes, `tcp_recv(h [, opts], cb)` → `{data, bytes, timed_out, closed}`, `tcp_close(h, cb)` (calls on one handle run in order). Hosts must be allowed by `http_allow` / `socket_allow` in query.ini.
  - Helper lib: `local Q = require("query")` → `Q.NULL`, `Q.with(profile, function(db, done) ... done() end [, on_error])`. Example plugin: `plugins/examples/query_demo.lua`.
```lua
local db
xmod.on("config", function()
  if db or not xmod.ext.available("query") then return end
  xmod.query.open("main", function(err, h) if err then return xmod.error(err) end
    db = h; xmod.query.exec(db, "CREATE TABLE IF NOT EXISTS pts(steamid TEXT PRIMARY KEY, points INTEGER NOT NULL DEFAULT 0)", function(e) if e then xmod.error(e) end end)
  end)
end)
xmod.cmd.say("/points", "", function(id)
  if not db then return end
  xmod.query.exec(db, "SELECT points FROM pts WHERE steamid = ?", { xmod.player.steamid(id) }, function(err, res)
    if err or not xmod.player.connected(id) then return end
    xmod.msg.chat(id, "{g}[pts] {y}You have {t}" .. (res.rows[1] and res.rows[1].points or 0) .. "{y} points")
  end)
end)
```
- `xmod.cvar.get/num/int` return an empty value for `rcon_password` and cvars flagged protected (`sv_password`).

### Sound and precache
`xmod.sound.emit(ent, channel, "sample.wav" [, volume=1, attn=0.8, flags=0, pitch=100])` (channels `xmod.sound.CHAN_AUTO/WEAPON/VOICE/ITEM`)
· `xmod.precache.sound/model/generic(path)` — **only inside the `precache` event**. Return the engine index, or **0** when the precache was
refused (the server never crashes): outside the precache event, file missing on the server, a non-model passed to `model()` (only .mdl/.spr/.bsp),
a `sound/` prefix given to `sound()` (sound paths are relative to `sound/`), bad path, or the engine list nearly full (512). Every refusal is
logged with the plugin's file and line.

## 7. Admin system (plugins/admin.lua + admin_cmds.lua)
- `configs/users.ini` lines: `"auth" "password" "access" "account flags"`. Account flags: `a` kick on bad password, `b` auth is clan tag (name prefix), `c` auth is SteamID, `d` auth is IP, `e` skip password check; none = exact name. Password: player runs `setinfo _pw "pass"`.
- Access letters: `a` immunity · `b` reserved slot · `c` kick · `d` ban/unban · `e` slay/slap/team/gag · `f` map/restart · `g` cvars · `h` configs/admin management · `i` admin chat/say · `j` votes · `k` sv_password · `l` rcon · `m` fun (hp, god, noclip, teleport, give…) · `n`–`t` free for custom plugins · `u` admin menu · `z` normal user (everybody has `z` via `xmod_default_access`).
- Console: `xmod_who`, `xmod_admins`, `xmod_reloadadmins`, `xmod_addadmin <auth> <access> [pass] [flags]`, `xmod_removeadmin <auth>`; moderation: `xmod_kick/ban/addban/unban/slay/slap/team/swap/nick/map/restart/cvar/cfg/rcon/votemap/votekick/vote/cancelvote/say/csay/hsay/psay/chat/gag/ungag`; fun: `xmod_hp/armor/money/god/noclip/freeze/blind/give/teleport/bring/goto`. Every command also exists as `amx_<name>`. Targets: name part, `#userid`, `STEAM_…`, `@all @t @ct @spec @alive @dead @bots @me`. Say: `/admin` menu, `@text` talks to admins.
- Exports usable by other plugins: `xmod.call("admin.kick", admin_id, target_id, reason)`, `"admin.ban"(admin, target, minutes, reason)`, `"admin.slay"(admin, target)`, `"admin.slap"(admin, target, damage)`, `"admin.team"(admin, target, 1|2|3)`, `"admin.map"(admin, mapname)`, `"maps.list"()`, `"admin.reload"()`.
- Cvars: `xmod_show_activity` (0/1/2), `xmod_default_access`, `xmod_admin_join_msg`, `xmod_flood_time`, `xmod_gag_default`, `xmod_vote_*`, `xmod_extend_*`, `xmod_ads_*`, `xmod_reserved_slots`, `xmod_killstreaks`.

## 8. Libraries (`require("name")`; each returns a table)
**util**: `flag(letter)` · `flags_to_string(mask)` · `split(s, sep)` · `trim(s)` · `startswith(s, p)` · `clamp(v, lo, hi)` · `fmt_time(sec)` ("m:ss") · `shuffle(t)` ·
`name(id)` (0 → "SERVER") · `reply(id, text)` (console or server) · `say(text)` (chat to all) · `activity(admin, text)` (respects xmod_show_activity) · `log(file, text)` (→ data/file) ·
`cmd(name, flags, fn, desc)` · `target(caller, arg, {multi, alive, dead, nobots, self})` → `list | nil, err` · `target1(caller, arg, opts)` → `id | nil, err` (respects immunity) ·
`each_player(fn)` · `count_humans()` · `is_admin(id)` · `admins()` · `timeleft()` (seconds or nil if no limit) · `IMMUNITY`.
**menu**: `menu.show(id, {title="…", items={{"Label", value}, …}, on_select=function(id, label, value) end, timeout=-1})` (auto-paged: 7 items/page, 8 next, 9 back, 0 exit) · `menu.close(id)` · `menu.is_open(id)`.
**vote**: `vote.start{title, options={…}, time=15, voters=function(id) return true end, on_done=function(winner_index|nil, counts, total) end}` → `true | false, reason` · `vote.running()` · `vote.finish()`.
**json**: `json.encode(value [, pretty])` · `json.decode(str)` → value | nil, err.
**query**: `Q.NULL` (SQL NULL in parameter lists) · `Q.with(profile, function(db, done) ... done() end [, on_error])` (open, use, close) — see "xmod.query" above.
**nexus**: the client-side library — Part II.
Writing a library: put `lib/<name>.lua`, end with `return M`. Libraries are loaded once, run as their own pseudo-plugin (handlers they register belong to the library).

## 9. AMX Mod X → xmod cheat sheet
| AMXX (Pawn) | xmod (Lua) |
|---|---|
| `register_plugin(n,v,a)` | `xmod.register(n,v,a)` |
| `plugin_init / plugin_precache / plugin_cfg / plugin_end` | top-level code / `precache` / `config` / `shutdown` |
| `register_clcmd("x", "fn", ADMIN_KICK)` | `xmod.cmd.client("x", "c", fn)` (say: `xmod.cmd.say`) |
| `register_concmd` (both consoles) | `U.cmd("x", "c", fn)` |
| `register_srvcmd` | `xmod.cmd.server` |
| `read_argv(1, buf)` / `read_args` | `args[1]` / `args.text` |
| `register_event("DeathMsg","fn","a")` | `xmod.on("message:DeathMsg", fn)` |
| `register_logevent` | use `xmod.on("message:…")`, reapi `CSGameRules_*` hooks, or chat/`client_command` |
| `client_connect/putinserver/disconnect/infochanged` | same event names |
| `client_PreThink / client_PostThink` | `prethink` / `postthink` |
| `RegisterHam(Ham_TakeDamage,"player","fn")` | `xmod.on("ham:takedamage:player", fn)` (`:post` = Post) |
| `SetHamParamFloat(4, v)` | `xmod.hook.set_arg(3, v)` (n excludes `this`) |
| `return HAM_SUPERCEDE` / `SetHamReturnInteger` | `return xmod.HC_SUPERCEDE, value` |
| `RegisterHookChain(RG_CBasePlayer_TakeDamage, "fn")` | `xmod.on("reapi:CBasePlayer_TakeDamage", fn)` |
| `set_task(2.0,"fn",id)` / `remove_task` | `xmod.timer.after(2.0, fn, id)` / `xmod.timer.cancel(tid)` (repeat: `every`, `times`) |
| `get_user_name/authid/ip/userid` | `xmod.player.name/steamid/ip/userid` |
| `is_user_connected/alive/bot` | `xmod.player.connected/alive/is_bot` |
| `get_user_health / set_user_health` (also armor, frags) | `xmod.player.health/set_health` … |
| `get_user_origin / entity_get_vector(id, EV_VEC_origin)` | `xmod.player.origin(id)` / `xmod.ent.get(e, "origin")` |
| `pev(id, pev_health)` / `set_pev` | `xmod.ent.get(id, "health")` / `xmod.ent.set(id, "health", v)` |
| `get_pdata_int(id, ofs, linux)` | `xmod.mem.get_int(id, ofs, linux)` |
| `cs_get_user_team / cs_set_user_team` | `xmod.player.team / set_team` |
| `cs_get_user_money / cs_set_user_money` | `xmod.player.money / set_money` |
| `give_item / fm_give_item` | `xmod.player.give_item` |
| `client_print(id, print_chat, …)` / `client_print_color` | `xmod.msg.chat(id, "{g}…")` / `xmod.msg.print(id,"chat"|"console"|"center", …)` |
| `set_hudmessage + show_hudmessage` | `xmod.msg.hud(id, text, {…})` |
| `message_begin/write_byte/message_end` | `xmod.msg.begin/byte/finish` |
| `menu_create / menu_additem / menu_display` | `require("menu").show(id, {…})` |
| `get_cvar_string / get_pcvar_num / register_cvar` | `xmod.cvar.get / int / create` |
| `nvault_open / nvault_get / nvault_set / nvault_prune / nvault_touch` | `xmod.vault.open(name)` → `v:get / v:set / v:prune / v:touch` |
| `nvault_array` / nvault + strings packed with `^n` | `v:set(key, {tables, arrays})` — stored as real tables |
| `client_print_color(id, sender, "^4…")` | `xmod.msg.chat(id, "^4…" [, sender])` (AMXX codes work as-is) |
| `get_players(...)` | `xmod.player.all()` |
| `cmd_target(...)` | `U.target / U.target1` |
| `server_cmd("…")` | `xmod.exec("…")` |
| `get_mapname` / `get_timeleft` | `xmod.map()` / `U.timeleft()` |
| `emit_sound` | `xmod.sound.emit` |
| `engfunc(EngFunc_TraceLine…)` | `xmod.trace.line` |
| `create_entity / remove_entity / find_ent_by_class` | `xmod.ent.create / remove / find` |
| `fm_set_user_...` / fakemeta forwards (`FM_Touch`, `FM_Think`, `FM_EmitSound`, `FM_CmdStart`) | events `touch`, `think`, `emit_sound`, `cmd_start` |

## 10. Recipes
```lua
-- 1) Damage multiplier (works on stock CS via Ham)
xmod.on("ham:takedamage:player", function(victim, inflictor, attacker, dmg, bits)
  if attacker >= 1 and attacker <= xmod.maxplayers() then xmod.hook.set_arg(3, dmg * 1.5) end
end)

-- 2) Spawn protection (3 s godmode) + per-player data
xmod.on("ham:spawn:player:post", function(id)
  if not xmod.player.alive(id) then return end
  xmod.ent.set(id, "takedamage", 0)
  xmod.timer.after(3.0, function(pid)
    if xmod.player.connected(pid) and xmod.player.alive(pid) then xmod.ent.set(pid, "takedamage", 2) end
  end, id)
end)

-- 3) Persistent player rank with kv (+ chat command)
xmod.on("message:DeathMsg", function(_, _, _, a)
  local k = a[1]; if k < 1 or k == a[2] or not xmod.player.connected(k) then return end
  local key = xmod.player.steamid(k)
  xmod.kv.set("ranks", key, (tonumber(xmod.kv.get("ranks", key, "0")) or 0) + 1)
end)
xmod.cmd.say("/kills", "", function(id)
  xmod.msg.chat(id, "{g}[rank] {y}Lifetime kills: {t}" .. xmod.kv.get("ranks", xmod.player.steamid(id), "0"))
end)

-- 4) Admin-only command with target selection and logging
local U = require("util")
U.cmd("heal", "m", function(id, args)
  local list, err = U.target(id, args[1], { multi = true, alive = true })
  if not list then return U.reply(id, err) end
  for _, t in ipairs(list) do xmod.player.set_health(t, 100) end
  U.activity(id, "heal " .. #list .. " player(s)")
  U.log("admin.log", U.name(id) .. " healed " .. #list)
end, "heal <target|@group>")

-- 5) Block a say command + custom event between plugins
xmod.on("chat", function(id, text) if text:lower():find("badword") then xmod.msg.chat(id, "{g}No swearing!"); return true end end)
xmod.export("myplugin.double", function(n) return n * 2 end)      -- other plugin: xmod.call("myplugin.double", 21)
```

## 10b. When a plugin breaks
- Errors are printed to the server console (also on listen servers), the Metamod log and `addons/xmod/data/xmod_errors.log`, as:
  plugin + file, the message, **the source line with its neighbours**, a plain-language **hint** and the call chain. Misspelled xmod functions get
  **"did you mean xmod.msg.chat?"**.
- At load time every plugin's `xmod.*` names are checked: `plugins/x.lua:12: 'xmod.msg.chatt' does not exist - did you mean xmod.msg.chat ?`
- `xmod plugins` shows the reason next to a DISABLED plugin (25 errors disable a plugin). `xmod check` re-checks every file (syntax + names) without running it.
- Ownership follows the code: a plugin function called back by a library (Nexus `on_click`, menus) counts as that plugin (`xmod.player.data`, timers, `xmod.on`).

## 11. Limits, gotchas, not (yet) available
- Lua itself has no sockets/SQL/threads: use the `xmod.query` extension (async) for SQL, HTTP and sockets, `xmod.vault` for local data. Anything else that must block needs a C++ extension (Part III). No listening (server) sockets.
- `team/money` use CS private-data offsets (team 114, money 115 ints, +5 on Linux) — correct for CS 1.6 / ReGameDLL; other mods differ.
- `reapi:*` only with ReGameDLL; `xmod.reapi.available()` tells. No reapi "rg_*" helper functions yet (round end/restart: `xmod.exec("sv_restartround 1")`).
- Ham: Linux/Windows virtual indices come from AMXX's CS gamedata; functions with `Vector` by value are not hookable. Windows uses thiscall hooks (tested on Linux only).
- Messages: `message:*` hooks observe/block but cannot modify values. `xmod.msg.*` writers bypass other Metamod plugins' hooks.
- A timer/event callback must finish within `xmod_script_timeout` ms (default 100); heavy work must be split across timers.
- `add_to_full_pack` is the only hook that is installed lazily: it exists only if a plugin registered it (or called `xmod.fullpack.native()`) at server start, so servers that don't use it pay nothing.
- Plugins cannot define new entity classes. `xmod.ent.create` only creates existing engine/game classes.
- Everything is in this file; `docs/REAPI_HOOKS.md` and `docs/HAM_FUNCTIONS.md` hold the same tables as Appendices A and B.

# Part II — Nexus 2.0 (client extension, built into xmod)

Nexus = a module **built into xmod** + a client library players install (`client/` in the xmod package). Lua plugins use it through
`local N = require("nexus")`: they draw HUD elements, world labels, menus, read keys and mouse clicks, move the camera, play music,
open links (player-confirmed), change the player's view (FOV, shake, sway, punch) and input (block / force / sensitivity), change how
entities look **for that player only**, draw 3D shapes in the world, add dynamic lights, particle effects and positional sounds, and
talk to native client plugins. Players without the client simply do not see these things.

---------------------------------------------------------------------------------------------------------------------------
## N1. Rules an AI must follow

1. Use the library: `local N = require("nexus")`. Raw `xmod.nexus.*` exists (section N6) but the library adds names, colours,
   style memory, callbacks and fallbacks.
2. **Build the HUD in `N.on_ready(function(id, info) ... end)`**. It fires when the player's client finished the handshake
   (≈2 s after joining) and **again after every map change** (the client drops all elements on map change). Never build HUDs in
   `client_putinserver` — the client is not ready yet and the calls just address 0 players.
3. Every function takes a player id; **0 = every player that has Nexus**. Players without the client are skipped silently
   (calls return the number of players addressed). Always keep a non-Nexus path for anything important (chat, classic menu).
   `N.has(id)` tells you whether a player has it.
4. Elements are **persistent**: create once, then update only what changed (`N.set(id, "hp", {value = 0.4})`). Do **not**
   re-send everything every frame. Prefer `N.anim` (runs on the client) over timers that move things.
   Update at most a few times per second per element; the server paces traffic (`nexus_rate`, default 5000 B/s per player).
5. Positions: `x, y` are fractions of the screen (0..1), `px, py` are virtual pixels (the screen is 768 virtual pixels high,
   × the player's `nexus_scale`). Use `anchor` to stick things to corners/edges so they work on every resolution.
6. Element names are global across all plugins (one library instance). Prefix them with your plugin name: `"shop.bg"`.
7. Paths (images, sounds, music) are relative, `[A-Za-z0-9_./-]`, no `..`, max 63 chars. Files must exist on the client:
   precache/download them (`xmod.precache.generic("gfx/myplugin/icon.png")` in the `precache` event; sounds with
   `xmod.precache.sound`). Music: `.mp3` relative to the game dir (e.g. `sound/myplugin/theme.mp3`, precache_generic it).
8. Text is UTF-8, max 1024 bytes per element, max 64 lines. Escape player-provided text with `N.escape()` when `markup = true`.
9. Keys/cursor change how the game feels: only capture keys while a feature needs them, release them after (`N.unbind`,
   `N.cursor(id, false)`). ESC and the console key can never be captured; players can turn key reporting off (`nexus_keys 0`).
10. Keep a plugin's UI state in **local tables of the plugin** (`local open = {}`), not `xmod.player.data`: with xmod 0.3.0,
   code called back by the library (`on_click`, binds, `on_ready`) runs as `lib:nexus`, so `xmod.player.data` there returns the
   library's table. xmod 0.3.1 fixes that (ownership follows the code), local tables work with both.
11. `N.unbind(id)` without a key removes **every** bind of that player, also other plugins' binds: unbind your own keys by name.
12. Errors in your callbacks are caught by the library and logged as `[lib:nexus] error in on_click callback: plugins/x.lua:LINE: ...`.
13. Nexus 2 effects are **client-side**: perfect for feel and visuals, but a modified client can ignore them. Anything that must be
   enforced (a freeze, a speed limit, who can shoot) must ALSO be enforced on the server (e.g. `xmod.ent.set(id, "maxspeed", 1)`).
   Check `N.info(id).fx` before relying on them (older clients ignore them; the calls just address 0 players).
14. Never trust client data: `nexus_data` / click / key events come from the player's PC. Validate everything.

---------------------------------------------------------------------------------------------------------------------------
## N2. Install (summary)
Server: nothing to do — Nexus is part of xmod 0.4+ (`nexus.lua` ships in `addons/xmod/lib/`). If an older standalone
`nexus_mm` line is still in `addons/metamod/plugins.ini`, remove it. Console: `nexus` shows every player's state.
Player: rename `cstrike/cl_dlls/client.dll` → `client_orig.dll` (Linux `client.so` → `client_orig.so`), copy Nexus
`client.dll`/`client.so` there. Console: `nexus_info`. Load problems: read `cl_dlls/nexus_load.log` (rewritten every start).

---------------------------------------------------------------------------------------------------------------------------
## N3. Lua library — `local N = require("nexus")`

### Players
| function | returns |
|---|---|
| `N.available()` | the server extension is loaded |
| `N.has(id)` | player has a ready Nexus client |
| `N.info(id)` | `{version, proto, w, h, gl, cursor, world, images, urls, caps}` or nil. `urls=false` → the player switched links off (or client < 1.1). `gl=false` → software renderer: **images are not drawn**, text uses the engine font |
| `N.players()` | list of ids with Nexus |
| `N.on_ready(fn(id, info))` | register; also called at once for players already ready |
| `N.on_lost(fn(id))` | disconnect / map change / client gone |

### Elements
`N.set(id, name, props)` creates or updates; only given props change. `name` = string (→ id 20001..60000) or a number
(raw id 1..20000 is yours; ids never collide with names). Shortcuts set the type: `N.text`, `N.rect`, `N.image`, `N.bar`
(`(id, name, props)`). `N.remove(id, name)`, `N.clear(id [, group])`, `N.hide(id, name)`, `N.show(id, name)`.
Limit: 512 elements per player on the client.

| prop | meaning |
|---|---|
| `type` | `"text"` `"rect"` `"image"` `"bar"` (needed the first time; shortcuts set it) |
| `x, y` | position, fraction of screen width/height, measured from the anchor point |
| `px, py` | extra offset in virtual pixels (768 = screen height) |
| `anchor` | `"topleft"` (default) `"top"` `"topright"` `"left"` `"center"` `"right"` `"bottomleft"` `"bottom"` `"bottomright"` — the screen point **and** the point of the element placed there. `anchor="bottomright", px=-10, py=-10` → 10 px from that corner |
| `w, h` / `pw, ph` | size (fraction / virtual px). rect & bar need a size. **text:** width = wrap width (word wrap). **image:** omitted → natural size; only one given → keeps aspect |
| `color` | `"#RRGGBB"`, `"#RRGGBBAA"`, name (`white black red green blue yellow orange cyan magenta gray gold ct t spec`), `{r,g,b[,a]}` |
| `color2` | text: background box (padded) · bar: track · rect with `outline=true`: border colour |
| `scale` | text/image size factor; text is 16 virtual px high at 1 |
| `text` | UTF-8, `\n` = new line |
| `markup` | `true`: `^1` red `^2` green `^3` yellow `^4` blue `^5` cyan `^6` magenta `^7` white `^8` gray `^9` orange `^0` element colour, `^^` = `^` |
| `shadow`, `outline` | text drop shadow / 8-way outline (rect: border, uses color2) |
| `image` | `.png .tga .bmp .jpg` path below the game dir (≤2048×2048; 256 different images per client) |
| `value` | bar fill 0..1 (horizontal if w ≥ h, else vertical, filling upwards) |
| `layer` | -127..127, higher draws on top (default 0; menus use 90+) |
| `group` | 0..254, for `N.clear(id, group)`. **250 is used by N.menu** |
| `additive` | additive blending (images) |
| `hidden` | keep but don't draw |
| `on_click` | `function(id, name, button)` (button 1 left, 2 right, 3 middle). Needs the cursor on (`N.cursor`). Hover is highlighted. `false` removes it |
| `ent` + `offset={x,y,z}` | glue to an entity (players: their id). Hidden when the entity is not sent to that client (out of PVS / dead / gone) |
| `pos3={x,y,z}` | glue to a fixed world point |
| `maxdist` | world elements: hidden beyond this distance (fades the last 15 %) |
| `occlude` | world elements: hidden behind world geometry (trace from the eye) |
| `distscale` | world elements: smaller when far (×0.35..1.6) |
| `life` | seconds, then the element deletes itself on the client |
| `fadein`, `fadeout` | seconds (fadeout = last part of `life`) |

World elements still use `anchor` for alignment around the projected point (`"bottom"` = centred above it).

### Animation (runs on the client)
`N.anim(id, name, {target..., time = 0.5, ease = "out"|"linear"|"inout", remove = false})`
targets: `x`+`y` (both), `px`+`py` (both), `alpha` 0..255, `scale`, `value`, `color`. Several targets in one call run together.
A new animation of the same property replaces the old one; setting `x/y/px/py` with `N.set` cancels position animations.
`remove = true` deletes the element when done. Max 8 running animations per element.

### Ready-made
| function | |
|---|---|
| `N.toast(id, text [, {time=3, color, y=0.22, scale=1.3, markup, box=true}])` | centred message, slides in, fades out |
| `N.label3d(id, name, ent, text [, {offset={0,0,40}, color, maxdist=1500, occlude=true, distscale=true, scale, markup, layer, group}])` | name tag above an entity |
| `N.float_text(id, {x,y,z}, text [, {color="yellow", time=1, rise=40, scale=1.2, maxdist=2500}])` | damage-number style text that rises and fades |
| `N.menu(id, menu)` | see below; returns `"nexus"`, `"classic"` (fallback) or `false` |
| `N.menu_close(id)`, `N.menu_open(id)` | |

Examples: `nexus_motd.lua` (paged window with buttons), `nexus_wheel.lua` (radial menu, cooldown bars, world markers), `nexus_song.lua` (YouTube links). A complete windowed UI (pages, buttons, keyboard, fallback) is in `lua/plugins/nexus_motd.lua` — copy its `put()` / `button()` helpers for your own windows.

`menu = { title, items = { {label, value [, disabled=true]}, ... }, on_select = fn(id, label, value) (return true to keep it open),
on_close = fn(id, reason) ("exit" | "replaced" | "lost" | "timeout"), cursor = false (true = mouse + clickable rows),
timeout, anchor = "left", x, y, px = 16, width = 300, bg, border, color, title_color }`.
7 items per page, keys 1-7 pick, 8/9 page, 0 exit. While open, digit keys are captured (weapon slots don't switch).
Players without Nexus get xmod's classic menu (`require("menu")`) with the same `on_select`.

### Input, camera, sound
| function | |
|---|---|
| `N.bind(id, key, fn(id, keynum, down) [, block])` | key: `"f"`, `"F3"`, `"TAB"`, `"MOUSE4"`, `"MWHEELUP"`… or a number. `block=true`: the game doesn't see it. Max 48 keys per player. Keys typed in chat/console are not seen |
| `N.unbind(id [, key])` | one key or all |
| `N.keynum(key)`, `N.KEY` | key numbers (letters = lower-case ASCII; `F1`=135…`F12`=146, `MOUSE1`=241) |
| `N.cursor(id, on)` | mouse cursor; while on, the view is frozen, attack is blocked, clicks go to `on_click` |
| `N.camera(id, {mode="third", dist=120, height=0, side=0, collide=true, time=0.4})` | third person (experimental) |
| `N.camera(id, {mode="fixed", pos={x,y,z}, angles={pitch,yaw,roll}, time})` | fixed camera; `N.camera(id)` = back to normal |
| `N.sound(id, "path.wav" [, vol 0..1])` | local sound, path below `sound/` |
| `N.music(id, "sound/x/theme.mp3" [, loop])` / `N.music(id)` stop | per-player MP3 |
| `N.send(id, channel, data)` | to native client plugins; channel ≤31 chars, data ≤96 bytes (binary ok) |
| `N.open_url(id, url, label [, cb(id, result)])` | ask to open an **https** link in the player's browser. The CLIENT shows its own prompt (host name big, Y = open, N/ESC = no, 20 s timeout); the server can't answer it. `result`: `opened` `declined` `disabled` `busy` `bad_url` `no_nexus`. Max 200 chars, one prompt at a time, 5 s apart. Fullscreen players must Alt+Tab to the browser |
| `N.youtube(id, words [, cb])` | `open_url` of a YouTube search for `words` |
| `N.urlencode(text)` | for query strings (`%20` etc.) |
| `N.on_data(channel, fn(id, data))` | from client plugins |

### Nexus 2 — view, input, entities, world (client-side; need `N.info(id).fx`)
| function | |
|---|---|
| `N.fov(id, fov [, seconds=0.25])` / `N.fov(id, 0 [, s])` | smooth zoom / widen; 0 = back to the game's fov (the game's own zoom still works underneath) |
| `N.shake(id [, degrees=2, frequency=12, seconds=0.8])` | camera shake that fades out |
| `N.sway(id, degrees [, speed=1])` / `N.sway(id, 0)` | slow drunk / underwater sway |
| `N.punch(id, pitch, yaw)` | view kick that settles by itself (recoil / hit feedback) |
| `N.input(id, {block = {...}, force = {...}, sens = 1, invert_x, invert_y})` / `N.input(id)` | `block`: `attack attack2 jump duck move strafe use reload look`; `force`: `duck walk jump attack attack2 use`; `sens` mouse multiplier (0..10); `look` freezes the view |
| `N.freeze(id, on)` · `N.drunk(id, strength)` | presets of the above (`N.drunk(id, 0)` = sober) |
| `N.entity(viewer, ent, {hide = true} / {rendermode=, renderamt=, color=, renderfx=, scale=})` | how one entity looks **for this viewer only**. `rendermode`: `normal color texture glow alpha add`; `renderfx`: `glowshell hologram pulse_fast distort ...`; `N.entity(viewer, ent)` removes; `N.clear_entities(viewer)` |
| `N.glow(viewer, ent, color [, thickness=20])` / `N.glow(viewer, ent)` | glowing outline (team highlights, objectives) |
| `N.shape(id, name, {type="line", from=, to=} / {type="box"|"quad", mins=, maxs=} / {type="circle"|"cylinder", center=, radius=, height=, segments=}, color=, width=2, fill, xray (through walls), additive, pulse, ent=e (coords become offsets, follows it), life=s)` | 3D lines / boxes / zones in the world (OpenGL only), max 256 |
| `N.remove_shape(id, name)` | |
| `N.light(id, name, {pos= | ent=, offset=, radius=200, color=, life=s})` / `N.remove_light(id, name)` | coloured dynamic light, can follow an entity, max 32 |
| `N.effect(id, kind, {pos=, to=, color=, size=, count=, life=})` | particles: `explosion sparks teleport lava blob burst implosion beam` (beam needs `to`; burst `color` = palette index in r) |
| `N.sound3d(id, "path.wav", {pos= | ent=, volume=1, attn=0.8, pitch=100})` | sound heard from a place, only by this player |
| `N.tint(id, color [, fade=0.3, hold])` / `N.tint(id)` | full-screen colour (night vision, damage, underwater) |

Example: `nexus_fx.lua` (say /fx) uses every one of these.

### Helpers
`N.color(c [, alpha])` → `{r,g,b,a}` · `N.escape(text)` (for markup) · `N.id(name)` · `N.temp_id()` (60001..65535, for elements
with `life`).

---------------------------------------------------------------------------------------------------------------------------
## N4. Examples

```lua
xmod.register("myhud", "1.0", "me")
local N = require("nexus")

N.on_ready(function(id)
  N.rect(id, "myhud.bg",  { anchor = "bottomleft", px = 16, py = -16, pw = 220, ph = 40, color = {0, 0, 0, 160}, color2 = "gold", outline = true })
  N.text(id, "myhud.lvl", { anchor = "bottomleft", px = 28, py = -44, text = "Level 1", color = "gold", shadow = true, layer = 1 })
  N.bar(id,  "myhud.xp",  { anchor = "bottomleft", px = 28, py = -24, pw = 196, ph = 6, value = 0, color = "#4CE06A", color2 = {255,255,255,40}, layer = 1 })
end)

local function set_xp(id, level, frac)                     -- call when it changes, not every frame
  N.set(id, "myhud.lvl", { text = "Level " .. level })
  N.anim(id, "myhud.xp", { value = frac, time = 0.4 })
end

-- label over every player's head for every Nexus player
N.on_ready(function(viewer)
  for _, p in ipairs(xmod.player.all()) do
    if p ~= viewer then N.label3d(viewer, "myhud.tag." .. p, p, N.escape(xmod.player.name(p))) end
  end
end)
xmod.on("client_disconnect", function(p) N.remove(0, "myhud.tag." .. p) end)

-- a clickable menu with the mouse
xmod.cmd.say("/shop", "", function(id)
  N.menu(id, { title = "Shop", cursor = true, items = { {"AK-47", "ak"}, {"Armor", "armor"} },
    on_select = function(p, label, value) if value == "ak" then xmod.player.give_item(p, "weapon_ak47") end end })
end)

-- key without a bind
N.on_ready(function(id) N.bind(id, "F3", function(p, _, down) if down then N.toast(p, "F3 pressed") end end, true) end)
```

---------------------------------------------------------------------------------------------------------------------------
## N5. Events (also usable directly with `xmod.on`)
| event | args |
|---|---|
| `nexus_ready` | id, info |
| `nexus_lost` | id |
| `nexus_key` | id, keynum, down (bool) — only keys you subscribed |
| `nexus_click` | id, elem_id (0 = clicked on nothing), button 1..3 |
| `nexus_data` | id, channel, data (string, may contain zero bytes) |
| `nexus_url` | id, request, result (1 opened, 0 declined/timeout, 2 disabled, 3 busy, 4 bad url) — the library turns this into the `open_url` callback |

---------------------------------------------------------------------------------------------------------------------------
## N6. Raw extension API `xmod.nexus.*` (all synchronous; they only queue bytes)
`has(id)` · `info(id)` · `players()` · `set(id|0, elem 1..65535, props)` (props as the library, but `type` always required,
colours only as `{r,g,b,a}` tables, `click=true` instead of `on_click`; **the style fields `anchor layer group` + flags are one
block: if you send any of them, send all** — the library does that for you) · `del(id|0, elem)` · `clear(id|0 [, group])` ·
`anim(id|0, elem, {...})` · `keys(id|0, { keynum | {key=, block=} ... })` (replaces the list) · `cursor(id|0, bool)` ·
`sound(id|0, path [, vol])` · `music(id|0 [, path, loop])` · `camera(id|0, {...})` · `send(id|0, channel, data)` · `reset(id|0)` ·
`open_url(id, url, label)` → request id (0 = no Nexus, -1 = links not available) · `stats()` → `{ready, without_nexus, sent_bytes, sent_messages, queued_ops, dropped_ops, version, proto}`.
Errors (bad type, bad path, too long) raise Lua errors. Returns = number of players addressed.

---------------------------------------------------------------------------------------------------------------------------
## N7. Cvars and commands
Server: `nexus_rate` (bytes/s per player, default 5000, min 500) · `nexus_debug 1` (log handshakes) · console `nexus`.
Client: `nexus_enable 0/1` · `nexus_scale` (0.5..3, UI size) · `nexus_keys 0` (never report keys) · `nexus_cursor_speed` ·
`nexus_cl_debug` · `nexus_urls 0` (never show link prompts) · `nexus_version` (read by the server to detect Nexus) · command `nexus_info`.

---------------------------------------------------------------------------------------------------------------------------
## N8. Native client plugins (C/C++)
`include/nexus_plugin.h`. A plugin exports `int NexusPlugin_Init(const NexusApi *api, NexusPlugin *out)` and returns
`NEXUS_PLUGIN_ABI`. It is loaded only if the **player** lists it in `cstrike/nexus/plugins.ini`; the file is
`cstrike/nexus/plugins/<name>.dll` (`.so`). Servers can never install or enable plugins.
Callbacks: `on_frame`, `on_draw` (after Nexus elements; use `api->rect/text/text_width/image/image_draw/world_to_screen`,
real pixels, colours `0xRRGGBBAA`), `on_key` (return 1 = swallow), `on_data(channel, data, len)`, `on_session(ready)`,
`on_shutdown`. `api->send(channel, data, len)` → Lua `N.on_data`. `api->engine` = the SDK `cl_enginefunc_t`.
Example: `examples/client_plugin/fps_plugin.cpp` (FPS counter; answers channel `fps.query` with `fps.value`).

---------------------------------------------------------------------------------------------------------------------------
## N9. How it works / limits
* Detection: ~1.5 s after a player takes a slot the server queries the client cvar `nexus_version`; if present it sends HELLO with
  a random token, the client answers `nexus_hello <proto> <version> <w> <h> <caps> <token>`. Then `nexus_ready` fires.
  Old protocol → ignored (logged). Engines without client cvar queries → Nexus stays off.
* Server → client: user message `Nexus` (≤190 bytes, several ops each), per-player queue (max 4096 ops, updates of the same
  element are merged), token-bucket pacing so the reliable channel never overflows. Client → server: `nexus_*` console
  commands, limited to 80/s per player on the server and 60/s on the client.
* The client only does what `shared/nexus_proto.h` lists. Sound/music names are validated on both sides (no `;`, quotes,
  spaces, `..`, absolute paths); there is no op that executes commands or changes cvars.
* Drawing: after the game's HUD, OpenGL 1.1 on the engine's context with all state saved/restored; fallback to the engine's
  2D functions (software mode: rects + console font, no images).
* Client library: proxy `client.dll` that loads `client_orig.dll` from the same folder and forwards everything (all 43 SDK
  functions, `F`, `CreateInterface`); it hooks Initialize, HUD_Init, HUD_VidInit, HUD_Redraw, HUD_Frame, HUD_Key_Event,
  CL_CreateMove, CL_IsThirdPerson, V_CalcRefdef, HUD_Shutdown. No memory patching or signature scanning.
* Map change: server and client both drop the session; HELLO again → `nexus_ready` again.

## N10. Test status (be honest with users)
Tested in this repo: the server module inside xmod in a fake HLDS (72 checks incl. every Nexus 2 function and its error cases), the client core + renderer
on Mesa off-screen OpenGL and the engine fallback (~230 checks incl. 30 000 random messages, FOV / shake / input / entity overrides /
lights / particles / 3D sound, and a 3D render of shapes with an x-ray shape behind a wall), the shipped 32-bit `client.so` loaded like the engine,
and the shipped Windows `client.dll` the same way under Wine. **Not tested on a real game client or a real HLDS.** Nexus 1 features were
confirmed working by the user in game; Nexus 2 features (FOV, input control, entity overrides, shapes, lights, effects) are new and
need an in-game check. VAC behaviour of a replaced client.dll is unknown.

# Part III — Writing an xmod extension (a C++ Metamod plugin that plugs into xmod)

> **When the user wants a new native module**: a separate Metamod plugin in C++ that adds functions to Lua as
> `xmod.<namespace>.<function>(...)`. Follow this part exactly. Use only the API below (`include/xmod_ext.h`); do not edit
> xmod's core. Deliver: one directory `extensions/xmod_<name>/` with the `.cpp` and a `CMakeLists.txt`, the two install
> lines, and a short Lua usage example (Lua side: Part I). The template is `extensions/xmod_hello/`; a complete real
> example is `extensions/xmod_query/` (SQLite / MySQL / HTTP / TCP / UDP). Nexus is NOT an extension any more (it is built in).

## E1. The model in 6 rules
1. An extension is an ordinary Metamod plugin (`xmod_<name>_mm_i386.so` / `xmod_<name>_mm.dll`), loaded **after** xmod.
   It connects with `xmod_ext_connect()` and registers functions. `include/xmod_ext_glue.h` does all of that for you.
2. **Sync function** (`register_func`): runs on the game thread when Lua calls it. Must return fast. Never block there
   (no sockets, no SQL, no big files, no sleeping).
3. **Async function** (`register_async`): runs on a **worker thread** of xmod's pool and may block as long as it needs.
   Lua passes a callback as the **last** argument; xmod calls `callback(err, result)` later **on the game thread**.
4. Inside a worker you may call **only**: `job_*`, `val_*`, `post_main`, `emit_event`, `log`, `log_error`, `color_chat`,
   `client_print`, `is_main_thread`. **Never** touch the engine (`g_engfuncs`, `MESSAGE_BEGIN`, entities, cvars) or Lua from a worker.
   Need the engine after blocking work? `post_main(fn, user)` runs `fn` on the game thread at the next frame.
5. Data crosses the boundary as **value trees** (`XmodValue`): nil, bool, int (64-bit), number, string (binary-safe),
   array, map. A Lua array table becomes an array; any other table becomes a map with string keys. No functions/userdata.
6. Handles (database, socket, file…) are **integers** you hand to Lua. Register the functions that use a handle with
   `strand_arg = <1-based position of the handle argument>`: xmod then runs all jobs with the same handle value
   **one at a time, in call order** (so `BEGIN; INSERT; COMMIT` on one connection can never interleave).

## E2. Minimal extension (complete file)
`extensions/xmod_demo/demo.cpp`
```cpp
#define XMOD_EXT_NAME    "xmod_demo"      // must be unique
#define XMOD_EXT_VERSION "1.0"
#include "xmod_ext_glue.h"                // Metamod entry points + g_xmod (the API) + g_xext (your handle)
#include <string>

// sync:  xmod.demo.add(a, b) -> number            (game thread, must be fast)
static void f_add(XmodCall *c, void *) {
    if (g_xmod->call_argc(c) < 2) { g_xmod->call_error(c, "usage: xmod.demo.add(a, b)"); return; }   // raises a Lua error
    double a = g_xmod->val_get_num(g_xmod->call_arg(c, 0), 0), b = g_xmod->val_get_num(g_xmod->call_arg(c, 1), 0);
    g_xmod->call_return(c, g_xmod->val_num(a + b));
}

// async: xmod.demo.slow_upper(text, function(err, res) end)   (worker thread, may block)
static void j_slow_upper(XmodJob *j, void *) {
    size_t n = 0; const char *s = g_xmod->val_get_str(g_xmod->job_arg(j, 0), &n);    // args are 0-based, the callback is not included
    if (!s) { g_xmod->job_fail(j, "text must be a string"); return; }                 // -> callback("text must be a string", nil)
    std::string out(s, n); for (auto &ch : out) ch = (char)toupper((unsigned char)ch);
    /* ... blocking work is fine here ... */
    XmodValue *res = g_xmod->val_map();                                               // -> callback(nil, { text = "...", length = n })
    g_xmod->val_set(res, "text", g_xmod->val_str(out.data(), out.size()));
    g_xmod->val_set(res, "length", g_xmod->val_int((int64_t)n));
    g_xmod->job_done(j, res);                                                         // xmod owns `res` now
}

static bool xext_init(const XmodApi *api, XmodExt *ext) {                             // called in Meta_Attach, game thread
    return api->register_func(ext, "demo", "add", f_add, nullptr)
        && api->register_async(ext, "demo", "slow_upper", j_slow_upper, nullptr, 0);  // last arg: strand_arg (0 = none)
}
static void xext_shutdown(void) { /* close your sockets/handles so blocked workers return; free resources */ }
```
`extensions/xmod_demo/CMakeLists.txt`
```cmake
xmod_extension(xmod_demo demo.cpp)                 # more .cpp/.c files can follow; link extra libs with target_link_libraries(xmod_demo ...)
```
Build with the normal scripts (`./build_linux.sh`, `./build_windows.sh`): every directory under `extensions/` that has a
`CMakeLists.txt` is built and copied to `addons/xmod/dlls/`. Install:
```
addons/metamod/plugins.ini      linux addons/xmod/dlls/xmod_mm_i386.so          <- xmod FIRST
                                linux addons/xmod/dlls/xmod_demo_mm_i386.so     <- then extensions   (Windows: win32 ...\xmod_demo_mm.dll)
addons/xmod/configs/extensions.ini   add a line:  demo                          <- the Lua namespace (see section E5)
```
Lua:
```lua
print(xmod.demo.add(2, 3))                                               -- 5.0
xmod.demo.slow_upper("hello", function(err, res)
  if err then xmod.error(err) return end
  xmod.log(res.text .. " (" .. res.length .. ")")                        -- runs on the game thread, a few frames later
end)
```
A complete, tested example of every feature is `extensions/xmod_hello/hello.cpp` (copy it). A large real one is
`extensions/xmod_query/` (SQL, HTTP, sockets).

## E3. API reference (`const XmodApi *g_xmod`)
| Group | Functions |
|---|---|
| Register (game thread, in `xext_init`) | `register_func(ext, ns, name, fn, user)` · `register_async(ext, ns, name, fn, user, strand_arg)` → 1 ok / 0 failed (reserved namespace, duplicate name, bad characters: only `[A-Za-z0-9_]`) |
| Sync call | `call_argc(c)` · `call_arg(c, i)` · `call_return(c, value)` · `call_error(c, "msg")` |
| Async job | `job_argc(j)` · `job_arg(j, i)` · `job_done(j, value)` · `job_fail(j, "msg")` — call exactly one of done/fail; returning without either gives the callback an error |
| Build values | `val_nil()` `val_bool(b)` `val_int(i64)` `val_num(d)` `val_str(ptr, len)` `val_cstr(z)` `val_array()` `val_map()` · `val_push(array, v)` · `val_set(map, "key", v)` (both take ownership of `v`) · `val_free(v)` only for values you never handed over |
| Read values | `val_type(v)` → `XV_NIL/BOOL/INT/NUM/STR/ARRAY/MAP` · `val_get_int(v, default)` · `val_get_num(v, default)` · `val_get_bool(v, default)` · `val_get_str(v, &len)` (NULL if not a string) · `val_len(v)` · `val_at(array, i)` · `val_get(map, "key")` · `val_key(map, i)` / `val_val(map, i)` to iterate. All accept NULL and return the default. |
| From any thread | `post_main(fn, user)` · `emit_event("event_name", args_array)` (fires `xmod.on("event_name", function(a, b, ...) end)`; takes ownership) · `log(msg)` · `log_error(msg)` · `is_main_thread()` |
| Game helpers, any thread | `color_chat(player_id, text)` — coloured chat, `0` = all human players, tags `{y}` default, `{t}` team colour, `{g}` green, max ~188 chars · `client_print(player_id, "console"\|"center"\|"chat", text)` |

Argument values (`call_arg` / `job_arg`) belong to xmod: read them, never free or keep them after the function returns
(copy strings you need later). `user` is the pointer you passed at registration.
Reserved namespaces (cannot be used): `core player ent mem trace msg cmd cvar timer kv fs sound precache fullpack hook reapi ham stats ext async`.

## E4. Patterns
```cpp
// (a) options table:  xmod.demo.fetch(url, { timeout = 3, headers = { ["X-A"] = "1" } }, cb)
const XmodValue *o = g_xmod->job_arg(j, 1);
double timeout = (g_xmod->val_type(o) == XV_MAP) ? g_xmod->val_get_num(g_xmod->val_get(o, "timeout"), 10.0) : 10.0;

// (b) handles + strand:  open -> handle;  use(handle, ...);  close(handle)
static std::mutex g_mu; static std::map<int64_t, std::shared_ptr<Thing>> g_things; static std::atomic<int64_t> g_next{1};
static void j_open(XmodJob *j, void *) { auto t = std::make_shared<Thing>(/* blocking connect */); int64_t h = g_next++;
    { std::lock_guard<std::mutex> l(g_mu); g_things[h] = t; } g_xmod->job_done(j, g_xmod->val_int(h)); }
static void j_use(XmodJob *j, void *)  { std::shared_ptr<Thing> t; { std::lock_guard<std::mutex> l(g_mu); auto it = g_things.find(g_xmod->val_get_int(g_xmod->job_arg(j, 0), 0)); if (it != g_things.end()) t = it->second; }
    if (!t) { g_xmod->job_fail(j, "invalid or closed handle"); return; } /* work with t */ }
api->register_async(ext, "demo", "open", j_open, nullptr, 0);
api->register_async(ext, "demo", "use",  j_use,  nullptr, 1);     // strand on argument #1 = the handle: ordered, never concurrent per handle
api->register_async(ext, "demo", "close", j_close, nullptr, 1);

// (c) push events to Lua from a long-running worker (progress, incoming data):
XmodValue *a = g_xmod->val_array(); g_xmod->val_push(a, g_xmod->val_int(percent)); g_xmod->emit_event("demo_progress", a);

// (d) engine work after blocking work: collect data in the worker, then
struct Ctx { int id; std::string text; };
static void on_main(void *u) { Ctx *c = (Ctx *)u; /* game thread: engine calls are allowed here */ delete c; }
g_xmod->post_main(on_main, new Ctx{id, text});
```

## E5. What the Lua side sees (tell the user)
- Lua scripts are loaded **before** extensions attach. So extension functions exist from the `config` / `init` /
  gameplay events on, **not** at the top level of a script. List your namespace in `configs/extensions.ini`: then
  `local d = xmod.demo` is valid at load time (a stable table) and fills in when the extension attaches.
- Calling a function of a missing/unloaded extension raises a clear Lua error. Check with `xmod.ext.available("demo")`
  or `xmod.ext.available("demo", "add")`. `xmod.ext.list()` and console `xmod ext` list loaded extensions.
- An async call returns a job id; `xmod.async.cancel(id)` drops the callback (the work itself is not interrupted).
  `xmod.async.stats()` → `{threads, max_threads, idle, running, queued, pending, ...}`.
- Callbacks are never called after `xmod reload` or after the calling plugin was disabled; the player a job was about
  may have left — callbacks must re-check `xmod.player.connected(id)`.
- Server cvars: `xmod_async_threads` (4, max 16), `xmod_async_budget_ms` (2: ms per frame spent delivering results),
  `xmod_async_queue_max` (4096 pending jobs).

## E6. Checklist before you hand the code over
- [ ] Every sync function is non-blocking; everything that can wait is async.
- [ ] No engine / Lua / game-state access in worker code; shared C++ state is protected by a mutex.
- [ ] Every code path of an async function ends in exactly one `job_done` or `job_fail`.
- [ ] Arguments are validated (`val_get_*` return your default on a wrong type; send a useful `job_fail`/`call_error` text).
- [ ] Functions taking a handle use `strand_arg`; handle tables use `shared_ptr` so closing while a job runs is safe.
- [ ] Limits: cap sizes, timeouts and counts that scripts control. Secrets (passwords, tokens, hosts) come from a config
      file read by the extension, not from Lua.
- [ ] `xext_shutdown` unblocks workers (close sockets/handles). xmod then waits up to 5 s for your running jobs.
- [ ] Third-party libraries: link statically, or load at run time (`dlopen` / `LoadLibrary`) and fail with a clear message.
      GoldSrc servers are **32-bit**: a 64-bit `.so`/`.dll` cannot be loaded.
- [ ] Portable: Linux (gcc, `-m32`) and Windows (MinGW, as used by `build_windows.sh`; `std::mutex`/`std::atomic` compile there).
- [ ] Tested: add checks to `test/` like `test/hello_test.lua` and run `test/run_ext.sh` (mock engine).

## E7. Limits you should know
- A value passed from Lua may nest at most 8 levels and hold ~200 000 items; functions, userdata and coroutines cannot be passed.
- Lua integers are 64-bit and survive unchanged; floats are doubles. `val_get_int` also accepts floats with an integral value.
- xmod delivers results within a per-frame time budget, so thousands of results arriving at once spread over a few frames.
- If you also need engine or game hooks in the same plugin, don't include `xmod_ext_glue.h`; write the Metamod entry
  points yourself (see xmod's `src/main.cpp`) and call `xmod_ext_connect()` / `ext_register()` / `ext_unregister()` yourself.
- The API struct only grows at the end. `g_xmod->abi_version` is 1; check `g_xmod->size` before using a field added later.


# Appendices


## Appendix A — ReAPI hookchains (event `reapi:<Hook>[:post]`)
Use `xmod.on("reapi:<Name>", fn)` / `xmod.on("reapi:<Name>:post", fn)`.
Args are passed to `fn` in order; `(mod)` args can be changed with `xmod.hook.set_arg(n, value)` (n counts after `this`). Return `xmod.HC_SUPERCEDE, value` to skip the original. Post hooks get the return value as an extra last arg.

| Hook | Args | Returns |
|---|---|---|
| `CBasePlayer_Spawn` | this:ent | - |
| `CBasePlayer_Precache` | this:ent | - |
| `CBasePlayer_ObjectCaps` | this:ent | int |
| `CBasePlayer_Classify` | this:ent | int |
| `CBasePlayer_TraceAttack` | this:ent, ent, float, vec{x,y,z}, nil, int | - |
| `CBasePlayer_TakeDamage` | this:ent, ent, ent, float(mod), int | int |
| `CBasePlayer_TakeHealth` | this:ent, float, int | int |
| `CBasePlayer_Killed` | this:ent, ent, int | - |
| `CBasePlayer_AddPoints` | this:ent, int, int | - |
| `CBasePlayer_AddPointsToTeam` | this:ent, int, int | - |
| `CBasePlayer_AddPlayerItem` | this:ent, ent | int |
| `CBasePlayer_RemovePlayerItem` | this:ent, ent | int |
| `CBasePlayer_GiveAmmo` | this:ent, int, string, int | int |
| `CBasePlayer_ResetMaxSpeed` | this:ent | - |
| `CBasePlayer_Jump` | this:ent | - |
| `CBasePlayer_Duck` | this:ent | - |
| `CBasePlayer_PreThink` | this:ent | - |
| `CBasePlayer_PostThink` | this:ent | - |
| `CBasePlayer_UpdateClientData` | this:ent | - |
| `CBasePlayer_ImpulseCommands` | this:ent | - |
| `CBasePlayer_RoundRespawn` | this:ent | - |
| `CBasePlayer_Blind` | this:ent, float, float, float, int | - |
| `CBasePlayer_Observer_IsValidTarget` | this:ent, int, int | ent |
| `CBasePlayer_SetAnimation` | this:ent, int | - |
| `CBasePlayer_GiveDefaultItems` | this:ent | - |
| `CBasePlayer_GiveNamedItem` | this:ent, string | ent |
| `CBasePlayer_AddAccount` | this:ent, int, int, int | - |
| `CBasePlayer_GiveShield` | this:ent, int | - |
| `CBasePlayer_SetClientUserInfoModel` | this:ent, string, string | - |
| `CBasePlayer_SetClientUserInfoName` | this:ent, string, string | int |
| `CBasePlayer_HasRestrictItem` | this:ent, int, int | int |
| `CBasePlayer_DropPlayerItem` | this:ent, string | ent |
| `CBasePlayer_DropShield` | this:ent, int | ent |
| `CBasePlayer_OnSpawnEquip` | this:ent, int, int | - |
| `CBasePlayer_Radio` | this:ent, string, string, int, int | - |
| `CBasePlayer_Disappear` | this:ent | - |
| `CBasePlayer_MakeVIP` | this:ent | - |
| `CBasePlayer_MakeBomber` | this:ent | int |
| `CBasePlayer_StartObserver` | this:ent, vec{x,y,z}, vec{x,y,z} | - |
| `CBasePlayer_GetIntoGame` | this:ent | int |
| `CBaseAnimating_ResetSequenceInfo` | this:ent | - |
| `GetForceCamera` | ent | int |
| `PlayerBlind` | ent, ent, ent, float, float, int, vec{x,y,z} | - |
| `RadiusFlash_TraceLine` | ent, ent, ent, vec{x,y,z}, vec{x,y,z}, nil | - |
| `RoundEnd` | int, int, float | int |
| `InstallGameRules` | - | nil |
| `PM_Init` | nil | - |
| `PM_Move` | nil, int | - |
| `PM_AirMove` | int | - |
| `HandleMenu_ChooseAppearance` | ent, int | - |
| `HandleMenu_ChooseTeam` | ent, int | int |
| `ShowMenu` | ent, int, int, int, string | - |
| `ShowVGUIMenu` | ent, int, int, string | - |
| `BuyGunAmmo` | ent, ent, int | int |
| `BuyWeaponByWeaponID` | ent, int | ent |
| `InternalCommand` | ent, string, string | - |
| `CSGameRules_FShouldSwitchWeapon` | ent, ent | int |
| `CSGameRules_GetNextBestWeapon` | ent, ent | int |
| `CSGameRules_FlPlayerFallDamage` | ent | float |
| `CSGameRules_FPlayerCanTakeDamage` | ent, ent | int |
| `CSGameRules_PlayerSpawn` | ent | - |
| `CSGameRules_FPlayerCanRespawn` | ent | int |
| `CSGameRules_GetPlayerSpawnSpot` | ent | ent |
| `CSGameRules_ClientUserInfoChanged` | ent, string | - |
| `CSGameRules_PlayerKilled` | ent, ent, ent | - |
| `CSGameRules_DeathNotice` | ent, ent, ent | - |
| `CSGameRules_CanHavePlayerItem` | ent, ent | int |
| `CSGameRules_DeadPlayerWeapons` | ent | int |
| `CSGameRules_ServerDeactivate` | - | - |
| `CSGameRules_CheckMapConditions` | - | - |
| `CSGameRules_CleanUpMap` | - | - |
| `CSGameRules_RestartRound` | - | - |
| `CSGameRules_CheckWinConditions` | - | - |
| `CSGameRules_RemoveGuns` | - | - |
| `CSGameRules_GiveC4` | - | ent |
| `CSGameRules_ChangeLevel` | - | - |
| `CSGameRules_GoToIntermission` | - | - |
| `CSGameRules_BalanceTeams` | - | - |
| `CSGameRules_OnRoundFreezeEnd` | - | - |
| `CSGameRules_CanPlayerHearPlayer` | ent, ent | int |
| `PM_UpdateStepSound` | - | - |
| `CBasePlayer_StartDeathCam` | this:ent | - |
| `CBasePlayer_SwitchTeam` | this:ent | - |
| `CBasePlayer_CanSwitchTeam` | this:ent, int | int |
| `CBasePlayer_ThrowGrenade` | this:ent, ent, vec{x,y,z}, vec{x,y,z}, float, int | ent |
| `CWeaponBox_SetModel` | this:ent, string | - |
| `CGrenade_DefuseBombStart` | this:ent, ent | - |
| `CGrenade_DefuseBombEnd` | this:ent, ent, int | - |
| `CGrenade_ExplodeHeGrenade` | this:ent, nil, int | - |
| `CGrenade_ExplodeFlashbang` | this:ent, nil, int | - |
| `CGrenade_ExplodeSmokeGrenade` | this:ent | - |
| `CGrenade_ExplodeBomb` | this:ent, nil, int | - |
| `ThrowHeGrenade` | ent, vec{x,y,z}, vec{x,y,z}, float, int, int | ent |
| `ThrowFlashbang` | ent, vec{x,y,z}, vec{x,y,z}, float | ent |
| `ThrowSmokeGrenade` | ent, vec{x,y,z}, vec{x,y,z}, float, int | ent |
| `PlantBomb` | ent, vec{x,y,z}, vec{x,y,z} | ent |
| `CBasePlayer_SetSpawnProtection` | this:ent, float | - |
| `CBasePlayer_RemoveSpawnProtection` | this:ent | - |
| `IsPenetrableEntity` | vec{x,y,z}, vec{x,y,z}, ent, ent | int |
| `CBasePlayer_HintMessageEx` | this:ent, string, float, int, int | int |
| `CBasePlayer_UseEmpty` | this:ent | - |
| `CBasePlayerWeapon_CanDeploy` | this:ent | int |
| `CBasePlayerWeapon_DefaultDeploy` | this:ent, string, string, int, string, int | int |
| `CBasePlayerWeapon_DefaultReload` | this:ent, int, int, float | int |
| `CBasePlayerWeapon_DefaultShotgunReload` | this:ent, int, int, float, float, string, string | int |
| `CBasePlayer_DropIdlePlayer` | this:ent, string | - |
| `CreateWeaponBox` | ent, ent, string, vec{x,y,z}, vec{x,y,z}, vec{x,y,z}, float, int | ent |
| `SpawnHeadGib` | ent | ent |
| `SpawnRandomGibs` | ent, int, int | - |
| `CGib_Spawn` | this:ent, string | - |
| `CGib_BounceGibTouch` | this:ent, ent | - |
| `CGib_WaitTillLand` | this:ent | - |
| `CBaseEntity_FireBullets` | this:ent, int, vec{x,y,z}, vec{x,y,z}, vec{x,y,z}, float, int, int, int, ent | - |
| `CBaseEntity_FireBuckshots` | this:ent, int, vec{x,y,z}, vec{x,y,z}, vec{x,y,z}, float, int, int, ent | - |
| `CBaseEntity_FireBullets3` | this:ent, vec{x,y,z}, vec{x,y,z}, float, float, int, int, int, float, ent, int, int | vec{x,y,z} |
| `CBasePlayer_Observer_SetMode` | this:ent, int | - |
| `CBasePlayer_Observer_FindNextPlayer` | this:ent, int, string | - |
| `CBasePlayer_Pain` | this:ent, int, int | - |
| `CBasePlayer_DeathSound` | this:ent | - |
| `CBasePlayer_JoiningThink` | this:ent | - |
| `FreeGameRules` | nil | - |
| `PM_LadderMove` | nil | - |
| `PM_WaterJump` | - | - |
| `PM_CheckWaterJump` | - | - |
| `PM_Jump` | - | - |
| `PM_Duck` | - | - |
| `PM_UnDuck` | - | - |
| `PM_PlayStepSound` | int, float | - |
| `PM_AirAccelerate` | nil, float, float | - |
| `ClearMultiDamage` | - | - |
| `AddMultiDamage` | ent, ent, float, int | - |
| `ApplyMultiDamage` | ent, ent | - |
| `BuyItem` | ent, int | - |
| `CSGameRules_Think` | - | - |
| `CSGameRules_TeamFull` | int | int |
| `CSGameRules_TeamStacked` | int, int | int |
| `CSGameRules_PlayerGotWeapon` | ent, ent | - |
| `CBotManager_OnEvent` | int, ent, ent | - |
| `CBasePlayer_CheckTimeBasedDamage` | this:ent | - |
| `CBasePlayer_EntSelectSpawnPoint` | this:ent | ent |
| `CBasePlayerWeapon_ItemPostFrame` | this:ent | - |
| `CBasePlayerWeapon_KickBack` | this:ent, float, float, float, float, float, float, int | - |
| `CBasePlayerWeapon_SendWeaponAnim` | this:ent, int, int | - |
| `CSGameRules_SendDeathMessage` | ent, ent, ent, ent, string, int, int | - |
| `CBasePlayer_PlayerDeathThink` | this:ent | - |
| `CBasePlayer_Observer_Think` | this:ent | - |
| `CBasePlayer_RemoveAllItems` | this:ent, int | - |
| `CBasePlayer_UpdateStatusBar` | this:ent | - |
| `CBasePlayer_TakeDamageImpulse` | this:ent, ent, float, float | - |
| `SendSayMessage` | ent, string, int, string, string, string, int, string, int | - |


## Appendix B — Ham functions (event `ham:<func>:<classname>[:post]`)
Use `xmod.on("ham:<func>:<classname>", fn)` / `...:post`. Examples of classnames: `player`, `weapon_ak47`, `func_door`, `info_target`.
The first arg of `fn` is always the entity (`this`); then the args below. Pre-hooks may return `xmod.HC_SUPERCEDE, value`; `xmod.hook.set_arg(n, v)` changes numeric args (n counts after `this`). Post hooks get the return value as extra last arg.

| Function | Args | Returns |
|---|---|---|
| `spawn` | - | - |
| `precache` | - | - |
| `keyvalue` | int | - |
| `objectcaps` | - | int |
| `activate` | - | - |
| `setobjectcollisionbox` | - | - |
| `classify` | - | int |
| `deathnotice` | ent | - |
| `takedamage` | ent, ent, float, int | int |
| `takehealth` | float, int | int |
| `killed` | ent, int | - |
| `bloodcolor` | - | int |
| `istriggered` | ent | int |
| `mymonsterpointer` | - | ent |
| `mysquadmonsterpointer` | - | ent |
| `gettogglestate` | - | int |
| `addpoints` | int, int | - |
| `addpointstoteam` | int, int | - |
| `addplayeritem` | ent | int |
| `removeplayeritem` | ent | int |
| `giveammo` | int, string, int | int |
| `getdelay` | - | float |
| `ismoving` | - | int |
| `overridereset` | - | - |
| `damagedecal` | int | int |
| `settogglestate` | int | - |
| `startsneaking` | - | - |
| `stopsneaking` | - | - |
| `oncontrols` | ent | int |
| `issneaking` | - | int |
| `isalive` | - | int |
| `isbspmodel` | - | int |
| `reflectgauss` | - | int |
| `hastarget` | int | int |
| `isinworld` | - | int |
| `isplayer` | - | int |
| `isnetclient` | - | int |
| `teamid` | - | string |
| `getnexttarget` | - | ent |
| `think` | - | - |
| `touch` | ent | - |
| `use` | ent, ent, int, float | - |
| `blocked` | ent | - |
| `respawn` | - | ent |
| `updateowner` | - | - |
| `fbecomeprone` | - | int |
| `illumination` | - | int |
| `fvisible` | ent | int |
| `changeyaw` | int | float |
| `hashumangibs` | - | int |
| `hasaliengibs` | - | int |
| `fademonster` | - | - |
| `gibmonster` | - | - |
| `getdeathactivity` | - | int |
| `becomedead` | - | - |
| `irelationship` | ent | int |
| `painsound` | - | - |
| `reportaistate` | - | - |
| `monsterinitdead` | - | - |
| `look` | int | - |
| `bestvisibleenemy` | - | ent |
| `finviewcone` | ent | int |
| `player_jump` | - | - |
| `player_duck` | - | - |
| `player_prethink` | - | - |
| `player_postthink` | - | - |
| `player_shouldfadeondeath` | - | int |
| `player_impulsecommands` | - | - |
| `player_updateclientdata` | - | - |
| `item_addtoplayer` | ent | int |
| `item_addduplicate` | ent | int |
| `item_candeploy` | - | int |
| `item_deploy` | - | int |
| `item_canholster` | - | int |
| `item_holster` | int | - |
| `item_updateiteminfo` | - | - |
| `item_preframe` | - | - |
| `item_postframe` | - | - |
| `item_drop` | - | - |
| `item_kill` | - | - |
| `item_attachtoplayer` | ent | - |
| `item_primaryammoindex` | - | int |
| `item_secondaryammoindex` | - | int |
| `item_updateclientdata` | ent | int |
| `item_getweaponptr` | - | ent |
| `item_itemslot` | - | int |
| `weapon_extractammo` | ent | int |
| `weapon_extractclipammo` | ent | int |
| `weapon_addweapon` | - | int |
| `weapon_playemptysound` | - | int |
| `weapon_resetemptysound` | - | - |
| `weapon_isusable` | - | int |
| `weapon_primaryattack` | - | - |
| `weapon_secondaryattack` | - | - |
| `weapon_reload` | - | - |
| `weapon_weaponidle` | - | - |
| `weapon_retireweapon` | - | - |
| `weapon_shouldweaponidle` | - | int |
| `weapon_usedecrement` | - | int |
| `cstrike_restart` | - | - |
| `cstrike_roundrespawn` | - | - |
| `cstrike_item_candrop` | - | int |
| `cstrike_item_isweapon` | - | int |
| `cstrike_item_getmaxspeed` | - | float |
| `cstrike_weapon_sendweaponanim` | int, int | - |
| `cstrike_player_resetmaxspeed` | - | - |
| `cstrike_player_isbot` | - | int |
| `cstrike_player_ontouchingweapon` | ent | - |


## Appendix C — Index of every xmod Lua function and constant (generated from the running build)

Every name below exists in xmod 0.4.0 (with Nexus built in and the xmod_query extension loaded). Sections above explain each one.
`x` is an alias of `xmod`. Extension functions (`xmod.query.*`) exist only when that extension is loaded.

- **`xmod.async`**: `cancel` · `stats`
- **`xmod`**: `call` · `emit` · `error` · `exec` · `export` · `flags` · `gametime` · `has_export` · `is_dedicated` · `log` · `map` · `map_valid` · `maxentities` · `maxplayers` · `off` · `on` · `plugins` · `register` · `server_print` · `time`
- **`xmod.cmd`**: `client` · `say` · `server`
- **`xmod.cvar`**: `create` · `exists` · `get` · `int` · `num` · `query_client` · `set` · `watch`
- **`xmod.ent`**: `classname` · `create` · `find` · `find_sphere` · `get` · `remove` · `set` · `set_model` · `set_origin` · `set_size` · `spawn` · `valid`
- **`xmod.ext`**: `available` · `list`
- **`xmod.fs`**: `exists` · `exists_game` · `read` · `read_game` · `write`
- **`xmod.fullpack`**: `clear` · `get_render` · `hide` · `hide_all` · `native` · `set_effects` · `set_origin` · `set_render`
- **`xmod.ham`**: `list`
- **`xmod.hook`**: `set_arg`
- **`xmod.kv`**: `del` · `get` · `keys` · `save` · `set`
- **`xmod.mem`**: `get_float` · `get_int` · `set_float` · `set_int`
- **`xmod.msg`**: `angle` · `begin` · `block` · `byte` · `char` · `chat` · `coord` · `entity` · `fade` · `finish` · `hud` · `long` · `print` · `shake` · `short` · `show_menu` · `string`
- **`xmod.nexus`**: `anim` · `camera` · `clear` · `cursor` · `del` · `effect` · `entity` · `fov` · `has` · `info` · `input` · `keys` · `light` · `music` · `open_url` · `players` · `punch` · `reset` · `send` · `set` · `shake` · `shape` · `sound` · `sound3d` · `stats` · `sway`
- **`xmod.player`**: `access` · `alive` · `all` · `armor` · `connected` · `data` · `find` · `frags` · `give_item` · `has_access` · `health` · `infokey` · `ingame` · `ip` · `is_bot` · `kick` · `kill` · `money` · `name` · `origin` · `ping` · `set_access` · `set_armor` · `set_frags` · `set_health` · `set_infokey` · `set_money` · `set_origin` · `set_team` · `set_velocity` · `steamid` · `team` · `team_name` · `userid` · `velocity` · `view_angles`
- **`xmod.precache`**: `generic` · `model` · `sound`
- **`xmod.reapi`**: `available` · `list`
- **`xmod.sound`**: `emit`
- **`xmod.stats`**: `enable` · `get` · `info` · `reset`
- **`xmod.timer`**: `after` · `cancel` · `every` · `times`
- **`xmod.trace`**: `contents` · `hull` · `line` · `line_n`
- **`xmod.vault`**: `list` · `open`
- **`xmod.query`**: `close` · `exec` · `http` · `http_get` · `http_post` · `open` · `profiles` · `reload_config` · `resolve` · `status` · `tcp_close` · `tcp_open` · `tcp_recv` · `tcp_request` · `tcp_send` · `udp_request`
- **`(vault object v = xmod.vault.open(name))`**: `set` · `get` · `has` · `remove` · `touch` · `timestamp` · `incr` · `keys` · `count` · `prune` · `clear` · `save` · `each`

Constants:
- **`xmod`**: `EF_BRIGHTLIGHT=2` · `EF_DIMLIGHT=8` · `EF_NODRAW=128` · `FL_CLIENT=8` · `FL_DUCKING=16384` · `FL_FAKECLIENT=8192` · `FL_FROZEN=4096` · `FL_GODMODE=64` · `FL_NOTARGET=128` · `FL_ONGROUND=512` · `FL_SPECTATOR=67108864` · `FX_DISTORT=15` · `FX_GLOWSHELL=19` · `FX_NONE=0` · `FX_PULSE_FAST=2` · `FX_PULSE_SLOW=1` · `HC_BREAK=2` · `HC_CONTINUE=0` · `HC_SUPERCEDE=1` · `IN_ATTACK=1` · `IN_ATTACK2=2048` · `IN_BACK=16` · `IN_DUCK=4` · `IN_FORWARD=8` · `IN_JUMP=2` · `IN_MOVELEFT=512` · `IN_MOVERIGHT=1024` · `IN_RELOAD=8192` · `IN_USE=32` · `MOVETYPE_FLY=5` · `MOVETYPE_NOCLIP=8` · `MOVETYPE_NONE=0` · `MOVETYPE_TOSS=6` · `MOVETYPE_WALK=3` · `RENDER_GLOW=3` · `RENDER_NORMAL=0` · `RENDER_TRANSADD=5` · `RENDER_TRANSALPHA=4` · `RENDER_TRANSCOLOR=1` · `RENDER_TRANSTEXTURE=2` · `SOLID_BBOX=2` · `SOLID_NOT=0` · `SOLID_TRIGGER=1` · `TEAM_CT=2` · `TEAM_SPEC=3` · `TEAM_T=1` · `TEAM_UNASSIGNED=0` · `TRACE_IGNORE_GLASS=256` · `TRACE_IGNORE_MONSTERS=1` · `TRACE_NORMAL=0` · `gamedir=gd` · `version=0.4.0`
- **`xmod.cvar`**: `ARCHIVE=1` · `PROTECTED=32` · `SERVER=4` · `SPONLY=64`
- **`xmod.mem`**: `LINUX_PLAYER=5`
- **`xmod.msg`**: `ALL=2` · `BROADCAST=0` · `INIT=3` · `ONE=1` · `ONE_UNRELIABLE=8` · `PAS=5` · `PVS=4` · `TEMPENTITY=23`
- **`xmod.sound`**: `CHAN_AUTO=0` · `CHAN_ITEM=3` · `CHAN_VOICE=2` · `CHAN_WEAPON=1`


## Appendix D — The default configuration files (`addons/xmod/configs/`)

### `xmod.cfg`
```
// xmod settings - executed on every map start (before the 'config' event)
xmod_script_timeout 100       // ms a single script call may run before it is aborted (0 = off, loading gets 10x)
xmod_async_threads 4          // worker threads for extension async functions (xmod.query etc.), 1-16
xmod_async_budget_ms 2        // ms per frame spent delivering async results to Lua
xmod_async_queue_max 4096     // max pending async jobs
xmod_show_activity 2          // 0 = silent, 1 = "ADMIN: ...", 2 = "ADMIN name: ..."
xmod_default_access z
xmod_admin_join_msg 1
xmod_flood_time 0.75
xmod_gag_default 600
xmod_vote_enabled 1
xmod_vote_time 120
xmod_vote_maps 5
xmod_extend_step 15
xmod_extend_max 90
xmod_vote_ratio 0.5
xmod_ads_interval 90
xmod_ads_mode 0
xmod_reserved_slots 0
xmod_killstreaks 1
```

### `plugins.ini`
```
; xmod plugins - one Lua file from addons/xmod/plugins/ per line (subfolders allowed, '#' or ';' start a comment)
; Order matters only for plugins that call each other at load time.

; --- administration ---
admin.lua              ; users.ini admins, xmod_who, xmod_addadmin ...
admin_cmds.lua         ; kick / ban / slay / slap / map / cvar / rcon / fun commands
admin_chat.lua         ; say / csay / psay / "say @text" / gag
admin_menus.lua        ; say /admin
votes.lua              ; votemap / votekick / custom votes

; --- server utilities ---
mapmanager.lua         ; nextmap, timeleft, end-of-map vote, extend
antiflood.lua
scrollmsg.lua          ; server ads from configs/ads.ini
adminslots.lua         ; reserved slots (xmod_reserved_slots)
killstreaks.lua        ; first blood / streaks

; --- Nexus (client extension, built into xmod; players need the Nexus client.dll - see client/INSTALL.txt) ---
nexus/nexus_fx.lua         ; say /fx    - zoom, shake, drunk, freeze, glow, 3D zones, lights, particles
nexus/nexus_wheel.lua      ; say /test  - radial "power wheel" menu with cooldown bars
nexus/nexus_song.lua       ; say /song  - open a song on YouTube (the player confirms with Y)
;nexus/nexus_motd.lua      ; clickable 3-page MOTD shown on join (+ say /motd)
;nexus/nexus_demo.lua      ; custom HUD, name tags, damage numbers, shop (say /shop)

; --- demos ---
hello.lua
menu_demo.lua
;xmod_debug.lua         ; diagnostics + benchmarks -> data/xmod_debug.log  (see configs/xmod_debug.cfg)
;examples/ham_demo.lua
;examples/reapi_demo.lua
;examples/entity_demo.lua
;examples/query_demo.lua   ; needs the xmod_query extension
```

### `extensions.ini`
```
; Lua namespaces provided by xmod extensions (Metamod plugins listed AFTER xmod in addons/metamod/plugins.ini).
; A namespace listed here exists from the start (as a stable table), so scripts may write  local q = xmod.query  at load time.
; Functions appear in it when the extension attaches; calling one before that raises a clear "extension not loaded" error.
; Nexus is built into xmod since 0.4 - it needs no line here (and no line in metamod/plugins.ini).
query
```

### `users.ini`
```
; xmod admins  (AMX Mod X compatible format)
; "auth" "password" "access flags" "account flags"
;
; Access flags (what the admin may do):
;   a immunity (cannot be targeted)   b reserved slot      c kick               d ban / unban
;   e slay / slap / team / gag        f change map/restart g change cvars        h exec configs / manage admins
;   i admin chat / say commands       j start votes        k sv_password        l rcon access
;   m fun commands (hp, god, noclip, teleport, give...)    n-t free for your own plugins
;   u admin menu                      z normal user (given to everybody automatically)
;
; Account flags (how "auth" is matched):
;   a  kick the player if the password is wrong     b  "auth" is a clan tag (name prefix)
;   c  "auth" is a SteamID                          d  "auth" is an IP address
;   e  do not check the password
; With none of b/c/d the auth is matched against the exact player NAME.
; Players supply their password with   setinfo _pw "yourpassword"   in their console.
;
; Examples (remove the leading ; and edit):
; "STEAM_0:1:12345678"  ""         "abcdefghijklmnopqrstu"  "ce"
; "192.168.1.50"        ""         "abcdefghijklmnopqrstu"  "de"
; "Andrew"              "secret"   "abcdefghijklmnopqrstu"  "a"
; "[MOD]"               "clanpass" "bcefijmu"               "ab"
"STEAM_0:0:777" "" "ce" "ce"
"STEAM_0:0:777" "" "ce" "ce"
"STEAM_0:0:777" "" "ce" "ce"
"STEAM_0:0:777" "" "ce" "ce"
```

### `query.ini`
```
; xmod_query settings  (extension: addons/xmod/dlls/xmod_query_mm_i386.so / xmod_query_mm.dll - list it AFTER xmod in addons/metamod/plugins.ini)
; A line starting with ; or # is a comment. After a value, " ; text" (space before and after the ;) is a comment too;
; a ';' or '#' glued to other characters is part of the value, so passwords like  p@ss;word#1  work.
; Reload without restart:  xmod.query.reload_config()   (open connections keep working)
;
; Scripts NEVER see host/user/password: they only ask for a profile by name ->  xmod.query.open("main", function(err, db) ... end)

[settings]
max_rows            = 10000          ; rows returned per SQL result (extra rows are dropped and result.truncated = true)
max_response_bytes  = 1048576        ; HTTP body / socket read limit per request
default_timeout     = 10             ; seconds, used when a script gives no timeout
max_timeout         = 60             ; a script can never ask for more than this
max_connections     = 8              ; open DB connections per profile
max_sockets         = 16             ; open persistent TCP sockets (tcp_open)

; WHO may scripts talk to?  Comma separated patterns, * and ? wildcards, optional :port.
; THE DEFAULT IS * (everything) - restrict it if you run scripts you did not write!  Examples:
;   http_allow   = api.example.com, *.mycompany.net, 127.0.0.1
;   socket_allow = 127.0.0.1:27015, game.example.com:*
; With a restricted http_allow, redirects are not followed (the 3xx answer is returned instead).
http_allow          = *
socket_allow        = *

; log_requests  = 1                            ; log every outgoing HTTP request (method + host:port)
; ca_file       = /path/to/cacert.pem          ; CA bundle for HTTPS via libcurl (default: the system store)
; curl_library  = /usr/lib32/libcurl.so.4      ; Linux: explicit 32-bit libcurl (HTTPS needs it; plain http works without)
; mysql_library = /usr/lib32/libmariadb.so.3   ; explicit 32-bit MySQL/MariaDB client library (otherwise common names are tried)

; ---------------------------------------------------------------------------------------------------------------
; database profiles:  [db:NAME]   driver = sqlite | mysql
[db:main]
driver = sqlite
file   = xmod.db                     ; stored in addons/xmod/data/ (no path tricks allowed); use :memory: for a throw-away database
; wal  = 0                           ; journal_mode=WAL is on by default

; [db:stats]
; driver   = mysql
; host     = 127.0.0.1
; port     = 3306
; user     = xmod
; pass     = change-me
; database = xmod
; charset  = utf8mb4
; timeout  = 5                       ; connect timeout, seconds
; read_timeout = 30
```

### `maps.ini`
```
; maps used by the map vote and the admin map menu (one per line). If this file has no valid maps, mapcycle.txt is used.
de_dust2
de_inferno
de_nuke
de_train
cs_italy
de_aztec
cs_assault
```

### `ads.ini`
```
; one message per line; placeholders: %map% %timeleft% %players% %max% %nextmap% %time%
Welcome! Type {g}/menu{y} for the demo menu and {g}/me{y} for your stats.
Current map: {t}%map%{y} - time left: {t}%timeleft%{y} - players: %players%/%max%
Need an admin? Type {g}@your message{y} in chat.
```

### `xmod_debug.cfg`
```
# xmod_debug settings (key = value; '#' or ';' start a comment). Read when the server starts.
log_file        = xmod_debug.log   # written to addons/xmod/data/
append          = 0                # 0 = start a fresh log at every server start, 1 = keep appending
max_log_kb      = 8192             # rotate to xmod_debug_1.log, _2.log ... after this size
report_interval = 10               # seconds between PERFORMANCE REPORTS
flush_interval  = 2                # seconds between log file writes (each write opens/closes the file)
autostats       = 1                # 1 = measure every hooked event (cheap: 2 clock reads per call)
log_players     = 1                # connect / putinserver / disconnect / infochanged
log_chat        = 1
log_commands    = 1                # every client console command
log_messages    = 1                # hook ~30 common user messages (counts + first samples)
log_damage      = 1                # Ham/ReAPI damage lines (one log line per damage event)
hot_sample      = 3                # log only the first N occurrences of hot events (touch, cmd_start, messages ...)
ham_log         = 1                # Ham observers (spawn / killed / takedamage on players)
reapi_log       = 1                # ReAPI observers (needs ReGameDLL; skipped automatically otherwise)
warn_cpu_pct    = 10               # WARN line if one event uses more than this % of a core in Lua
warn_max_ms     = 5                # WARN line if a single call took longer than this many ms

# ---- AddToFullPack stress test (the hottest hook in GoldSrc: every entity x every client x every frame) ----
# The hook can only be installed at server start, so set atfp_register=1 and RESTART the server. Then switch workload live with: xmod_dbg mode <0-3>
atfp_register   = 0
atfp_mode       = 1                # 0 idle | 1 counter | 2 lookups | 3 real Lua work (culls far entities, glows teammates) | 4 NATIVE hide table (no Lua per entity)
atfp_hide_dist  = 3000             # mode 3: non-player entities farther than this are hidden from the client (demo - affects gameplay!)
```
