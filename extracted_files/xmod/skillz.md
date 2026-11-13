# SKILLZ.md — xmod knowledge file (attach this to a conversation so Claude can write xmod plugins)

> **Instructions for Claude (read first).** The user (Andrew, AMXX/CS 1.6 developer) runs **xmod**, a Metamod plugin
> that replaces AMX Mod X's Pawn VM with **Lua 5.4**. When he asks for a plugin, write **one `.lua` file** that follows
> this document exactly. Use only the API listed here — do not invent functions. If something is not possible with this
> API, say so and propose the closest alternative (or a small C++ addition to xmod). Reply in the user's language
> (Romanian by default unless he writes in English). Always add the `plugins.ini` line he must add. Prefer the libraries
> in `lib/` (`util`, `menu`, `vote`, `json`) over re-implementing helpers.

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
Constants: `FL_ONGROUND FL_DUCKING FL_FROZEN FL_GODMODE FL_NOTARGET FL_FAKECLIENT FL_SPECTATOR FL_CLIENT MOVETYPE_NONE/WALK/NOCLIP/FLY/TOSS SOLID_NOT/BBOX/TRIGGER IN_ATTACK IN_ATTACK2 IN_JUMP IN_DUCK IN_FORWARD IN_BACK IN_MOVELEFT IN_MOVERIGHT IN_USE IN_RELOAD TRACE_NORMAL TRACE_IGNORE_MONSTERS TRACE_IGNORE_GLASS TEAM_UNASSIGNED TEAM_T TEAM_CT TEAM_SPEC` (all directly on `xmod`).

### Messages — `xmod.msg.*`
High level (`id` 0 = all in-game human players):
- `chat(id|0, text [, sender_id])` coloured chat; colour tags `{y}` yellow/default, `{t}` team colour, `{g}` green. Max ~188 chars.
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
`xmod.fs.read(rel)` (relative to `addons/xmod/`) · `read_game(rel)` (relative to the mod dir, e.g. `mapcycle.txt`) · `write(rel, text [, append])` (only `data/…` and `configs/…`) · `exists(rel)` · `exists_game(rel)`. Paths with `..`, absolute paths and `:` are rejected. For tables use `require("json")` + kv/fs.

### AddToFullPack, performance and diagnostics
- `xmod.fullpack.set_render(mode, amount, fx, r, g, b)` (nil keeps a value) · `get_render()` · `set_effects(mask)` · `set_origin(x,y,z)` — only inside `add_to_full_pack:post` handlers. Constants: `RENDER_NORMAL/TRANSCOLOR/TRANSTEXTURE/GLOW/TRANSALPHA/TRANSADD`, `FX_NONE/GLOWSHELL/PULSE_SLOW/PULSE_FAST/DISTORT`, `EF_NODRAW/BRIGHTLIGHT/DIMLIGHT`.
- **Native hide table (fast path, no Lua per entity):** call `xmod.fullpack.native()` once at plugin load (installs the hook), then at a low rate (e.g. 1 Hz timer) call `xmod.fullpack.hide(host, ent, true|false)`, `xmod.fullpack.hide_all(ent, true|false)`, `xmod.fullpack.clear()`. C++ applies the table every frame (~14 ns per check). The table is cleared on map change. `xmod.maxentities()` gives the edict count.
- `xmod.stats.enable(bool)` · `reset()` · `get()` → list `{name, calls, total_us, avg_us, max_us, handlers}` sorted by CPU time (names: event names, `<timers>`, `<commands>`) · `info()` → `{frames, wall_s, fps, lua_kb, on, frame_ms_avg, frame_ms_max}`. Call counts are exact; timing is sampled (every call for the first 256, then 1 in 16), so it costs ~10 ns per event and `max_us` is the slowest *sampled* call (use `frame_ms_max` to spot hitches).
- `xmod.trace.line_n(...)` = `trace.line` without the result table (8 return values: fraction, ent, x, y, z, nx, ny, nz) — ~10x cheaper.
- **Real listen-server measurement (Windows build, 1 human + bots, 20 000 add_to_full_pack calls/s):** a counter-only Lua handler costs ~0.63 µs per call (about 1.3 % of a core at that rate); once-per-frame events (prethink, cmd_start, frame) show 3–30 µs per call because their code/data is cold in cache by the time the next frame comes. Mock-engine numbers below are 5–7x lower because they run in a tight loop with warm caches.
- Measured on a 3 GHz-class x86 core with the mock engine (not a live server!): empty Lua function call ~13 ns; `xmod.player.health(id)` ~18 ns; `xmod.ent.get(e,"health")` ~27 ns (`"origin"` ~50 ns); `xmod.cvar.int` ~200 ns; `string.format` ~450 ns; table creation ~105 ns; one hooked Lua call from the engine (dispatch + handler) ~85–90 ns idle, ~180 ns with two native calls and math. A full 20-player server can see ~1–3 million `add_to_full_pack` calls/s: Lua handlers there cost 10–50 % of a core, the native hide table ~2 %. **Rule: per-entity/per-frame hooks should do almost nothing in Lua; decide in Lua at low frequency, apply natively.**
- `plugins/xmod_debug.lua` (+ `configs/xmod_debug.cfg`) is the diagnostics plugin: logs everything to `data/xmod_debug.log`, writes PERFORMANCE REPORTS from `xmod.stats`, `xmod_dbg test` (API self-test, PASS/FAIL), `xmod_dbg bench` (micro-benchmarks), AddToFullPack stress modes (`xmod_dbg mode 0-4`).

