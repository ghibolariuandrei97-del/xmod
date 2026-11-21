-- Nexus 2 effects test:  say /fx
-- A clickable menu that shows what a server can now do on a player's PC with xmod + Nexus (none of this exists in AMX Mod X):
-- zoom, camera shake, drunk view, freezing input, glowing players seen only by you, 3D zones and lasers in the world,
-- dynamic coloured lights that follow you, particle effects, positional sounds and a night-vision screen tint.
xmod.register("Nexus FX Test", "1.0", "Andrew")
local N = require("nexus")

local function feet(id) local x, y, z = xmod.player.origin(id); return { math.floor(x), math.floor(y), math.floor(z - 34) } end
local function head(id) local x, y, z = xmod.player.origin(id); return { math.floor(x), math.floor(y), math.floor(z + 30) } end

local ACTIONS = {
  { "Zoom in (3 s)", function(id) N.fov(id, 35, 0.3); xmod.timer.after(3, function(p) N.fov(p, 0, 0.4) end, id) end },
  { "Earthquake", function(id) N.shake(id, 4, 14, 2.5); N.sound3d(id, "weapons/explode3.wav", { pos = feet(id), volume = 1 }) end },
  { "Drunk for 10 s", function(id) N.drunk(id, 1.5); N.tint(id, { 255, 180, 60, 50 }, 0.6, 10); xmod.timer.after(10, function(p) N.drunk(p, 0) end, id) end },
  { "Freeze 3 s", function(id) N.freeze(id, true); N.tint(id, { 120, 200, 255, 70 }, 0.2, 3); N.toast(id, "^5Frozen!", { markup = true })
      xmod.timer.after(3, function(p) N.freeze(p, false) end, id) end },
  { "Glow all players 10 s", function(id)
      for _, p in ipairs(xmod.player.all()) do
        if p ~= id and xmod.player.alive(p) then
          N.glow(id, p, xmod.player.team(p) == 1 and "#FF4040" or "#4080FF", 30)
          N.shape(id, "fx.ring" .. p, { type = "circle", ent = p, center = { 0, 0, -34 }, radius = 28, color = "gold", width = 3, pulse = true, xray = true, life = 10 })
        end
      end
      xmod.timer.after(10, function(v) for _, p in ipairs(xmod.player.all()) do N.glow(v, p) end end, id) end },
  { "Zone at my feet", function(id)
      local f = feet(id)
      N.shape(id, "fx.zone", { type = "cylinder", center = f, radius = 120, height = 90, color = { 80, 255, 120, 200 }, fill = true, additive = true, width = 3, life = 15 })
      N.toast(id, "^2A 15 second zone was drawn around you", { markup = true }) end },
  { "Beacon light (follows you)", function(id) N.light(id, "fx.beacon", { ent = id, offset = { 0, 0, 40 }, radius = 260, color = "magenta", life = 12 }) end },
  { "Laser to the sky", function(id) local h = head(id); N.shape(id, "fx.laser", { type = "line", from = h, to = { h[1], h[2], h[3] + 600 }, color = "red", width = 4, additive = true, life = 6 })
      N.effect(id, "beam", { pos = h, to = { h[1] + 200, h[2], h[3] + 200 }, color = "cyan", life = 2 }) end },
  { "Sparks + explosion", function(id) local h = head(id); N.effect(id, "sparks", { pos = { h[1] + 60, h[2], h[3] } }); N.effect(id, "explosion", { pos = { h[1] + 140, h[2], h[3] } }); N.punch(id, -6, 2) end },
  { "Night vision 8 s", function(id) N.tint(id, { 40, 255, 80, 70 }, 0.3, 8); N.light(id, "fx.nv", { ent = id, radius = 500, color = { 60, 255, 90 }, life = 8 }) end },
  { "Reset everything", function(id) N.fov(id, 0); N.sway(id, 0); N.input(id); N.clear_entities(id); N.tint(id)
      for _, n in ipairs({ "fx.zone", "fx.laser" }) do N.remove_shape(id, n) end; N.remove_light(id, "fx.beacon"); N.remove_light(id, "fx.nv") end },
}

xmod.cmd.say("/fx", "", function(id)
  local info = N.info(id)
  if not info then xmod.msg.chat(id, "{g}[FX] {y}You need the Nexus client to see these effects."); return end
  if not info.fx then xmod.msg.chat(id, "{g}[FX] {y}Your Nexus client is too old for effects - get Nexus 2."); return end
  local items = {}; for i, a in ipairs(ACTIONS) do items[i] = { a[1], i } end
  N.menu(id, { title = "Nexus FX", items = items, cursor = true, anchor = "right",
    on_select = function(p, _, i) ACTIONS[i][2](p); return true end })      -- return true: the menu stays open
end, "Nexus effects test menu")
