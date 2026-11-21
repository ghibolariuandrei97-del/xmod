-- ============================================================================================================
--  lib/codex.lua  -  helper library for the Codex extension (xmod.codex)          local cx = require("codex")
--
--  Codex itself is xmod.codex.* (C++). This library adds the things every plugin would otherwise write by hand:
--    cx.ready(fn)                     run fn once Codex is attached (scripts load BEFORE extensions attach)
--    cx.define(defs [, opts])         define many functions at once (a "gamedata" table), with a summary of failures
--    cx.try / cx.call                 calls that return nil, err instead of raising
--    cx.hook                          hook with a clear log line on failure (handlers stay in YOUR plugin: xmod.on)
--    cx.limit(...) / cx.patch(...)    hard-coded constants and byte patches as handles you can :restore()
--    cx.pdata / cx.set_pdata          AMXX-style private data offsets (get_pdata_int(id, 115, 5))
--    cx.fields{...}                   named, typed private data fields for both platforms
--    cx.dump(names)                   print gamedata (signatures of THIS build) to paste into a plugin
--    cx.where(addr), cx.hex(n), cx.vec(x, y, z), cx.platform(), cx.available(), cx.describe(name)
--
--  Why handlers are not registered here: xmod gives an event handler to the file that calls xmod.on. A handler
--  registered by this library would belong to the library, not to your plugin (wrong error attribution, and it
--  would stay alive if your plugin is disabled). So always write   xmod.on("codex:<name>", fn)   in your plugin.
-- ============================================================================================================
local M = {}
local C = xmod.codex
M.C = C
M.VERSION = "1.0"

local function log(fmt, ...) xmod.log("codex.lua: " .. (select("#", ...) > 0 and fmt:format(...) or fmt)) end
local function err(fmt, ...) xmod.error("codex.lua: " .. (select("#", ...) > 0 and fmt:format(...) or fmt)) end

-- ------------------------------------------------------------------------------------------------- state
--- true when the xmod_codex extension is loaded and attached (its functions can be called)
function M.available()
  return (pcall(function() return C.status() end))
end

local plat_cache
--- "windows" or "linux" (nil if Codex is not attached yet)
function M.platform()
  if not plat_cache and M.available() then plat_cache = C.status().platform end
  return plat_cache
end

-- ------------------------------------------------------------------------------------------------- ready
local pending, hooked_init = {}, false
local function run_pending()
  local list = pending
  pending = {}
  for _, fn in ipairs(list) do
    local ok, e = pcall(fn, C)
    if not ok then err("a codex.ready() callback failed: %s", tostring(e)) end
  end
