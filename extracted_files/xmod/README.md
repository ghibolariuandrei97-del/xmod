# xmod 0.3.1 — next-generation scripting for GoldSrc / CS 1.6

A Metamod plugin that replaces AMX Mod X's Pawn VM with **Lua 5.4**: no compiling, hot reload (`xmod reload`),
sandboxed scripts (watchdog aborts a single call longer than `xmod_script_timeout` ms — zero overhead normally; 96 MB memory cap; auto-disable after 25 errors), one shared event bus.
**Ham** virtual hooks and **ReAPI** (ReGameDLL) hookchains are events too. Linux `.so` and Windows `.dll` included.

> Writing plugins with Claude? Attach **`skillz.md`** — it documents every function, event, convention and an AMXX→xmod cheat sheet.

## Install
1. Copy `addons/xmod` into `cstrike/addons/`.
2. Add one line to `cstrike/addons/metamod/plugins.ini`:
   - Linux: `linux addons/xmod/dlls/xmod_mm_i386.so`
   - Windows: `win32 addons/xmod/dlls/xmod_mm.dll`
3. Edit `addons/xmod/configs/users.ini` (admins), `plugins.ini` (which scripts load), `xmod.cfg` (settings). Restart. Console: `xmod version|plugins|cmds|reload`.

## What's included
| Plugin | What it does |
|---|---|
| `admin.lua` | admins from `users.ini` (AMXX format, SteamID/IP/name/clan tag, passwords via `setinfo _pw`), `xmod_who/admins/reloadadmins/addadmin/removeadmin` |
| `admin_cmds.lua` | kick, ban/addban/unban (banid/addip), slay, slap, team, swap, nick, map, restart, cvar, cfg, rcon + fun: hp, armor, money, god, noclip, freeze, blind, give, teleport, bring, goto. All also as `amx_*` |
| `admin_chat.lua` | say/csay/hsay/psay/chat, `say @text` (talk to admins), gag/ungag |
| `admin_menus.lua` | `say /admin` — kick/ban/slay/slap/team/map/votemap menus |
| `votes.lua` | `xmod_votemap`, `xmod_votekick`, `xmod_vote "Q" "a" "b"` |
| `mapmanager.lua` | nextmap/timeleft/thetime, end-of-map map vote with "extend", level change at intermission |
| `antiflood.lua`, `scrollmsg.lua`, `adminslots.lua`, `killstreaks.lua` | chat anti-flood, server ads (`ads.ini`), reserved slots, first-blood/streak HUD |
| `hello.lua`, `menu_demo.lua`, `examples/*` | learning material: events, chat commands, menus, **Ham**, **ReAPI**, traces/entities |

Libraries (`lib/`): `util` (targets, commands, logging, helpers), `menu`, `vote`, `json`.

## Extensions: other Metamod plugins can plug into xmod (new in 0.3)
xmod exports a small C API (`include/xmod_ext.h`). Any Metamod plugin loaded after xmod can register functions that
appear in Lua as `xmod.<namespace>.<fn>`: **sync** functions (game thread) or **async** functions that run on xmod's
worker thread pool and hand their result back to a Lua callback on the game thread, so blocking work never stalls the
server. The API also offers thread-safe `color_chat`, `client_print`, `emit_event`, `post_main` and logging.
`include/xmod_ext_glue.h` contains all the Metamod boilerplate; `extensions/xmod_hello/` is the template.
**To have an AI write a new extension, give it `module_skillz.md`.**

Install order in `addons/metamod/plugins.ini` (xmod first):
```
linux addons/xmod/dlls/xmod_mm_i386.so
linux addons/xmod/dlls/xmod_query_mm_i386.so        (Windows: win32 addons\xmod\dlls\xmod_mm.dll, then xmod_query_mm.dll)
```

### xmod_query — SQL, HTTP and sockets for Lua (`xmod.query.*`, all async)
SQLite (bundled, no dependencies), MySQL/MariaDB, HTTP/HTTPS, TCP, UDP, DNS. Settings in `configs/query.ini`:
database profiles (scripts ask for a profile by name and never see the password), host allow-lists, size/time limits.
```lua
xmod.query.open("main", function(err, db)                                   -- [db:main] in query.ini
  xmod.query.exec(db, "SELECT name FROM visits WHERE steamid = ?", { sid }, function(err, res) ... res.rows[1].name ... end)
end)
xmod.query.http_get("https://api.example.com/x", { timeout = 5 }, function(err, r) ... r.status, r.body ... end)
```
Run-time dependencies (loaded only when used, with a clear error if missing): **MySQL** needs the 32-bit client library
(`libmariadb.so.3` / `libmysqlclient`, on Windows a 32-bit `libmariadb.dll` or `libmysql.dll` next to `hlds.exe`);
**HTTPS on Linux** needs the 32-bit `libcurl.so.4` (plain http has a built-in client; Windows uses WinHTTP).
Example plugin: `plugins/examples/query_demo.lua`. Full function list: `skillz.md`.

