-- Nexus demo: a custom HUD, name tags, floating damage numbers, a clickable shop, key binds and a third person camera.
-- Players without the Nexus client still get the menu (classic xmod menu) and chat text.
xmod.register("Nexus Demo", "1.0", "xmod")
local N = require("nexus")
xmod.on("config", function() if not N.available() then xmod.log("nexus_demo: the Nexus extension is not loaded - see the Nexus README (metamod/plugins.ini + xmod extensions.ini)") end end)

local SHOP = { { "Health +50", "hp" }, { "Armor 100", "armor" }, { "Third person camera", "cam" }, { "First person camera", "cam_off" } }
local third = {}

local function shop_pick(id, label, value)
  if not xmod.player.alive(id) then xmod.msg.chat(id, "{g}[nexus] {y}You must be alive."); return end
  if value == "hp" then xmod.player.set_health(id, math.min(xmod.player.health(id) + 50, 200))
  elseif value == "armor" then xmod.player.set_armor(id, 100)
  elseif value == "cam" then third[id] = true; N.camera(id, { mode = "third", dist = 130, height = 12, time = 0.5 })
  elseif value == "cam_off" then third[id] = nil; N.camera(id) end
  N.toast(id, "^2Bought: ^7" .. label, { markup = true, time = 2 })
  N.sound(id, "items/gunpickup2.wav")
end
local function open_shop(id, with_cursor)
  N.menu(id, { title = "Nexus shop", items = SHOP, on_select = shop_pick, cursor = with_cursor, anchor = "left" })
end

local function build_hud(id)
  -- bottom centre: a health bar with a label. The elements stay; only the value / text change later.
  N.rect(id, "hud.panel", { anchor = "bottom", py = -70, pw = 260, ph = 34, color = { 10, 12, 18, 170 }, color2 = { 255, 255, 255, 40 }, outline = true, layer = 1 })
  N.bar(id, "hud.hp", { anchor = "bottom", py = -76, pw = 244, ph = 10, color = "#4CE06A", color2 = { 255, 255, 255, 30 }, value = 1, layer = 2 })
  N.text(id, "hud.hptext", { anchor = "bottom", py = -88, text = "HP 100", scale = 0.85, shadow = true, layer = 3 })
  N.text(id, "hud.hint", { anchor = "topright", px = -12, py = 10, text = "^3F3^0 shop   ^3F4^0 camera", markup = true, scale = 0.8, shadow = true, color = { 255, 255, 255, 200 } })
  N.bind(id, "F3", function(p, _, down) if down then if N.menu_open(p) then N.menu_close(p) else open_shop(p, true) end end end, true)
  N.bind(id, "F4", function(p, _, down)
    if not down then return end
    third[p] = not third[p] or nil
    if third[p] then N.camera(p, { mode = "third", dist = 130, height = 12, time = 0.5 }) else N.camera(p) end
  end, true)
  N.toast(id, "Nexus client detected - welcome, " .. N.escape(xmod.player.name(id)), { color = "gold" })
end
N.on_ready(function(id) build_hud(id) end)
N.on_lost(function(id) third[id] = nil end)

-- refresh the health bar twice a second, only when the value changed
local last_hp = {}
xmod.on("nexus_lost", function(id) last_hp[id] = nil end)
xmod.timer.every(0.5, function()
  for _, id in ipairs(N.players()) do
    local hp = xmod.player.alive(id) and xmod.player.health(id) or 0
    if last_hp[id] ~= hp then
      last_hp[id] = hp
      N.anim(id, "hud.hp", { value = math.min(hp / 100, 1), time = 0.3 })
      N.set(id, "hud.hp", { color = hp > 50 and "#4CE06A" or (hp > 25 and "#FFC83C" or "#FF4040") })
      N.set(id, "hud.hptext", { text = "HP " .. math.floor(hp) })
    end
  end
end)

-- name tags above every player, for everybody who has Nexus (hidden behind walls, fade out with distance)
local function tag(viewer, target)
  if viewer == target then return end
  N.label3d(viewer, "tag." .. target, target, N.escape(xmod.player.name(target)), { offset = { 0, 0, 42 }, maxdist = 1200, group = 7, color = xmod.player.team(target) == 1 and "t" or "ct" })
end
N.on_ready(function(id) for _, other in ipairs(xmod.player.all()) do tag(id, other) end end)
xmod.on("client_putinserver", function(id) for _, viewer in ipairs(N.players()) do tag(viewer, id) end end)
xmod.on("client_disconnect", function(id) N.remove(0, "tag." .. id) end)

-- floating damage numbers for the attacker
xmod.on("ham:takedamage:player:post", function(victim, inflictor, attacker, dmg)
  if attacker and attacker >= 1 and attacker <= xmod.maxplayers() and attacker ~= victim and N.has(attacker) then
    local ox, oy, oz = xmod.player.origin(victim)
    N.float_text(attacker, { ox + math.random(-8, 8), oy + math.random(-8, 8), oz + 30 }, tostring(math.floor(dmg)), { color = dmg >= 50 and "red" or "yellow" })
  end
end)

xmod.cmd.say("/shop", "", function(id) open_shop(id, false) end, "open the Nexus demo shop")
xmod.cmd.say("/nexus", "", function(id)
  local i = N.info(id)
  if i then xmod.msg.chat(id, string.format("{g}[nexus] {y}client {t}%s{y}, %dx%d, OpenGL: %s", i.version, i.w, i.h, tostring(i.gl)))
  else xmod.msg.chat(id, "{g}[nexus] {y}You do not have the Nexus client. The server works without it; with it you get the custom HUD.") end
end, "show your Nexus client status")
