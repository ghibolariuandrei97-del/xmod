# codex_skills.md — complete knowledge file for Codex 1.0.1 (xmod extension `xmod_codex`)

> **For AI assistants.** This file is everything needed to write correct, safe xmod Lua plugins that use Codex: every
> function, every option, every type conversion, the exact semantics of hooks, the platform differences, the error
> messages, the helper library, recipes and the rules to follow. It complements `xmod_skills.md` (the xmod Lua API:
> `xmod.on`, `xmod.cmd`, `xmod.player`, `xmod.cvar`, `xmod.timer`, Ham, ReAPI ...). When both apply, read both.
>
> The person you help is usually a CS 1.6 server/plugin developer coming from AMX Mod X (Pawn, Orpheu, Fakemeta, Ham,
> ReAPI). Write Lua for **xmod**, not Pawn. Prefer xmod's built-in events (ReAPI hookchains `reapi:*`, Ham `ham:*`)
> when they cover the need — they are maintained by the game DLL and need no gamedata. Use Codex for everything they
> do not cover: non-virtual functions, engine internals, functions without hookchains, hard-coded limits, raw memory.

---------------------------------------------------------------------------------------------------------------------

## Table of contents
1. Mental model
2. Installation, load order, lifecycle
3. API conventions (return values, errors)
4. Modules
5. Defining functions — `codex.func{}`
6. Calling conventions and `this`
7. Types — Lua ⇄ native conversion
8. Calling — `codex.call`, `codex.callh`, the fault guard
9. Hooking — detours, vtable hooks, events, handlers
10. Finding code — signatures, symbols, string references, disassembly
11. Hard-coded constants — `codex.consts`, `codex.set_const`
12. Memory, private data, patches
13. Vtables, engine and DLL function tables
14. Misc functions
15. The helper library `lib/codex.lua`
16. The console tool `codex_tools.lua`
17. Workflows (new function on Linux / Windows, cross-platform gamedata, porting Orpheu / AMXX)
18. Recipes
19. Rules for writing Codex code (checklist)
20. Error messages and what they mean
21. Limits and things that are not supported
22. Internals (for debugging)
23. Function index

---------------------------------------------------------------------------------------------------------------------

## 1. Mental model

- Codex is C++ (a Metamod plugin, `xmod_codex_mm.dll` / `xmod_codex_mm_i386.so`) that registers Lua functions in the
  namespace **`xmod.codex`**. All Codex functions are **synchronous** and must be used from the main (game) thread —
  which is where every xmod Lua callback runs anyway.
- You **define** a native function once with a name (`codex.func{ name = "X", ... }`): where it is (symbol, signature,
  string reference, offset, vtable index) and what it looks like (calling convention, `this`, argument types, return
  type). Codex resolves it to an address and remembers it.
- Then you **call** it (`codex.call("X", ...)`), **hook** it (`codex.hook("X")` + `xmod.on("codex:X", fn)`), or use
  its address (`codex.addr("X")`, `codex.consts("X", 16000)`).
- Everything is keyed by **name**. Names are global across all plugins: two plugins that define the same name share
  the definition (the first resolved one wins). Use descriptive names (`"CBasePlayer_AddAccount"`) or a prefix.
- Codex is 32-bit x86 only (GoldSrc servers are 32-bit).

```lua
local C = xmod.codex                         -- the raw API (this document calls it codex.* / C.*)
local cx = require("codex")                  -- the helper library (section 15) — recommended
```

---------------------------------------------------------------------------------------------------------------------

## 2. Installation, load order, lifecycle

### Files
| file | purpose |
|---|---|
| `addons/xmod/dlls/xmod_codex_mm.dll` / `xmod_codex_mm_i386.so` | the extension |
| `addons/xmod/lib/codex.lua` | helper library (`require("codex")`) |
| `addons/xmod/plugins/codex_tools.lua` | server console command `codex ...` |
| `addons/xmod/plugins/examples/codex_demo.lua` | demo (GiveNamedItem call + hook, money limit) |
| `addons/xmod/data/codex/cache.ini` | resolution cache, generated, safe to delete |

