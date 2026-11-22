-- ============================================================================================================
--  xmod_debug.lua  -  diagnostic + benchmark plugin.  Writes everything to  addons/xmod/data/xmod_debug.log
--
--  What it does
--    * logs lifecycle, player, chat, client-command, user-message, Ham and ReAPI events (with sampling for hot ones)
--    * every `report_interval` seconds writes a PERFORMANCE REPORT built from xmod's native per-event timers:
--        calls, calls/s, average / max microseconds per call, % of one CPU core spent in Lua, server FPS
--      -> this answers "is Lua fast enough for hot hooks?" with real numbers from YOUR server
--    * optional AddToFullPack stress test (the hottest GoldSrc hook, modes 0-4 incl. a native table with no Lua per entity): configs/xmod_debug.cfg (atfp_register=1)
--    * self-test (`xmod_dbg test`) that exercises the whole API and logs PASS/FAIL per function
--    * micro-benchmarks (`xmod_dbg bench`) of the cost of Lua->engine calls
--
--  Console (server console, or client console with access flag l):   xmod_dbg help
--  Settings:  configs/xmod_debug.cfg
-- ============================================================================================================
xmod.register("xmod Debug", "1.0", "xmod")
local U, json, menu = require("util"), require("json"), require("menu")

-- ---------------------------------------------------------------------------------------------- config
local CFG = {
  log_file = "xmod_debug.log", append = 0, max_log_kb = 8192,
  report_interval = 10, autostats = 1, flush_interval = 2,
  log_players = 1, log_chat = 1, log_commands = 1, log_messages = 1, log_damage = 1,
  hot_sample = 3,                       -- log the first N occurrences of every hot event / message
  ham_log = 1, reapi_log = 1,
  atfp_register = 0, atfp_mode = 1, atfp_hide_dist = 3000,
  warn_cpu_pct = 10, warn_max_ms = 5,
}
do
  local text = xmod.fs.read("configs/xmod_debug.cfg")
  if text then
    for line in (text .. "\n"):gmatch("(.-)\r?\n") do
      line = line:gsub("[;#].*$", ""):gsub("^%s+", ""):gsub("%s+$", "")
      local k, v = line:match("^([%w_]+)%s*=%s*(.-)$")
      if k and CFG[k] ~= nil then
        if type(CFG[k]) == "number" then CFG[k] = tonumber(v) or CFG[k] else CFG[k] = v end
      end
    end
  end
end

-- ---------------------------------------------------------------------------------------------- logging
local buf, nbuf, written, part, first = {}, 0, 0, 0, true
local function fname() return part == 0 and CFG.log_file or (CFG.log_file:gsub("%.log$", "") .. "_" .. part .. ".log") end
local function flush()
  if nbuf == 0 then return end
  local text = table.concat(buf, "", 1, nbuf); buf, nbuf = {}, 0
  xmod.fs.write("data/" .. fname(), text, not (first and CFG.append == 0)); first = false
  written = written + #text
  if written > CFG.max_log_kb * 1024 then part = part + 1; written = 0; first = true end
end
local function log(cat, msg, ...)
  if select("#", ...) > 0 then
    local ok, r = pcall(string.format, msg, ...)
    msg = ok and r or (msg .. "  <format error: " .. tostring(r) .. ">")
  end
  nbuf = nbuf + 1
  buf[nbuf] = string.format("[%s %8.1f][%-6s] %s\n", os.date("%H:%M:%S"), xmod.gametime(), cat, msg)
  if nbuf >= 256 then flush() end
end
local function pn(id) return xmod.player.connected(id) and xmod.player.name(id) or ("#" .. tostring(id)) end
local function dump(v) if type(v) == "table" then local ok, s = pcall(json.encode, v); return ok and s or tostring(v) end return tostring(v) end
local seen = {}
local function sample(key) local n = (seen[key] or 0) + 1; seen[key] = n; return n <= CFG.hot_sample, n end
-- register an event handler whose errors are caught and written to the log (instead of counting towards the 25-error limit)
local function on(ev, fn, raw)
  local ok, err = pcall(xmod.on, ev, raw and fn or function(...)
    local ok2, r = pcall(fn, ...)
    if not ok2 then log("ERROR", "handler for %s: %s", ev, tostring(r)); return nil end
    return r
  end)
  if not ok then log("WARN", "cannot register %s: %s", ev, tostring(err)) end
  return ok
end