end
--- Run fn(codex) as soon as Codex is attached: right now if it already is (e.g. after "xmod reload"),
--- otherwise at the next "init" (map start). Call it at the top level of your plugin. Callbacks run in order.
--- If the extension is not installed at all, fn never runs and one line is logged.
function M.ready(fn)
  assert(type(fn) == "function", "codex.ready(fn): function expected")
  if M.available() then
    local ok, e = pcall(fn, C)
    if not ok then err("a codex.ready() callback failed: %s", tostring(e)) end
    return true
  end
  pending[#pending + 1] = fn
  if not hooked_init then
    hooked_init = true
    xmod.on("init", function()
      if #pending == 0 then return end
      if M.available() then run_pending()
      else log("the xmod_codex extension is not loaded (metamod plugins.ini + configs/extensions.ini) - %d plugin callback(s) skipped", #pending); pending = {} end
    end)
  end
  return false
end

-- ------------------------------------------------------------------------------------------------- definitions
--- Define many functions. defs is either a map { Name = {def...}, ... } (the key becomes the name) or an array of defs.
--- opts: { module = "game" (default module for entries without one), prefix = "" (name prefix) }
--- Returns: number defined, table of failures { name = why }. (Codex itself logs every failure, with the reason.)
function M.define(defs, opts)
  opts = opts or {}
  local okn, failed = 0, {}
  local function one(name, d)
    local def = {}
    for k, v in pairs(d) do def[k] = v end
    def.name = (opts.prefix or "") .. (def.name or name)
    if def.module == nil and opts.module then def.module = opts.module end
    if C.func(def) then okn = okn + 1
    else
      local i = C.info(def.name)
      failed[def.name] = (i and i.why) or C.last_error()
    end
  end
  if defs[1] ~= nil then for _, d in ipairs(defs) do one(d.name, d) end
  else
    local names = {}
    for k in pairs(defs) do names[#names + 1] = k end
    table.sort(names)
    for _, k in ipairs(names) do one(k, defs[k]) end
  end
  return okn, failed
end

--- true if the function is defined and resolved
function M.has(name)
  local i = M.available() and C.info(name)
  return i and i.resolved or false
end

--- true if every named function is resolved, otherwise false + list of the missing ones
function M.need(...)
  local missing = {}
  for _, n in ipairs({ ... }) do if not M.has(n) then missing[#missing + 1] = n end end
  if #missing == 0 then return true end
  return false, missing
end

--- one line: "int add(int, int)  OK  cs.so+1A2B0  symbol cx_add  hooks: detour"
function M.describe(name)
  local i = C.info(name)
  if not i then return name .. ": not defined" end
  return ("%s  %s  %s  %s%s%s"):format(i.proto, i.resolved and "OK" or "UNRESOLVED", i.addr and M.where(i.addr) or "-",
    i.how or "", i.why and ("  (" .. i.why .. ")") or "", #i.hooks > 0 and ("  hooks: " .. table.concat(i.hooks, ",")) or "")
end

-- ------------------------------------------------------------------------------------------------- calls / hooks
--- ok, result  |  false, error     (never raises; a crash inside the function is caught by Codex too)
function M.try(name, ...)
  local r = table.pack(pcall(C.call, name, ...))
  if r[1] then return true, r[2] end
  return false, tostring(r[2])
end
--- result  |  nil, error
function M.call(name, ...)
  local ok, r = M.try(name, ...)
  if ok then return r end
  return nil, r
end
--- the same through the hooks (as if the game called it)
function M.callh(name, ...)
  local ok, r = pcall(C.callh, name, ...)
  if ok then return r end
  return nil, tostring(r)
end

--- codex.hook with a log line on failure. Returns true | false, error.  Register the handler yourself:
---   xmod.on("codex:<name>", function(this, ...) end)   /   xmod.on("codex:<name>:post", function(..., ret) end)
function M.hook(name, opts)
  if not M.has(name) then
    local i = C.info(name)
    local why = i and (i.why or "unresolved") or "not defined"
    log("cannot hook '%s': %s", name, why)
    return false, why
  end
  if C.hook(name, opts) then return true end
  return false, C.last_error()                       -- Codex already logged why
end

-- ------------------------------------------------------------------------------------------------- patches as handles
local Handle = {}
Handle.__index = Handle
function Handle:restore()
  for i = #self.ids, 1, -1 do C.unpatch(self.ids[i]) end
  self.ids = {}
  self.active = false
  return true
end
local function handle(ids, extra)
  local h = setmetatable({ ids = ids, active = true }, Handle)
  for k, v in pairs(extra or {}) do h[k] = v end
  return h
end

--- Change a constant baked into a function (a hard-coded limit) everywhere it is used.
---   local h = cx.limit("AddAccount", 16000, 100000, { near = true })   -- near: also 15999 / 16001 ("x >= 16000")
---   h:restore()
--- Returns a handle {ids, sites, active, restore()}  |  nil, error. All sites or none.
function M.limit(func, old, new, opts)
  local sites = C.consts(func, old, opts)
  if not sites then return nil, C.last_error() end
  if #sites == 0 then return nil, ("no instruction of %s uses %s%s"):format(tostring(func), tostring(old), (opts and opts.near) and " (or +-1)" or "") end
  local ids = C.set_const(sites, new)
  if not ids then return nil, C.last_error() end
  return handle(ids, { sites = sites, func = func, old = old, new = new })
end

--- codex.patch as a handle: cx.patch(addr, "90 90", "74 05") -> handle | nil, error
function M.patch(addr, bytes, expect)
  local id = C.patch(addr, bytes, expect)
  if not id then return nil, C.last_error() end
  return handle({ id }, { addr = addr })
end

-- ------------------------------------------------------------------------------------------------- private data
--- AMXX-style: cx.pdata(id, 115, "i32", 5)  ==  get_pdata_int(id, 115, 5)   (offset in 4-byte units, linux diff in units)
--- type defaults to "i32"; linux_diff defaults to 5 for players (use 4 for weapons, as in AMXX).
function M.pdata(id, index, ty, linux_diff)
  ty = ty or "i32"
  linux_diff = linux_diff or 5
  return C.get(id, index * 4, ty, (index + linux_diff) * 4)
end
function M.set_pdata(id, index, value, ty, linux_diff)
  ty = ty or "i32"
  linux_diff = linux_diff or 5
  return C.set(id, index * 4, ty, value, (index + linux_diff) * 4)
end

--- Named fields, byte offsets:  local F = cx.fields{ money = { windows = 460, linux = 480, type = "i32" } }
---   F.get(id, "money")   F.set(id, "money", 20000)   F.offset("money") -> offset for this platform
--- A field may also be given in AMXX units:  money = { index = 115, linux_diff = 5 }
function M.fields(spec)
  local F = {}
  local function off(name)
    local s = spec[name]
    if not s then error("codex fields: unknown field '" .. tostring(name) .. "'", 3) end
    if s.index then return s.index * 4, (s.index + (s.linux_diff or 5)) * 4, s.type or "i32" end
    return s.windows or s.offset, s.linux or s.windows or s.offset, s.type or "i32"
  end
  function F.get(id, name) local w, l, t = off(name); return C.get(id, w, t, l) end
  function F.set(id, name, v) local w, l, t = off(name); return C.set(id, w, t, v, l) end
  function F.offset(name) local w, l = off(name); return M.platform() == "linux" and l or w end
  return F
end

-- ------------------------------------------------------------------------------------------------- gamedata export
--- Gamedata for THIS build, as Lua text: signatures Codex generated for each resolved function. Run it on Windows and on
--- Linux and merge, e.g.   GiveNamedItem = { sig = { windows = "...", linux = "..." } }
--- names: list of function names (default: all). print = true also writes it to the server console. Returns the text.
function M.dump(names, print)
  names = names or C.list()
  local p = M.platform() or "?"
  local out = { ("-- codex gamedata for %s, generated %s"):format(p, os and os.date and os.date("%Y-%m-%d") or "") }
  for _, n in ipairs(names) do
    local i = C.info(n)
    if i and i.resolved and i.addr then
      local s = C.sig_make(n)
      if s and s.unique then
        out[#out + 1] = ('%s = { module = "%s", sig = { %s = "%s" } },   -- %s, found by %s'):format(n, i.module, p, s.sig, i.proto, i.how)
      else
        out[#out + 1] = ("-- %s: no unique signature on this build (%s) - use symbol / xref / index"):format(n, s and s.error or C.last_error())
      end
    elseif i and i.vindex then
      out[#out + 1] = ("%s = { vindex = { %s = %d } },   -- %s"):format(n, p, i.vindex, i.proto)
    else
      out[#out + 1] = ("-- %s: not resolved"):format(n)
    end
  end
  local text = table.concat(out, "\n")
  if print then for line in text:gmatch("[^\n]+") do xmod.server_print(line) end end
  return text
end

-- ------------------------------------------------------------------------------------------------- small helpers
--- "cs.so+1A2B0" for an address (or the hex address if it is in no module)
function M.where(addr)
  if not addr then return "nil" end
  local m = C.module_of(addr)
  return m and ("%s+%X"):format(m.name, addr - m.base) or ("%08X"):format(addr)
end
function M.hex(n) return n and ("%08X"):format(n) or "nil" end
--- a vector table as Codex expects it
function M.vec(x, y, z) return { x = x or 0, y = y or 0, z = z or 0 } end

return M
