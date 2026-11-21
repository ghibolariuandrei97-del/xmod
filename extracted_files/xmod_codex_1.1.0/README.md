# Codex 1.1.0 — call, hook, patch and map any game function from Lua (an xmod extension)

Codex is a Metamod plugin that plugs into **xmod** and gives Lua scripts (`xmod.codex.*`) direct access to the
game DLL, the engine and any other loaded module, on **Windows and Linux**:

- **find** a function by symbol (also `"Class::Method"`), signature, a string it uses, an offset or a vtable index —
  or by the **call graph**: what it calls, who calls it, or its **treemap** fingerprint (the Okapi idea)
- **map the code**: `codex.callers`, `codex.callees`, `codex.calltree`, `codex.analyze` (strings, constants, size)
- **call** it with typed arguments (cdecl, stdcall, thiscall, fastcall; ints, floats, doubles, int64, strings, entities, vectors)
- **hook** it (detour or vtable): change arguments, replace the return value, skip the original — like Ham/ReAPI, for *any* function
- **read/write memory**, private data fields, **patch code**, **find/replace values** (`find_value` / `replace_value`), and
  **change hard-coded limits** (`codex.consts` / `codex.set_const`)
- **generate signatures automatically** and cache them per game build, so updates are survived without editing anything

It is designed to **never take the server down** when something does not match: a missing function stays
unresolved and the plugin keeps running, a call that crashes is caught and reported, memory access to unmapped
addresses returns nil, patches are verified and undone on unload.

