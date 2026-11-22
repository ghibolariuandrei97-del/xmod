# ModCreator 1.0.0 — make new mods with xmod

ModCreator is an xmod extension + Lua framework with every tool a scripter needs to build a complete mod:

- **Entities & NPCs** — entity classes (think / touch / use / damage / killed per classname), keyvalues, virtual calls (ExecuteHamB),
  HL-style NPCs with AI (hunter, guard, wander, follow, paths, melee and ranged attacks, pain/death animations, rewards),
  studio models (sequences, frame advance, controllers, blending, body groups, attachments, bones), engine movement.
- **Rounds** — new/start/end events, end a round with any winner, custom win conditions, round timer, scores, stop the game's own round end.
- **WeaponMod** — custom weapons on a base CS weapon: models (v/p/w), damage, clip size, ammo, fire rate, recoil, speed, knockback,
  sounds, animations, fully custom shots (bullets/shotgun/laser), hit callbacks, buy menu.
- **Teams & objectives** — custom teams with relations and friendly fire, team scores, balance; capture / destroy / reach / collect /
  kills / survive / custom objectives with progress, HUD and round wins.
- **Tools** — named private data (859 CS members + 76 game-rules members, Windows/Linux, vanilla/ReGameDLL), bullets, radius damage,
  temp-entity effects, HUD helpers, spawn points, player helpers.

Requirements: **xmod 0.4.0 or newer (tested on 0.4.4 — no xmod update needed)**, Counter-Strike 1.6 (ReGameDLL recommended;
the original game DLL works too, except `MC.round.block_end`). Windows and Linux.

## Install
1. Copy `addons/` over your `cstrike/addons/` (nothing of xmod is overwritten; ModCreator only adds files).
2. `addons/metamod/plugins.ini` — add the line for your OS **after** the xmod line:
   ```
   win32 addons/xmod/dlls/modcreator_mm.dll
   linux addons/xmod/dlls/modcreator_mm_i386.so
   ```
3. Keep `addons/xmod/configs/extensions-modcreator.ini` (it contains `mc`).
4. Restart the server. The log shows `[modcreator] ModCreator 1.0.0 ready: xmod.mc.* + require("modcreator")  (…, game rules: …)`.
5. Try the demo: in `configs/plugins-modcreator.ini` remove the `;` before `modcreator/mc_demo.lua`, change the map.

## Files
| file | |
|---|---|
| `addons/xmod/dlls/modcreator_mm.dll`, `modcreator_mm_i386.so` | the extension |
| `addons/xmod/lib/modcreator.lua` | the framework (`require("modcreator")`) |
| `addons/xmod/configs/extensions-modcreator.ini` | declares the `xmod.mc` namespace |
| `addons/xmod/configs/plugins-modcreator.ini` | demo + examples (off) |
| `addons/xmod/plugins/modcreator/mc_demo.lua` | "Bot Siege" demo mod |
| `addons/xmod/plugins/modcreator/examples/*.lua` | NPC, weapons, King of the Hill |
| `modcreator_skills.md` | the complete reference (for you and for an AI) |
| `source/` | C++ source, tests and generators |

## Building from source
Copy `source/extensions/modcreator` into the xmod source tree (`extensions/modcreator`) and build xmod as usual
(`build_windows.sh` / `build_linux.sh`): every folder in `extensions/` is built automatically. `tools/gen_offsets.py` regenerates
`offsets.inc` from AMX Mod X gamedata. `tests/run.sh` runs 94 checks against the real ReGameDLL `cs.so` (Linux).

## Changelog
- 1.0.0 — first release.
