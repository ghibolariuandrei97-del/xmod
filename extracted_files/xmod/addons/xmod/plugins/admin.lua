-- Admin Base: loads configs/users.ini, assigns access flags, who/list/reload/add/remove commands.
xmod.register("Admin Base", "0.2", "xmod")
local U = require("util")

xmod.cvar.create("xmod_default_access", "z")          -- access given to everybody (z = normal user)
xmod.cvar.create("xmod_admin_join_msg", "1")          -- announce admins joining

local admins = {}                                     -- list of { auth, pass, access, flags }

local function parse_line(line)
  line = line:gsub("//.*$", ""):gsub("^%s+", "")
  if line == "" or line:sub(1, 1) == ";" then return nil end
  local f = {}
  for q in line:gmatch('"([^"]*)"') do f[#f + 1] = q end
  if #f < 3 then return nil end
  return { auth = f[1], pass = f[2], access = f[3], aflags = f[4] or "", mask = xmod.flags(f[3]) }
end

local function load_admins()
  admins = {}
  local text = xmod.fs.read("configs/users.ini")
  if not text then xmod.error("configs/users.ini not found"); return end
  for line in (text .. "\n"):gmatch("(.-)\r?\n") do
    local a = parse_line(line); if a then admins[#admins + 1] = a end
  end
  xmod.log(("loaded %d admin entries"):format(#admins))
end

local function matches(a, id)
  local fl = a.aflags
  if fl:find("c", 1, true) then return xmod.player.steamid(id) == a.auth end
  if fl:find("d", 1, true) then return xmod.player.ip(id):match("^[^:]+") == a.auth end
  if fl:find("b", 1, true) then return xmod.player.name(id):sub(1, #a.auth) == a.auth end
  return xmod.player.name(id) == a.auth
end

local function apply(id)
  local d = xmod.player.data(id)
  local base = xmod.flags(xmod.cvar.get("xmod_default_access"))
  d.admin = nil
  xmod.player.set_access(id, base)
  if xmod.player.is_bot(id) then return end
  for _, a in ipairs(admins) do
    if matches(a, id) then
      local pw = xmod.player.infokey(id, "_pw")
      if a.aflags:find("e", 1, true) or a.pass == "" or pw == a.pass then
        xmod.player.set_access(id, a.mask | base); d.admin = a
        return true
      elseif a.aflags:find("a", 1, true) then
        xmod.player.kick(id, "Invalid admin password"); return
      end
    end
  end
end

xmod.on("client_putinserver", function(id)
  if apply(id) and xmod.cvar.int("xmod_admin_join_msg") ~= 0 then
    xmod.msg.chat(0, ("{g}[admin] {t}%s {y}joined the server."):format(xmod.player.name(id)))
  end
end)
xmod.on("client_infochanged", function(id, old, new)
  local had = xmod.player.data(id).admin ~= nil
  apply(id)
end)
xmod.on("config", function() load_admins(); for _, id in ipairs(xmod.player.all()) do apply(id) end end)

-- ---- commands ---------------------------------------------------------------------------------------------------
U.cmd("reloadadmins", "h", function(id)
  load_admins(); for _, p in ipairs(xmod.player.all()) do apply(p) end
  U.reply(id, ("Admins reloaded (%d entries)."):format(#admins))
end, "Reload configs/users.ini")

U.cmd("who", "", function(id)
  U.reply(id, "\n  #  name                             userid  steamid               ip               access")
  for _, p in ipairs(xmod.player.all()) do
    U.reply(id, ("%3d  %-32s %-7d %-21s %-16s %s"):format(p, xmod.player.name(p):sub(1, 32), xmod.player.userid(p),
      xmod.player.steamid(p), (xmod.player.ip(p):match("^[^:]+") or ""), U.flags_to_string(xmod.player.access(p))))
  end
  U.reply(id, "")
end, "List players (with access)")

U.cmd("admins", "", function(id)
  U.reply(id, "Admins online:")
  for _, p in ipairs(U.admins()) do U.reply(id, ("  %s  [%s]"):format(xmod.player.name(p), U.flags_to_string(xmod.player.access(p)))) end
end, "List admins online")

-- xmod_addadmin <auth> <access> [password] [account flags]   (appends to users.ini)
U.cmd("addadmin", "h", function(id, args)
  if args.n < 2 then U.reply(id, "usage: xmod_addadmin <steamid|ip|name> <access letters> [password] [account flags]"); return end
  local auth, access, pass, af = args[1], args[2], args[3] or "", args[4]
  if not af then af = auth:match("^STEAM_") and "ce" or (auth:match("^%d+%.%d+%.%d+%.%d+$") and "de" or (pass == "" and "e" or "")) end
  if (auth .. access .. pass .. af):find('["\n\r]') then U.reply(id, "Illegal characters."); return end
  xmod.fs.write("configs/users.ini", ('"%s" "%s" "%s" "%s"\n'):format(auth, pass, access, af), true)
  load_admins(); for _, p in ipairs(xmod.player.all()) do apply(p) end
  U.reply(id, "Admin added: " .. auth)
end, "Add an admin to users.ini")

-- xmod_removeadmin <auth>  (rewrites users.ini without that entry)
U.cmd("removeadmin", "h", function(id, args)
  if args.n < 1 then U.reply(id, "usage: xmod_removeadmin <auth>"); return end
  local text, out, removed = xmod.fs.read("configs/users.ini") or "", {}, 0
  for line in (text .. "\n"):gmatch("(.-)\r?\n") do
    local a = parse_line(line)
    if a and a.auth == args[1] then removed = removed + 1 else out[#out + 1] = line end
  end
  while out[#out] == "" do out[#out] = nil end
  xmod.fs.write("configs/users.ini", table.concat(out, "\n") .. "\n")
  load_admins(); for _, p in ipairs(xmod.player.all()) do apply(p) end
  U.reply(id, ("Removed %d entries for %s"):format(removed, args[1]))
end, "Remove an admin from users.ini")

xmod.export("admin.reload", function() load_admins() end)
