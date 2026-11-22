-- ==============================================================================================================
--  lib/metalog.lua  -  debug + logging system for xmod                 local log = require("metalog").new("myplugin")
--
--  * levels trace < debug < info < warn < error, written to the server console and to Markdown files
--    (addons/xmod/data/logs/<name>_<date>.md, plus data/logs/errors_<date>.md with the warnings/errors of ALL plugins)
--  * MASTER SWITCH below: set  enabled = false  and every metalog function becomes an EMPTY function
--    (nothing is printed, nothing is written, the functions you wrapped with log:fn() / metalog.debug_only() are
--    replaced by an empty function and never run). Edit, save, then  xmod reload.
--  * debug tools: dump tables, Markdown tables, timers, counters, once / every (rate limit), assert, try / wrap
--    (catch errors with their arguments), trace_calls (log every call of a module), bench, player info
--  * server console:  metalog  (status, level, on/off, flush, tail, errors, test)
--
--  Full documentation: xmod_skills.md, section "metalog".
-- ==============================================================================================================
local M = {}
M.VERSION = "1.0"

-- =============================================  EDIT HERE  ====================================================
M.CONFIG = {
  enabled      = true,         -- MASTER SWITCH. false = all metalog functions are empty functions (zero output, ~zero cost)
  level        = "debug",      -- lowest level that is kept: "trace" | "debug" | "info" | "warn" | "error" | "off"
  console      = true,         -- print kept lines to the server console
  file         = true,         -- write the Markdown log files
  dir          = "data/logs/", -- folder of the .md files (must be below data/). If it is missing, files go to data/
  rotate       = "day",        -- "day" = <name>_2026-11-21.md (one file per day), "none" = <name>.md
  max_file_kb  = 2048,         -- a file stops growing after this size (a note is written); 0 = no limit
  flush_every  = 0.5,          -- seconds between buffered writes. warn/error lines are written at once
  combined     = true,         -- also copy warn + error lines of every logger into errors_<date>.md
  to_xmod      = true,         -- error lines also go to xmod.error (-> data/xmod_errors.log, Metamod log)
  location     = true,         -- add the caller's "file.lua:line" to every line
  admins       = false,        -- also print warn + error lines in the console of online admins ...
  admin_flag   = "l",          -- ... that have this access flag (l = rcon)
  console_min  = "trace",      -- lowest level printed in the server console (file gets everything >= level)
}
-- ==============================================================================================================

local CFG = M.CONFIG
local LEVELS = { trace = 1, debug = 2, info = 3, warn = 4, error = 5, off = 99 }
local NAMES = { "trace", "debug", "info", "warn", "error" }
local TAG = { trace = "TRACE", debug = "DEBUG", info = "INFO", warn = "WARN", error = "ERROR" }
M.LEVELS = LEVELS

local function NOOP() end
M.NOOP = NOOP

local loggers, order = {}, {}           -- name -> logger
local files = {}                        -- path -> { buf = {}, size = n, full = bool, header = bool, map = "" }
local runtime_off = false               -- "metalog off" from the console (CONFIG.enabled stays the load-time switch)

local function lvl(name, default)
  if type(name) == "number" then return name end
  return LEVELS[tostring(name or ""):lower()] or default or LEVELS.debug
end

-- ------------------------------------------------------------------------------------------------- helpers
--- "plugins/x.lua:12" of the function `depth` levels above the caller of M.where (1 = the caller itself)
function M.where(depth)
  local _, m = pcall(error, "", (depth or 1) + 2)
  m = tostring(m or ""):gsub(":%s*$", "")
  return m
end

