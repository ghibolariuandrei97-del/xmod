-- xmod util library:  local U = require("util")
local U = {}

U.IMMUNITY = 1                    -- access flag 'a'
function U.flag(letter) return 1 << (letter:byte() - 97) end
function U.flags_to_string(n)
  local t = {}
  for i = 0, 25 do if n & (1 << i) ~= 0 then t[#t + 1] = string.char(97 + i) end end
  return table.concat(t)
end

function U.split(s, sep)
  local out = {}
  for piece in (s .. sep):gmatch("(.-)" .. sep:gsub("%p", "%%%0")) do out[#out + 1] = piece end
  return out
end
function U.trim(s) return (s:gsub("^%s+", ""):gsub("%s+$", "")) end
function U.startswith(s, p) return s:sub(1, #p) == p end
function U.clamp(v, lo, hi) return v < lo and lo or (v > hi and hi or v) end
function U.fmt_time(sec)
  sec = math.max(0, math.floor(sec)); return string.format("%d:%02d", sec // 60, sec % 60)
end
function U.shuffle(t)
  for i = #t, 2, -1 do local j = math.random(i); t[i], t[j] = t[j], t[i] end
  return t
end

-- ---- output -------------------------------------------------------------------
function U.name(id) return (id == 0) and "SERVER" or xmod.player.name(id) end
function U.reply(id, text)        -- console reply to a player or to the server console
  if id == 0 then xmod.server_print(text) else xmod.msg.print(id, "console", text) end
end
function U.say(text) xmod.msg.chat(0, text) end

xmod.cvar.create("xmod_show_activity", "2")           -- 0 = silent, 1 = hide admin name, 2 = show admin name
function U.activity(admin, text)
  local mode = xmod.cvar.int("xmod_show_activity")
  if mode <= 0 then return end
  if mode == 1 then U.say("{g}ADMIN{y}: " .. text) else U.say("{g}ADMIN {t}" .. U.name(admin) .. "{y}: " .. text) end
end

function U.log(file, text)
  xmod.fs.write("data/" .. file, os.date("%Y-%m-%d %H:%M:%S") .. "  " .. text .. "\n", true)
end

-- ---- command registration: one handler for client console + server console + amx_ alias --------------
-- handler(id, args): id == 0 when run from the server console. flags = access letters needed by clients.
function U.cmd(name, flags, handler, desc)
  for _, prefix in ipairs({ "xmod_", "amx_" }) do
    xmod.cmd.client(prefix .. name, flags, function(id, args) return handler(id, args) end, desc)
    xmod.cmd.server(prefix .. name, function(args) return handler(0, args) end, desc)
  end
end

-- ---- target selection ------------------------------------------------------------------------------
-- U.target(caller, arg, opts) -> list_of_ids | nil, error_message
-- opts: multi (allow @all/@t/@ct/@spec/@alive/@dead/@bots/@me), alive, dead, nobots, self (allow caller even if immune)
function U.target(caller, arg, o)
  o = o or {}
  if not arg or arg == "" then return nil, "No target given" end
  local function pass(id)
    if not xmod.player.ingame(id) then return false end
    if o.alive and not xmod.player.alive(id) then return false end
    if o.dead and xmod.player.alive(id) then return false end
    if o.nobots and xmod.player.is_bot(id) then return false end
    return true
  end
  local function immune(id) return id ~= caller and (xmod.player.access(id) & U.IMMUNITY) ~= 0 end
  local list = {}
  if arg:sub(1, 1) == "@" then
    if not o.multi then return nil, "Group targets (@all, @t, ...) are not allowed for this command" end
    local g = arg:lower()
    for _, id in ipairs(xmod.player.all()) do
      local team = xmod.player.team(id)
      local match = (g == "@all") or (g == "@t" and team == 1) or (g == "@ct" and team == 2) or (g == "@spec" and team == 3)
        or (g == "@alive" and xmod.player.alive(id)) or (g == "@dead" and not xmod.player.alive(id))
        or (g == "@bots" and xmod.player.is_bot(id)) or (g == "@me" and id == caller)
      if match and pass(id) and not immune(id) then list[#list + 1] = id end
    end
    if #list == 0 then return nil, "No matching players for " .. arg end
    return list
  end
  local id, hits = xmod.player.find(arg)
  if hits == 0 then return nil, "No player matches '" .. arg .. "'" end
  if hits > 1 then return nil, "More than one player matches '" .. arg .. "' - be more specific (or use #userid)" end
  if not pass(id) then return nil, "That player cannot be targeted right now" end
  if immune(id) and not o.self then return nil, "That player has immunity" end
  return { id }
end
function U.target1(caller, arg, o) o = o or {}; o.multi = false; local l, e = U.target(caller, arg, o); return l and l[1], e end

-- ---- misc helpers --------------------------------------------------------------------------------------
function U.each_player(fn) for _, id in ipairs(xmod.player.all()) do fn(id) end end
function U.count_humans() local n = 0; for _, id in ipairs(xmod.player.all()) do if not xmod.player.is_bot(id) then n = n + 1 end end; return n end
function U.is_admin(id) return xmod.player.has_access(id, "c") or (xmod.player.access(id) & ~U.flag("z")) ~= 0 end
function U.admins() local t = {}; for _, id in ipairs(xmod.player.all()) do if U.is_admin(id) then t[#t + 1] = id end end; return t end
function U.timeleft()
  local limit = xmod.cvar.num("mp_timelimit")
  if limit <= 0 then return nil end
  return limit * 60 - xmod.gametime()
end
return U
