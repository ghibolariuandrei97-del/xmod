-- ============================================================================================================
--  codex_tools.lua  -  console tools for the Codex extension (xmod.codex)
--
--  Server console / rcon only (players cannot use it).   codex help
--  Use it to explore a game build: find functions, make signatures, look at code, find hard-coded limits.
-- ============================================================================================================
xmod.register("Codex Tools", "1.0", "xmod")
local C = xmod.codex

local function out(fmt, ...) xmod.server_print((select("#", ...) > 0 and fmt:format(...) or fmt)) end
local function hex(n) return n and ("%08X"):format(n) or "nil" end
local function num(s) if not s then return nil end return tonumber(s) or tonumber((s:gsub("^0[xX]", "")), 16) end
-- a codex function name, a "module+offset" or a raw address
local function where(s)
  if not s then return nil end
  local mod, off = s:match("^([^+]+)%+(%x+)$")
  if mod then local m = C.module(mod); return m and (m.base + tonumber(off, 16)) end
  local n = num(s)
  if n and n > 0x10000 then return n end
  return C.addr(s)
end
local function modoff(a)
  local m = a and C.module_of(a)
  return m and ("%s+%X"):format(m.name, a - m.base) or hex(a)
end

local H = {}
local HELP = {
  { "status", "version, platform, counts" },
  { "modules", "loaded modules (name, base, size)" },
  { "find <module> <sig...>", "scan a signature (IDA \"55 8B EC ??\" or \\x55\\x8B\\x2A style)" },
  { "sym <module> <name>", "look up a symbol (Linux: also \"Class::Method\")" },
  { "xref <module> <text...>", "functions that use a string" },
  { "sig <func|addr|mod+off>", "generate a unique signature for that address" },
  { "insn <func|addr|mod+off> [n]", "disassemble n instruction lengths/bytes (default 12)" },
  { "read <addr|mod+off> <type> [n]", "read memory: i8 u8 i16 u16 i32 u32 f32 f64 ptr str vec bytes" },
  { "consts <func|addr> <value> [near]", "every instruction of the function that uses a constant (hard-coded limits)" },
  { "list", "functions defined by plugins (codex.func)" },
  { "info <func>", "how a function was resolved, hooks, calls, faults" },
  { "patches", "active code patches" },
  { "cache_clear", "forget cached addresses (they are re-resolved on the next map)" },
}

function H.help() out("Codex %s - commands:", C.status().version); for _, h in ipairs(HELP) do out("  codex %-34s %s", h[1], h[2]) end end

function H.status()
  local s = C.status()
  out("Codex %s on %s | hooks run Lua inside the call: %s | safe calls: %s", s.version, s.platform, tostring(s.sync_hooks), tostring(s.safe_calls))
  out("functions %d (resolved %d), hooks %d, patches %d", s.functions, s.resolved, s.hooks, s.patches)
end

function H.modules()
  for _, m in ipairs(C.modules()) do out("  %-28s %s  %8.1f KB  %s", m.name, hex(m.base), m.size / 1024, m.path or "") end
end

