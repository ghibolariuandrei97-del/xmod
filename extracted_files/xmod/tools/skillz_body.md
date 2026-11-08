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
- Safety limits: a single script call may run at most **100 ms** (infinite loops are killed), Lua memory cap **96 MB**,
  a plugin that raises **25 errors** is disabled automatically (others keep running). Errors are logged with a traceback.
- Hooks: engine/DLL events, user-message interception, **Ham** (virtual function hooks, stock CS and ReGameDLL) and
  **ReAPI hookchains** (ReGameDLL required). All are just events on the same event bus (`xmod.on`).
- `x` is an alias for `xmod`. Player ids are `1..xmod.maxplayers()` and are also their entity indices. Entities are plain integers (edict index), never pointers.

## 2. Files and layout (`cstrike/addons/xmod/`)
```
dlls/xmod_mm_i386.so | xmod_mm.dll      the Metamod module
configs/plugins.ini     list of plugin files (one per line, ';' comments, subfolders allowed e.g. examples/foo.lua)
configs/users.ini       admins (AMXX format)        configs/xmod.cfg   cvars, exec'd every map start
configs/maps.ini        maps for votes/menus         configs/ads.ini    server ad lines
plugins/*.lua           plugins        lib/*.lua  shared libraries (require("name"))
data/                   writable: *.kv stores, logs (xmod.fs.write only allows data/ and configs/)
```
Install: add `linux addons/xmod/dlls/xmod_mm_i386.so` (Linux) or `win32 addons/xmod/dlls/xmod_mm.dll` (Windows) to `addons/metamod/plugins.ini`.
To add a plugin: put `myplugin.lua` in `plugins/`, add the line `myplugin.lua` to `configs/plugins.ini`, then `xmod reload` (or restart).
Server console: `xmod version | plugins | cmds | reload`.

## 3. Lifecycle (order of events)
1. Metamod loads xmod → **scripts execute top-to-bottom once** (register commands/events/cvars here; this is the "plugin_init" for most things).
2. Per map: world spawn → `precache` → ServerActivate → `init(maxplayers)` → (2 frames later, after `xmod.cfg` was exec'd) `config`
   → gameplay events → `map_end` (ServerDeactivate). Timers survive map changes. `shutdown` fires when xmod unloads.
3. `xmod reload` re-creates the Lua state and re-runs all scripts, then fires `init` and `config` (not `precache`; precache calls fail after the map started).
- Top-level code runs *before* cvars from `xmod.cfg` are applied: read cvar values inside handlers / the `config` event, not at load time.
- Per-player state: `xmod.player.data(id)` returns a Lua table private to your plugin; it is cleared when the player connects/disconnects.

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
- No networking/sockets, no SQL, no threads. Use kv/json files; for MySQL ask for a C++ extension.
- `team/money` use CS private-data offsets (team 114, money 115 ints, +5 on Linux) — correct for CS 1.6 / ReGameDLL; other mods differ.
- `reapi:*` only with ReGameDLL; `xmod.reapi.available()` tells. No reapi "rg_*" helper functions yet (round end/restart: `xmod.exec("sv_restartround 1")`).
- Ham: Linux/Windows virtual indices come from AMXX's CS gamedata; functions with `Vector` by value are not hookable. Windows uses thiscall hooks (tested on Linux only).
- Messages: `message:*` hooks observe/block but cannot modify values. `xmod.msg.*` writers bypass other Metamod plugins' hooks.
- A timer/event callback runs under a 100 ms budget; heavy work must be split across timers.
- Plugins cannot define new entity classes. `xmod.ent.create` only creates existing engine/game classes.
- Everything here is in `skillz.md`; extra generated tables: `docs/REAPI_HOOKS.md`, `docs/HAM_FUNCTIONS.md` (same content as the appendices).