> Writing plugins with an AI? Give it **`codex_skills.md`** (together with xmod's `xmod_skills.md`). It contains everything.

---

## Requirements
- **xmod 0.4.2** (recommended): it reads this package's `configs/extensions-codex.ini` and `configs/plugins-codex.ini`,
  so installing Codex needs no edits to your config files, and long Codex calls do not trip `xmod_script_timeout`.
- xmod 0.4.1: works too (full hooks) — add `codex` to `configs/extensions.ini` and `codex_tools.lua` to `configs/plugins.ini` by hand.
- xmod 0.4.0: Codex works, but hooks are observe-only (handlers run after the call, one frame later, and cannot change anything).
- Metamod (or Metamod-R), HLDS / ReHLDS, any GoldSrc mod. Examples use Counter-Strike 1.6 / ReGameDLL.
- 32-bit (as every GoldSrc server). Linux build needs glibc 2.16+ (any distribution from 2012 on); no libstdc++ dependency.

## Install
1. Copy `addons/` from this package over `cstrike/addons/` (it only adds files):
   - `xmod/dlls/xmod_codex_mm.dll` (Windows), `xmod/dlls/xmod_codex_mm_i386.so` (Linux)
   - `xmod/lib/codex.lua` (helper library), `xmod/plugins/codex_tools.lua` (console), `xmod/plugins/examples/codex_demo.lua` (demo)
   - `xmod/configs/extensions-codex.ini` (declares `xmod.codex`), `xmod/configs/plugins-codex.ini` (loads `codex_tools.lua`)
2. `cstrike/addons/metamod/plugins.ini` — **after** the xmod line:
   ```
   win32 addons/xmod/dlls/xmod_codex_mm.dll
   linux addons/xmod/dlls/xmod_codex_mm_i386.so
   ```
3. Restart. Server console: `codex status`, `codex help`. To try the demo, remove the `;` in front of
   `examples/codex_demo.lua` in `configs/plugins-codex.ini`.

Codex writes only to `addons/xmod/data/codex/cache.ini` (safe to delete) and to the xmod log.

## Quick start
```lua
-- plugins/give.lua
xmod.register("give", "1.0", "me")
local cx = require("codex")                 -- helper library (lib/codex.lua)

cx.ready(function(C)                        -- runs once Codex is attached (scripts load before extensions)
  cx.define({
    GiveNamedItem = { module = "game", conv = "thiscall", this = "entity", args = { "astring" }, ret = "void",
                      symbol = "CBasePlayer::GiveNamedItem",          -- Linux: symbol
                      xref   = "NULL Ent in GiveNamedItem!" },        -- Windows: the function that uses this string
  })
  cx.hook("GiveNamedItem")
end)

-- handlers are normal xmod events (always register them in your plugin)
xmod.on("codex:GiveNamedItem", function(id, item)
  if item == "weapon_c4" then return xmod.HC_SUPERCEDE end    -- nobody gets the bomb
end)

xmod.cmd.client("give_ak", "m", function(id) cx.call("GiveNamedItem", id, "weapon_ak47") end)
```

Breaking a hard-coded limit:
```lua
local h = cx.limit("AddAccount", 16000, 100000, { near = true })   -- every instruction of AddAccount that uses 16000
-- ... later
h:restore()
```

## The console (`codex_tools.lua`, server console / rcon only)
```
codex status | modules | list | info <func> | patches | cache_clear
codex sym  <module> <Class::Method>      codex find <module> <signature>      codex xref <module> <text>
codex sig  <func|addr|module+offset>     codex insn <func|addr|module+offset> [n]
codex read <addr|module+offset> <type> [n]      codex consts <func|addr> <value> [near]
codex callees <func|sym|addr>    codex callers <func|sym|addr> [refs]    codex tree <func|sym|addr> [depth] [up]
codex analyze <func|sym|addr>    codex treemap <func|sym|addr>           codex tmfind <module> <treemap>
codex symat <addr|module+offset> codex findval <module> <type> <value>  codex report
```
The demo adds `codex_give <#userid|name> <item>`, `codex_gamedata` (signatures of this build), cvars `codex_block_c4`, `codex_maxmoney`.

**Finding a function on Windows** (no symbols): `codex xref game "<a message it prints>"` → you get the function →
`codex sig game+<offset>` → put the signature (or just `xref = "..."`) in the definition. On Linux the symbol is enough.
`codex_gamedata` / `cx.dump()` prints ready-to-paste signatures of every resolved function for the platform you run it on.

## The call graph (new in 1.1, inspired by Okapi)
```lua
C.callees("CBasePlayer::GiveNamedItem")          -- every call it makes: {at, to, kind = call|jmp|import|virtual|indirect, name, vindex}
C.callers("CBasePlayer::GiveNamedItem")          -- who calls it: {at, func, name, kind}   ({refs = true}: also vtables / callbacks)
C.calltree("CBasePlayer::Spawn", { depth = 2 })  -- nested tree down; { dir = "up" } = callers of callers
C.analyze("GiveNamedItem")                       -- size, instructions, strings it uses, constants, direct callees
local map = C.treemap("GiveNamedItem")           -- fingerprint: the function + what it calls, no addresses
C.treemap_find("game", map)                      -- after an update: { {addr, score, name}, ... }
-- in definitions (resolvers 7-9, used when symbol / sig / xref fail):
C.func{ name = "A", callee = { of = "CBasePlayer::Spawn", index = 2 } }   -- the 2nd function Spawn calls
C.func{ name = "B", caller = "SomeKnownFunction" }                        -- the only function that calls it
C.func{ name = "C", treemap = { windows = "cx1:...", linux = "cx1:..." } }
-- helper library: find a function by what it does
cx.find_func{ strings = { "some message" }, calls = { "A" }, called_by = { "B" }, consts = { 16000 } }
cx.dump(nil, true, { treemap = true })           -- gamedata with signature AND treemap for every function
```
Plus Okapi's memory tools: `C.find_value("game", "str" | "i32" | "f32" | "bytes" ..., v)`, `C.replace_value(...)`
(reversible patches), `C.symbol_at(addr)`, and `cx.wl(win, lin)`. A full Okapi → Codex table is in `codex_skills.md`.

## What Codex protects against
| situation | result |
|---|---|
| signature / symbol / string not found, ambiguous, wrong module | the function stays unresolved, the plugin continues, `codex info <name>` says exactly why |
| a call crashes (wrong definition, bad pointer, divide by zero, endless recursion) | caught: the call is aborted, a Lua error with the place of the crash, the function is marked *faulted* until `codex.reset(name)` |
| read/write of unmapped memory | nil / false, never a crash |
| a patch over unexpected bytes (other build), overlapping patches | refused |
| a function whose first bytes cannot be moved safely | detour refused with the reason (use a vtable hook) |
| a constant that does not fit the instruction (`cmp eax, 10` has 1 byte) | refused; `set_const` patches all sites or none |
| the same function hooked by several plugins / found again by signature while hooked | works: signatures and constant searches see the original bytes |
| unload, `xmod reload`, shutdown | every hook and patch removed in reverse order; a patch somebody else changed in the meantime is left alone |

What it **cannot** protect against: a definition that *works* but is wrong (arguments in the wrong order, wrong
`this`), values you write into the game's memory that break it later, and the game's own crashes inside functions
you hooked. Test a definition with `codex.call` first (calls are guarded), then hook it.

## Compared with Orpheu
| | Orpheu (AMXX) | Codex (xmod) |
|---|---|---|
| definitions | JSON files written by hand | Lua tables in the plugin; one definition for both platforms |
| finding functions | signature or symbol | symbol (also demangled), signature, string reference, offset, vtable index, follow call/jmp, per-build cache, signatures learned from older builds |
| signatures | made by hand in IDA | generated automatically (`sig_make`, cached per build); anchored inside the function when the start is not unique |
| function not found | plugin fails | stays unresolved, plugin keeps running, exact reason |
| a call crashes | server crashes | caught and reported, server keeps running |
| hard-coded limits | manual memory patches | `consts` / `set_const`: every branch searched, fit checks, all-or-nothing, reversible |
| strings the game keeps | | `astring` / `string_t` types use the engine string pool |

## Compared with Okapi
| | Okapi (AMXX) | Codex (xmod) |
|---|---|---|
| treemaps | `okapi_get_treemap_ptr` | `codex.treemap` / `treemap_find` / `treemap =` in a definition, with a score and a uniqueness check |
| call graph | (the treemap only) | `callers`, `callees` (direct, PLT/GOT, imports, virtual calls with vtable index), `calltree`, `callee`/`caller` resolvers, `find_func` |
| find/replace values | `okapi_mod_find_*` / `okapi_mod_replace_*` | `find_value` / `replace_value` — reversible, all-or-nothing, a `max` safety net |
| a call crashes | server crashes | caught and reported |
| signatures | by hand | generated, cached per build, learned across builds |

## What was tested
- Self test inside xmod's mock engine on Linux: **182/182** (every calling convention, pre/post hooks, set_arg, supercede,
  float/double/int64/string/vector, vtable hooks, caught crashes incl. stack overflow twice in a row, signatures, cache,
  limits, the call graph through PLT stubs, callee/caller/treemap resolvers, find/replace values, the helper library).