### Extensions, async calls and `xmod.query` (SQL / HTTP / sockets)
- **Extensions** are separate Metamod plugins (C++) that add functions as `xmod.<namespace>.<fn>`. They are listed after xmod in metamod's `plugins.ini` and attach **after** the Lua scripts were loaded: use their functions from `config`/`init`/gameplay events and commands, **never at the top level of a script**. Namespaces listed in `configs/extensions.ini` (`query` is) exist as tables at load time, so `local q = xmod.query` at the top is fine. `xmod.ext.available("query" [, "http_get"])` → bool · `xmod.ext.list()` → `{name, version, functions, loaded}` · console `xmod ext`. Calling a function whose extension is not loaded raises a Lua error. To write a new extension, give the AI `module_skillz.md` (not this file).
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
· `xmod.precache.sound/model/generic(path)` — **only inside the `precache` event** (otherwise error).

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
| `nvault_*` | `xmod.kv.*` (+ `json` for tables) |
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

## 11. Limits, gotchas, not (yet) available
- Lua itself has no sockets/SQL/threads: use the `xmod.query` extension (async) for SQL, HTTP and sockets, `xmod.kv`/`json` for small local data. Anything else that must block needs a C++ extension (`module_skillz.md`). No listening (server) sockets.
- `team/money` use CS private-data offsets (team 114, money 115 ints, +5 on Linux) — correct for CS 1.6 / ReGameDLL; other mods differ.
- `reapi:*` only with ReGameDLL; `xmod.reapi.available()` tells. No reapi "rg_*" helper functions yet (round end/restart: `xmod.exec("sv_restartround 1")`).
- Ham: Linux/Windows virtual indices come from AMXX's CS gamedata; functions with `Vector` by value are not hookable. Windows uses thiscall hooks (tested on Linux only).
- Messages: `message:*` hooks observe/block but cannot modify values. `xmod.msg.*` writers bypass other Metamod plugins' hooks.
- A timer/event callback must finish within `xmod_script_timeout` ms (default 100); heavy work must be split across timers.
- `add_to_full_pack` is the only hook that is installed lazily: it exists only if a plugin registered it (or called `xmod.fullpack.native()`) at server start, so servers that don't use it pay nothing.
- Plugins cannot define new entity classes. `xmod.ent.create` only creates existing engine/game classes.
- Everything here is in `skillz.md`; extra generated tables: `docs/REAPI_HOOKS.md`, `docs/HAM_FUNCTIONS.md` (same content as the appendices).

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
