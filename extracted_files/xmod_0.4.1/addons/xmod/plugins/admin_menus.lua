-- Admin Menus: say /admin  (or xmod_menu).  Player pickers for kick/ban/slay/slap/team, map menu, vote map.
xmod.register("Admin Menus", "0.2", "xmod")
local U, menu = require("util"), require("menu")

local function pick_player(id, title, cb, opts)
  local items = {}
  for _, p in ipairs(xmod.player.all()) do
    local immune = (xmod.player.access(p) & U.IMMUNITY) ~= 0
    if p ~= id and not immune and (not opts or not opts.alive or xmod.player.alive(p)) then items[#items + 1] = { xmod.player.name(p), p } end
  end
  if #items == 0 then xmod.msg.chat(id, "{g}[xmod] {y}No suitable players."); return end
  menu.show(id, { title = title, items = items, on_select = function(admin, _, p) if xmod.player.connected(p) then cb(admin, p) end end })
end
local function maps_menu(id, title, cb)
  local list = xmod.call("maps.list") or {}
  if #list == 0 then xmod.msg.chat(id, "{g}[xmod] {y}No maps configured."); return end
  local items = {}; for i, m in ipairs(list) do items[i] = { m, m } end
  menu.show(id, { title = title, items = items, on_select = function(admin, _, m) cb(admin, m) end })
end

local ENTRIES = {
  { "Kick player", "c", function(id) pick_player(id, "Kick", function(a, p) xmod.call("admin.kick", a, p, "Kicked by admin") end) end },
  { "Ban player", "d", function(id) pick_player(id, "Ban who?", function(a, p)
      menu.show(a, { title = "Ban duration", items = { { "5 minutes", 5 }, { "1 hour", 60 }, { "1 day", 1440 }, { "1 week", 10080 }, { "Permanent", 0 } },
        on_select = function(adm, _, mins) if xmod.player.connected(p) then xmod.call("admin.ban", adm, p, mins, "Banned by admin") end end })
    end) end },
  { "Slay player", "e", function(id) pick_player(id, "Slay", function(a, p) xmod.call("admin.slay", a, p) end, { alive = true }) end },
  { "Slap player", "e", function(id) pick_player(id, "Slap", function(a, p) xmod.call("admin.slap", a, p, 5) end, { alive = true }) end },
  { "Move to team", "e", function(id) pick_player(id, "Move who?", function(a, p)
      menu.show(a, { title = "Team", items = { { "Terrorist", 1 }, { "Counter-Terrorist", 2 }, { "Spectator", 3 } },
        on_select = function(adm, _, t) if xmod.player.connected(p) then xmod.call("admin.team", adm, p, t) end end })
    end) end },
  { "Change map", "f", function(id) maps_menu(id, "Change map", function(a, m) xmod.call("admin.map", a, m) end) end },
  { "Vote for map", "j", function(id) maps_menu(id, "Vote map", function(a, m) xmod.exec("xmod_votemap " .. m) end) end },
  { "Restart round", "f", function(id) xmod.exec("sv_restart 1") end },
}
local function main_menu(id)
  local items = {}
  for _, e in ipairs(ENTRIES) do if xmod.player.has_access(id, e[2]) then items[#items + 1] = { e[1], e[3] } end end
  if #items == 0 then return end
  menu.show(id, { title = "Admin menu", items = items, on_select = function(adm, _, fn) fn(adm) end })
end
xmod.cmd.say("/admin", "u", function(id) main_menu(id) end, "open the admin menu")
xmod.cmd.say("admin", "u", function(id) main_menu(id) end)
xmod.cmd.client("xmod_menu", "u", function(id) main_menu(id) end, "open the admin menu")
xmod.cmd.client("amx_menu", "u", function(id) main_menu(id) end, "open the admin menu")