- On **real binaries** (ReGameDLL cs.so / mp.dll, ReHLDS engine_i486.so / swds.dll), about 11 000 functions:
  0 wrong trampolines, detour install + restore correct on all; unsafe prologues refused instead of patched.
  Auto signatures unique for ~95% (the rest are byte-identical functions → use symbol / xref / index).
- Call graph on ReGameDLL cs.so, compared with the symbol table: callees identical for 99.8% of 4608 functions (no false
  calls), callers identical for 286/300 functions (no caller missed); on mp.dll every callee edge is found back by
  `callers`. Treemap self-find: 98% unique first place on both cs.so and mp.dll (the rest are byte-identical clones).
- **Not yet tested:** a real Windows machine (only compiled; the code-analysis parts were tested on the real mp.dll and
  swds.dll mapped on Linux), and treemaps across two *different* game versions. First thing to do on Windows:
  `codex status`, then the demo, then `codex callees GiveNamedItem`.

## Building from source
`source/xmod_codex/` is the extension. Copy it to `extensions/xmod_codex/` of an **xmod 0.4.2** source tree (it needs
`include/xmod_ext.h` with `fire_hook_sync`, xmod ≥ 0.4.1; every folder in `extensions/` is built automatically), then
`./build_linux.sh` or `./build_windows.sh` (MinGW i686).
Tests: `extensions/xmod_codex/tests/run.sh` (self test), `run.sh mass <cs.so>` / `run.sh pemass <mp.dll>` (real binaries),
`run.sh graph <cs.so> <mp.dll>` (call graph and treemaps on real binaries).