## Diagnostics and performance testing
`plugins/xmod_debug.lua` (enable it in `configs/plugins.ini`, settings in `configs/xmod_debug.cfg`) logs lifecycle/player/chat/command/message/Ham/ReAPI events to `data/xmod_debug.log`, writes a **performance report** every 10 s from xmod's built-in per-event timers (calls/s, µs per call, % of a core, server FPS, frame hitches), and offers `xmod_dbg test` (self-test of the whole API, PASS/FAIL per function), `xmod_dbg bench` (cost of Lua→engine calls) and an **AddToFullPack stress test** (`atfp_register=1`, modes 0-4). In my mock-engine run one hooked Lua call costs ~85–180 ns, so per-entity hooks scale badly on busy servers; the native hide table (`xmod.fullpack.hide`, mode 4) costs ~14 ns per check and needs no Lua call. Run it on your server to get real numbers.

## Hooks in 10 seconds
```lua
xmod.on("ham:takedamage:player", function(victim, inflictor, attacker, dmg, bits)   -- Ham: any CS entity class
  xmod.hook.set_arg(3, dmg * 2)                                                     -- change damage; or: return xmod.HC_SUPERCEDE, 0
end)
xmod.on("reapi:CBasePlayer_Killed:post", function(victim, attacker, gib, ret) end)   -- ReAPI (needs ReGameDLL), 150 hooks
xmod.on("message:DeathMsg", function(name, dest, ent, a) end)                        -- user-message observer / blocker
```
Full tables: `docs/HAM_FUNCTIONS.md`, `docs/REAPI_HOOKS.md` (generated; also appendices in `skillz.md`).

## Building from source
```
./fetch_deps.sh            # HLSDK headers + Lua 5.4 into ./deps
./build_linux.sh           # -> addons/xmod/dlls/xmod_mm_i386.so   (gcc-multilib g++-multilib cmake)
./build_windows.sh         # -> addons/xmod/dlls/xmod_mm.dll       (MinGW cross-compile: g++-mingw-w64-i686 cmake)
# or on Windows with Visual Studio:  cmake -B build-msvc -A Win32 && cmake --build build-msvc --config Release
python3 tools/gen_reapi.py && python3 tools/gen_ham.py && python3 tools/gen_skillz.py   # regenerate hook bindings/docs (needs ./fetch_deps.sh --regen)
cd test && ./run.sh        # smoke test with a mock engine (loads every plugin, drives commands/menus/votes/ham)
cd test && ./run_ext.sh    # extension API + xmod_query tests (mock engine + local HTTP/TCP/UDP servers; needs python3)
extensions/xmod_query/tests/run_backend_tests.sh   # MySQL + libcurl/HTTPS backend tests (native build, needs a local MariaDB)
```
Source map: `src/main.cpp` Metamod glue · `core.cpp` Lua VM/sandbox/events · `api_*.cpp` Lua API · `ham.cpp` vtable hooks · `reapi.cpp` + generated `reapi_decl.h` hookchains · `hookutil.h` shared marshalling.

## Fixed after the first live-server test (0.2.3)
- **Crash when a plugin sent a message from a `message:*` handler** (e.g. HUD on DeathMsg): messages with listeners are now captured and replayed, sends are deferred when needed, and `xmod.msg.begin/finish` is atomic. Blocking a message also left the engine mid-message before; fixed.
- **Bots were invisible** (ReGameDLL bots skip ClientConnect): registered from `CreateFakeClient`, `putinserver` on first think, lazy registration for anything missed.
- **Ham hooks missed bots** (bots have their own vtable): now hooked too.

## Status of the extension system (0.3.0)
- Tested in the mock engine on Linux: extension API (sync/async, ordering per handle, events from worker threads, `xmod reload`, extension unload) and `xmod_query` with SQLite, plain HTTP, TCP, UDP, DNS against local servers.
- Tested natively (64-bit build of the same backend code): MySQL against a real MariaDB 10.11 (types, escaping, reconnect, 4 threads) and libcurl including HTTPS certificate verification.
- **Not tested:** a live HLDS; the Windows DLLs (WinHTTP, WinSock and the Windows thread pool are compile-checked only); MySQL/libcurl as 32-bit libraries inside a real server.

## Honest status
- Verified with the **mock engine** (Linux build): all plugins load, admin flow, targeting/immunity, bans, menus, votes, timers, Ham pre/post/supercede through a patched vtable. **Not yet run on a live HLDS.**
- The **Windows DLL** is cross-compiled with MinGW and has not been executed on Windows. Ham on Windows relies on the `thiscall` convention; ReAPI on Windows relies on MSVC vtable layout (handled by a single-slot destructor shim). If the DLL crashes on load, test the MSVC build.
- ReAPI hooks need ReGameDLL (checked at startup; `xmod.reapi.available()`); Ham works with stock CS 1.6 and ReGameDLL.
- Team/money use CS private-data offsets (confirmed against AMXX gamedata: team 456 B / money 460 B, +20 B on Linux).

## Changes in 0.3.1
* Script errors are now visible on listen servers too (printed to the console), written to `addons/xmod/data/xmod_errors.log`,
  and `xmod plugins` shows the reason next to a DISABLED plugin (before, they only went to Metamod's error log).
* Ownership follows the code: when a library (e.g. Nexus) calls a plugin's function back, `xmod.player.data`, timers,
  `xmod.on`, commands, `xmod.log` and async jobs belong to that plugin, not to the library.