log("BOOT", "xmod_debug starting. xmod %s, gamedir=%s, dedicated=%s", xmod.version, xmod.gamedir, tostring(xmod.is_dedicated()))
log("BOOT", "config: %s", dump(CFG))
log("BOOT", "reapi available: %s, ham functions: %d, script timeout cvar: %s ms", tostring(xmod.reapi.available()), #xmod.ham.list(), xmod.cvar.get("xmod_script_timeout"))
log("BOOT", "script errors from OTHER plugins are in the normal server log (L...log / console), not here")

-- ---------------------------------------------------------------------------------------------- lifecycle
local spawned = {}
on("precache", function() log("LIFE", "precache (map %s)", xmod.map()) end)
on("spawn", function(ent, class) spawned[class] = (spawned[class] or 0) + 1 end, true)
on("init", function(maxp)
  log("LIFE", "init: map=%s maxplayers=%d", xmod.map(), maxp)
  local rows = {}
  for class, n in pairs(spawned) do rows[#rows + 1] = { class, n } end
  table.sort(rows, function(a, b) return a[2] > b[2] end)
  local parts = {}
  for i = 1, math.min(#rows, 25) do parts[#parts + 1] = rows[i][1] .. "=" .. rows[i][2] end
  log("LIFE", "entities spawned while loading the map (top %d classes): %s", #parts, table.concat(parts, " "))
  spawned = {}
end)
on("config", function()
  log("LIFE", "config (xmod.cfg applied). plugins:")
  for _, p in ipairs(xmod.plugins()) do log("LIFE", "   %-24s v%-8s %-10s %s%s", p.name, p.version, p.author, p.file, p.enabled and "" or "   [DISABLED]") end
  log("LIFE", "cvars: xmod_script_timeout=%s xmod_show_activity=%s sys_ticrate=%s mp_timelimit=%s", xmod.cvar.get("xmod_script_timeout"),
      xmod.cvar.get("xmod_show_activity"), xmod.cvar.get("sys_ticrate"), xmod.cvar.get("mp_timelimit"))
end)
on("map_end", function() log("LIFE", "map_end (map %s)  -- timers persist across maps", xmod.map()); flush() end)
on("shutdown", function() log("LIFE", "shutdown"); flush() end)

-- ---------------------------------------------------------------------------------------------- players / chat / commands
on("client_connect", function(id, name, ip) if CFG.log_players ~= 0 then log("PLAYER", "connect     #%d name=%q ip=%s", id, name, ip) end end)
on("client_putinserver", function(id)
  if CFG.log_players == 0 then return end
  log("PLAYER", "putinserver #%d %q steam=%s ip=%s userid=%d bot=%s team=%d alive=%s access=%s", id, xmod.player.name(id), xmod.player.steamid(id),
      xmod.player.ip(id), xmod.player.userid(id), tostring(xmod.player.is_bot(id)), xmod.player.team(id), tostring(xmod.player.alive(id)),
      U.flags_to_string(xmod.player.access(id)))
end)
on("client_disconnect", function(id) if CFG.log_players ~= 0 then log("PLAYER", "disconnect  #%d %q", id, pn(id)) end end)
on("client_infochanged", function(id, old, new) if CFG.log_players ~= 0 then log("PLAYER", "infochanged #%d %q -> %q", id, old, new) end end)
on("chat", function(id, text, team) if CFG.log_chat ~= 0 then log("CHAT", "%s%s: %s", team and "(team) " or "", pn(id), text) end end)
on("client_command", function(id, cmd, args)
  if CFG.log_commands ~= 0 then log("CMD", "#%d %s: %s %s", id, pn(id), cmd, args.text or "") end
end)

-- ---------------------------------------------------------------------------------------------- hot events (counters only; timing comes from xmod.stats)
local C = { frame = 0, prethink = 0, postthink = 0, cmd_start = 0, touch = 0, think = 0, emit_sound = 0 }
on("frame",      function() C.frame = C.frame + 1 end, true)
on("prethink",   function() C.prethink = C.prethink + 1 end, true)
on("postthink",  function() C.postthink = C.postthink + 1 end, true)
on("cmd_start",  function(id, buttons, impulse, fwd, side, up)
  C.cmd_start = C.cmd_start + 1
  local ok, n = sample("cmd_start"); if ok then log("HOT", "cmd_start #%d buttons=%d impulse=%d fwd=%.0f side=%.0f up=%.0f (first %d samples)", id, buttons, impulse, fwd, side, up, CFG.hot_sample) end
end, true)
on("touch",      function(ent, other) C.touch = C.touch + 1
  local ok = sample("touch"); if ok then log("HOT", "touch ent=%d (%s) other=%d", ent, xmod.ent.classname(ent), other) end end, true)
on("think",      function(ent) C.think = C.think + 1 end, true)
on("emit_sound", function(ent, chan, sample_name, vol, att, flags, pitch)
  C.emit_sound = C.emit_sound + 1
  local ok = sample("emit_sound"); if ok then log("HOT", "emit_sound ent=%d chan=%d %q vol=%.2f att=%.2f flags=%d pitch=%d", ent, chan, sample_name, vol, att, flags, pitch) end
end, true)

-- ---------------------------------------------------------------------------------------------- user messages
local MSGS = { "DeathMsg", "Damage", "ScreenFade", "ScreenShake", "TeamInfo", "ScoreInfo", "SayText", "TextMsg", "HudTextArgs", "Money", "CurWeapon",
  "AmmoX", "Health", "Battery", "ResetHUD", "RoundTime", "StatusIcon", "BombDrop", "BombPickup", "ShowMenu", "VGUIMenu", "WeaponList", "Radar",
  "SetFOV", "StatusValue", "ClCorpse", "Scenario", "SendAudio", "Flashlight", "svc:23", "svc:30" }
local msgcount = {}
if CFG.log_messages ~= 0 then
  for _, name in ipairs(MSGS) do
    on("message:" .. name, function(n, dest, ent, args)
      msgcount[n] = (msgcount[n] or 0) + 1
      local ok = sample("msg_" .. n)
      if ok then log("MSG", "%s dest=%d ent=%d args=%s (first %d samples)", n, dest, ent, dump(args), CFG.hot_sample) end
    end, true)
  end
end

-- ---------------------------------------------------------------------------------------------- Ham / ReAPI observers
if CFG.ham_log ~= 0 then
  on("ham:spawn:player:post", function(id) local ok = sample("ham_spawn"); if ok then log("HAM", "spawn %s alive=%s team=%d", pn(id), tostring(xmod.player.alive(id)), xmod.player.team(id)) end end)
  on("ham:killed:player:post", function(victim, attacker, gib) log("HAM", "killed %s by ent %d (gib=%d)", pn(victim), attacker, gib) end)
  on("ham:takedamage:player", function(victim, inflictor, attacker, dmg, bits)
    if CFG.log_damage ~= 0 then log("HAM", "takedamage %s <- ent %d (inflictor %d) dmg=%.1f bits=0x%x", pn(victim), attacker, inflictor, dmg, bits) end
  end)
end
if CFG.reapi_log ~= 0 then
  if xmod.reapi.available() then
    on("reapi:CBasePlayer_Killed:post", function(victim, attacker, gib) log("REAPI", "Killed:post %s attacker ent %d gib=%d", pn(victim), attacker, gib) end)
    on("reapi:CBasePlayer_TakeDamage", function(victim, inflictor, attacker, dmg, bits)
      if CFG.log_damage ~= 0 then log("REAPI", "TakeDamage %s <- ent %d dmg=%.1f bits=0x%x", pn(victim), attacker, dmg, bits) end
    end)
    on("reapi:CSGameRules_RestartRound", function() log("REAPI", "RestartRound") end)
  else
    log("REAPI", "ReGameDLL not detected: reapi hooks skipped (Ham hooks still work)")
  end
end

-- ---------------------------------------------------------------------------------------------- AddToFullPack stress test (hottest GoldSrc hook)
-- Modes:  0 handler returns at once | 1 counter | 2 counter + lookups | 3 real work in Lua (2 native calls + math, culls far entities, glows teammates)
--         4 NATIVE table: Lua recomputes visibility once per second and xmod.fullpack.hide() lets C++ apply it - NO Lua call per entity
local atfp = { mode = CFG.atfp_mode, calls = 0, hidden = 0, glow = 0, post = 0, native_hidden = 0 }
local atfp_pre, atfp_post, atfp_lua = nil, nil, false
if CFG.atfp_register ~= 0 then
  xmod.fullpack.native()                                           -- installs the hook (needed for mode 4); must happen at load time
  local team_cache = {}
  local ent_get, ent_valid, p_origin, p_conn = xmod.ent.get, xmod.ent.valid, xmod.player.origin, xmod.player.connected
  local HIDE2 = CFG.atfp_hide_dist * CFG.atfp_hide_dist
  local function refresh_native()                                  -- mode 4: runs once per second
    xmod.fullpack.clear(); local n = 0
    for _, host in ipairs(xmod.player.all()) do
      local hx, hy, hz = p_origin(host)
      for ent = xmod.maxplayers() + 1, xmod.maxentities() - 1 do
        if ent_valid(ent) then
          local ex, ey, ez = ent_get(ent, "origin"); local dx, dy, dz = ex - hx, ey - hy, ez - hz
          if dx * dx + dy * dy + dz * dz > HIDE2 then xmod.fullpack.hide(host, ent, true); n = n + 1 end
        end
      end
    end
    atfp.native_hidden = n
  end
  xmod.timer.every(1.0, function()
    for _, id in ipairs(xmod.player.all()) do team_cache[id] = xmod.player.team(id) end
    if atfp.mode == 4 then local ok, err = pcall(refresh_native); if not ok then log("ERROR", "refresh_native: %s", tostring(err)) end end
  end)
  atfp_pre = function(ent, host, player)
    local m = atfp.mode
    if m == 0 then return end
    atfp.calls = atfp.calls + 1
    if m == 1 then return end
    if m == 2 then local t = team_cache[ent]; if t and t ~= team_cache[host] then atfp.glow = atfp.glow end return end
    if player == 0 and ent > 32 and p_conn(host) and ent_valid(ent) then
      local ex, ey, ez = ent_get(ent, "origin")
      local hx, hy, hz = p_origin(host)
      local dx, dy, dz = ex - hx, ey - hy, ez - hz
      if dx * dx + dy * dy + dz * dz > HIDE2 then atfp.hidden = atfp.hidden + 1; return true end
    end
  end
  atfp_post = function(ent, host, player, ret)
    atfp.post = atfp.post + 1
    if atfp.mode < 3 then return end
    if player == 1 and ent ~= host and team_cache[ent] and team_cache[ent] == team_cache[host] then
      atfp.glow = atfp.glow + 1
      xmod.fullpack.set_render(nil, 20, xmod.FX_GLOWSHELL, 0, 255, 0)      -- green glow on teammates
    end
  end
  function atfp.set_mode(m)             -- mode 4 removes the Lua handlers entirely: then the engine never calls into Lua per entity
    atfp.mode = m
    if m == 4 and atfp_lua then xmod.off("add_to_full_pack", atfp_pre); xmod.off("add_to_full_pack:post", atfp_post); atfp_lua = false
    elseif m ~= 4 and not atfp_lua then xmod.on("add_to_full_pack", atfp_pre); xmod.on("add_to_full_pack:post", atfp_post); atfp_lua = true end
    if m ~= 4 then xmod.fullpack.clear() end
  end
  if atfp.mode == 4 then atfp_lua = false else xmod.on("add_to_full_pack", atfp_pre); xmod.on("add_to_full_pack:post", atfp_post); atfp_lua = true end
  log("ATFP", "add_to_full_pack hooks installed, mode=%d. Change live: xmod_dbg mode <0-4>", atfp.mode)
end

-- ---------------------------------------------------------------------------------------------- performance report
local cumulative = {}
local function report(reason)
  local info, rows = xmod.stats.info(), xmod.stats.get()
  local wall = info.wall_s
  if not info.on or wall <= 0 or (wall < 1 and not (reason or ""):find("manual")) then return end
  local sum = 0
  for _, r in ipairs(rows) do sum = sum + r.total_us; local c = cumulative[r.name] or { calls = 0, total_us = 0, max_us = 0 }
    c.calls = c.calls + r.calls; c.total_us = c.total_us + r.total_us; if r.max_us > c.max_us then c.max_us = r.max_us end; cumulative[r.name] = c end
  log("PERF", "==== %s  window=%.1fs  server fps=%.1f (sys_ticrate=%s)  frame interval avg=%.2f ms max=%.1f ms  Lua memory=%.0f KB  script CPU=%.3f%% of one core ====",
      reason or "report", wall, info.fps, xmod.cvar.get("sys_ticrate"), info.frame_ms_avg, info.frame_ms_max, info.lua_kb, sum / (wall * 1e6) * 100)
  log("PERF", "  (call counts are exact; timings are sampled 1-in-16 after the first 256 calls, so max_us shows the slowest SAMPLED call - frame interval max catches hitches)")
  if #rows == 0 then log("PERF", "  (no hooked event fired in this window)") end
  for i = 1, math.min(#rows, 30) do
    local r = rows[i]; local pct = r.total_us / (wall * 1e6) * 100
    log("PERF", "  %-30s calls=%10d (%9.0f/s)  avg=%8.3f us  max=%10.1f us  total=%10.1f ms  cpu=%7.3f%%  handlers=%d",
        r.name, r.calls, r.calls / wall, r.avg_us, r.max_us, r.total_us / 1000, pct, r.handlers)
    if pct > CFG.warn_cpu_pct then log("WARN", "  '%s' uses %.1f%% of a core in Lua (limit warning %.0f%%) - this hook is too heavy for Lua at this call rate", r.name, pct, CFG.warn_cpu_pct) end
    if r.max_us > CFG.warn_max_ms * 1000 then log("WARN", "  '%s' had a single call of %.1f ms (>%d ms) -> that is a visible server hitch", r.name, r.max_us / 1000, CFG.warn_max_ms) end
  end
  log("PERF", "  counters: frame=%d prethink=%d postthink=%d cmd_start=%d touch=%d think=%d emit_sound=%d | atfp: mode=%d lua_calls=%d post=%d lua_hidden=%d glow=%d native_hidden_pairs=%d",
      C.frame, C.prethink, C.postthink, C.cmd_start, C.touch, C.think, C.emit_sound, atfp.mode, atfp.calls, atfp.post, atfp.hidden, atfp.glow, atfp.native_hidden)
  local mc = {}; for k, v in pairs(msgcount) do mc[#mc + 1] = k .. "=" .. v end; table.sort(mc)
  if #mc > 0 then log("PERF", "  messages seen: %s", table.concat(mc, " ")) end
  xmod.stats.reset()
end
if CFG.autostats ~= 0 then xmod.stats.enable(true) end
xmod.timer.every(CFG.report_interval, function() report("interval report") end)
xmod.timer.every(CFG.flush_interval, function() flush() end)       -- every flush opens/closes the log file (~0.2-3 ms, shows up under <timers> in the report)
xmod.timer.every(60.0, function() log("BEAT", "alive: map=%s players=%d lua=%.0f KB (timers survive map changes)", xmod.map(), #xmod.player.all(), xmod.stats.info().lua_kb) end)

-- ---------------------------------------------------------------------------------------------- self test
local T
local function check(name, cond, detail)
  if cond then T.pass = T.pass + 1; log("TEST", "PASS  %s", name)
  else T.fail = T.fail + 1; log("TEST", "FAIL  %s   %s", name, detail or "") end
end
local function approx(a, b) return type(a) == "number" and type(b) == "number" and math.abs(a - b) < 0.01 end
local function group(name, fn) local ok, err = pcall(fn); if not ok then T.fail = T.fail + 1; log("TEST", "FAIL  [%s] raised an error: %s", name, tostring(err)) end end

local function run_tests(caller, with_timeout)
  T = { pass = 0, fail = 0 }
  log("TEST", "=========== SELF TEST START (caller=%s) ===========", tostring(caller))
  local p = (caller and caller > 0 and xmod.player.ingame(caller)) and caller or xmod.player.all()[1]

  group("core", function()
    check("xmod.version is a string", type(xmod.version) == "string")
    local found = false; for _, pl in ipairs(xmod.plugins()) do if pl.name == "xmod Debug" then found = true end end
    check("xmod.plugins() lists this plugin", found)
    check("xmod.map()/maxplayers()", #xmod.map() > 0 and xmod.maxplayers() > 0, xmod.map() .. "/" .. xmod.maxplayers())
    check("xmod.gametime() is a number", type(xmod.gametime()) == "number")
  end)
  group("sandbox", function()
    check("io/debug/package/dofile/loadfile are removed", io == nil and debug == nil and package == nil and dofile == nil and loadfile == nil)
    check("os.execute/os.remove are removed, os.time works", os.execute == nil and os.remove == nil and type(os.time()) == "number")
    check("math/string/table/utf8 available", math and string and table and utf8 ~= nil)
  end)
  group("cvar", function()
    xmod.cvar.create("xmod_dbg_test", "5")
    check("cvar.create/get", xmod.cvar.get("xmod_dbg_test") == "5", xmod.cvar.get("xmod_dbg_test"))
    check("cvar.int/num", xmod.cvar.int("xmod_dbg_test") == 5 and approx(xmod.cvar.num("xmod_dbg_test"), 5))
    xmod.cvar.set("xmod_dbg_test", 7.5)
    check("cvar.set(number)", approx(xmod.cvar.num("xmod_dbg_test"), 7.5), xmod.cvar.get("xmod_dbg_test"))
    check("cvar.exists", xmod.cvar.exists("xmod_dbg_test") and not xmod.cvar.exists("xmod_no_such_cvar_zz"))
    check("cvar.set rejects ';'", not pcall(xmod.cvar.set, "xmod_dbg_test", "1;quit"))
  end)
  group("kv", function()
    xmod.kv.set("dbgtest", "a", 123); xmod.kv.set("dbgtest", "b\ttab", "multi\nline")
    check("kv.get", xmod.kv.get("dbgtest", "a") == "123")
    check("kv.get default / missing", xmod.kv.get("dbgtest", "zz", "dflt") == "dflt" and xmod.kv.get("dbgtest", "zz") == nil)
    check("kv escapes tab/newline", xmod.kv.get("dbgtest", "b\ttab") == "multi\nline")
    check("kv.keys", #xmod.kv.keys("dbgtest") == 2)
    xmod.kv.save("dbgtest"); xmod.kv.del("dbgtest", "a")
    check("kv.del", xmod.kv.get("dbgtest", "a") == nil)
  end)
  group("fs", function()
    check("fs.write", xmod.fs.write("data/xmod_dbg_test.txt", "hello\n") == true)
    check("fs.write append + read", xmod.fs.write("data/xmod_dbg_test.txt", "world\n", true) and xmod.fs.read("data/xmod_dbg_test.txt") == "hello\nworld\n")
    check("fs.exists", xmod.fs.exists("data/xmod_dbg_test.txt") and not xmod.fs.exists("data/definitely_missing.txt"))
    check("fs.write outside data/configs is refused", not pcall(xmod.fs.write, "plugins/evil.lua", "x"))
    check("fs.read rejects '..' and absolute paths", xmod.fs.read("../../server.cfg") == nil and xmod.fs.read("/etc/passwd") == nil)
    log("TEST", "INFO  fs.read_game('server.cfg') -> %s", xmod.fs.read_game("server.cfg") and "readable" or "not found (ok)")
  end)
  group("export/call/emit", function()
    xmod.export("dbg.add", function(a, b) return a + b, "ok" end)
    local s, tag = xmod.call("dbg.add", 2, 3)
    check("export/call (multiple returns)", s == 5 and tag == "ok")
    check("has_export", xmod.has_export("dbg.add") and not xmod.has_export("dbg.nope"))
    check("emit without handlers returns nil", xmod.emit("dbg_nobody_listens", 1) == nil)
    xmod.on("dbg_custom", function(a, b) if a == 1 and b == "x" then return true end end)
    check("custom event + consume (true)", xmod.emit("dbg_custom", 1, "x") == true)
    xmod.on("dbg_reject", function() return false, "nope" end)
    local r, why = xmod.emit("dbg_reject")
    check("custom event reject (false, reason)", r == false and why == "nope")
  end)
  group("util/json", function()
    check("util.split/trim", #U.split("a,b,c", ",") == 3 and U.trim("  x ") == "x")
    check("util.fmt_time(125)", U.fmt_time(125) == "2:05", U.fmt_time(125))
    check("util.flag/flags_to_string", U.flag("c") == 4 and U.flags_to_string(5) == "ac")
    check("util.clamp", U.clamp(5, 1, 3) == 3 and U.clamp(-1, 0, 9) == 0)
    local t = { a = 1, b = { 1, 2, 3 }, c = "x\ny", d = false, e = { f = true } }
    local back = json.decode(json.encode(t))
    check("json round trip", back and back.a == 1 and #back.b == 3 and back.c == "x\ny" and back.d == false and back.e.f == true)
    check("json.decode reports errors", select(2, json.decode("{bad")) ~= nil)
  end)
  group("ent/trace/mem", function()
    local e = xmod.ent.create("info_target")
    check("ent.create", e and e > 0, tostring(e))
    if e and e > 0 then
      xmod.ent.set(e, "targetname", "xmod_dbg_ent"); xmod.ent.set_origin(e, 11, 22, 33); xmod.ent.set(e, "health", 42.5); xmod.ent.set(e, "iuser1", 7)
      check("ent.get/set string", xmod.ent.get(e, "targetname") == "xmod_dbg_ent")
      local x, y, z = xmod.ent.get(e, "origin"); check("ent.set_origin / get origin", approx(x, 11) and approx(y, 22) and approx(z, 33), x .. "," .. y .. "," .. z)
      check("ent.get/set float+int", approx(xmod.ent.get(e, "health"), 42.5) and xmod.ent.get(e, "iuser1") == 7)
      check("ent.classname", xmod.ent.classname(e) == "info_target")
      local found, ent = 0, 0
      repeat ent = xmod.ent.find(ent, "info_target"); if ent ~= 0 then found = found + 1 end until ent == 0 or found > 500
      check("ent.find iterates classname", found >= 1, tostring(found))
      check("ent.find by field", xmod.ent.find(0, "xmod_dbg_ent", "targetname") == e)
      local near = xmod.ent.find_sphere(11, 22, 33, 5); local has = false; for _, v in ipairs(near) do if v == e then has = true end end
      check("ent.find_sphere", has)
      check("ent.get unknown field raises", not pcall(xmod.ent.get, e, "no_such_field"))
      xmod.ent.remove(e)
      check("ent.remove sets FL_KILLME", (xmod.ent.get(e, "flags") & (1 << 30)) ~= 0 or not xmod.ent.valid(e))
      check("ent.remove refuses players", not pcall(xmod.ent.remove, 1))
      xmod.timer.after(0.5, function() check("removed entity is invalid a moment later", not xmod.ent.valid(e)) end)
    end
    check("trace.contents returns int", math.type(xmod.trace.contents(0, 0, 0)) == "integer")
    if p then
      local ox, oy, oz = xmod.player.origin(p)
      local tr = xmod.trace.line(ox, oy, oz, ox, oy, oz - 8192, 0, p)
      check("trace.line (down from player)", type(tr) == "table" and tr.fraction >= 0 and tr.fraction <= 1, dump(tr))
      log("TEST", "INFO  trace down: fraction=%.4f ent=%d end=(%.0f %.0f %.0f) normal z=%.2f", tr.fraction, tr.ent, tr.x, tr.y, tr.z, tr.nz)
      check("mem.get_int(team offset) == player.team", xmod.mem.get_int(p, 114, xmod.mem.LINUX_PLAYER) == xmod.player.team(p),
            tostring(xmod.mem.get_int(p, 114, xmod.mem.LINUX_PLAYER)) .. " vs " .. xmod.player.team(p))
      check("mem refuses huge offsets", not pcall(xmod.mem.get_int, p, 100000))
    end
  end)
  group("player", function()
    if not p then log("TEST", "SKIP  player tests: nobody is in game (join the server and run `xmod_dbg test` again)"); return end
    local name = xmod.player.name(p)
    check("player.connected/ingame/all", xmod.player.connected(p) and xmod.player.ingame(p) and #xmod.player.all() >= 1)
    check("player.name/steamid/ip/userid", #name > 0 and #xmod.player.steamid(p) > 0 and #xmod.player.ip(p) > 0 and xmod.player.userid(p) > 0)
    local hp = xmod.player.health(p); xmod.player.set_health(p, hp)
    check("player.health/set_health round trip", approx(xmod.player.health(p), hp))
    local a = xmod.player.armor(p); xmod.player.set_armor(p, a); check("player.armor", approx(xmod.player.armor(p), a))
    local ping, loss = xmod.player.ping(p); check("player.ping", type(ping) == "number", tostring(ping))
    local vx, vy, vz = xmod.player.velocity(p); check("player.velocity", type(vz) == "number")
    local pitch = xmod.player.view_angles(p); check("player.view_angles", type(pitch) == "number")
    check("player.team in 0..3", xmod.player.team(p) >= 0 and xmod.player.team(p) <= 3, tostring(xmod.player.team(p)))
    check("player.team_name", xmod.player.team_name(2) == "CT")
    local money = xmod.player.money(p); check("player.money readable", type(money) == "number" and money >= 0, tostring(money))
    check("player.access/has_access", xmod.player.has_access(p, "") and type(xmod.player.access(p)) == "number")
    local id, hits = xmod.player.find(name:sub(1, math.max(1, math.min(#name, 4))))
    check("player.find (partial name)", hits >= 1, tostring(hits))
    check("player.find (#userid)", select(1, xmod.player.find("#" .. xmod.player.userid(p))) == p)
    check("player.infokey(name)", xmod.player.infokey(p, "name") == name)
    local d = xmod.player.data(p); d.x = 5; check("player.data is a persistent per-player table", xmod.player.data(p).x == 5)
    log("TEST", "INFO  %s: steam=%s ip=%s team=%d hp=%.0f armor=%.0f money=%d ping=%d origin=%.0f,%.0f,%.0f", name, xmod.player.steamid(p), xmod.player.ip(p),
        xmod.player.team(p), hp, a, money, ping, xmod.player.origin(p))
    xmod.msg.chat(p, "{g}[xmod_debug] {y}chat test: {t}team colour {g}green {y}yellow")
    xmod.msg.print(p, "console", "[xmod_debug] console print works")
    xmod.msg.print(p, "center", "xmod_debug center print")
    xmod.msg.hud(p, "xmod_debug HUD message", { y = 0.3, hold = 3, r = 0, g = 255, b = 120, channel = 2 })
    xmod.msg.fade(p, 0.3, 0.1, 0, 255, 255, 255, 40)
    log("TEST", "INFO  sent chat/console/center/HUD/fade messages to %s - verify visually in game", name)
    if caller and caller > 0 then
      menu.show(p, { title = "xmod_debug menu test", items = { { "Item A", "a" }, { "Item B", "b" } }, timeout = 8,
        on_select = function(pid, label, value) log("TEST", "PASS  menu selection received: %s (%s)", label, value) end })
      log("TEST", "INFO  opened a menu on %s - press 1 or 2 within 8 s to verify menuselect handling", name)
    end
  end)
  group("hooks", function()
    check("ham.list has >50 functions", #xmod.ham.list() > 50, tostring(#xmod.ham.list()))
    check("unknown ham function raises", not pcall(xmod.on, "ham:nosuchfunc:player", function() end))
    check("ham event without classname raises", not pcall(xmod.on, "ham:takedamage", function() end))
    if xmod.reapi.available() then check("reapi.list has hooks", #xmod.reapi.list() > 100, tostring(#xmod.reapi.list()))
      check("unknown reapi hook raises", not pcall(xmod.on, "reapi:NopeNope", function() end))
    else log("TEST", "SKIP  reapi checks (ReGameDLL not detected)") end
    check("fullpack.set_render outside its hook raises", not pcall(xmod.fullpack.set_render, 0, 0, 0))
    check("xmod.maxentities() sane", xmod.maxentities() > xmod.maxplayers())
    check("hook.set_arg outside a hook raises", not pcall(xmod.hook.set_arg, 1, 1))
  end)
  group("timers", function()
    local R = { once = 0, times = 0, every = 0, cancelled = 0, args = nil, false_cancel = 0 }
    xmod.timer.after(0.2, function() R.once = R.once + 1 end)
    xmod.timer.times(3, 0.15, function() R.times = R.times + 1 end)
    xmod.timer.every(0.1, function() R.every = R.every + 1; if R.every >= 4 then return false end end)
    local id = xmod.timer.after(0.1, function() R.cancelled = R.cancelled + 1 end); xmod.timer.cancel(id)
    xmod.timer.after(0.1, function(a, b, c) R.args = a .. tostring(b) .. c end, "x", 7, "z")
    xmod.timer.every(0.1, function() R.false_cancel = R.false_cancel + 1; return false end)
    xmod.timer.after(1.5, function()
      check("timer.after fires once", R.once == 1, tostring(R.once))
      check("timer.times(3) fires 3 times", R.times == 3, tostring(R.times))
      check("timer.every stops when callback returns false", R.every == 4, tostring(R.every))
      check("timer.cancel prevents firing", R.cancelled == 0)
      check("timer extra args", R.args == "x7z", tostring(R.args))
      check("timer returning false cancels immediately", R.false_cancel == 1, tostring(R.false_cancel))
    end)
  end)
  if with_timeout then
    group("watchdog", function()
      local limit = xmod.cvar.num("xmod_script_timeout")
      if limit <= 0 then log("TEST", "SKIP  watchdog test: xmod_script_timeout is 0 (off)"); return end
      local t0 = os.clock()
      local ok, err = pcall(function() while true do end end)
      local ms = (os.clock() - t0) * 1000
      check("watchdog aborts a runaway loop", not ok and tostring(err):find("timeout"), tostring(err))
      check("abort latency within 3x the limit", ms < limit * 3 + 30, string.format("%.0f ms (limit %.0f)", ms, limit))
      log("TEST", "INFO  runaway loop aborted after %.0f ms (limit %.0f ms)", ms, limit)
    end)
  end
  xmod.timer.after(2.0, function()
    log("TEST", "=========== SELF TEST DONE: %d passed, %d failed ===========", T.pass, T.fail)
    if caller then U.reply(caller, ("xmod_debug self test: %d passed, %d failed (details in data/%s)"):format(T.pass, T.fail, fname())) end
    flush()
  end)
end

-- ---------------------------------------------------------------------------------------------- micro benchmarks
local BENCH = {}
local function add_bench(name, mk) BENCH[#BENCH + 1] = { name = name, make = mk } end
local function time_loop(fn)       -- doubles n until the loop takes >= 6 ms (stays far below the script time limit), returns ns/op
  local n, el = 2000, 0
  while true do
    local t0 = os.clock(); fn(n); el = os.clock() - t0
    if el >= 0.006 or n >= 4000000 then break end
    n = n * 2
  end
  return el / n * 1e9, n
end
local function setup_bench(p)
  BENCH = {}
  local nop = function() end
  add_bench("empty for-loop iteration", function() return function(n) for i = 1, n do end end end)
  add_bench("Lua function call (no-op)", function() return function(n) for i = 1, n do nop() end end end)
  add_bench("table creation {x,y,z}", function() return function(n) for i = 1, n do local t = { i, i, i } end end end)
  add_bench("string.format('%d:%s')", function() return function(n) for i = 1, n do local s = string.format("%d:%s", i, "x") end end end)
  add_bench("math.sqrt + arithmetic", function() return function(n) local a = 0; for i = 1, n do a = a + math.sqrt(i * 1.5) end end end)
  add_bench("xmod.gametime()", function() local f = xmod.gametime; return function(n) for i = 1, n do f() end end end)
  add_bench("xmod.cvar.int('sv_gravity')", function() local f = xmod.cvar.int; return function(n) for i = 1, n do f("sv_gravity") end end end)
  add_bench("xmod.kv.get (hit)", function() xmod.kv.set("dbgbench", "k", "v"); local f = xmod.kv.get; return function(n) for i = 1, n do f("dbgbench", "k") end end end)
  if p then
    add_bench("xmod.player.health(id)", function() local f = xmod.player.health; return function(n) for i = 1, n do f(p) end end end)
    add_bench("xmod.player.origin(id) [3 returns]", function() local f = xmod.player.origin; return function(n) for i = 1, n do f(p) end end end)
    add_bench("xmod.player.connected(id)", function() local f = xmod.player.connected; return function(n) for i = 1, n do f(p) end end end)
    add_bench("xmod.ent.get(ent,'health')", function() local f = xmod.ent.get; return function(n) for i = 1, n do f(p, "health") end end end)
    add_bench("xmod.ent.get(ent,'origin') [3 returns]", function() local f = xmod.ent.get; return function(n) for i = 1, n do f(p, "origin") end end end)
    add_bench("xmod.ent.valid(ent)", function() local f = xmod.ent.valid; return function(n) for i = 1, n do f(p) end end end)
    add_bench("xmod.ent.classname(ent)", function() local f = xmod.ent.classname; return function(n) for i = 1, n do f(p) end end end)
    add_bench("xmod.player.data(id).x = ...", function() local f = xmod.player.data; return function(n) for i = 1, n do f(p).x = i end end end)
    add_bench("xmod.trace.line (short) [returns table]", function() local f = xmod.trace.line; local x, y, z = xmod.player.origin(p); return function(n) for i = 1, n do f(x, y, z, x, y, z - 64, 0, p) end end end)
    add_bench("xmod.trace.line_n (short) [8 returns]", function() local f = xmod.trace.line_n; local x, y, z = xmod.player.origin(p); return function(n) for i = 1, n do f(x, y, z, x, y, z - 64, 0, p) end end end)
  end
end
local bench_running = false
local function run_bench(caller)
  if bench_running then U.reply(caller, "benchmark already running"); return end
  local p = (caller and caller > 0 and xmod.player.ingame(caller)) and caller or xmod.player.all()[1]
  setup_bench(p); bench_running = true
  log("BENCH", "=========== MICRO BENCHMARK START (%d tests, one per timer tick; player-based tests %s) ===========", #BENCH, p and "enabled" or "SKIPPED: no player in game")
  local i, results = 0, {}
  local function step()
    i = i + 1
    local b = BENCH[i]
    if not b then
      log("BENCH", "=========== MICRO BENCHMARK DONE ===========")
      log("BENCH", "reading guide: a hot hook such as add_to_full_pack runs ~%d-%d calls/s on a full server; multiply by the per-call cost below + ~0.3-1.0 us of hook dispatch to estimate CPU use", 50000, 3000000)
      bench_running = false; flush(); if caller then U.reply(caller, "benchmark done - see data/" .. fname()) end
      return
    end
    local ok, ns, n = pcall(function() return time_loop(b.make()) end)
    if ok then log("BENCH", "%-42s %9.1f ns/op   (n=%d)", b.name, ns, n) else log("BENCH", "%-42s FAILED: %s", b.name, tostring(ns)) end
    xmod.timer.after(0.05, step)
  end
  xmod.timer.after(0.05, step)
end

-- ---------------------------------------------------------------------------------------------- console command
local HELP = {
  "xmod_dbg status          - show settings, counters, stats",
  "xmod_dbg report          - write a performance report to the log now",
  "xmod_dbg stats on|off|reset",
  "xmod_dbg test [timeout]  - API self test (PASS/FAIL per function); 'timeout' also tests the watchdog (burns ~100 ms CPU)",
  "xmod_dbg bench           - micro-benchmark Lua->engine call costs",
  "xmod_dbg mode <0-4>      - AddToFullPack workload: 0 idle, 1 counter, 2 lookups, 3 real Lua work, 4 native hide table (needs atfp_register=1 + server restart)",
  "xmod_dbg mark <text>     - write a marker line to the log",
  "xmod_dbg flush           - flush the log file now",
  "xmod_dbg hot <n>         - number of samples logged for hot events (0 = none)",
  "log file: addons/xmod/data/" .. CFG.log_file,
}
U.cmd("dbg", "l", function(id, a)
  local sub = (a[1] or "help"):lower()
  if sub == "status" then
    local info = xmod.stats.info()
    U.reply(id, ("xmod_debug: log=data/%s | stats=%s fps=%.1f lua=%.0f KB | atfp mode=%d calls=%d | timeout=%s ms"):format(fname(), tostring(info.on), info.fps, info.lua_kb, atfp.mode, atfp.calls, xmod.cvar.get("xmod_script_timeout")))
    U.reply(id, ("counters: frame=%d prethink=%d postthink=%d cmd_start=%d touch=%d think=%d emit_sound=%d"):format(C.frame, C.prethink, C.postthink, C.cmd_start, C.touch, C.think, C.emit_sound))
  elseif sub == "report" then report("manual report"); flush(); U.reply(id, "report written to data/" .. fname())
  elseif sub == "stats" then
    local v = (a[2] or ""):lower()
    if v == "on" then xmod.stats.enable(true) elseif v == "off" then xmod.stats.enable(false) elseif v == "reset" then xmod.stats.reset() else U.reply(id, "usage: xmod_dbg stats on|off|reset"); return end
    U.reply(id, "stats " .. v)
  elseif sub == "test" then run_tests(id, (a[2] or "") == "timeout"); U.reply(id, "self test started - results in data/" .. fname() .. " in ~2 s")
  elseif sub == "bench" then run_bench(id)
  elseif sub == "mode" then
    local m = tonumber(a[2]); if not m or m < 0 or m > 4 then U.reply(id, "usage: xmod_dbg mode <0-4>"); return end
    if not atfp.set_mode then U.reply(id, "AddToFullPack not installed: set atfp_register=1 in configs/xmod_debug.cfg and restart the server"); return end
    atfp.set_mode(math.floor(m)); log("ATFP", "mode set to %d by %s", atfp.mode, U.name(id)); U.reply(id, "atfp mode = " .. atfp.mode)
  elseif sub == "mark" then log("MARK", "%s", a.text:gsub("^%s*%S+%s*", "")); flush(); U.reply(id, "marked")
  elseif sub == "flush" then flush(); U.reply(id, "flushed")
  elseif sub == "hot" then CFG.hot_sample = tonumber(a[2]) or CFG.hot_sample; seen = {}; U.reply(id, "hot sample count = " .. CFG.hot_sample)
  else for _, l in ipairs(HELP) do U.reply(id, l) end end
end, "xmod debug / benchmark tools (xmod_dbg help)")

log("BOOT", "xmod_debug ready. Try: xmod_dbg test | xmod_dbg bench | xmod_dbg report")
flush()
