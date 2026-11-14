-- Minimal mock of xmod 0.4 + the xmod_bots extension, to run xbots.lua outside a server.
local M = { t = 0, logs = {}, errors = {}, cmds = {}, said = {}, exec = {} }
local ROOT = ...

local handlers, exports = {}, {}
local timers, tseq = {}, 0
local cvars = {}
local players = {}      -- [id] = {name, team, alive, bot, x,y,z, money, hp, weapons, steamid, flags, vel}
local MAXP = 16

local function log(s) M.logs[#M.logs + 1] = s; if s:find("error") then M.errors[#M.errors + 1] = s end end

xmod = {}
x = xmod
xmod.version = "0.4.0"
xmod.FL_ONGROUND, xmod.FL_DUCKING, xmod.EF_NODRAW, xmod.MOVETYPE_FLY = 512, 16384, 128, 5
xmod.TRACE_IGNORE_MONSTERS = 1
function xmod.register() end
function xmod.on(ev, fn) handlers[ev] = handlers[ev] or {}; table.insert(handlers[ev], fn) end
function xmod.emit(ev, ...)
  for _, fn in ipairs(handlers[ev] or {}) do
    local r = fn(...)
    if r == true then return true end
  end
  return nil
end
function xmod.export(n, fn) exports[n] = fn end
function xmod.call(n, ...) return exports[n](...) end
function xmod.has_export(n) return exports[n] ~= nil end
function xmod.gametime() return M.t end
function xmod.time() return M.t end
function xmod.log(s) log(s) end
function xmod.error(s) log("error: " .. s) end
function xmod.exec(s) M.exec[#M.exec + 1] = s end
function xmod.map() return "de_dust2" end
function xmod.maxplayers() return MAXP end

xmod.timer = {}
local function addt(delay, every, count, fn, ...)
  tseq = tseq + 1
  timers[tseq] = { at = M.t + delay, every = every, count = count, fn = fn, args = table.pack(...) }
  return tseq
end
function xmod.timer.after(d, fn, ...) return addt(d, nil, 1, fn, ...) end
function xmod.timer.every(d, fn, ...) return addt(d, d, -1, fn, ...) end
function xmod.timer.cancel(i) timers[i] = nil end

xmod.cvar = {}
function xmod.cvar.create(n, v) if cvars[n] == nil then cvars[n] = tostring(v) end end
function xmod.cvar.get(n) return cvars[n] or "" end
function xmod.cvar.num(n) return tonumber(cvars[n] or "0") or 0 end
function xmod.cvar.int(n) return math.floor(tonumber(cvars[n] or "0") or 0) end
function xmod.cvar.set(n, v) cvars[n] = tostring(v) end
cvars.mp_freezetime = "3"

xmod.player = {}
local PL = xmod.player
local function need(id) local p = players[id]; if not p then error("invalid player " .. tostring(id), 3) end; return p end
function PL.connected(id) return players[id] ~= nil end
function PL.all() local l = {}; for i = 1, MAXP do if players[i] then l[#l + 1] = i end end; return l end
function PL.name(id) return need(id).name end
function PL.steamid(id) return need(id).bot and "BOT" or ("STEAM_0:0:" .. id) end
function PL.is_bot(id) return need(id).bot end
function PL.alive(id) return need(id).alive end
function PL.team(id) return need(id).team end
function PL.origin(id) local p = need(id); return p.x, p.y, p.z end
function PL.health(id) return need(id).hp end
function PL.armor(id) return 0 end
function PL.money(id) return need(id).money end
function PL.has_access(id, f) return id == 1 end
function PL.find(s) for i, p in pairs(players) do if p.name:lower():find(s:lower(), 1, true) then return i, 1 end end; return nil, 0 end
function PL.view_angles(id) return 0, 0, 0 end

xmod.ent = {}
function xmod.ent.get(e, f)
  local p = players[e]
  if p then
    if f == "weapons" then return p.weapons end
    if f == "flags" then return p.flags end
    if f == "velocity" then return p.vx, p.vy, 0 end
    if f == "fov" then return 90 end
    if f == "view_ofs" then return 0, 0, 17 end
    if f == "movetype" then return 3 end
    if f == "origin" then return p.x, p.y, p.z end
  end
  if f == "absmin" then return -100, -100, 0 end
  if f == "absmax" then return 100, 100, 100 end
  if f == "origin" then return 0, 0, 0 end
  if f == "model" then return "" end
  if f == "health" or f == "effects" then return 0 end
  return 0
end
local ENTS = { func_bomb_target = { 100, 101 }, info_player_deathmatch = { 102 }, info_player_start = { 103 } }
function xmod.ent.find(start, cls)
  local l = ENTS[cls] or {}
  for _, e in ipairs(l) do if e > start then return e end end
  return 0
end
xmod.msg = { ONE_UNRELIABLE = 8, TEMPENTITY = 23 }
function xmod.msg.chat() end
function xmod.msg.hud() end
for _, n in ipairs({ "begin", "byte", "coord", "short", "finish" }) do xmod.msg[n] = function() end end
xmod.precache = { model = function() return 5 end }
xmod.trace = { line = function(x1, y1, z1, x2, y2, z2) return { fraction = 1, x = x2, y = y2, z = z2 } end }
xmod.reapi = { available = function() return false end }
xmod.ext = { available = function(ns, fn) return ns == "bot" end }
xmod.fs = { read = function(rel) local f = io.open(ROOT .. "/addons/xmod/" .. rel); if not f then return nil end; local s = f:read("a"); f:close(); return s end }

local vaults = {}
xmod.vault = { open = function(name)
  local data = vaults[name] or {}
  vaults[name] = data
  local v = {}
  function v:get(k, d) if data[k] == nil then return d end; return data[k] end
  function v:set(k, val) data[k] = val end
  function v:has(k) return data[k] ~= nil end
  function v:remove(k) data[k] = nil end
  function v:keys(prefix) local l = {}; for k in pairs(data) do if not prefix or k:sub(1, #prefix) == prefix then l[#l + 1] = k end end; table.sort(l); return l end
  return v
end }
M.vaults = vaults

local cmdreg = { client = {}, server = {}, say = {} }
xmod.cmd = {
  client = function(n, f, fn) cmdreg.client[n] = fn end,
  server = function(n, fn) cmdreg.server[n] = fn end,
  say = function(n, f, fn) cmdreg.say[n] = fn end,
}
M.cmdreg = cmdreg

-- ---------------------------------------------------------------------------------------------
-- xmod.bot mock
local Bt = {}
local bstate = {}
xmod.bot = Bt
local function needb(id) if not bstate[id] then error("xmod.bot: not a bot " .. tostring(id), 3) end; return bstate[id] end
function Bt.add(name)
  for i = 1, MAXP do
    if not players[i] then
      players[i] = { name = name, team = 0, alive = false, bot = true, x = 0, y = 0, z = 36, money = 800, hp = 100,
                     weapons = (1 << 29) | (1 << 16), flags = 512, vx = 0, vy = 0 }
      bstate[i] = { moving = false, look = 0 }
      xmod.emit("client_connect", i, name, "127.0.0.1")
      return i
    end
  end
  return "server is full"
end
function Bt.remove(id) if bstate[id] then M.disconnect(id); return true end return false end
function Bt.is_bot(id) return bstate[id] ~= nil end
function Bt.list() local l = {}; for i in pairs(bstate) do l[#l + 1] = i end; return l end
function Bt.cmd(id, line)
  needb(id)
  M.cmds[#M.cmds + 1] = players[id].name .. ": " .. line
  for c in line:gmatch("[^;]+") do
    local a, b = c:match("^(%S+)%s*(%S*)")
    if a == "jointeam" or a == "menuselect" then
      local t = tonumber(b); if t == 5 then t = (id % 2) + 1 end
      if t == 1 or t == 2 then players[id].team = t end
    end
  end
end
function Bt.say(id, text, team) needb(id); M.said[#M.said + 1] = players[id].name .. (team and " (team)" or "") .. ": " .. text end
function Bt.set(id, t) needb(id); bstate[id].prof = t end
function Bt.go(id, x, y, z, o) local s = needb(id); s.moving = true; s.goal = { x, y, z }; return 5 end
function Bt.stop(id) needb(id).moving = false end
function Bt.strafe(id) needb(id) end
function Bt.look(id, x, y, z) assert(type(z) == "number"); needb(id).look = 1 end
function Bt.look_ent(id, e, part) needb(id).look = 2; assert(part == "head" or part == "body" or part == "feet") end
function Bt.look_path(id) needb(id).look = 0 end
function Bt.press(id) needb(id) end
function Bt.hold(id) needb(id) end
function Bt.release(id) needb(id) end
function Bt.fire(id, mode) needb(id).fire = mode end
function Bt.state(id) local s = needb(id); return { moving = s.moving, look = s.look, path_left = 3 } end
function Bt.scan(id)
  needb(id)
  local me = players[id]
  local out = {}
  for i, p in pairs(players) do
    if i ~= id and p.alive and p.team ~= me.team and (p.team == 1 or p.team == 2) then
      local d = math.sqrt((p.x - me.x) ^ 2 + (p.y - me.y) ^ 2)
      if d < 1500 then out[#out + 1] = { id = i, dist = d, part = "head" } end
    end
  end
  return out
end
function Bt.visible() return "head" end
function Bt.see_point() return true end
function Bt.weapon(id) if not players[id] then return nil end; return { name = "usp", clip = math.random(0, 12), wid = 16, ent = 50 } end
function Bt.config() end
local navnodes = {}
function Bt.nav_info() return { nodes = #navnodes, links = 0, analyzing = false, progress = 1, dirty = false, autolearn = true, max_nodes = 6000 } end
function Bt.nav_analyze() M.analyzed = true; xmod.timer.after(1, function()
  for i = 1, 20 do navnodes[i] = { x = i * 64, y = 0, z = 36, flags = (i > 15) and 8 or 0, radius = 32, links = {}, danger_t = 0, danger_ct = 0, value_t = 0, value_ct = 0 } end
  xmod.emit("bot_nav_done", 20, 38) end); return true end
function Bt.nav_save() return true end
function Bt.nav_load() return true end
function Bt.nav_clear() navnodes = {} end
function Bt.nav_find(f) local l = {}; for i, n in ipairs(navnodes) do if (n.flags & f) == f then l[#l + 1] = i end end; return l end
function Bt.nav_node(i) return navnodes[i] end
function Bt.nav_near() local l = {}; for i in ipairs(navnodes) do l[#l + 1] = i end; return l end
function Bt.nav_nearest() return #navnodes > 0 and 1 or 0 end
function Bt.nav_best() return #navnodes > 0 and math.random(#navnodes) or 0 end
function Bt.nav_random() return #navnodes > 0 and math.random(#navnodes) or 0 end
function Bt.nav_learn(x, y, z, team, kind) assert(kind == "death" or kind == "kill") end
function Bt.nav_path(a, b) return { a, b } end
function Bt.nav_add() navnodes[#navnodes + 1] = { x = 0, y = 0, z = 0, flags = 0, radius = 32, links = {}, danger_t = 0, danger_ct = 0, value_t = 0, value_ct = 0 }; return #navnodes end
function Bt.nav_link() return true end
function Bt.nav_flags(i, f) if f then navnodes[i].flags = f end; return navnodes[i].flags end
function Bt.nav_remove(i) table.remove(navnodes, i) end
function Bt.list() local l = {}; for i in pairs(bstate) do l[#l + 1] = i end; return l end

-- ---------------------------------------------------------------------------------------------
-- libraries
local util = {}
function util.trim(s) return (s:gsub("^%s+", ""):gsub("%s+$", "")) end
function util.reply(id, s) M.logs[#M.logs + 1] = "reply> " .. s end
local menu = { show = function(id, m) M.menu = m end }
local json = {}
function json.encode(v)
  local t = type(v)
  if t == "table" then
    if #v > 0 then local p = {}; for _, x in ipairs(v) do p[#p + 1] = json.encode(x) end; return "[" .. table.concat(p, ",") .. "]" end
    local p = {}; for k, x in pairs(v) do p[#p + 1] = string.format("%q:%s", tostring(k), json.encode(x)) end; return "{" .. table.concat(p, ",") .. "}"
  elseif t == "string" then return string.format("%q", v) else return tostring(v) end
end
function json.decode(s)
  local f = load("return " .. s:gsub('"([^"]-)":', '["%1"]='):gsub("%[", "{"):gsub("%]", "}"))
  return f and f() or nil
end
package.preload.util = function() return util end
package.preload.menu = function() return menu end
package.preload.json = function() return json end

-- ---------------------------------------------------------------------------------------------
-- driver helpers
function M.human(id, name, team)
  players[id] = { name = name, team = team, alive = true, bot = false, x = 500, y = 300, z = 36, money = 800, hp = 100,
                  weapons = (1 << 29) | (1 << 17), flags = 512, vx = 200, vy = 0 }
  xmod.emit("client_connect", id, name, "1.2.3.4")
  xmod.emit("client_putinserver", id)
end
function M.disconnect(id)
  xmod.emit("client_disconnect", id)
  players[id], bstate[id] = nil, nil
end
function M.spawn_all()
  for i, p in pairs(players) do
    if p.team == 1 or p.team == 2 then
      p.alive, p.hp = true, 100
      p.x = (p.team == 1 and -800 or 800) + math.random(-300, 300); p.y = math.random(-300, 300)
      if p.bot and i % 3 == 0 then p.weapons = p.weapons | (1 << 6) end   -- some T carries c4
    end
  end
end
function M.run(seconds, step)
  step = step or 0.05
  local stop = M.t + seconds
  while M.t < stop do
    M.t = M.t + step
    -- move bots a bit towards their goal
    for i, s in pairs(bstate) do
      local p = players[i]
      if p and p.alive and s.moving and s.goal then
        local dx, dy = s.goal[1] - p.x, s.goal[2] - p.y
        local d = math.sqrt(dx * dx + dy * dy)
        if d < 20 then s.moving = false; xmod.emit("bot_arrived", i)
        else p.x, p.y = p.x + dx / d * 12, p.y + dy / d * 12 end
      end
    end
    local due = {}
    for k, tm in pairs(timers) do if tm.at <= M.t then due[#due + 1] = k end end
    table.sort(due)
    for _, k in ipairs(due) do
      local tm = timers[k]
      if tm then
        local ok, r = pcall(tm.fn, table.unpack(tm.args, 1, tm.args.n))
        if not ok then log("timer error: " .. tostring(r)) end
        if tm.every and r ~= false then tm.at = tm.at + tm.every else timers[k] = nil end
      end
    end
  end
end
M.players, M.handlers, M.cvars, M.bstate = players, handlers, cvars, bstate
return M
