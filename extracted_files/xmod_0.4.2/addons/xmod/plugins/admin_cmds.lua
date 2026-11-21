-- Admin Commands: kick/ban/unban/slay/slap/map/cvar/cfg/rcon + fun commands. Every command works from the
-- client console (needs the access flag), the server console, and as both xmod_<name> and amx_<name>.
xmod.register("Admin Commands", "0.2", "xmod")
local U = require("util")

local PROTECTED_CVARS = { rcon_password = true, sv_password = "k", sv_downloadurl = true, sv_cheats = true }

-- ---- helpers ---------------------------------------------------------------------------------------------------------
local function one(id, arg, opts)
  local t, err = U.target1(id, arg, opts)
  if not t then U.reply(id, err) end
  return t
end
local function many(id, arg, opts)
  opts = opts or {}; opts.multi = true
  local list, err = U.target(id, arg, opts)
  if not list then U.reply(id, err) end
  return list
end
local function reason_from(args, i) local t = {}; for k = i, args.n do t[#t + 1] = args[k] end; return table.concat(t, " ") end
local function clean(s) return (s:gsub('[";\n\r]', " ")) end

-- ---- core actions (also exported for the menus) -----------------------------------------------------------------------
local function do_kick(admin, t, reason)
  reason = clean(reason ~= "" and reason or "Kicked by admin")
  U.activity(admin, ("kick {t}%s{y} (%s)"):format(xmod.player.name(t), reason))
  U.log("admin.log", ("%s kicked %s (%s)"):format(U.name(admin), xmod.player.name(t), reason))
  xmod.player.kick(t, reason)
end
local function valid_steam(a) return a and a:match("^STEAM_%d:%d:%d+$") ~= nil end
local function do_ban(admin, t, minutes, reason)
  minutes = math.max(0, math.floor(tonumber(minutes) or 0)); reason = clean(reason ~= "" and reason or "Banned")
  local auth, ip = xmod.player.steamid(t), (xmod.player.ip(t):match("^[^:]+") or "")
  local name = xmod.player.name(t)
  U.activity(admin, ("ban {t}%s{y} for %s (%s)"):format(name, minutes == 0 and "ever" or (minutes .. " min"), reason))
  U.log("admin.log", ("%s banned %s [%s / %s] %d min (%s)"):format(U.name(admin), name, auth, ip, minutes, reason))
  xmod.player.kick(t, ("Banned %s: %s"):format(minutes == 0 and "permanently" or (minutes .. " min"), reason))
  if valid_steam(auth) then xmod.exec(("banid %d %s"):format(minutes, auth)); xmod.exec("writeid")
  elseif ip ~= "" then xmod.exec(("addip %d %s"):format(minutes, ip)); xmod.exec("writeip") end
end
local function do_slay(admin, t) if xmod.player.alive(t) then xmod.player.kill(t); return true end end
local function do_slap(admin, t, dmg)
  if not xmod.player.alive(t) then return end
  local vx, vy, vz = xmod.player.velocity(t)
  xmod.player.set_velocity(t, vx + math.random(-250, 250), vy + math.random(-250, 250), vz + math.random(150, 320))
  local hp = xmod.player.health(t) - (dmg or 0)
  if hp < 1 then xmod.player.kill(t) else xmod.player.set_health(t, hp) end
end
local function do_team(admin, t, team) xmod.player.set_team(t, team) end
local function do_map(admin, map)
  if not xmod.map_valid(map) then U.reply(admin, "Map not found: " .. map); return false end
  U.activity(admin, "change map to {t}" .. map)
  U.log("admin.log", U.name(admin) .. " changed map to " .. map)
  xmod.timer.after(2.0, function() xmod.exec("changelevel " .. map) end)
  return true
end
xmod.export("admin.kick", do_kick); xmod.export("admin.ban", do_ban); xmod.export("admin.slay", do_slay)
xmod.export("admin.slap", do_slap); xmod.export("admin.team", do_team); xmod.export("admin.map", do_map)

-- ---- moderation ---------------------------------------------------------------------------------------------------------
U.cmd("kick", "c", function(id, a) local t = one(id, a[1]); if t then do_kick(id, t, reason_from(a, 2)) end end, "kick <target> [reason]")
U.cmd("ban", "d", function(id, a)
  if a.n < 2 then U.reply(id, "usage: xmod_ban <target> <minutes, 0 = permanent> [reason]"); return end
  local t = one(id, a[1]); if t then do_ban(id, t, a[2], reason_from(a, 3)) end
end, "ban <target> <minutes> [reason]")
U.cmd("addban", "d", function(id, a)    -- offline ban by SteamID / IP
  if a.n < 2 then U.reply(id, "usage: xmod_addban <steamid|ip> <minutes> [reason]"); return end
  local who, min = a[1], math.max(0, math.floor(tonumber(a[2]) or 0))
  if valid_steam(who) then xmod.exec(("banid %d %s"):format(min, who)); xmod.exec("writeid")
  elseif who:match("^%d+%.%d+%.%d+%.%d+$") then xmod.exec(("addip %d %s"):format(min, who)); xmod.exec("writeip")
  else U.reply(id, "Not a valid SteamID or IP"); return end
  U.log("admin.log", ("%s banned %s %d min (%s)"):format(U.name(id), who, min, reason_from(a, 3)))
  U.reply(id, "Banned " .. who)
end, "addban <steamid|ip> <minutes> [reason]")
U.cmd("unban", "d", function(id, a)
  local who = a[1]; if not who then U.reply(id, "usage: xmod_unban <steamid|ip>"); return end
  if valid_steam(who) then xmod.exec("removeid " .. who); xmod.exec("writeid")
  elseif who:match("^%d+%.%d+%.%d+%.%d+$") then xmod.exec("removeip " .. who); xmod.exec("writeip")
  else U.reply(id, "Not a valid SteamID or IP"); return end
  U.log("admin.log", U.name(id) .. " unbanned " .. who); U.reply(id, "Unbanned " .. who)
end, "unban <steamid|ip>")
U.cmd("slay", "e", function(id, a) local l = many(id, a[1], { alive = true }); if not l then return end
  for _, t in ipairs(l) do do_slay(id, t) end
  U.activity(id, ("slay {t}%s"):format(#l == 1 and xmod.player.name(l[1]) or (#l .. " players"))) end, "slay <target|@group>")
U.cmd("slap", "e", function(id, a) local l = many(id, a[1], { alive = true }); if not l then return end
  for _, t in ipairs(l) do do_slap(id, t, tonumber(a[2]) or 0) end
  U.activity(id, ("slap {t}%s{y} with %d damage"):format(#l == 1 and xmod.player.name(l[1]) or (#l .. " players"), tonumber(a[2]) or 0)) end, "slap <target|@group> [damage]")
U.cmd("team", "e", function(id, a)
  local l = many(id, a[1], {}); if not l then return end
  local tn = (a[2] or ""):upper(); local team = ({ T = 1, TERRORIST = 1, CT = 2, SPEC = 3, SPECTATOR = 3 })[tn]
  if not team then U.reply(id, "usage: xmod_team <target|@group> <T|CT|SPEC>"); return end
  for _, t in ipairs(l) do do_team(id, t, team) end
  U.activity(id, ("transfer {t}%s{y} to %s"):format(#l == 1 and xmod.player.name(l[1]) or (#l .. " players"), xmod.player.team_name(team)))
end, "team <target|@group> <T|CT|SPEC>")
U.cmd("swap", "e", function(id, a)
  local t1, t2 = one(id, a[1], { self = true }), one(id, a[2], { self = true }); if not (t1 and t2) then return end
  local a1, a2 = xmod.player.team(t1), xmod.player.team(t2)
  if a1 < 1 or a2 < 1 then U.reply(id, "Both players must be on a team"); return end
  xmod.player.set_team(t1, a2); xmod.player.set_team(t2, a1)
  U.activity(id, ("swap teams of {t}%s{y} and {t}%s"):format(xmod.player.name(t1), xmod.player.name(t2)))
end, "swap <player1> <player2>")
U.cmd("nick", "m", function(id, a)
  local t = one(id, a[1]); if not t or not a[2] then return end
  local nn = clean(reason_from(a, 2)):sub(1, 31)
  U.activity(id, ("rename {t}%s{y} to %s"):format(xmod.player.name(t), nn)); xmod.player.set_infokey(t, "name", nn)
end, "nick <target> <new name>")

-- ---- server --------------------------------------------------------------------------------------------------------------
U.cmd("map", "f", function(id, a) if not a[1] then U.reply(id, "usage: xmod_map <map>"); return end do_map(id, a[1]:gsub("[^%w_%-]", "")) end, "map <mapname>")
U.cmd("restart", "f", function(id, a)
  local s = U.clamp(math.floor(tonumber(a[1]) or 1), 1, 20)
  xmod.exec("sv_restart " .. s); U.activity(id, ("restart the game in %d s"):format(s))
end, "restart [seconds]")
U.cmd("cvar", "g", function(id, a)
  local name = a[1]; if not name or name:find("[^%w_%.%-]") then U.reply(id, "usage: xmod_cvar <cvar> [value]"); return end
  if not xmod.cvar.exists(name) then U.reply(id, "Unknown cvar: " .. name); return end
  local p = PROTECTED_CVARS[name:lower()]
  if p and (p == true or (id ~= 0 and not xmod.player.has_access(id, p))) and id ~= 0 then U.reply(id, "That cvar is protected."); return end
  if a.n < 2 then U.reply(id, ('"%s" is "%s"'):format(name, xmod.cvar.get(name))); return end
  local v = clean(reason_from(a, 2))
  xmod.cvar.set(name, v); U.log("admin.log", ("%s set %s to %s"):format(U.name(id), name, v))
  U.reply(id, ('Cvar "%s" changed to "%s"'):format(name, v)); U.activity(id, ("set cvar {t}%s{y} to {t}%s"):format(name, p and "***" or v))
end, "cvar <name> [value]")
U.cmd("cfg", "h", function(id, a)
  local f = a[1]; if not f or f:find("[^%w_%./%-]") or f:find("..", 1, true) then U.reply(id, "usage: xmod_cfg <file.cfg>"); return end
  xmod.exec("exec " .. f); U.reply(id, "Executing " .. f)
end, "cfg <file>")
U.cmd("rcon", "l", function(id, a)
  if a.n < 1 then U.reply(id, "usage: xmod_rcon <command>"); return end
  local c = reason_from(a, 1); if c:lower():find("rcon_password", 1, true) and id ~= 0 then U.reply(id, "Denied."); return end
  U.log("admin.log", ("%s rcon: %s"):format(U.name(id), c)); xmod.exec(c); U.reply(id, "Command sent.")
end, "rcon <server command>")

-- ---- fun ------------------------------------------------------------------------------------------------------------------
local function each(id, a, opts, fn, what)
  local l = many(id, a[1], opts); if not l then return end
  for _, t in ipairs(l) do fn(t) end
  if what then U.activity(id, what:format(#l == 1 and xmod.player.name(l[1]) or (#l .. " players"))) end
end
U.cmd("hp", "m", function(id, a) local v = U.clamp(tonumber(a[2]) or 100, 1, 10000)
  each(id, a, { alive = true }, function(t) xmod.player.set_health(t, v) end, "set health of {t}%s{y} to " .. v) end, "hp <target|@group> <amount>")
U.cmd("armor", "m", function(id, a) local v = U.clamp(tonumber(a[2]) or 100, 0, 999)
  each(id, a, { alive = true }, function(t) xmod.player.set_armor(t, v) end, "set armor of {t}%s{y} to " .. v) end, "armor <target|@group> <amount>")
U.cmd("money", "m", function(id, a) local v = U.clamp(math.floor(tonumber(a[2]) or 0), 0, 16000)
  each(id, a, {}, function(t) xmod.player.set_money(t, v) end, "set money of {t}%s{y} to $" .. v) end, "money <target|@group> <amount>")
U.cmd("god", "m", function(id, a) local on = tonumber(a[2]) ~= 0
  each(id, a, { alive = true }, function(t) xmod.ent.set(t, "takedamage", on and 0 or 2) end, "%s: godmode " .. (on and "ON" or "OFF")) end, "god <target|@group> [1|0]")
U.cmd("noclip", "m", function(id, a) local on = tonumber(a[2]) ~= 0
  each(id, a, { alive = true }, function(t) xmod.ent.set(t, "movetype", on and xmod.MOVETYPE_NOCLIP or xmod.MOVETYPE_WALK) end, "%s: noclip " .. (on and "ON" or "OFF")) end, "noclip <target|@group> [1|0]")
U.cmd("freeze", "m", function(id, a) local on = tonumber(a[2]) ~= 0
  each(id, a, { alive = true }, function(t)
    local f = xmod.ent.get(t, "flags")
    xmod.ent.set(t, "flags", on and (f | xmod.FL_FROZEN) or (f & ~xmod.FL_FROZEN))
    if on then xmod.player.set_velocity(t, 0, 0, 0) end
  end, "%s: freeze " .. (on and "ON" or "OFF")) end, "freeze <target|@group> [1|0]")
U.cmd("blind", "m", function(id, a) local s = U.clamp(tonumber(a[2]) or 5, 1, 30)
  each(id, a, { alive = true }, function(t) xmod.msg.fade(t, s, s, 0, 255, 255, 255, 255) end, "blind {t}%s{y} for " .. s .. " s") end, "blind <target|@group> [seconds]")
U.cmd("give", "m", function(id, a)
  local item = a[2] or ""
  if not (item:match("^weapon_%w+$") or item:match("^item_%w+$") or item:match("^ammo_%w+$")) then U.reply(id, "usage: xmod_give <target|@group> <weapon_*|item_*|ammo_*>"); return end
  each(id, a, { alive = true }, function(t) xmod.player.give_item(t, item) end, "give {t}" .. item .. "{y} to %s") end, "give <target|@group> <item>")
U.cmd("teleport", "m", function(id, a)
  local x, y, z = tonumber(a[2]), tonumber(a[3]), tonumber(a[4])
  if not (x and y and z) then U.reply(id, "usage: xmod_teleport <target> <x> <y> <z>"); return end
  local t = one(id, a[1], { alive = true }); if t then xmod.player.set_origin(t, x, y, z); U.activity(id, "teleport {t}" .. xmod.player.name(t)) end
end, "teleport <target> <x> <y> <z>")
U.cmd("bring", "m", function(id, a)
  if id == 0 then U.reply(id, "Players only."); return end
  local t = one(id, a[1], { alive = true }); if not t then return end
  local x, y, z = xmod.player.origin(id); xmod.player.set_origin(t, x + 40, y, z + 8); U.activity(id, "bring {t}" .. xmod.player.name(t))
end, "bring <target>")
U.cmd("goto", "m", function(id, a)
  if id == 0 then U.reply(id, "Players only."); return end
  local t = one(id, a[1], { alive = true, self = true }); if not t then return end
  local x, y, z = xmod.player.origin(t); xmod.player.set_origin(id, x + 40, y, z + 8)
end, "goto <target>")
