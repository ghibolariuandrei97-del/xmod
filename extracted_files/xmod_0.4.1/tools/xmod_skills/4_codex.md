
# Part IV — Codex

## X1. Codex — call, hook and patch any game function (`xmod.codex`, extension `xmod_codex`)
Full manual: `docs/CODEX.md`; complete AI reference: `docs/codex_skills.md`; helper library: `local cx = require("codex")` (`cx.ready`, `cx.define`, `cx.try`, `cx.limit`, `cx.pdata`, `cx.dump`). Needs xmod 0.4.1 for hooks that can change the call. Use `xmod.codex.*` in `init` or later
(extensions attach after scripts load); register `xmod.on("codex:...")` handlers anywhere.
```lua
local C = xmod.codex
xmod.on("init", function()
  C.func{ name = "GiveNamedItem", module = "game", conv = "thiscall", this = "entity", args = {"astring"}, ret = "void",
          symbol = "CBasePlayer::GiveNamedItem", xref = "NULL Ent in GiveNamedItem!" }   -- Linux symbol / Windows string ref
  C.hook("GiveNamedItem")
end)
xmod.on("codex:GiveNamedItem", function(id, item) if item == "weapon_c4" then return xmod.HC_SUPERCEDE end end)
```
- `codex.func{name, module="game"|"engine"|file, conv="cdecl"|"stdcall"|"thiscall"|"fastcall", this="entity"|"ptr", args={...}, ret=...,
  symbol=, sig=, xref=, address=, offset=, vindex=, adjust=, follow="call"|"jmp", index=, replace=}` → true / nil. Any field can be
  `{windows=..., linux=...}`. Order: per-build cache → address/offset → symbol → sig → learned sigs → xref. A missing function
  stays unresolved (never a crash); `codex.info(name).why` says why. Signatures accept `"55 8B EC ??"` and `"\x55\x8B\x2A"`.
- Types: `void bool i8 u8 i16 u16 int uint i64 float double ptr string astring string_t entity edict entvars vector vecval` (entities = indexes). Use `astring` (engine string pool) for strings the game keeps (classnames, models): a plain `string` is only valid during the call.
- `codex.call(name, ...)` (bypasses hooks) / `codex.callh` (through hooks). A crash inside the call is caught: Lua error + log,
  function marked faulted until `codex.reset(name)`. Wrap in `pcall` to handle failures.
- `codex.hook(name [, {class=|entity=|object=}])` (detour, or vtable hook with `vindex`), `codex.unhook(name)`. Events
  `codex:<name>` (args) and `codex:<name>:post` (args..., ret). Change args with `xmod.hook.set_arg(n, v)` / `codex.set_arg(n, v)`
  (n counts after `this`), return `xmod.HC_SUPERCEDE, value` to skip the original / replace the return, or `codex.set_ret(v)`.
- Find: `codex.find(mod, sig [, {index, adjust}])`, `find_all`, `symbol(mod, "Class::Method")`, `xref(mod, text)` → `{{ref, func}}`,
  `func_start(addr)`, `sig_make(func|addr)` → `{sig, unique, ...}`, `insn(addr, n)`, `module(name)`, `modules()`, `module_of(addr)`,
  `engfunc("pfnX")`, `dllfunc("pfnX")`, `vtable/vfunc/vindex`.
- Limits: `codex.consts(func, value [, {near=true}])` → every instruction using the constant; `codex.set_const(sites|site, new)` or
  `codex.set_const(func, old, new)` → patch ids (refuses values that do not fit; all or nothing); `codex.unpatch(id)`.
- Memory: `read(addr, type [, n])`, `write(addr, type, v)`, `readable/writable`, `get/set(ent|ptr, offset, type [, v] [, linux_offset])`,
  `pdata(id)`, `ent(ptr)`, `patch(addr, "hex" [, "expected hex"])`, `unpatch`, `patches()`, `alloc/free`. Unmapped memory → nil, not a crash.
- Misc: `info(name)`, `list()`, `addr(name)`, `status()`, `last_error()`, `cache_clear()`, `safe(bool)`.
- Console `codex help` (plugin `codex_tools.lua`, server console only). Demo: `plugins/examples/codex_demo.lua`.
- Tell users honestly: Codex protects against missing/ambiguous functions, crashing calls, bad memory access and bad
  patches; it cannot make a wrong-but-working definition correct, and values written into game memory can still break the
  game later. Test a definition with `codex.call` before hooking it.