function H.find(a)
  local mod = a[2]; local sig = a.text:match("^%S+%s+%S+%s+(.+)$")
  if not (mod and sig) then return out("usage: codex find <module> <signature>") end
  local all = C.find_all(mod, sig, 16)
  if not all or #all == 0 then return out("not found (%s)", C.last_error()) end
  out("%d match(es)%s:", #all, #all >= 16 and " (first 16)" or "")
  for _, p in ipairs(all) do out("  %s  %s", hex(p), modoff(p)) end
end

function H.sym(a)
  if not a[3] then return out("usage: codex sym <module> <name>") end
  local p = C.symbol(a[2], a[3])
  out(p and ("%s = %s  (%s)"):format(a[3], hex(p), modoff(p)) or ("not found: " .. C.last_error()))
end

function H.xref(a)
  local mod = a[2]; local text = a.text:match("^%S+%s+%S+%s+(.+)$")
  if not (mod and text) then return out("usage: codex xref <module> <text>") end
  text = text:gsub("\\n", "\n")
  local r = C.xref(mod, text, 16)
  if not r or #r == 0 then return out("nothing references \"%s\" (%s)", text, C.last_error()) end
  for _, x in ipairs(r) do out("  used at %s  in function %s", modoff(x.ref), x.func and x.func ~= 0 and modoff(x.func) or "?") end
end

function H.sig(a)
  local p = where(a[2]); if not p then return out("usage: codex sig <func|addr|mod+off>") end
  local s = C.sig_make(p)
  if not s then return out("failed: %s", C.last_error()) end
  out("%s  (%s, %d bytes, %s)", s.sig, modoff(p), s.len or 0, s.unique and "unique" or ("NOT unique: " .. tostring(s.error)))
  if s.unique then out("use it as:  sig = \"%s\"", s.sig) end
end

function H.insn(a)
  local p = where(a[2]); if not p then return out("usage: codex insn <func|addr|mod+off> [n]") end
  for _, i in ipairs(C.insn(p, tonumber(a[3]) or 12) or {}) do
    local extra = i.target and (" -> " .. modoff(i.target)) or ""
    if i.imm then extra = extra .. (" imm=%d"):format(i.imm) end
    out("  %s  %-30s%s%s", modoff(i.addr), i.hex, extra, i.ret and "  (ret)" or "")
  end
end

function H.read(a)
  local p = where(a[2]); local t = a[3] or "i32"; local n = tonumber(a[4]) or 1
  if not p then return out("usage: codex read <addr|mod+off> <type> [n]") end
  if not C.readable(p) then return out("%s is not readable", hex(p)) end
  if t == "bytes" or t == "str" then return out("%s: %s", modoff(p), tostring(C.read(p, t, n > 1 and n or 64))) end
  local size = ({ i8 = 1, u8 = 1, i16 = 2, u16 = 2, f64 = 8, vec = 12 })[t] or 4
  for k = 0, math.min(n, 64) - 1 do
    local v = C.read(p + k * size, t)
    if type(v) == "table" then v = ("%.3f %.3f %.3f"):format(v[1] or v.x or 0, v[2] or v.y or 0, v[3] or v.z or 0) end
    out("  %s  %s", modoff(p + k * size), tostring(v))
  end
end

function H.consts(a)
  local p = where(a[2]); local v = num(a[3])
  if not (p and v) then return out("usage: codex consts <func|addr> <value> [near]") end
  local r = C.consts(p, v, { near = a[4] == "near" })
  if not r or #r == 0 then return out("no instruction uses %d there (%s)", v, C.last_error()) end
  for _, s in ipairs(r) do out("  field %s (%d bytes)  value %d  insn %s  %s", modoff(s.addr), s.size, s.value, modoff(s.insn), s.hex) end
  out("change with:  codex.set_const(codex.consts(<func>, %d%s), new_value)", v, a[4] == "near" and ", {near = true}" or "")
end

function H.list()
  local l = C.list()
  if #l == 0 then return out("no functions defined yet (plugins define them with codex.func{...})") end
  for _, n in ipairs(l) do
    local i = C.info(n)
    out("  %-24s %-10s %-22s %s%s", n, i.resolved and "OK" or "UNRESOLVED", i.addr and modoff(i.addr) or "-", i.how or "",
        i.faulted and "  FAULTED" or (#(i.hooks or {}) > 0 and ("  hooks: " .. table.concat(i.hooks, ",")) or ""))
  end
end

function H.info(a)
  local i = a[2] and C.info(a[2]); if not i then return out("usage: codex info <func> (%s)", C.last_error()) end
  out("%s  [%s]", i.proto, i.resolved and "resolved" or "UNRESOLVED")
  if i.addr then out("  address %s (%s)", hex(i.addr), modoff(i.addr)) end
  out("  how: %s%s", i.how or "-", i.why and ("  |  " .. i.why) or "")
  if i.sig then out("  signature: %s", i.sig) end
  out("  calls %d, hooked calls %d, hooks: %s", i.calls, i.hooked_calls, #i.hooks > 0 and table.concat(i.hooks, ", ") or "none")
  out("  events: %s  /  %s", i.pre_event, i.post_event)
  if i.faulted then out("  FAULTED: %s  (codex.reset(\"%s\") after fixing the definition)", i.fault, i.name) end
end

function H.patches()
  local p = C.patches()
  if #p == 0 then return out("no patches") end
  for _, x in ipairs(p) do out("  #%d %s  %s -> %s  (%s)", x.id, modoff(x.addr), x.orig, x.now, x.owner) end
end

function H.cache_clear() C.cache_clear(); out("cache cleared") end

xmod.cmd.server("codex", function(args)
  local sub = (args[1] or "help"):lower()
  local f = H[sub]
  if not f then return out("unknown subcommand '%s' - codex help", sub) end
  local ok, err = pcall(f, args)
  if not ok then out("codex %s: %s", sub, tostring(err)) end
end, "Codex tools (codex help)")
