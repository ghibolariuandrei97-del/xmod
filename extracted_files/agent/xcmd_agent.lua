-- xcmd live agent: feeds `xcmd monitor` and `xcmd map` (installed by xcmd, removable with `xcmd remove agent`).
--   * writes addons/xmod/data/xcmd_live.json a few times per second (local xcmd reads it: no network, no rcon)
--   * answers the server command `xcmd_dump` with the same JSON (remote xcmd asks through rcon)
--   * publishes the FCVAR_SERVER cvar xmod_version so server browsers (xcmd browser) can see the server runs xmod
-- cvars: xcmd_live_rate 4 (writes per second, 0 = off) · xcmd_live_stats 1 (xmod.stats for fps / frame time)
plugin = { name = "xcmd_agent", version = "1.0.1", author = "xcmd" }
local P, E = xmod.player, xmod.ent

pcall(xmod.cvar.create, "xmod_version", xmod.version, xmod.cvar.SERVER)
pcall(xmod.cvar.set, "xmod_version", xmod.version)
xmod.cvar.create("xcmd_live_rate", "4")
xmod.cvar.create("xcmd_live_stats", "1")

-- ---- tiny JSON encoder
local function esc(s)
  return '"' .. tostring(s):gsub('[%c"\\]', function(c)
    if c == '"' then return '\\"' elseif c == "\\" then return "\\\\" elseif c == "\n" then return "\\n" end
    return string.format("\\u%04x", c:byte())
  end) .. '"'
end
local enc
local function arr(t) local o = {} for i = 1, #t do o[i] = enc(t[i]) end return "[" .. table.concat(o, ",") .. "]" end
enc = function(v)
  local tv = type(v)
  if tv == "nil" then return "null" elseif tv == "boolean" then return v and "true" or "false"
  elseif tv == "number" then
    if v ~= v or v == math.huge or v == -math.huge then return "0" end
    if math.type and math.type(v) == "integer" then return tostring(v) end
    return string.format("%.1f", v)
  elseif tv == "string" then return esc(v)
  elseif tv == "table" then
    if #v > 0 or next(v) == nil then return arr(v) end
    local o = {} for k, x in pairs(v) do o[#o + 1] = esc(k) .. ":" .. enc(x) end
    return "{" .. table.concat(o, ",") .. "}"
  end
  return "null"
end

-- ---- state collected from game messages
local weapon = {}            -- id -> active weapon (CSW_*)
local deaths = {}            -- id -> deaths
local score = { ct = 0, t = 0 }
local round_secs, round_at = -1, 0
local kills = {}             -- last kills
local bomb = nil
xmod.on("message:CurWeapon", function(_, _, ent, a) if ent and a[1] == 1 then weapon[ent] = a[2] end end)
xmod.on("message:ScoreInfo", function(_, _, _, a) if a[1] then deaths[a[1]] = a[3] end end)
xmod.on("message:TeamScore", function(_, _, _, a)
  local v = math.floor(tonumber(a[2]) or 0)
  if a[1] == "CT" then score.ct = v elseif a[1] == "TERRORIST" then score.t = v end
end)
xmod.on("message:RoundTime", function(_, _, _, a) round_secs, round_at = a[1] or -1, xmod.gametime() end)
xmod.on("message:DeathMsg", function(_, _, _, a)
  local k, v = a[1], a[2]
  local function who(id) if id and id > 0 and P.connected(id) then return P.name(id), P.team(id) end return "", 0 end
  local kn, kt = who(k); local vn, vt = who(v)
  table.insert(kills, { math.floor(xmod.gametime()), kn, vn, kt, vt, (a[3] or 0) == 1, a[4] or "" })
  while #kills > 8 do table.remove(kills, 1) end
end)
xmod.on("client_disconnect", function(id) weapon[id] = nil; deaths[id] = nil end)
xmod.on("map_end", function() kills = {}; score = { ct = 0, t = 0 } end)

local function snapshot()
  local players = {}
  for _, id in ipairs(P.all()) do
    if P.ingame(id) then
      local x, y, z = P.origin(id)
      local _, yaw = P.view_angles(id)
      local alive = P.alive(id)
      local function int(v) return math.floor(tonumber(v) or 0) end
      players[#players + 1] = { id, P.name(id), P.team(id), alive, int(P.health(id)), int(P.armor(id)), int(P.money(id)),
        int(P.frags(id)), int(deaths[id]), int(P.ping(id)), int(x), int(y), int(z), int(yaw),
        int(weapon[id]), int(E.get(id, "weapons")), P.is_bot(id) }
    end
  end
  local left = -1
  if round_secs >= 0 then left = math.max(0, math.floor(round_secs - (xmod.gametime() - round_at))) end
  local d = { v = 1, agent = plugin.version, xmod = xmod.version, host = xmod.cvar.get("hostname"), map = xmod.map(),
    max = xmod.maxplayers(), time = math.floor(xmod.time()), gt = xmod.gametime(), left = left, ct = score.ct, t = score.t,
    dl = xmod.cvar.get("sv_downloadurl"), p = players, k = kills }
  if xmod.cvar.int("xcmd_live_stats") == 1 then
    local ok, s = pcall(xmod.stats.info)
    if ok and s then d.fps, d.fms, d.fmax, d.lua = s.fps, s.frame_ms_avg, s.frame_ms_max, s.lua_kb end
  end
  return enc(d)
end

-- printed in pieces: the engine's console print buffer is a few KB, a full server's JSON can be bigger
xmod.cmd.server("xcmd_dump", function()
  local s = "XCMD" .. snapshot()
  for i = 1, #s, 900 do xmod.server_print(s:sub(i, i + 899)) end
  xmod.server_print("\n")
end)

local acc = 0
xmod.on("init", function() if xmod.cvar.int("xcmd_live_stats") == 1 then pcall(xmod.stats.enable, true) end end)
xmod.timer.every(0.05, function()
  local rate = xmod.cvar.int("xcmd_live_rate")
  if rate <= 0 then return end
  acc = acc + 0.05
  if acc + 1e-6 < 1 / rate then return end
  acc = 0
  xmod.fs.write("data/xcmd_live.json", snapshot())
end)