local function sorted_keys(t)
  local ks = {}
  for k in pairs(t) do ks[#ks + 1] = k end
  table.sort(ks, function(a, b)
    local ta, tb = type(a), type(b)
    if ta ~= tb then return ta < tb end
    if ta == "number" or ta == "string" then return a < b end
    return tostring(a) < tostring(b)
  end)
  return ks
end

--- value -> readable text. Tables are pretty printed (depth limit, cycles marked). Never raises.
function M.dump(v, depth, indent, seen)
  depth = depth or 4; indent = indent or ""; seen = seen or {}
  local t = type(v)
  if t == "string" then return string.format("%q", v) end
  if t ~= "table" then return tostring(v) end
  if seen[v] then return "<cycle>" end
  if depth <= 0 then return "{...}" end
  seen[v] = true
  local n, keys = #v, sorted_keys(v)
  if #keys == 0 then seen[v] = nil; return "{}" end
  local parts, inner, short = {}, indent .. "  ", true
  for _, k in ipairs(keys) do
    local val = v[k]
    local ks = (type(k) == "number" and k >= 1 and k <= n and math.type(k) == "integer") and ""
      or (type(k) == "string" and k:match("^[%a_][%w_]*$") and (k .. " = ")) or ("[" .. M.dump(k, 1) .. "] = ")
    local vs = M.dump(val, depth - 1, inner, seen)
    if type(val) == "table" or #vs > 40 then short = false end
    parts[#parts + 1] = ks .. vs
  end
  seen[v] = nil
  if short and #keys <= 8 then return "{ " .. table.concat(parts, ", ") .. " }" end
  return "{\n" .. inner .. table.concat(parts, ",\n" .. inner) .. "\n" .. indent .. "}"
end

--- format like string.format; never raises (bad format -> values joined with spaces); tables are dumped
function M.format(fmt, ...)
  local n = select("#", ...)
  if type(fmt) ~= "string" then
    local out = { M.dump(fmt, 2) }
    for i = 1, n do out[#out + 1] = M.dump((select(i, ...)), 2) end
    return table.concat(out, " ")
  end
  if n == 0 then return fmt end
  local args = table.pack(...)
  for i = 1, n do
    local a = args[i]
    if type(a) == "table" then args[i] = M.dump(a, 2) elseif type(a) ~= "number" and type(a) ~= "string" then args[i] = tostring(a) end
  end
  local ok, s = pcall(string.format, fmt, table.unpack(args, 1, n))
  if ok then return s end
  local out = { fmt }
  for i = 1, n do out[#out + 1] = tostring(args[i]) end
  return table.concat(out, " ")
end

--- "Name <#userid> <STEAM_..> <T|CT|SPEC> hp=100" (safe for any id)
function M.who(id)
  if type(id) ~= "number" then return tostring(id) end
  if id == 0 then return "SERVER" end
  if not xmod.player.connected(id) then return ("#%d (not connected)"):format(id) end
  local ok, s = pcall(function()
    local P = xmod.player
    local team = ({ [0] = "-", "T", "CT", "SPEC" })[P.team(id)] or "?"
    return ("%s <#%d> <%s> <%s>%s%s"):format(P.name(id), P.userid(id), P.steamid(id), team,
      P.is_bot(id) and " BOT" or "", P.alive(id) and (" hp=" .. math.floor(P.health(id))) or " dead")
  end)
  return ok and s or ("#%d"):format(id)
end

local function md_escape_cell(s) return (tostring(s):gsub("|", "\\|"):gsub("\r?\n", " ")) end

--- Markdown table text from headers + rows (rows = list of lists, or list of maps keyed by header)
function M.md_table(headers, rows)
  local out = { "| " .. table.concat((function() local h = {} for i, x in ipairs(headers) do h[i] = md_escape_cell(x) end return h end)(), " | ") .. " |",
                "|" .. string.rep("---|", #headers) }
  for _, r in ipairs(rows or {}) do
    local cells = {}
    for i, h in ipairs(headers) do
      local v = r[i]; if v == nil then v = r[h] end
      cells[i] = md_escape_cell(v == nil and "" or (type(v) == "table" and M.dump(v, 1) or tostring(v)))
    end
    out[#out + 1] = "| " .. table.concat(cells, " | ") .. " |"
  end
  return table.concat(out, "\n")
end

-- ------------------------------------------------------------------------------------------------- files
local function today() return os.date("%Y-%m-%d") end
local function safe_name(s) return (tostring(s):gsub("[^%w_%-]", "_")) end
local fallback_dir = false              -- set when data/logs/ does not exist

local function file_path(base)
  local dir = fallback_dir and "data/" or CFG.dir
  local prefix = fallback_dir and "logs_" or ""
  if CFG.rotate == "none" then return dir .. prefix .. base .. ".md" end
  return dir .. prefix .. base .. "_" .. today() .. ".md"
end

local function fstate(path)
  local f = files[path]
  if not f then
    local old = xmod.fs.read(path)
    f = { buf = {}, size = old and #old or 0, full = false, header = old ~= nil and #old > 0, map = "" }
    files[path] = f
  end
  return f
end

local function raw_write(path, text)
  local ok = xmod.fs.write(path, text, true)
  if not ok and not fallback_dir and path:sub(1, #CFG.dir) == CFG.dir then
    fallback_dir = true                  -- data/logs/ is missing: switch to data/logs_*.md (once)
    xmod.log("metalog: folder " .. CFG.dir .. " is missing - writing data/logs_<name>.md instead (create the folder to fix)")
    return false, "retry"
  end
  return ok
end

local function flush_file(path, f)
  if #f.buf == 0 then return end
  local text = table.concat(f.buf); f.buf = {}
  local ok, why = raw_write(path, text)
  if why == "retry" then
    -- re-queue into the fallback file
    local np = path:gsub("^" .. CFG.dir:gsub("%p", "%%%0"), "data/logs_")
    local nf = fstate(np); nf.buf[#nf.buf + 1] = text; nf.size = nf.size + #text
    flush_file(np, nf)
  end
end

function M.flush_all()
  for path, f in pairs(files) do flush_file(path, f) end
end

local function header_for(title)
  return ("# metalog - %s - %s\n\nServer: %s | xmod %s | started %s\n\n"):format(title, today(),
    md_escape_cell(xmod.cvar.get("hostname") or "?"), tostring(xmod.version), os.date("%H:%M:%S"))
end

local function queue(base, title, text, now)
  if not CFG.file then return end
  local path = file_path(base)
  local f = fstate(path)
  if f.full then return end
  local chunk = {}
  if not f.header then chunk[#chunk + 1] = header_for(title); f.header = true end
  local map = xmod.map() or ""
  if f.map ~= map then chunk[#chunk + 1] = ("\n## %s - map %s\n\n"):format(os.date("%H:%M:%S"), map ~= "" and map or "?"); f.map = map end
  chunk[#chunk + 1] = text
  local s = table.concat(chunk)
  if CFG.max_file_kb > 0 and f.size + #s > CFG.max_file_kb * 1024 then
    f.full = true
    s = ("\n> **metalog: file size limit reached (%d KB) - nothing more is written today.** Raise `max_file_kb` in lib/metalog.lua.\n"):format(CFG.max_file_kb)
  end
  f.buf[#f.buf + 1] = s; f.size = f.size + #s
  if now then flush_file(path, f) end
end

-- ------------------------------------------------------------------------------------------------- output
local function admins_print(text)
  for _, id in ipairs(xmod.player.all()) do
    if not xmod.player.is_bot(id) and xmod.player.has_access(id, CFG.admin_flag) then
      pcall(xmod.msg.print, id, "console", text .. "\n")
    end
  end
end

local function indent_block(text) return "  ```\n  " .. text:gsub("\n", "\n  ") .. "\n  ```\n" end

-- the one place where a line is emitted. L = logger, level = "warn", msg = text, extra = optional code block
local function emit(L, level, msg, where, extra)
  local n = LEVELS[level]
  local clock = os.date("%H:%M:%S")
  local loc = (CFG.location and L.location ~= false and where and where ~= "") and where or nil
  if loc and msg:sub(1, #loc + 2) == loc .. ": " then msg = msg:sub(#loc + 3) end   -- error text already starts with the same place
  if CFG.console and L.console ~= false and n >= lvl(CFG.console_min, 1) then
    xmod.server_print(("[%s] %s %s%s%s"):format(L.name, TAG[level], msg, loc and ("  (" .. loc .. ")") or "",
      extra and ("\n" .. extra) or ""))
  end
  if L.file ~= false then
    local first, rest = msg:match("^([^\n]*)\n(.*)$")
    local line = ("- `%s` **%s** %s%s\n"):format(clock, TAG[level], loc and ("`" .. loc .. "` ") or "", first or msg)
    if rest then line = line .. indent_block(rest) end
    if extra then line = line .. indent_block(extra) end
    queue(L.base, L.name, line, n >= LEVELS.warn)
    if CFG.combined and n >= LEVELS.warn then
      local cl = ("- `%s` **%s** [%s] %s%s\n"):format(clock, TAG[level], L.name, loc and ("`" .. loc .. "` ") or "", first or msg)
      if rest then cl = cl .. indent_block(rest) end
      queue("errors", "warnings and errors of all plugins", cl, true)
    end
  end
  if n >= LEVELS.error and CFG.to_xmod then xmod.error(("[%s] %s%s"):format(L.name, msg, loc and ("  (" .. loc .. ")") or "")) end
  if n >= LEVELS.warn and CFG.admins then admins_print(("[%s] %s %s"):format(L.name, TAG[level], msg)) end
end
M._emit = emit

-- ------------------------------------------------------------------------------------------------- logger
local Logger = {}            -- real methods
local Quiet = {}             -- what a method becomes when its level is off (nil = becomes NOOP)

local function make_level(level)
  return function(self, fmt, ...)
    emit(self, level, M.format(fmt, ...), M.where(2))
  end
end
for _, name in ipairs(NAMES) do Logger[name] = make_level(name) end

-- which level each tool method belongs to (it is an empty function when that level is off)
local TOOL_LEVEL = {
  dump = "debug", table = "info", section = "info", once = nil, every = nil, time = "debug", time_end = "debug",
  count = "debug", counts = "debug", bench = "debug", player = "debug",
}

--- log:dump(label, value [, depth=4])  -> debug line + the value pretty printed in a code block
function Logger:dump(label, value, depth)
  if value == nil and type(label) ~= "string" then value, label = label, "value" end
  emit(self, "debug", tostring(label), M.where(2), M.dump(value, depth or 4))
end

--- log:table(title, headers, rows)  -> a real Markdown table in the file (and plain lines in the console)
function Logger:table(title, headers, rows)
  local md = M.md_table(headers, rows)
  if CFG.console and self.console ~= false then
    xmod.server_print(("[%s] TABLE %s (%d rows)"):format(self.name, tostring(title), #(rows or {})))
    for line in md:gmatch("[^\n]+") do xmod.server_print("  " .. line) end
  end
  if self.file ~= false then queue(self.base, self.name, ("\n**%s** (%s)\n\n%s\n\n"):format(md_escape_cell(title), os.date("%H:%M:%S"), md), false) end
end

--- log:section("Round 3")  -> a "### Round 3" heading in the file
function Logger:section(title)
  if CFG.console and self.console ~= false then xmod.server_print(("[%s] ---- %s ----"):format(self.name, tostring(title))) end
  if self.file ~= false then queue(self.base, self.name, ("\n### %s\n\n"):format(md_escape_cell(title)), false) end
end

--- log:once(level, fmt, ...)  -> logged only the first time this exact text is seen (per map)
function Logger:once(level, fmt, ...)
  if not self:is(level) then return end
  local msg = M.format(fmt, ...)
  if self._once[msg] then return end
  self._once[msg] = true
  emit(self, lvl(level) >= LEVELS.error and "error" or NAMES[lvl(level)] or "info", msg, M.where(2))
end

--- log:every(seconds, level, fmt, ...)  -> rate limit: at most one line per `seconds` for the same format text
--- (the number of suppressed lines is added to the next one)
function Logger:every(seconds, level, fmt, ...)
  if not self:is(level) then return end
  local key = tostring(fmt)
  local st = self._every[key]
  local now = xmod.gametime()
  if st and now - st.t < seconds then st.skipped = st.skipped + 1; return end
  local msg = M.format(fmt, ...)
  if st and st.skipped > 0 then msg = msg .. (" (+%d similar lines suppressed)"):format(st.skipped) end
  self._every[key] = { t = now, skipped = 0 }
  emit(self, NAMES[lvl(level)] or "info", msg, M.where(2))
end

--- log:time(label) ... log:time_end(label)  -> "label: 1.234 ms" (CPU time, os.clock)
function Logger:time(label) self._timers[label or "default"] = os.clock() end
function Logger:time_end(label)
  label = label or "default"
  local t0 = self._timers[label]
  if not t0 then return emit(self, "warn", "time_end('" .. tostring(label) .. "') without time()", M.where(2)) end
  self._timers[label] = nil
  local ms = (os.clock() - t0) * 1000
  emit(self, "debug", ("%s: %.3f ms"):format(tostring(label), ms), M.where(2))
  return ms
end

--- log:count(label [, by=1]) -> counter; log:counts([reset]) writes them all as a table
function Logger:count(label, by) self._counts[label] = (self._counts[label] or 0) + (by or 1); return self._counts[label] end
function Logger:counts(reset)
  local rows = {}
  for _, k in ipairs(sorted_keys(self._counts)) do rows[#rows + 1] = { tostring(k), self._counts[k] } end
  self:table("counters", { "counter", "value" }, rows)
  if reset then self._counts = {} end
end

--- log:bench(label, times, fn, ...) -> runs fn `times` times, logs total + per call. Keep it short (script timeout!)
function Logger:bench(label, times, fn, ...)
  times = math.max(1, math.floor(times or 1000))
  local t0 = os.clock()
  for _ = 1, times do fn(...) end
  local total = (os.clock() - t0) * 1000
  emit(self, "debug", ("bench %s: %d calls, %.3f ms total, %.3f us per call"):format(tostring(label), times, total, total * 1000 / times), M.where(2))
  return total
end

--- log:player(id [, text]) -> debug line describing the player (name, #userid, steamid, team, hp)
function Logger:player(id, text)
  emit(self, "debug", M.who(id) .. (text and (" - " .. tostring(text)) or ""), M.where(2))
end

-- ----- methods that keep their behaviour when logging is off (they only stop logging) -----
--- log:assert(cond, fmt, ...) -> returns cond. When cond is false/nil an error line is logged (no exception)
function Logger:assert(cond, fmt, ...)
  if not cond and self:is("error") then emit(self, "error", "assertion failed: " .. M.format(fmt or "?", ...), M.where(2)) end
  return cond
end

--- ok, results... = log:try(fn, ...)  -> pcall that logs the error together with the arguments
function Logger:try(fn, ...)
  local res = table.pack(pcall(fn, ...))
  if not res[1] and self:is("error") then
    local args = {}
    for i = 1, select("#", ...) do args[i] = M.dump((select(i, ...)), 1) end
    emit(self, "error", tostring(res[2]), M.where(2), #args > 0 and ("arguments: " .. table.concat(args, ", ")) or nil)
  end
  return table.unpack(res, 1, res.n)
end

--- f = log:wrap(fn [, label]) -> fn protected: an error is logged (with the arguments) and the call returns nil
---   instead of raising. When the logger is off, fn itself is returned (errors then go to xmod as usual).
function Logger:wrap(fn, label)
  if not self:is("error") then return fn end
  local me = self
  label = label or M.where(2)
  return function(...)
    local res = table.pack(pcall(fn, ...))
    if res[1] then return table.unpack(res, 2, res.n) end
    local args = {}
    for i = 1, select("#", ...) do args[i] = M.dump((select(i, ...)), 1) end
    emit(me, "error", ("%s: %s"):format(label, tostring(res[2])), nil, #args > 0 and ("arguments: " .. table.concat(args, ", ")) or nil)
  end
end

--- f = log:fn(fn [, level="debug"]) -> fn when `level` is on for this logger, otherwise an EMPTY function.
---   Use it for debug-only helpers:  local debug_hud = log:fn(function(id) ... end)
---   Decided when called (normally at load): change CONFIG / the level and  xmod reload.
function Logger:fn(fn, level)
  if self:is(level or "debug") then return fn end
  return NOOP
end

--- log:strip(tbl [, level="debug"]) -> every function in tbl becomes an empty function when `level` is off; returns tbl
function Logger:strip(tbl, level)
  if not self:is(level or "debug") then for k, v in pairs(tbl) do if type(v) == "function" then tbl[k] = NOOP end end end
  return tbl
end

--- log:trace_calls(tbl [, prefix]) -> every function in tbl logs "prefix.name(args) -> results (ms)" at trace level.
---   Does nothing (tbl unchanged) when trace is off. Returns tbl.
function Logger:trace_calls(tbl, prefix)
  if not self:is("trace") then return tbl end
  prefix = prefix or "call"
  local me = self
  for k, f in pairs(tbl) do
    if type(f) == "function" then
      tbl[k] = function(...)
        local a = {}
        for i = 1, select("#", ...) do a[i] = M.dump((select(i, ...)), 1) end
        local t0 = os.clock()
        local res = table.pack(f(...))
        local r = {}
        for i = 1, res.n do r[i] = M.dump(res[i], 1) end
        emit(me, "trace", ("%s.%s(%s) -> %s  (%.3f ms)"):format(prefix, tostring(k), table.concat(a, ", "),
          res.n > 0 and table.concat(r, ", ") or "nothing", (os.clock() - t0) * 1000), nil)
        return table.unpack(res, 1, res.n)
      end
    end
  end
  return tbl
end

--- log:is(level) -> true when lines of this level are kept (use it to skip expensive debug code)
function Logger:is(level)
  return CFG.enabled and not runtime_off and lvl(level) >= self._level and self._level < LEVELS.off
end

function Logger:set_level(level) self.level = level; M.rebind(self); return self end
function Logger:get_level() return NAMES[self._level] or "off" end
function Logger:path() return file_path(self.base) end
function Logger:flush() local p = file_path(self.base); if files[p] then flush_file(p, files[p]) end end

-- methods that must never become empty (they change program behaviour or are needed to configure)
local KEEP = { assert = true, try = true, wrap = true, fn = true, strip = true, trace_calls = true, is = true,
  set_level = true, get_level = true, path = true, flush = true, count = true }

--- (internal) bind every method of L for the current switches: methods whose level is off become NOOP
function M.rebind(L)
  L._level = (not CFG.enabled or runtime_off) and LEVELS.off or lvl(L.level or CFG.level)
  for name, f in pairs(Logger) do
    local lev
    if LEVELS[name] then lev = name else lev = TOOL_LEVEL[name] end
    if KEEP[name] then L[name] = f
    elseif lev and lvl(lev) < L._level then L[name] = NOOP
    elseif L._level >= LEVELS.off then L[name] = NOOP
    else L[name] = f end
  end
  return L
end

--- log = metalog.new([name] [, opts])
---   name: shown in the console and used for the file name (default: the caller's file name, e.g. "myplugin")
---   opts: { level = "info", console = false, file = false, location = false }  (per logger; without level the global CONFIG.level is used)
---   Calling new() twice with the same name returns the same logger (opts are applied again).
function M.new(name, opts)
  if type(name) == "table" then opts, name = name, nil end
  if not name then
    name = M.where(2):match("([^/\\]+)%.lua") or "plugin"
  end
  opts = opts or {}
  local L = loggers[name]
  if not L then
    L = { name = name, base = safe_name(name), _once = {}, _every = {}, _timers = {}, _counts = {} }
    loggers[name] = L; order[#order + 1] = name
  end
  if opts.level ~= nil then L.level = opts.level end
  if opts.console ~= nil then L.console = opts.console end
  if opts.file ~= nil then L.file = opts.file end
  if opts.location ~= nil then L.location = opts.location end
  return M.rebind(L)
end

function M.get(name) return loggers[name] end
function M.list() local t = {} for i, n in ipairs(order) do t[i] = loggers[n] end return t end

--- metalog.debug_only(fn) -> fn if the master switch is on and the global level keeps "debug", else an empty function
function M.debug_only(fn)
  if CFG.enabled and not runtime_off and lvl(CFG.level) <= LEVELS.debug then return fn end
  return NOOP
end

--- metalog.enabled() -> current state of the switch (CONFIG.enabled and not "metalog off")
function M.enabled() return CFG.enabled and not runtime_off end

--- metalog.set_enabled(bool) -> runtime switch for every logger (functions from log:fn()/debug_only() stay as they are)
function M.set_enabled(on)
  runtime_off = not on
  for _, n in ipairs(order) do M.rebind(loggers[n]) end
end

--- metalog.set_level("myplugin" | "*", level)
function M.set_level(name, level)
  if name == "*" then CFG.level = level; for _, n in ipairs(order) do loggers[n].level = nil; M.rebind(loggers[n]) end; return true end
  local L = loggers[name]; if not L then return false end
  L:set_level(level); return true
end

-- default logger of the library itself (also used by `metalog test`)
local self_log = M.new("metalog")
M.log = self_log

-- ------------------------------------------------------------------------------------------------- timers + events
if CFG.enabled then
  xmod.timer.every(math.max(0.1, CFG.flush_every), function() M.flush_all() end)
end
xmod.on("map_end", function()
  M.flush_all()
  for _, n in ipairs(order) do loggers[n]._once = {}; loggers[n]._every = {} end
end)
xmod.on("shutdown", function() M.flush_all() end)

-- ------------------------------------------------------------------------------------------------- console
local function out(s) xmod.server_print(s) end
local function tail(path, n)
  local text = xmod.fs.read(path)
  if not text then return out("metalog: " .. path .. " does not exist") end
  local lines = {}
  for line in text:gmatch("[^\n]*") do lines[#lines + 1] = line end
  for i = math.max(1, #lines - n), #lines do out(lines[i]) end
end

xmod.cmd.server("metalog", function(args)
  local a, b, c = args[1] or "status", args[2], args[3]
  if a == "status" or a == "list" then
    out(("metalog %s: %s (master switch %s), global level %s, files %s in %s, rotate %s"):format(M.VERSION,
      M.enabled() and "ON" or "OFF", CFG.enabled and "on" or "OFF in lib/metalog.lua", CFG.level, CFG.file and "on" or "off",
      fallback_dir and "data/ (logs_*)" or CFG.dir, CFG.rotate))
    for _, n in ipairs(order) do
      local L = loggers[n]
      out(("  %-20s level %-6s console %-3s file %-3s -> %s"):format(n, L:get_level(), L.console == false and "off" or "on",
        L.file == false and "off" or "on", L:path()))
    end
  elseif a == "level" and b and c then
    if not LEVELS[c] then return out("metalog: level must be trace|debug|info|warn|error|off") end
    out(M.set_level(b, c) and ("metalog: " .. b .. " -> " .. c) or ("metalog: no logger named " .. b))
  elseif a == "on" or a == "off" then
    if a == "on" and not CFG.enabled then return out("metalog: the master switch is OFF in lib/metalog.lua (enabled = false); edit it and xmod reload") end
    M.set_enabled(a == "on"); out("metalog: " .. (a == "on" and "ON" or "OFF (log:fn()/debug_only() functions keep their load-time state)"))
  elseif a == "flush" then M.flush_all(); out("metalog: flushed")
  elseif a == "tail" then
    local L = loggers[b or "metalog"]
    if b == "errors" then M.flush_all(); return tail(file_path("errors"), tonumber(c) or 20) end
    if not L then return out("metalog: no logger named " .. tostring(b)) end
    L:flush(); tail(L:path(), tonumber(c) or 20)
  elseif a == "errors" then tail("data/xmod_errors.log", tonumber(b) or 20)
  elseif a == "test" then
    local L = M.new("metalog_test", { level = "trace" })
    L:section("metalog test " .. os.date("%H:%M:%S"))
    L:trace("trace line %d", 1); L:debug("debug line"); L:info("info line"); L:warn("warn line")
    L:dump("a table", { 1, 2, x = { y = "z" } })
    L:table("players", { "id", "name" }, (function() local r = {} for _, id in ipairs(xmod.player.all()) do r[#r + 1] = { id, xmod.player.name(id) } end return r end)())
    L:try(function() error("test error (expected)") end)
    L:flush(); out("metalog: test written to " .. L:path())
  else
    out("metalog status | level <logger|*> <trace|debug|info|warn|error|off> | on | off | flush")
    out("        tail <logger|errors> [lines] | errors [lines] (data/xmod_errors.log) | test")
  end
end, "metalog debug/log system (metalog help)")

return M