`addons/metamod/plugins.ini` needs the Codex line **after** xmod:
```
linux addons/xmod/dlls/xmod_mm_i386.so
linux addons/xmod/dlls/xmod_codex_mm_i386.so
```
(Windows: `win32 addons/xmod/dlls/xmod_mm.dll` then `win32 addons/xmod/dlls/xmod_codex_mm.dll`.)
`addons/xmod/configs/extensions.ini` needs a line `codex` — this declares the namespace `xmod.codex` for scripts.
Always tell the user which lines to add (plugins.ini of metamod, extensions.ini, xmod's plugins.ini).

### Version requirements
- **xmod ≥ 0.4.1**: full hooks (Lua handlers run inside the native call, can change arguments / return value /
  skip the original). `codex.status().sync_hooks == true`.
- xmod 0.4.0: Codex loads, calls / memory / signatures work, but hooks are **observe-only**: only `codex:<name>:post`
  handlers run, asynchronously (next frame), and nothing can be changed. Codex logs this when hooking.

### Load order — the most important rule
**xmod loads Lua plugins before extensions attach.** At the top level of a plugin file, `xmod.codex` exists (it is a
stable table) but its functions do not yet: calling `xmod.codex.func(...)` at load time raises
`xmod.codex.func is not available`. Even *reading* a field such as `C.status` raises that error, so
`pcall(C.status)` does not work — use `pcall(function() return C.status() end)`.

Correct patterns:
```lua
local cx = require("codex")
cx.ready(function(C)          -- runs now if Codex is attached (e.g. after "xmod reload"), else at the next "init"
  cx.define(GAMEDATA)
  cx.hook("SomeFunction")
end)
xmod.on("codex:SomeFunction", handler)        -- handlers CAN (and should) be registered at load time
```
or without the library:
```lua
local function setup() ... end
if pcall(function() return xmod.codex.status() end) then setup() else xmod.on("init", setup) end
```
(`init` fires on every map start; guard with a flag if setup must run once. Defining an existing name again and
hooking an already hooked function are harmless no-ops, so running setup on every map is also fine.)

### Lifecycle
- **Map change**: definitions, hooks and patches persist. Entity indexes change meaning — never cache entity pointers.
  Strings allocated with `astring`/`string_t` (engine string pool) are freed by the engine at map change.
- **`xmod reload`**: the whole Lua state is rebuilt and all plugins (and libraries) load again. Codex keeps its
  definitions, hooks and patches (they live in C++). Your plugin's handlers are registered again by your code;
  `codex.func` with an existing resolved name returns `true` immediately; `codex.hook` on a hooked function returns `true`.
  Patches created by the previous Lua state stay applied — if your plugin stores patch ids / handles, they are lost on
  reload; re-applying the same `set_const` then fails with `patch: overlaps patch #n`. Either restore patches in a handler
  for xmod's unload/shutdown event if available, or tolerate the overlap error (the patch is still in place), or look
  them up with `codex.patches()` (field `owner`).
- **Unload / server shutdown** (Metamod detach): Codex removes every hook and patch in reverse order and saves the cache.
  A patch whose bytes were changed by someone else in the meantime is left alone instead of being "restored" over them.

---------------------------------------------------------------------------------------------------------------------

## 3. API conventions

- **Lookups** (`find`, `symbol`, `module`, `addr`, `info`, `read`, `patch`, `consts`, `set_const`, `func`, `hook`, ...)
  return **`nil`** (or `false`) on failure; **`codex.last_error()`** returns the reason as text. `codex.func` and
  `codex.hook` also log the reason.
- **Misuse** (wrong argument types, missing arguments, a function name never defined, calling handler-only functions
  outside a handler) raises a **Lua error** with a message starting with `codex...`.
- **`codex.call` / `codex.callh` raise a Lua error on every failure** (unresolved, wrong argument count/type, invalid
  entity, crash inside the call). Wrap in `pcall` or use `cx.try` / `cx.call` from the library.
- Addresses are plain Lua integers (32-bit unsigned values, e.g. `0xF7A12340`). Entity indexes are small integers
  (0 = worldspawn, 1..maxplayers = players). Any integer argument may also be passed as a numeric string ("0x1234").
- Hex byte strings: `"55 8B EC"`, case-insensitive, separated by spaces/commas/tabs.

---------------------------------------------------------------------------------------------------------------------

## 4. Modules

Every lookup takes a module name. Names are case-insensitive. Special names:

| name | module |
|---|---|
| `"game"` (default), `"mod"`, `"gamedll"` | the game DLL, whatever its file name (`cs.so` / `mp.dll`; with ReGameDLL too) — found via `pfnGameInit` |
| `"engine"` | `engine_i486.so` / `engine_i686.so` / `engine_amd.so` (Linux), `swds.dll` (dedicated), `hw.dll` / `sw.dll` (listen server) |
| `"metamod"` | the Metamod DLL |
| `"xmod"` | `xmod_mm*` |
| `"codex"` / `"self"` | Codex itself |
| anything else | exact file name (`"libc.so.6"`, `"filesystem_stdio.dll"`), else the first module whose name contains it |

- `codex.module(name)` → `{name, path, base, size, build}` or nil. `build` is a fingerprint (hash of the first 64 KB of
  code) identifying the exact build.
- `codex.modules()` → array of all loaded modules (refreshes the list).
- `codex.module_of(addr)` → module table + `offset` (= addr − base), or nil.
- On a **listen server** (Windows, playing and hosting from the game client) the engine module is `hw.dll`, and the
  game client DLLs are loaded too. The game DLL is still `"game"`.

---------------------------------------------------------------------------------------------------------------------

## 5. Defining functions — `codex.func{...}` → `true` | `nil`

```lua
codex.func{
  name   = "GiveNamedItem",          -- required; events: codex:GiveNamedItem, codex:GiveNamedItem:post
  module = "game",                   -- default "game"
  conv   = "thiscall",               -- "cdecl" (default) | "stdcall" | "thiscall" (alias "method") | "fastcall"
  this   = "entity",                 -- "entity" | "ptr" | true | false   (thiscall implies this = "entity")
  args   = { "astring" },            -- argument types, after 'this' (section 7)
  ret    = "void",                   -- return type (default "void")
  -- where it is — any combination; the first that works is used (order below)
  symbol = "CBasePlayer::GiveNamedItem",
  sig    = "55 8B EC 83 EC ?? 53 56 57",
  xref   = "NULL Ent in GiveNamedItem!",
  -- address = 0x..., offset = 0x1234, vindex = 12,
  -- adjust = 0, follow = "call", index = 1, replace = false,
}
```

### Fields
| field | type | meaning |
|---|---|---|
| `name` | string | your name for it. Global across plugins. |
| `module` | string | module to search (section 4). The resolved address must be executable code **of this module** (so an engine function needs `module = "engine"`). |
| `conv` | string | calling convention (section 6). |
| `this` | `"entity"`, `"ptr"`, bool | the function has a `this` pointer, passed as the **first** Lua argument. `"entity"`: Lua sees/passes an entity index (CBaseEntity* ⇄ index). `"ptr"`: raw pointer number. `true` = `"entity"`. `thiscall` sets it automatically to `"entity"`; set `this = "ptr"` for non-entity objects. |
| `args` | array of type names | the arguments after `this`. |
| `ret` | type name | return type. `vecval` is not allowed as a return. |
| `symbol` | string or array | symbol names to try, in order (section 10). |
| `sig` / `signature` | string or array | signatures to try, in order. Both fields are merged. |
| `xref` | string or `{string = "...", index = n}` | a string literal the function uses; the function containing the n-th (default 1st) reference is taken. |
| `address` | integer | absolute address. |
| `offset` | integer | offset from the module base. |
| `vindex` | integer | virtual function: index in the vtable. Needs `this`. Resolved immediately, no module search; the actual function is read from the object's vtable at each call. |
| `adjust` | integer | bytes added to what was found (not applied to cached results). |
| `follow` | `"call"`, `"jmp"`, `"rel32"` | the match is a `call`/`jmp` (E8 / E9 / 0F 8x) — take its target. `"rel32"`: the match *is* a rel32 field. |
| `index` | integer | a signature matching several places: take the n-th match (1-based, in address order). Without it, a non-unique signature is rejected. |
| `replace` | bool | redefine an existing, resolved name (fails if it is hooked: unhook first). |

**Platform-specific values**: any field may be a table `{ windows = ..., linux = ... }`; Codex picks the entry for the
current platform (a missing entry = field not given):
```lua
sig    = { windows = "55 8B EC 83 E4 F8 ...", linux = "55 89 E5 57 56 53 ..." },
vindex = { windows = 12, linux = 12 },
offset = { windows = 0x1A2B0, linux = 0x8F3C0 },
```
Arrays inside work too: `symbol = { linux = { "A", "B" } }`.

### Resolution order (first success wins)
1. **cache** — `data/codex/cache.ini` has an entry for this module *build* and this name: the stored offset is used
   if it is still executable code and the stored auto-signature still matches there (verified, never blind).
2. `address`, then `offset`.
3. `symbol` names, in order.
4. `sig` / `signature` strings, in order (each must be unique unless `index` is given).
5. **learned signatures**: signatures Codex generated for this name on *other builds* of the same module file
   (survives game/engine updates that only moved code).
6. `xref`.
Then `adjust` / `follow` are applied (not for cache hits), the result must be executable code of `module`, and Codex
stores `module|build|name|offset|auto-signature` in the cache (the auto signature is generated with `sig_make`).

### Results
- `true`: resolved. `codex.info(name)` → `how` = `"cache"`, `"address"`, `"offset"`, `"symbol <s>"`, `"signature"`,
  `"learned signature"`, `"xref \"...\""`, or `"vtable[n]"`; `sig` = the signature used/generated.
- `nil`: not resolved. The definition **is still registered** (unresolved): `codex.info(name).why` lists what was
  tried and why each failed (e.g. `"symbol 'X' not found in cs.so; signature matches 3 places (make it longer or set
  index); "`). Calls raise `'X' is not resolved: ...`. The failure is logged.
- Defining a name that already exists **and is resolved** returns `true` without changing anything (shared gamedata:
  first definition wins) unless `replace = true`. An existing *unresolved* definition is replaced.
- Errors in the definition itself (unknown type, unknown conv, missing name, vindex without this) → `nil` + reason.

---------------------------------------------------------------------------------------------------------------------

## 6. Calling conventions and `this`

Getting this right matters: on Windows a wrong argument count with `stdcall`/`thiscall` corrupts the stack of the
caller (the callee pops the arguments). Codex catches crashes inside `codex.call`, but not stack damage that only shows
up later, and not anything in hooks when the *game* calls.

| conv | used for | `this` | arguments | who pops |
|---|---|---|---|---|
| `cdecl` | C functions, static functions, engine API (`pfn*`), most game globals, GCC (Linux) everything non-member | first stack arg (if `this` set) | stack, right to left | caller |
| `stdcall` | Win32 API, some MSVC exports | — | stack | **callee** |
| `thiscall` | C++ member functions (non-virtual and virtual) | **Windows (MSVC): ECX** · **Linux (GCC): first stack argument** | stack | **Windows: callee**, Linux: caller |
| `fastcall` | rare (`__fastcall`) | — | first two ≤32-bit non-float args in ECX, EDX; rest on the stack | callee |

- C++ methods (`CBasePlayer::GiveNamedItem`, `CHalfLifeMultiplay::CheckWinConditions`, virtual `TakeDamage`) → **always
  `conv = "thiscall"`** on both platforms; Codex places `this` correctly per platform.
- `this` is the first Lua argument in calls and the first handler argument in hooks. With `this = "entity"` it is an
  entity index; pass a player id directly. For the game rules object (`CHalfLifeMultiplay*`, not an entity) use
  `this = "ptr"` and pass the pointer (from a global, found e.g. with an xref + `codex.insn`).
- Floats: `float` arguments occupy one stack word, `double` and `i64` two. Return values: integers/pointers in EAX
  (EDX:EAX for i64), `float`/`double` in ST(0) — Codex handles all of them.
- Functions returning a struct/class **by value** (e.g. `Vector GetGunPosition()`) use a hidden pointer argument
  whose position differs between compilers: not supported directly (section 21).

---------------------------------------------------------------------------------------------------------------------

## 7. Types — Lua ⇄ native

Type names (aliases in parentheses) for `args`, `ret`:

| type | native | Lua → native (argument) | native → Lua (return / hook argument) |
|---|---|---|---|
| `void` | — | (not an argument) | `nil` |
| `bool` | bool (1 byte in a word) | `true`/`false`, number ≠ 0 | `true`/`false` |
| `i8` (`char`, `int8`), `u8` (`byte`, `uint8`) | 8-bit | number (truncated) | number (sign/zero-extended) |
| `i16` (`short`, `int16`), `u16` (`ushort`, `uint16`) | 16-bit | number | number |
| `int` (`i32`, `int32`, `long`) | 32-bit signed | number, bool, nil → 0 | number |
| `uint` (`u32`, `uint32`, `unsigned`) | 32-bit unsigned | number | number 0..4294967295 |
| `i64` (`int64`, `u64`, `uint64`) | 64-bit (2 words) | number | number |
| `float` (`f32`) | float | number | number |
| `double` (`f64`) | double (2 words) | number | number |
| `ptr` (`pointer`, `void*`) | pointer | number (address) | number (address, 0 = NULL) |
| `string` (`str`, `char*`, `const char*`) | `char*` | Lua string → **temporary copy valid only during the call**; `nil` → NULL; number → raw `char*` | Lua string (copied), NULL → `nil` |
| `astring` (`pstring`) | `char*` in the **engine string pool** | Lua string → `AllocString` → pointer that stays valid until the map changes; nil → NULL; number → raw | Lua string |
| `string_t` (`stringt`) | `int` offset into the string pool (`pev->classname` etc.) | Lua string → `AllocString` index; number → raw value; nil → 0 | Lua string (`""` for 0) |
| `entity` (`cbase`, `CBaseEntity*`) | `CBaseEntity*` (private data) | entity index → its private data (error if the entity has none); `-1`/`nil` → NULL; a number > 0x10000 is taken as a raw pointer | entity index, `-1` if NULL / not an entity |
| `edict` (`edict_t*`) | `edict_t*` | index → edict (error if free/invalid); -1/nil → NULL; > 0x10000 raw | index or -1 |
| `entvars` (`entvars_t*`) | `entvars_t*` (`pev`) | index → `&edict->v`; -1/nil → NULL; > 0x10000 raw | index or -1 |
| `vector` (`vec`, `Vector*`, `Vector&`) | pointer to 3 floats | `{x=,y=,z=}` or `{1,2,3}` → temporary copy for the call; number → raw pointer; nil → NULL | `{x=,y=,z=}` (copied), NULL → nil |
| `vecval` (`Vector`) | Vector **by value** (3 words) | `{x=,y=,z=}` / `{1,2,3}` | (only as argument) |

**String lifetime rule.** `string` is right for functions that only *read* the text during the call (most of them:
`ClientPrint`, `UTIL_*` messages, lookups). If the function **keeps the pointer** — creating entities by class name
(`CREATE_NAMED_ENTITY(MAKE_STRING(name))`, `GiveNamedItem`), setting `pev->classname`/`model`/`targetname`,
precaching (`PRECACHE_MODEL` keeps the pointer!) — use **`astring`**, otherwise the game later reads freed memory.
For `string_t` parameters (ints that are pool offsets) use `string_t`.

**Entity rule.** `entity` needs an entity with private data (a real game entity, not a free slot). A disconnected
player slot or a freed entity → the call raises `entity N has no private data (not a valid entity)`.

---------------------------------------------------------------------------------------------------------------------

## 8. Calling

- **`codex.call(name, ...)`** → the return value (converted per `ret`; `nil` for void). Calls the original code:
  **your own Codex hooks on that function are skipped** (detour → trampoline; vtable hook → original slot), so calling
  a function from inside its own hook handler does not recurse.
- **`codex.callh(name, ...)`** → same, but through the hooks (Lua handlers run), as if the game called it.
- The argument count must be exactly `#args` (+1 if `this`), else: `'X' expects N arguments (including this), got M`.
- Virtual functions (`vindex`): the target is read from the vtable of the `this` object at each call.

### The fault guard
Every `codex.call`/`callh` runs inside a fault guard (also every `codex.read`/`write`):
- Linux: SIGSEGV, SIGBUS, SIGILL, SIGFPE, on an alternate signal stack (so **stack overflow / endless recursion is
  caught too**). Signals outside guarded calls go to the previous handler unchanged.
- Windows: an SEH frame around the call: access violation, illegal/privileged instruction, divide by zero, stack
  overflow (the stack guard page is re-armed afterwards), any `0xC000xxxx` error and an uncaught MSVC C++ exception.
  Exceptions the called code handles itself are not touched.
- On a fault: the call is abandoned (the stack and FPU state are restored), the error is logged and raised:
  `'X' crashed: SIGSEGV (bad memory access) at f7a1c387 (cs.so+1387) touching 00000000 - the call was aborted, the
  server keeps running. Check the definition (args, conv, this).`, and the function is marked **faulted**: further
  calls are refused (`'X' crashed earlier (...) - fix the definition and call codex.reset("X")`) until
  `codex.reset(name)`.
- What the guard cannot undo: side effects the function did before crashing (half-updated game state, a lock it
  took, memory it allocated). A crash means the definition is wrong — fix it, do not retry in a loop.
- `codex.safe(false)` disables the guard (slightly faster calls; a crash then kills the server). `codex.safe()` returns
  the current state. Leave it on.

---------------------------------------------------------------------------------------------------------------------

## 9. Hooking

### Installing
- **`codex.hook(name)`** — **detour**: the first ≥5 bytes of the function are replaced by a jump to a Codex stub; the
  original instructions are relocated into a trampoline. Every caller (game, engine, other plugins) goes through it.
- **`codex.hook(name, {class = "player"})`**, **`{entity = id}`**, **`{object = ptr}`** — **vtable hook** for a function
  defined with `vindex`: the slot in the class's vtable is replaced. A vtable is shared by **all objects of the same
  C++ class** — `{entity = 5}` only says which class (the class of entity 5), it hooks all entities of that class.
  `{class = "weapon_ak47"}` finds an existing entity of that classname or creates a temporary one to read its vtable
  (removed immediately).
- Returns `true` (also when already hooked), or `nil` + reason. Hooking a resolved function twice is a no-op.
- **`codex.unhook(name [, label])`** → `true` if something was removed. `label` (the class label shown in
  `codex.info(name).hooks`, e.g. `"player"`) removes only that vtable hook; no label removes all hooks of the function.
- A function that is hooked cannot be redefined (`replace`) until unhooked.

### When a detour is refused (with the reason)
`function is shorter than 5 bytes`, `unconditional jump inside the first 5 bytes`, `loop/jecxz in the first bytes`,
`16-bit relative branch in the first bytes`, `a later branch jumps into the first bytes`, `cannot decode the instruction`.
These protect against corrupting the function. Use a vtable hook (if virtual) or hook a caller instead.
Position-dependent instructions in the first bytes are relocated correctly: `call`/`jmp`/`jcc` rel8/rel32, GCC PIC
thunks (`call __x86.get_pc_thunk.bx`), `call $+5; pop`. A function already detoured by another module (a 5-byte
`jmp` at its start) is chained: Codex relocates that jump.

### Events
- Pre: **`codex:<name>`** — handler arguments: `this` (if any), then the arguments, converted per type (section 7).
- Post: **`codex:<name>:post`** — the same arguments (as they were passed to the original, after pre changes) followed
  by the **return value** as the last argument (omitted for `void`).
- Register with `xmod.on(event, fn)` **in your plugin file** (not in a library — see section 15).

### What a handler can do
| goal | pre-hook | post-hook |
|---|---|---|
| change a numeric/entity argument | `xmod.hook.set_arg(n, v)` or `codex.set_arg(n, v)` | — |
| change a string / vector argument | `codex.set_arg(n, "text")` / `codex.set_arg(n, {x=,y=,z=})` | — |
| skip the original, return v | `return xmod.HC_SUPERCEDE, v` | — |
| skip the original (void, or keep a value set with `codex.set_ret`) | `return xmod.HC_SUPERCEDE` | — |
| replace the return value | (with supercede) | `return xmod.HC_SUPERCEDE, v` or `codex.set_ret(v)` |
| read an argument (current value) | `codex.arg(n)`, `codex.this()` | `codex.arg(n)` |
| read the return value | — | `codex.ret()` (or the last handler argument) |
| stop later handlers of other plugins | `return xmod.HC_BREAK` (original still runs) | same |

Exact semantics:
- `n` counts arguments **after `this`**, starting at 1 (like Ham). `codex.arg(0)` = `codex.this()`. `this` cannot be changed.
- `xmod.hook.set_arg(n, v)` (xmod's generic hook API) works for numbers, bools and entity indexes, for the first 16
  arguments. `codex.set_arg(n, v)` works for every type (strings, vectors, pointers too). Both only work in a pre-hook.
- All pre-handlers receive the **original** argument values as handler arguments (like Ham), even if an earlier
  handler changed them; `codex.arg(n)` returns the current (possibly changed) value. Changes made by `xmod.hook.set_arg`
  in several handlers: the last one wins.
- `return xmod.HC_SUPERCEDE, value`: `value` must be a number (also for bool/entity/ptr returns: entity index, 0/1)
  or, for `string`/`astring`/`string_t` returns, a string. A post-hook returning `HC_SUPERCEDE, v` replaces the return value.
- `codex.set_ret(v)` accepts any type matching `ret` (a string for string returns; not a new table for `vector`
  returns — refused, it would point to freed memory). In a pre-hook it only matters together with `HC_SUPERCEDE`
  (otherwise the original runs and produces its own value).
- `codex.ret()` errors in a pre-hook (unless `set_ret` was used).
- Handler-only functions (`arg`, `this`, `set_arg`, `ret`, `set_ret`) raise `codex: only valid inside a codex hook
  handler` elsewhere. They refer to the innermost active Codex hook.

### Behaviour details
- Handlers only run on the main thread and only if at least one Lua handler is registered for the event (otherwise
  the hook costs very little: no argument conversion, no Lua — the dispatcher calls the original right away).
- Re-entrancy: a hooked function called from inside its own handler (e.g. via `codex.callh`) runs handlers again up
  to a depth of 8, then calls the original without Lua (no infinite recursion). `codex.call` never re-enters.
- Lua errors in a handler are caught by xmod (logged, counted against the plugin like any handler error); the native
  call continues as if the handler returned nothing.
- A crash inside the *original* function while the game calls it is the game's own crash — not guarded (the guard
  protects calls *you* make).
- Performance: every hooked call with handlers converts arguments and enters Lua (microseconds). Do not hook
  functions called thousands of times per frame (e.g. `AddToFullPack`, `PM_*`, `UTIL_TraceLine`) unless needed.
- Do **not** hook the same virtual slot with both xmod Ham (`ham:*`) and a Codex vtable hook; pick one. Codex refuses
  two Codex hooks on the same slot (`that vtable slot is already hooked by 'X'`).
- ReGameDLL: many functions have hookchains (`reapi:*` events in xmod). Their `Class::Function` symbol is the wrapper
  that runs the chain; `Class::Function_OrigFunc` is the original body. Hook the wrapper to see every call (including
  ReAPI-modified args), or prefer the `reapi:*` event.

---------------------------------------------------------------------------------------------------------------------

## 10. Finding code

### Signatures
Two formats, auto-detected:
- **IDA style**: `"55 8B EC 83 EC ?? 53 56"` — hex bytes; wildcards `?`, `??`, `*`, `**`. Separators: space, comma, tab.
- **SourceMod/Orpheu `\x` style**: `"\x55\x8B\xEC\x2A\x53"` — `\x2A` is the wildcard (so a literal 0x2A byte cannot be
  expressed in this format — use IDA style for that).
- **Anchor suffix**: `"8B 45 08 E8 ?? ?? ?? ?? 85 C0 @-12"` — the match minus 12 is the function start. `sig_make`
  produces this when the function's first bytes are shared with other functions.
- A signature must contain at least one fixed byte. Only code (executable) sections are scanned.
- Scans see the code **as the game shipped it**: bytes under Codex's own patches and hooks are matched with their
  original values, so a function hooked by another plugin is still found.

| function | returns |
|---|---|
| `codex.find(module, sig [, {index = n, adjust = k}])` | address of the unique match (+ adjust/anchor), nil if none or several (`"signature matches 3 places in cs.so (make it longer or pass {index = n})"`) |
| `codex.find_all(module, sig [, max = 64])` | array of addresses (anchor applied), address order |
| `codex.sig_make(func_name_or_addr [, maxlen = 160])` | `{sig, unique, len, matches, module, offset, adjust, error}` |

`sig_make` decodes instructions from the address and wildcards everything that changes between builds or load
addresses: rel32 call/jmp targets, absolute addresses (disp/imm pointing into a module, and on Windows every dword
covered by a base relocation), PIC GOT offsets on Linux (`[ebx+disp32]`, large disp32, the `add reg, imm32` after a
PIC thunk call). It grows until unique (max `maxlen` bytes); if the start is not unique it anchors further inside the
function (`@-k`). `unique = false` + `error` when no unique signature exists (byte-identical functions, e.g. two
destructors) — then use `symbol`, `xref`, or `index`.

### Symbols — `codex.symbol(module, name)` → address | nil
- **Linux**: `dlsym`, then the full ELF symbol tables (`.dynsym` and `.symtab`, so non-exported functions work), then
  **demangled** names: `"CBasePlayer::GiveNamedItem"` (without parameters) or `"CBasePlayer::GiveNamedItem(char const*)"`
  (exact demangled text with parameters, GCC spelling: `char const*`, `int, bool`). Overloads → error
  `symbol '...' is ambiguous (2 overloads) - add the parameter list, e.g. ...`. Mangled names work too
  (`"_ZN11CBasePlayer14GiveNamedItemEPKc"`).
- **Windows**: only exported names (`GetProcAddress`). Game DLLs export almost nothing except the API entry points →
  use `sig` or `xref` on Windows.

### String references — `codex.xref(module, text [, max = 16])` → `{ {ref = addr, func = start}, ... }`
- Finds the string literal (exact, NUL-terminated) in the module, then every instruction that references it —
  absolute `push offset` / `mov reg, offset` on Windows, GOT-relative `lea reg, [ebx+off]` on Linux PIC — and the start
  of the function containing each reference (`func` = 0 if unknown).
- If `text` does not end in `\n`, `text .. "\n"` is also searched (game messages usually end in a newline).
- In `codex.func`, `xref = "text"` takes the first reference; `xref = {string = "text", index = 2}` the second.
- Very robust across updates (messages rarely change). Note: a string may be compiled out of some builds (e.g.
  ReGameDLL's Linux cs.so does not contain `"NULL Ent in GiveNamedItem!"`; its symbols work there).

### Other
- `codex.func_start(addr)` → start of the function containing addr (Linux: symbol table; else prologue heuristics).
- `codex.insn(addr [, n = 8])` → up to n (≤256) decoded instructions: `{addr, len, hex, target?, ret?, imm?, imm_size?,
  disp?, disp_abs?}`. `target` = destination of a relative call/jmp/jcc; `imm` = immediate operand; `disp` = memory
  displacement (`disp_abs = true` when it is an absolute address — a global variable).
  This is a *length* disassembler (no mnemonics): use `hex` with an opcode table, or objdump/IDA offline.

---------------------------------------------------------------------------------------------------------------------

## 11. Hard-coded constants

Game code often has limits compiled into instructions (`if (money > 16000) money = 16000;` → `cmp eax, 3E80h` /
`mov eax, 3E80h`). Codex finds and changes them safely.

**`codex.consts(func_or_addr, value [, opts])`** → array of sites, sorted by address:
`{addr = field_address, insn = instruction_address, size = 1|2|4, value = n, delta = d?, hex = "instruction bytes", disp = true?}`
- Walks the function by **recursive descent**: follows every conditional/unconditional branch inside the function
  (so code after an early `return` is seen), does not enter called functions. Limit: the symbol size (Linux) or
  8192 bytes; `opts.size` overrides.
- Matches immediates (`imm8`/`imm16` are compared sign-extended, so `cmp eax, -1` matches -1).
- `opts.near = true`: also matches `value-1` and `value+1` (`x >= 16000` is often compiled as `cmp x, 15999` + `jg`);
  such sites carry `delta = -1`/`+1`.
- `opts.disp = true`: also matches memory displacements equal to the value (structure offsets — rarely what you want).
- Searches the original bytes (works while the function is patched/hooked).

**`codex.set_const(sites | site | func, [old,] new)`** → array of patch ids | nil
- `set_const(sites, new)`: every site gets `new + site.delta` (so near-sites keep their off-by-one).
- `set_const(site, new)`: one site (a table with `addr`, `size`).
- `set_const(func, old, new)`: short form, **exact** matches of `old` only (no near).
- Refuses (nothing patched) if any new value does not fit its field: 1-byte field −128..127, 2-byte −32768..32767,
  4-byte −2147483648..4294967295 — message `value N does not fit the 1-byte field at ... - patch the instruction itself`.
- All or nothing: if one patch fails, the ones already applied are undone.
- Undo: `codex.unpatch(id)` for each id (or the library handle's `:restore()`).

Things to tell the user: a limit can exist in **several functions** (search each candidate: buy code, round end,
HUD messages, network message `WRITE_SHORT` that truncates to 16 bits...). Changing a limit in the server does not
change client-side limits (HUD display, client prediction). ReGameDLL replaced many hard-coded limits with cvars
(`mp_maxmoney`, ...) — check cvars first.

---------------------------------------------------------------------------------------------------------------------

## 12. Memory, private data, patches

### Raw memory
| function | |
|---|---|
| `codex.read(addr, type)` | one value, or nil (`memory not readable`) |
| `codex.read(addr, type, n)` | array of n values (stops at the first unreadable one; n ≤ 4096) |
| `codex.read(addr, "bytes", n)` | hex text of n bytes (default 16, ≤ 65536) |
| `codex.read(addr, "str")` | NUL-terminated string |
| `codex.write(addr, type, value)` | `true` / nil. **Data only**: writing to code is refused (`address is not writable memory (code? use codex.patch)`). |
| `codex.readable(addr [, n])`, `codex.writable(addr [, n])` | bool |

Memory types: `i8` (`char`), `u8` (`byte`, `bool`), `i16` (`short`), `u16`, `i32` (`int`), `u32` (`uint`), `i64`,
`f32` (`float`), `f64` (`double`), `ptr`, `str` (`string`), `vec` (`vector`: `{x,y,z}`, 12 bytes), `bytes` (hex text).
Unmapped/protected memory never crashes: reads return nil, writes return nil + error.

### Entities and private data
- `codex.pdata(id)` → `CBaseEntity*` (pvPrivateData) of entity `id`, or nil. `codex.ent(ptr)` → entity index or -1.
- `codex.get(ent_or_ptr, byte_offset, type [, linux_byte_offset])` → value | nil
- `codex.set(ent_or_ptr, byte_offset, type, value [, linux_byte_offset])` → true | nil
  - First argument: an entity index (→ its private data; error if none) or a raw pointer (any number > 0x10000).
  - Offsets are **bytes**. The optional last argument is the Linux offset (used only on Linux). `bytes` type not allowed.
  - AMXX `get_pdata_int(id, 115, 5)` = 4-byte units with a Linux difference of 5 units →
    `codex.get(id, 115*4, "i32", (115+5)*4)`, or with the library `cx.pdata(id, 115, "i32", 5)`.
    Common CS offsets (units, Windows; Linux +5 for players, +4 for weapons): `m_iAccount` 115, `m_iTeam` 114,
    `m_iKevlar` 112, `m_flNextAttack` 83 (float), `m_pActiveItem` 373 (ptr), weapons: `m_iClip` 51, `m_iId` 43,
    `m_flNextPrimaryAttack` 46 (float). Verify on the user's build when unsure (read and compare with known values).
- Entity variables (`pev`) are better accessed with xmod's own API (`xmod.ent.*`) — Codex is for private data.

### Patches (code or data)
- `codex.patch(addr, "hex bytes" [, "expected hex"])` → patch id | nil.
  - With `expected`: refused unless the current bytes are exactly those (protects against the wrong build:
    `patch: expected bytes [75 05] at 0805a1c3 but found [74 05] - wrong build or already patched`). **Always pass expected bytes for code patches.**
  - Refused if it overlaps another active patch (Codex hooks are patches too): `patch: overlaps patch #n (owner) at ...`.
  - Works on code (page protection handled) and data.
- `codex.unpatch(id)` → true if removed. Restores the original bytes **only if** the bytes are still what Codex wrote
  (if someone else changed them, they are left alone).
- `codex.patches()` → `{ {id, addr, orig = "hex", now = "hex", owner = "codex.patch" | "codex.set_const" | "codex hook X" | ...}, ... }`.
- All patches are removed on unload.

### Allocations
- `codex.alloc(size)` → pointer to zeroed memory (1..16 MB) owned by Codex. `codex.free(ptr)` → true (only memory from
  `codex.alloc`). Use for out-parameters (`int*`, buffers): pass the pointer as `ptr`, read with `codex.read`.
  It is never freed automatically (only at process exit) — free it.

---------------------------------------------------------------------------------------------------------------------

## 13. Vtables, engine and DLL function tables

- `codex.vtable(ent_or_ptr)` → vtable address. `codex.vfunc(ent_or_ptr, index)` → function address in slot `index`.
  `codex.vindex(ent_or_ptr, func_addr)` → slot index of that function (search up to 1024 slots) — use it to verify or
  discover indexes: `codex.vindex(id, codex.symbol("game", "CBasePlayer::TakeDamage"))` on Linux.
- CS 1.6 virtual indexes (from xmod's Ham table, same on Windows and Linux for these): `spawn` 0, `precache` 1,
  `keyvalue` 3, `objectcaps` 6, `classify` 9, `traceattack` 11, `takedamage` 12, `takehealth` 13, `killed` 14,
  `addplayeritem` 23, `think` 44, `touch` 45, `use` 46, `respawn` 48, `item_addtoplayer` 59, `item_deploy` 64,
  `player_jump` 76, `player_prethink` 78, `weapon_primaryattack` 87. (Full list: xmod `docs/HAM_FUNCTIONS.md`,
  `tools/ham_cstrike.json`; other mods have other indexes.) Most of these are already xmod `ham:*` events.
- `codex.engfunc(name)` → address of an engine function from the engine function table: `"pfnPrecacheModel"` or
  `"PrecacheModel"`, any `enginefuncs_t` member. `codex.dllfunc(name)` → address of a game DLL export from
  `DLL_FUNCTIONS` / `NEW_DLL_FUNCTIONS` (`"pfnSpawn"`, `"pfnClientConnect"`, `"pfnShouldCollide"`, ...).
  To define one: `codex.func{ name = "PrecacheModel", module = codex.module_of(a).name, address = a, args = {"astring"}, ret = "int" }`
  (the address must be inside `module`). xmod already exposes most of these as events — use Codex only for what is missing.

---------------------------------------------------------------------------------------------------------------------

## 14. Misc functions
| function | |
|---|---|
| `codex.addr(name)` | resolved address or nil |
| `codex.info(name)` | `{name, module, resolved, addr?, offset?, vindex?, how, why?, sig?, faulted, fault?, calls, hooked_calls, hooks = {"detour" | "vtable:<label>"}, proto = "int add(int, int)", pre_event, post_event}` or nil |
| `codex.list()` | array of all defined names (definition order) |
| `codex.reset(name)` | clear the *faulted* mark |
| `codex.status()` | `{version, platform = "windows"|"linux", sync_hooks, safe_calls, functions, resolved, hooks, patches}` |
| `codex.last_error()` | text of the last failure ("" if none since start; not cleared by successes — read it right after a failing call) |
| `codex.safe([bool])` | get/set the fault guard (default true) |
| `codex.cache_clear()` | empty the resolution cache (functions are re-resolved from their definitions on the next define / restart) |

---------------------------------------------------------------------------------------------------------------------

## 15. The helper library `lib/codex.lua`

`local cx = require("codex")`. Libraries in xmod are loaded once and run as their own pseudo-plugin. **Event handlers
belong to the file that calls `xmod.on`** — that is why the library never registers handlers for you: always write
`xmod.on("codex:<name>", fn)` in your plugin.

| function | description |
|---|---|
| `cx.C` | `xmod.codex` |
| `cx.available()` | true if the extension is attached (its functions are callable) |
| `cx.platform()` | `"windows"` / `"linux"` (nil before attach) |
| `cx.ready(fn)` | run `fn(codex)` now if attached (returns true), else at the next `init` (returns false). Errors in fn are caught and logged. If the extension is not installed, one line is logged and fn never runs. |
| `cx.define(defs [, opts])` | define many functions. `defs`: map `{ Name = {def}, ... }` (key = name; processed in sorted order) or array of defs with `name`. `opts.module` = default module, `opts.prefix` = name prefix. Returns `count_ok, failures` where `failures = { name = why }`. (Codex logs each failure.) |
| `cx.has(name)` | defined and resolved |
| `cx.need(name, ...)` | `true` or `false, {missing names}` |
| `cx.describe(name)` | one line: proto, OK/UNRESOLVED, module+offset, how, why, hooks |
| `cx.try(name, ...)` | `true, result` or `false, error` — never raises |
| `cx.call(name, ...)` | `result` or `nil, error` (note: a void function returns nil on success too — use `cx.try` when you need to know) |
| `cx.callh(name, ...)` | same through the hooks |
| `cx.hook(name [, opts])` | `true` or `false, error` (logs a line if the function is unresolved) |
| `cx.limit(func, old, new [, {near=, size=}])` | `codex.consts` + `codex.set_const` → handle `{ids, sites, func, old, new, active, restore()}` or `nil, error` (`no instruction of X uses N`) |
| `cx.patch(addr, bytes [, expect])` | handle `{ids, addr, active, restore()}` or `nil, error` |
| `handle:restore()` | undo the patches of the handle |
| `cx.pdata(id, index [, type = "i32", linux_diff = 5])` | AMXX-style read (offset in 4-byte units) |
| `cx.set_pdata(id, index, value [, type, linux_diff = 5])` | AMXX-style write |
| `cx.fields(spec)` | named fields: `spec = { money = { index = 115, linux_diff = 5 }, hp = { windows = 8, linux = 12, type = "f32" } }` → `F.get(id, name)`, `F.set(id, name, v)`, `F.offset(name)` |
| `cx.dump([names] [, print])` | Lua gamedata text for this platform (`Name = { module = "...", sig = { linux = "..." } }, -- proto, found by ...`), all functions by default; `print = true` writes it to the server console |
| `cx.where(addr)` | `"cs.so+1A2B0"` |
| `cx.hex(n)` | `"F7A12340"` |
| `cx.vec(x, y, z)` | `{x=, y=, z=}` |

---------------------------------------------------------------------------------------------------------------------

## 16. The console tool `codex_tools.lua`

Server console / rcon only (`xmod.cmd.server`), players cannot use it. `codex help` lists:
```
codex status                                  version, platform, sync hooks, safe calls, counts
codex modules                                 loaded modules (name, base, size, path)
codex find <module> <signature...>            matches of a signature (max 16)
codex sym <module> <name>                     symbol lookup
codex xref <module> <text...>                 functions using a string ("\n" allowed)
codex sig <func|addr|module+offset>           unique signature for an address
codex insn <func|addr|module+offset> [n]      instruction bytes / targets / immediates
codex read <addr|module+offset> <type> [n]    memory
codex consts <func|addr> <value> [near]       instructions using a constant
codex list                                    defined functions with status
codex info <func>                             everything about one function
codex patches                                 active patches
codex cache_clear
```
`module+offset` syntax: `game+1A2B0`, `engine+4F00`, `cs.so+8F3C0` (hex offset).
The demo plugin adds `codex_gamedata` (prints `cx.dump()`), `codex_give`, cvars `codex_block_c4`, `codex_maxmoney`.

---------------------------------------------------------------------------------------------------------------------

## 17. Workflows

### A new function, Linux
1. Find its name: `nm -DC cs.so | grep Class::` (or `codex sym game "CBasePlayer::RoundRespawn"` on the server).
2. Define with `symbol = "Class::Method"`, conv/this/args/ret from the SDK / ReGameDLL source.
3. Test with `codex.call` (guarded) before hooking.
4. Codex generates and caches a signature; `codex sig <name>` or `cx.dump()` prints it.

### A new function, Windows (no symbols)
1. Best: a **string** the function uses (an error/log message, a classname, a cvar name): `codex xref game "text"`.
   Use `xref = "text"` in the definition — robust across updates.
2. Otherwise a **signature**: make it on a build where you can locate the function (IDA/Ghidra with the ReGameDLL
   PDB if available, or by xref of a nearby function then `codex insn` to follow calls), then `codex sig game+<offset>`.
3. Or follow a call: find a caller by xref, locate the `E8` call in `codex insn` output, define with
   `sig = "<bytes around the call>"`, `adjust = <offset of E8 within the sig>`, `follow = "call"`.
4. Put both platforms in one definition: `symbol` (Linux) + `sig = { windows = "..." }` / `xref`.

### Cross-platform gamedata
Run `codex_gamedata` (or `cx.dump(names, true)`) on each platform and merge the lines into one table:
```lua
local GAMEDATA = {
  RoundRespawn = { conv = "thiscall", this = "entity", ret = "void",
                   symbol = "CBasePlayer::RoundRespawn",
                   sig = { windows = "<paste>", linux = "<paste>" } },
}
cx.ready(function() cx.define(GAMEDATA, { module = "game" }) end)
```
Gamedata tables can live in a library (`lib/gd_cstrike.lua` returning the table) shared by plugins:
`cx.define(require("gd_cstrike"))`.

### Porting Orpheu
| Orpheu | Codex |
|---|---|
| `configs/orpheu/functions/CBasePlayer/GiveNamedItem` JSON `{ "name", "class", "library": "mod", "arguments": [{"type": "char *"}], "identifiers": [{"os": "windows", "value": [0x55, "*", ...]}, {"os": "linux", "value": "_ZN11CBasePlayer14GiveNamedItemEPKc"}] }` | `{ module = "game", conv = "thiscall", this = "entity", args = {"astring"}, symbol = "_ZN11CBasePlayer14GiveNamedItemEPKc", sig = { windows = "55 ?? ..." } }` — Orpheu `"*"`/`"?"` bytes → `??` |
| `"library": "engine"` / `"mod"` | `module = "engine"` / `"game"` |
| class member (`"class"` present) | `conv = "thiscall", this = "entity"` (or `"ptr"`) |
| `OrpheuGetFunction("GiveNamedItem", "CBasePlayer")` | `codex.func{ name = "GiveNamedItem", ... }` |
| `OrpheuCall(f, id, "weapon_ak47")` | `codex.call("GiveNamedItem", id, "weapon_ak47")` |
| `OrpheuRegisterHook(f, "Handler", OrpheuHookPre)` | `codex.hook("GiveNamedItem")` + `xmod.on("codex:GiveNamedItem", fn)` |
| `OrpheuHookPost` | `xmod.on("codex:GiveNamedItem:post", fn)` |
| `return OrpheuSupercede` / `OrpheuSetReturn(v)` | `return xmod.HC_SUPERCEDE, v` |
| `OrpheuSetParam(2, v)` | `codex.set_arg(1, v)` (Codex counts after `this`; Orpheu counted `this` as 1) |
| `OrpheuGetFunctionFromObject(id, "TakeDamage", "CBaseEntity")` (virtual) | `vindex = 12` + `codex.hook(name, {entity = id})` |
| `OrpheuMemorySet("name", 1, value)` / memory files | `codex.patch(addr, "hex", "expected hex")`, `codex.write`, `codex.set_const` |
| `OrpheuGetEngineFunction("pfnX", "X")` | `codex.engfunc("pfnX")` + `address =` |
| Orpheu types `CBaseEntity *`, `edict_s *`, `entvars_s *`, `char *`, `Vector *`, `float`, `bool`, `int` | `entity`, `edict`, `entvars`, `astring`/`string`, `vector`, `float`, `bool`, `int` |

### Porting AMXX
| AMXX | xmod + Codex |
|---|---|
| `get_pdata_int(id, 115, 5)` / `set_pdata_int` | `cx.pdata(id, 115)` / `cx.set_pdata(id, 115, v)` |
| `get_pdata_float(id, 83, 5)` | `cx.pdata(id, 83, "f32")` |
| `get_pdata_cbase(id, 373, 5)` | `codex.ent(cx.pdata(id, 373, "ptr"))` → entity index |
| `ExecuteHam(Ham_TakeDamage, ...)` | xmod Ham API, or Codex `vindex = 12` + `codex.call` |
| fakemeta `engfunc(EngFunc_X)` | xmod API first; Codex `engfunc` + `call` otherwise |
| Okapi / Orpheu memory patches | `codex.patch` / `codex.set_const` |

---------------------------------------------------------------------------------------------------------------------

## 18. Recipes

All recipes assume `local cx = require("codex")` and `local C = xmod.codex`, with definitions inside `cx.ready`.

### 18.1 Give an item (call a member function) — both platforms
```lua
cx.ready(function()
  cx.define({ GiveNamedItem = { module = "game", conv = "thiscall", this = "entity", args = { "astring" }, ret = "void",
                                symbol = "CBasePlayer::GiveNamedItem", xref = "NULL Ent in GiveNamedItem!" } })
end)
local function give(id, item)
  if not xmod.player.alive(id) then return false end
  local ok, err = cx.try("GiveNamedItem", id, item)
  if not ok then xmod.log("give failed: " .. err) end
  return ok
end
```
(ReGameDLL's GiveNamedItem returns `CBaseEntity*` — use `ret = "entity"` to get the item's index there; `void` is
safe on every build because the return register is simply ignored.)

### 18.2 Hook it: block or log items
```lua
cx.ready(function() cx.hook("GiveNamedItem") end)
xmod.on("codex:GiveNamedItem", function(id, item)
  if item == "weapon_awp" and xmod.player.team(id) == 1 then return xmod.HC_SUPERCEDE end   -- no AWP for T
end)
xmod.on("codex:GiveNamedItem:post", function(id, item) xmod.log(("%s got %s"):format(xmod.player.name(id), item)) end)
```

### 18.3 Virtual hook: double the damage players take from headshots... (TakeDamage, vindex 12)
```lua
cx.ready(function()
  cx.define({ Player_TakeDamage = { conv = "thiscall", this = "entity", vindex = 12,
                                    args = { "entvars", "entvars", "float", "int" }, ret = "int" } })
  cx.hook("Player_TakeDamage", { class = "player" })
end)
xmod.on("codex:Player_TakeDamage", function(victim, inflictor, attacker, damage, bits)
  if attacker >= 1 and attacker <= xmod.maxplayers() and attacker ~= victim then
    xmod.hook.set_arg(3, damage * 2)            -- arg 3 = damage (counting after 'this')
  end
end)
```
(xmod's `ham:takedamage:player` event does the same without Codex — prefer it; this shows the mechanism, which also
works for virtual functions Ham does not list.)

### 18.4 Raise a hard-coded limit, reversible, controlled by a cvar
```lua
xmod.cvar.create("my_maxmoney", "0")
local h
local function apply()
  if h then h:restore(); h = nil end
  local max = xmod.cvar.int("my_maxmoney")
  if max <= 0 or not cx.has("AddAccount") then return end
  local err
  h, err = cx.limit("AddAccount", 16000, max, { near = true })
  if not h then xmod.log("limit unchanged: " .. err) end
end
cx.ready(function()
  cx.define({ AddAccount = { conv = "thiscall", this = "entity",
                             symbol = { "CBasePlayer::AddAccount_OrigFunc", "CBasePlayer::AddAccount" },
                             sig = { windows = "<signature for the user's mp.dll>" } } }, { module = "game" })
  apply()
  xmod.cvar.watch("my_maxmoney", function() apply() end)
end)
```
(No `args` needed: the function is only used as an address. Do not *call* or *hook* AddAccount without the exact
signature of that build: vanilla CS `(int amount, bool bTrackChange)`, ReGameDLL `(int amount, RewardType type, bool bTrackChange)`.)

### 18.5 Read and write private data
```lua
local F = cx.fields{
  money  = { index = 115, linux_diff = 5 },                     -- m_iAccount
  armor_type = { index = 112, linux_diff = 5 },                 -- m_iKevlar
  next_attack = { index = 83, linux_diff = 5, type = "f32" },   -- m_flNextAttack
}
local m = F.get(id, "money")
F.set(id, "next_attack", 0.0)
```
(Changing money this way does not update the HUD; xmod's `xmod.player.set_money` does both.)

### 18.6 Patch code with a safety check
```lua
-- NOP a conditional jump at a known place, only if the bytes are what we expect
local base = cx.has("SomeFunc") and C.addr("SomeFunc")
local h, err = cx.patch(base + 0x2C, "90 90", "74 0E")          -- je +0E  ->  nop nop
if not h then xmod.log("not patched: " .. err) end
```

### 18.7 A function that returns a float / takes a vector
```lua
C.func{ name = "VecLength", module = "game", symbol = "UTIL_VecLength", args = { "vector" }, ret = "float" }   -- hypothetical
local len = C.call("VecLength", { x = 3, y = 4, z = 0 })        -- 5.0
```

### 18.8 Out-parameters
```lua
local buf = C.alloc(4)
C.call("GetSomething", id, buf)                                 -- args = { "ptr" } where the function writes an int
local value = C.read(buf, "i32")
C.free(buf)
```

### 18.9 Discover the vtable index of a virtual function (Linux has symbols)
```lua
local a = C.symbol("game", "CBasePlayer::Jump")
print(C.vindex(1, a))           -- slot of CBasePlayer::Jump in the player's vtable (player 1 must exist)
```

### 18.10 Find a global variable through a function that uses it
```lua
for _, i in ipairs(C.insn(C.addr("SomeFunc"), 64)) do
  if i.disp_abs then xmod.log(("global at %08X used by %s"):format(i.disp, cx.where(i.addr))) end
end
```
(On Windows absolute addresses are common (`mov eax, [g_pGameRules]`); on Linux PIC code accesses globals through
`ebx`-relative GOT offsets, so `disp_abs` is rarer there — prefer symbols (`C.symbol("game", "g_pGameRules")` returns the
address of the variable) on Linux.)

### 18.11 Game rules (a non-entity object)
```lua
-- Linux: g_pGameRules is a global CGameRules*; read the pointer, call a member with this = "ptr"
local gr = C.read(C.symbol("game", "g_pGameRules"), "ptr")
C.func{ name = "GR_CheckWinConditions", module = "game", conv = "thiscall", this = "ptr",
        symbol = "CHalfLifeMultiplay::CheckWinConditions", ret = "void" }
C.call("GR_CheckWinConditions", gr)
```

### 18.12 Diagnose why something is not found
```lua
xmod.log(cx.describe("GiveNamedItem"))          -- ... UNRESOLVED ... (symbol '...' not found in mp.dll; no function references "..."; )
```
Console: `codex info GiveNamedItem`, `codex list`.

---------------------------------------------------------------------------------------------------------------------

## 19. Rules for writing Codex code (checklist)

1. Call Codex only inside `cx.ready(...)` (or after `init`); register `xmod.on("codex:...")` handlers in the plugin.
2. One definition for both platforms: `symbol` for Linux, `xref` and/or `sig = { windows = ... }` for Windows. Never
   invent signatures or offsets — if you do not know them, give the user the workflow (section 17) to get them, or use
   `xref` with a string from the source code you know is in that function.
3. `conv = "thiscall"` for every C++ member function; `this = "entity"` for entities, `"ptr"` for other objects.
4. Argument list must match the real C++ signature of **that build** exactly (count and sizes) — especially on Windows
   (`thiscall`/`stdcall` pop the stack). If unsure, do not hook; use the function only as an address.
5. `astring` for any string the game may keep (classnames, models, targetnames, precache); `string` otherwise.
6. Check results: `if not cx.has(name) then ... end`; use `cx.try` / `pcall` around `codex.call`.
7. Test a new definition with `codex.call` (guarded) before hooking it.
8. Always pass expected bytes to `codex.patch` on code. Prefer `set_const` over hand-written byte patches for constants.
9. Keep handlers light; do not hook per-frame-per-entity hot paths without need.
10. Prefer existing xmod events (`reapi:*`, `ham:*`, engine/game events) when they do the job.
11. Use unique function names (shared across plugins). Do not define the same name with different prototypes.
12. Tell the user what to add to metamod `plugins.ini`, `extensions.ini` and xmod `plugins.ini`, and what is untested
    (signatures for their build, Windows behaviour you could not verify).
13. Never write code whose purpose is cheating against other players' clients or attacking servers you do not own;
    Codex is for the user's own server.

---------------------------------------------------------------------------------------------------------------------

## 20. Error messages and what they mean

| message (start) | meaning / fix |
|---|---|
| `xmod.codex.X is not available` | called before the extension attached (load time) or the extension is not loaded → `cx.ready`, check metamod `plugins.ini` and `extensions.ini` |
| `module 'game' is not loaded` | wrong module name, or called before the game DLL is known |
| `codex: cannot resolve 'X' in M: ...` | every method failed; the list after `:` says why each failed |
| `symbol 'S' not found in M` / `... is not exported by M (Windows DLLs only have exports - use a signature)` | wrong name / Windows |
| `symbol 'S' is ambiguous (N overloads) - add the parameter list, e.g. ...` | use the demangled name with parameters |
| `signature not found` | different build / wrong bytes → make a new one (`codex sig`), use xref or symbol |
| `signature matches N places (make it longer or set index)` | not unique |
| `no function references "text"` | string not in this build (or different text) |
| `resolved address is not executable code of M` | wrong module for an address/offset, bad adjust |
| `follow: no call/jmp at the match` | `follow` used on a match that is not a call/jmp |
| `'X' is not resolved: ...` | call/hook of an unresolved function |
| `'X' expects N arguments (including this), got M` | argument count |
| `'X' argument #k: expected a number for int` (string, vector ...) | wrong Lua type |
| `entity N has no private data (not a valid entity)` | dead/free entity passed as `entity` |
| `'X': 'this' is NULL / not a valid entity` | invalid `this` |
| `'X' crashed: ... - the call was aborted, the server keeps running` | the definition is wrong (conv, args, this, address) |
| `'X' crashed earlier (...) - fix the definition and call codex.reset("X")` | faulted state |
| `virtual call: vtable slot N is not code (wrong index?)` | wrong vindex |
| `'X' is virtual: hook it with {class = ...}, {entity = id} or {object = ptr}` | vtable hook needs an object |
| `that vtable slot is already hooked by 'Y'` | two Codex hooks on one slot |
| `cannot detour 'X': ...` | unsafe prologue (section 9) |
| `codex.func: 'X' is hooked - unhook it before redefining` | `replace` on a hooked function |
| `patch: expected bytes [..] at A but found [..] - wrong build or already patched` | patch expectation failed |
| `patch: overlaps patch #n (owner) at A` | another patch/hook covers these bytes (also: re-applying a patch after `xmod reload`) |
| `value N does not fit the K-byte field at A ...` | `set_const` value too large for the instruction |
| `no constant site to change` / `no instruction of X uses N` | the constant is not in that function (ReGameDLL cvar?) |
| `address is not writable memory (code? use codex.patch)` | `codex.write` to code |
| `codex: only valid inside a codex hook handler` | `arg/this/set_arg/ret/set_ret` outside a handler |
| `set_arg only works in a pre-hook (the original already ran)` | in post |
| `the return value is only known in a post-hook (or after codex.set_ret)` | `codex.ret()` in pre |
| `cannot return a new Vector* from a hook ...` | set_ret with a table for a `vector` return |
| `hook 'X' is observe-only: xmod < 0.4.1 ...` | update xmod |

---------------------------------------------------------------------------------------------------------------------

## 21. Limits and things that are not supported

- 32-bit x86 only. Max 64 argument words per function. `xmod.hook.set_arg` covers the first 16 arguments (use
  `codex.set_arg` beyond).
- Return by value of structs/classes (hidden return pointer), `vecval` as return type, variadic *hooks* (calling a
  variadic cdecl function with a fixed argument list works), C++ member-function pointers, exceptions thrown across Lua.
- `fastcall` puts only ≤32-bit non-float arguments in registers (MSVC/GCC behaviour for ints/pointers).
- Hooks run Lua only on the main thread; calls from other threads run the original without handlers.
- Detours need ≥5 movable bytes at the function start; vtable hooks affect a whole class.
- Windows symbols: exports only. Linux: full symbol tables (stripped binaries → signatures/xref).
- The guard protects calls made through Codex, not code the game runs by itself, and cannot undo side effects.
- Patches and hooks made by a previous Lua state survive `xmod reload` (they belong to the C++ side) — section 2.

---------------------------------------------------------------------------------------------------------------------

## 22. Internals (for debugging)

- Length disassembler (x86-32 incl. SSE/AVX VEX/EVEX prefixes) validated against objdump on ~1.3 million instructions.
- Calls: a generic asm thunk pushes the argument words, sets ECX/EDX, aligns the stack to 16, calls, saves EAX/EDX/ST(0).
- Detours: `jmp stub` at the function; stub = `mov eax, Hook*; jmp codex_hook_entry`; the entry builds a frame, calls the
  dispatcher (`fire_hook_sync` into xmod's event system), calls the trampoline (relocated original bytes + jmp back),
  pops the right number of bytes for the convention, returns EAX/EDX/ST(0).
- Fault guard: Windows SEH frame (handler redirects EIP to a recovery stub, FPU env restored); Linux sigaction with
  `SA_SIGINFO | SA_NODEFER | SA_ONSTACK`, `sigsetjmp`/`siglongjmp`, previous handlers chained.
- Cache: `data/codex/cache.ini`, lines `module|build|name|0xoffset|auto signature`, saved atomically (tmp + rename) on the
  next frame after a change and at unload. Build = FNV-1a of the first 64 KB of the first code section.
- Verified on real binaries: ReGameDLL cs.so/mp.dll, ReHLDS engine_i486.so/swds.dll — ~11 000 functions, 0 wrong
  trampolines, unsafe prologues refused; auto signatures unique ~95% (the rest are byte-identical functions).

---------------------------------------------------------------------------------------------------------------------

## 23. Function index

`xmod.codex`: `module` `modules` `module_of` · `find` `find_all` `symbol` `xref` `func_start` `sig_make` `insn` ·
`consts` `set_const` · `read` `write` `readable` `writable` · `patch` `unpatch` `patches` · `alloc` `free` ·
`pdata` `ent` `get` `set` · `vtable` `vfunc` `vindex` · `engfunc` `dllfunc` · `func` `call` `callh` `addr` `info`
`list` `reset` · `hook` `unhook` · `arg` `this` `set_arg` `ret` `set_ret` · `last_error` `safe` `cache_clear` `status`

`require("codex")`: `C` `available` `platform` `ready` `define` `has` `need` `describe` `try` `call` `callh` `hook`
`limit` `patch` `pdata` `set_pdata` `fields` `dump` `where` `hex` `vec`

Events: `codex:<name>` (pre: this?, args...) · `codex:<name>:post` (this?, args..., ret?)
Return codes: `xmod.HC_CONTINUE` (0), `xmod.HC_SUPERCEDE` (1), `xmod.HC_BREAK` (2).
