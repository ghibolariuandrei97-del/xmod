# Codex 1.0.1 — call, hook and patch any game function from Lua (an xmod extension)

Codex is a Metamod plugin that plugs into **xmod** and gives Lua scripts (`xmod.codex.*`) direct access to the
game DLL, the engine and any other loaded module, on **Windows and Linux**:

- **find** a function by symbol (also `"Class::Method"`), signature, a string it uses, an offset or a vtable index
- **call** it with typed arguments (cdecl, stdcall, thiscall, fastcall; ints, floats, doubles, int64, strings, entities, vectors)
- **hook** it (detour or vtable): change arguments, replace the return value, skip the original — like Ham/ReAPI, for *any* function
- **read/write memory**, private data fields, **patch code**, and **change hard-coded limits** (`codex.consts` / `codex.set_const`)
- **generate signatures automatically** and cache them per game build, so updates are survived without editing anything

It is designed to **never take the server down** when something does not match: a missing function stays
unresolved and the plugin keeps running, a call that crashes is caught and reported, memory access to unmapped
addresses returns nil, patches are verified and undone on unload.

> Writing plugins with an AI? Give it **`codex_skills.md`** (together with xmod's `xmod_skills.md`). It contains everything.

---

## Requirements
- **xmod 0.4.1 or newer** for full hooks (handlers run *inside* the call and can change it). With xmod 0.4.0 Codex
  works, but hooks are observe-only (handlers run after the call, one frame later, and cannot change anything).
- Metamod (or Metamod-R), HLDS / ReHLDS, any GoldSrc mod. Examples use Counter-Strike 1.6 / ReGameDLL.
- 32-bit (as every GoldSrc server). Linux build needs glibc 2.16+ (any distribution from 2012 on); no libstdc++ dependency.

## Install
1. Copy `addons/xmod/...` from this package into `cstrike/addons/xmod/`:
   - `dlls/xmod_codex_mm.dll` (Windows), `dlls/xmod_codex_mm_i386.so` (Linux)
   - `lib/codex.lua` (helper library), `plugins/codex_tools.lua` (console), `plugins/examples/codex_demo.lua` (demo)
2. `cstrike/addons/metamod/plugins.ini` — **after** the xmod line:
   ```
   win32 addons/xmod/dlls/xmod_codex_mm.dll
   linux addons/xmod/dlls/xmod_codex_mm_i386.so
   ```
3. `addons/xmod/configs/extensions.ini` — add a line: `codex`
4. `addons/xmod/configs/plugins.ini` — add `codex_tools.lua` and, to try it, `examples/codex_demo.lua`
5. Restart. Server console: `codex status`, `codex help`.

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
```
The demo adds `codex_give <#userid|name> <item>`, `codex_gamedata` (signatures of this build), cvars `codex_block_c4`, `codex_maxmoney`.

**Finding a function on Windows** (no symbols): `codex xref game "<a message it prints>"` → you get the function →
`codex sig game+<offset>` → put the signature (or just `xref = "..."`) in the definition. On Linux the symbol is enough.
`codex_gamedata` / `cx.dump()` prints ready-to-paste signatures of every resolved function for the platform you run it on.

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

## What was tested
- Self test inside xmod's mock engine on Linux: **146/146** (every calling convention, pre/post hooks, set_arg, supercede,
  float/double/int64/string/vector, vtable hooks, caught crashes incl. stack overflow twice in a row, signatures, cache,
  limits, the helper library).
- On **real binaries** (ReGameDLL cs.so / mp.dll, ReHLDS engine_i486.so / swds.dll), about 11 000 functions:
  0 wrong trampolines, detour install + restore correct on all; unsafe prologues refused instead of patched.
  Auto signatures unique for ~95% (the rest are byte-identical functions → use symbol / xref / index).
- **Not yet run on a real Windows machine** (only compiled and checked statically; the instruction/trampoline code was
  tested on the real mp.dll and swds.dll). First thing to do on Windows: `codex status`, then the demo.

## Building from source
`source/xmod_codex/` is the extension. Copy it to `extensions/xmod_codex/` of an **xmod 0.4.1** source tree (it needs
0.4.1's `include/xmod_ext.h`, which has `fire_hook_sync`; every folder in `extensions/` is built automatically), then
`./build_linux.sh` or `./build_windows.sh` (MinGW i686).
Tests: `extensions/xmod_codex/tests/run.sh` (self test), `run.sh mass <cs.so>` / `run.sh pemass <mp.dll>` (real binaries).
