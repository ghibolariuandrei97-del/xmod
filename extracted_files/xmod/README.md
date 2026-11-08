# xmod 0.2 — next-generation scripting for GoldSrc / CS 1.6

A Metamod plugin that replaces AMX Mod X's Pawn VM with **Lua 5.4**: no compiling, hot reload (`xmod reload`),
sandboxed scripts (100 ms CPU limit per call, 96 MB memory cap, auto-disable after 25 errors), one shared event bus.
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
```
Source map: `src/main.cpp` Metamod glue · `core.cpp` Lua VM/sandbox/events · `api_*.cpp` Lua API · `ham.cpp` vtable hooks · `reapi.cpp` + generated `reapi_decl.h` hookchains · `hookutil.h` shared marshalling.

## Honest status
- Verified with the **mock engine** (Linux build): all plugins load, admin flow, targeting/immunity, bans, menus, votes, timers, Ham pre/post/supercede through a patched vtable. **Not yet run on a live HLDS.**
- The **Windows DLL** is cross-compiled with MinGW and has not been executed on Windows. Ham on Windows relies on the `thiscall` convention; ReAPI on Windows relies on MSVC vtable layout (handled by a single-slot destructor shim). If the DLL crashes on load, test the MSVC build.
- ReAPI hooks need ReGameDLL (checked at startup; `xmod.reapi.available()`); Ham works with stock CS 1.6 and ReGameDLL.
- Team/money use CS private-data offsets (confirmed against AMXX gamedata: team 456 B / money 460 B, +20 B on Linux).
