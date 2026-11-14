-- xbots_editor.lua - navigation editor for xmod bots (uses only the public xmod.bot.* API)
-- say /botnav  (menu)  ·  console: xbots_nav <show|add|del|from|to|unlink|flag|radius|info|path|go|bots_here>
-- Nodes are drawn with beams around you: green normal, blue crouch, purple ladder, red goal (bomb site),
-- yellow camp/sniper, cyan learned from players, white = links (orange = jump link).
xmod.register("xbots_editor", "1.0.0", "Andrew & Claude")

local U = require("util")
local menu = require("menu")
local B, P = xmod.bot, xmod.player

local NF = { crouch = 1, ladder = 2, camp = 4, goal = 8, rescue = 16, tspawn = 32, ctspawn = 64, sniper = 128,
             hostage = 256, avoid = 512, door = 1024, learned = 2048, jump = 4096 }
local LF = { jump = 1, crouch = 2, ladder = 4, drop = 8 }

local beam = 0
local show = {}       -- [id] = true while drawing
local mark = {}       -- [id] = node marked with "from"
local path_show = {}  -- [id] = { nodes, expire }

xmod.on("precache", function() beam = xmod.precache.model("sprites/laserbeam.spr") end)

local function flag_names(f)
  local t = {}
  for n, v in pairs(NF) do if (f & v) ~= 0 then t[#t + 1] = n end end
  table.sort(t)
  return #t > 0 and table.concat(t, ",") or "-"
end

local function can(id) return id == 0 or P.has_access(id, xmod.cvar.get("xbots_admin_flag") ~= "" and xmod.cvar.get("xbots_admin_flag") or "h") end
local function ok() return xmod.ext.available("bot") end

local function my_pos(id)
  local x, y, z = P.origin(id)
  if (xmod.ent.get(id, "flags") & xmod.FL_DUCKING) ~= 0 then z = z + 18 end   -- store standing height
  return x, y, z
end

local function aim_point(id)
  local x, y, z = P.origin(id)
  local _, _, oz = xmod.ent.get(id, "view_ofs")
  z = z + (oz or 17)
  local pitch, yaw = P.view_angles(id)
  local p, yw = math.rad(pitch), math.rad(yaw)
  local fx, fy, fz = math.cos(p) * math.cos(yw), math.cos(p) * math.sin(yw), -math.sin(p)
  local tr = xmod.trace.line(x, y, z, x + fx * 4096, y + fy * 4096, z + fz * 4096, xmod.TRACE_IGNORE_MONSTERS, id)
  return tr.x, tr.y, tr.z + 36
end

local function te_beam(id, x1, y1, z1, x2, y2, z2, r, g, b, width, life)
  if beam == 0 then return end
  xmod.msg.begin(xmod.msg.ONE_UNRELIABLE, xmod.msg.TEMPENTITY, 0, 0, 0, id)
  xmod.msg.byte(0)                                     -- TE_BEAMPOINTS
  xmod.msg.coord(x1); xmod.msg.coord(y1); xmod.msg.coord(z1)
  xmod.msg.coord(x2); xmod.msg.coord(y2); xmod.msg.coord(z2)
  xmod.msg.short(beam)
  xmod.msg.byte(0); xmod.msg.byte(0)                   -- start frame, frame rate
  xmod.msg.byte(life or 6)                             -- life in 0.1 s
  xmod.msg.byte(width or 10)
  xmod.msg.byte(0)                                     -- noise
  xmod.msg.byte(r); xmod.msg.byte(g); xmod.msg.byte(b)
  xmod.msg.byte(200); xmod.msg.byte(0)                 -- brightness, scroll
  xmod.msg.finish()
end

local function node_color(f)
  if (f & NF.goal) ~= 0 then return 255, 40, 40 end
  if (f & NF.ladder) ~= 0 then return 180, 60, 255 end
  if (f & NF.crouch) ~= 0 then return 60, 120, 255 end
  if (f & (NF.camp | NF.sniper)) ~= 0 then return 255, 220, 0 end
  if (f & NF.avoid) ~= 0 then return 90, 90, 90 end
  if (f & NF.learned) ~= 0 then return 0, 220, 220 end
  return 0, 255, 0
end

local function draw(id)
  local x, y, z = P.origin(id)
  local near = B.nav_near(x, y, z, 700)
  table.sort(near, function(a, b)
    local na, nb = B.nav_node(a), B.nav_node(b)
    return (na.x - x) ^ 2 + (na.y - y) ^ 2 < (nb.x - x) ^ 2 + (nb.y - y) ^ 2
  end)
  local inset, beams = {}, 0
  for i = 1, math.min(#near, 30) do inset[near[i]] = true end
  for i = 1, math.min(#near, 30) do
    local n = near[i]
    local nd = B.nav_node(n)
    local r, g, b = node_color(nd.flags)
    te_beam(id, nd.x, nd.y, nd.z - 36, nd.x, nd.y, nd.z + 36, r, g, b, 12)
    beams = beams + 1
    for _, l in ipairs(nd.links) do
      if inset[l.to] and beams < 70 then
        local o = B.nav_node(l.to)
        local jr = (l.flags & LF.jump) ~= 0
        te_beam(id, nd.x, nd.y, nd.z - 10, o.x, o.y, o.z - 10, 255, jr and 140 or 255, jr and 0 or 255, 4)
        beams = beams + 1
      end
    end
  end
  -- nearest node info
  local n = B.nav_nearest(x, y, z, 120)
  local info = B.nav_info()
  local txt = string.format("nav %s: %d nodes%s", xmod.map(), info.nodes, info.dirty and " (unsaved)" or "")
  if n > 0 then
    local nd = B.nav_node(n)
    txt = txt .. string.format("\nnode #%d  flags %s  radius %d  links %d\ndanger T %.2f CT %.2f  value T %.2f CT %.2f",
      n, flag_names(nd.flags), nd.radius, #nd.links, nd.danger_t, nd.danger_ct, nd.value_t, nd.value_ct)
  end
  if mark[id] then txt = txt .. "\nmarked: #" .. mark[id] end
  xmod.msg.hud(id, txt, { r = 0, g = 200, b = 255, x = 0.02, y = 0.25, hold = 0.6, fadein = 0, fadeout = 0, channel = 4 })
  -- a path being shown
  local ps = path_show[id]
  if ps and xmod.gametime() < ps.expire then
    for i = 1, #ps.nodes - 1 do
      local a, b2 = B.nav_node(ps.nodes[i]), B.nav_node(ps.nodes[i + 1])
      if a and b2 then te_beam(id, a.x, a.y, a.z + 5, b2.x, b2.y, b2.z + 5, 255, 0, 0, 20) end
    end
  end
end

xmod.timer.every(0.5, function()
  if not ok() then return end
  for id in pairs(show) do
    if P.connected(id) then draw(id) else show[id] = nil end
  end
end)

local function nearest_here(id, r)
  local x, y, z = my_pos(id)
  return B.nav_nearest(x, y, z, r or 80)
end

local function cmd(id, args)
  if id == 0 then U.reply(0, "[xbots_nav] use it in game"); return end
  if not can(id) then U.reply(id, "[xbots_nav] no access"); return end
  if not ok() then U.reply(id, "[xbots_nav] the xmod_bots extension is not loaded"); return end
  local sub = (args[1] or "show"):lower()
  if sub == "show" then
    show[id] = not show[id] or nil
    U.reply(id, "[xbots_nav] drawing " .. (show[id] and "on" or "off"))
  elseif sub == "add" then
    local x, y, z = my_pos(id)
    local f = (xmod.ent.get(id, "flags") & xmod.FL_DUCKING) ~= 0 and NF.crouch or 0
    if xmod.ent.get(id, "movetype") == xmod.MOVETYPE_FLY then f = f | NF.ladder end
    for i = 2, args.n do f = f | (NF[args[i]:lower()] or 0) end
    local n = B.nav_add(x, y, z, f)
    local linked = 0
    for _, o in ipairs(B.nav_near(x, y, z, 140)) do
      if o ~= n then
        local nd = B.nav_node(o)
        local tr = xmod.trace.line(x, y, z, nd.x, nd.y, nd.z, xmod.TRACE_IGNORE_MONSTERS, id)
        if tr.fraction >= 1 and math.abs(nd.z - z) <= 40 then B.nav_link(n, o, 0, true); linked = linked + 1 end
      end
    end
    U.reply(id, string.format("[xbots_nav] node #%d added (%s), %d links", n, flag_names(f), linked))
  elseif sub == "del" then
    local n = nearest_here(id, 64)
    if n > 0 then B.nav_remove(n); mark[id] = nil; U.reply(id, "[xbots_nav] node removed") else U.reply(id, "[xbots_nav] no node here") end
  elseif sub == "from" then
    mark[id] = nearest_here(id, 80)
    U.reply(id, mark[id] > 0 and ("[xbots_nav] marked #" .. mark[id]) or "[xbots_nav] no node here")
    if mark[id] == 0 then mark[id] = nil end
  elseif sub == "to" or sub == "unlink" then
    local a, b = mark[id], nearest_here(id, 80)
    if not a or b == 0 or a == b then U.reply(id, "[xbots_nav] mark a node with 'from', then stand on the other one"); return end
    if sub == "unlink" then B.nav_unlink(a, b, true); U.reply(id, "[xbots_nav] unlinked #" .. a .. " <-> #" .. b); return end
    local f, both = 0, false
    for i = 2, args.n do local w = args[i]:lower(); if w == "both" then both = true else f = f | (LF[w] or 0) end end
    B.nav_link(a, b, f, both)
    U.reply(id, string.format("[xbots_nav] linked #%d -> #%d%s", a, b, both and " (both ways)" or ""))
  elseif sub == "flag" then
    local n, f = nearest_here(id, 80), NF[(args[2] or ""):lower()]
    if n == 0 or not f then U.reply(id, "usage: xbots_nav flag <crouch|ladder|camp|goal|rescue|sniper|avoid|door|jump> (stand on the node)"); return end
    local cur = B.nav_flags(n)
    B.nav_flags(n, cur ~ f)
    U.reply(id, "[xbots_nav] #" .. n .. " flags: " .. flag_names(B.nav_flags(n)))
  elseif sub == "radius" then
    local n = nearest_here(id, 80)
    if n > 0 then B.nav_radius(n, tonumber(args[2] or "32") or 32); U.reply(id, "[xbots_nav] radius set") end
  elseif sub == "info" then
    local n = nearest_here(id, 200)
    if n == 0 then U.reply(id, "[xbots_nav] no node near"); return end
    local nd = B.nav_node(n)
    U.reply(id, string.format("[xbots_nav] #%d (%.0f %.0f %.0f) flags %s radius %d", n, nd.x, nd.y, nd.z, flag_names(nd.flags), nd.radius))
    for _, l in ipairs(nd.links) do U.reply(id, string.format("   -> #%d flags %d", l.to, l.flags)) end
  elseif sub == "path" then
    local a = nearest_here(id, 200)
    local x, y, z = aim_point(id)
    local b = B.nav_nearest(x, y, z, 300)
    if a == 0 or b == 0 then U.reply(id, "[xbots_nav] no node near you or near the aim point"); return end
    local p = B.nav_path(a, b, P.team(id))
    if not p then U.reply(id, "[xbots_nav] NO PATH from #" .. a .. " to #" .. b); return end
    path_show[id] = { nodes = p, expire = xmod.gametime() + 8 }
    show[id] = true
    U.reply(id, "[xbots_nav] path: " .. #p .. " nodes (red)")
  elseif sub == "go" or sub == "bots_here" then
    local x, y, z
    if sub == "go" then x, y, z = aim_point(id) else x, y, z = my_pos(id) end
    local n = 0
    for _, b in ipairs(B.list()) do
      if P.alive(b) then
        if xmod.has_export("xbots.task") then xmod.call("xbots.task", b, { kind = "go", x = x, y = y, z = z, time = 40 })
        else B.go(b, x, y, z) end
        n = n + 1
      end
    end
    U.reply(id, "[xbots_nav] " .. n .. " bots sent")
  else
    U.reply(id, "xbots_nav show | add [flags] | del | from | to [jump|crouch|ladder] [both] | unlink | flag <name> | radius <r> | info | path (to aim) | go (bots to aim) | bots_here")
  end
end

xmod.cmd.client("xbots_nav", "", function(id, args) cmd(id, args) end, "xmod bots navigation editor")

local function open_menu(id)
  if not can(id) then return end
  menu.show(id, {
    title = "Bot navigation (" .. (ok() and B.nav_info().nodes or 0) .. " nodes)",
    items = {
      { (show[id] and "Ascunde" or "Arata") .. " nodurile", "show" }, { "Adauga nod aici", "add" },
      { "Sterge nodul de aici", "del" }, { "Marcheaza nod (from)", "from" }, { "Leaga marcat -> aici", "to" },
      { "Leaga ambele sensuri", "toboth" }, { "Flag: camp", "camp" }, { "Flag: goal", "goal" }, { "Flag: avoid", "avoid" },
      { "Path pana unde ma uit", "path" }, { "Trimite botii unde ma uit", "go" },
      { "Analizeaza harta", "analyze" }, { "Salveaza", "save" },
    },
    on_select = function(p, _, v)
      local a = { v, n = 1 }
      if v == "toboth" then a = { "to", "both", n = 2 }
      elseif v == "camp" or v == "goal" or v == "avoid" then a = { "flag", v, n = 2 } end
      if v == "analyze" then B.nav_analyze(false, xmod.cvar.int("xbots_nav_max")); U.reply(p, "[xbots_nav] analyzing...")
      elseif v == "save" then U.reply(p, "[xbots_nav] saved: " .. tostring(B.nav_save()))
      else cmd(p, a) end
      xmod.timer.after(0.2, function() if P.connected(p) then open_menu(p) end end)
    end,
  })
end
xmod.cmd.say("/botnav", "", function(id) open_menu(id) end, "bot navigation editor")

xmod.on("client_disconnect", function(id) show[id], mark[id], path_show[id] = nil, nil, nil end)
