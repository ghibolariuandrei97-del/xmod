-- Server side test plugin, driven by test/server/host.cpp through the client command "nxt <step>".
xmod.register("Nexus Test", "1.0", "test")
local N = require("nexus")
local pass, fail = 0, 0
local function ok(c, what) if c then pass = pass + 1 else fail = fail + 1; xmod.log("NXT FAIL " .. what) end end
local seen = { ready = {}, lost = {}, keys = {}, clicks = {}, data = {}, sel = {}, closed = {} }
N.on_ready(function(id, info) seen.ready[#seen.ready + 1] = id; seen.info = info end)
N.on_lost(function(id) seen.lost[#seen.lost + 1] = id end)
N.on_data("test.chan", function(id, data) seen.data[#seen.data + 1] = data end)
local LONG = "Nexus poate trimite texte lungi: ăîâșț ĂÎÂȘȚ. " .. string.rep("Lorem ipsum dolor sit amet, consectetur adipiscing elit. ", 9) .. "Sfârșit."

local steps = {}
function steps.state(id)
  ok(N.available(), "extension available"); ok(type(xmod.nexus.set) == "function", "xmod.nexus.set exists")
  ok(#seen.ready == 1 and seen.ready[1] == 1, "exactly one nexus_ready, for player 1 (got " .. #seen.ready .. ")")
  ok(N.has(1) and not N.has(2) and not N.has(3), "has(): only player 1")
  local i = N.info(1); ok(i and i.version == "1.1.0" and i.w == 1280 and i.h == 720 and i.gl == true and i.proto == 1, "info() fields")
  ok(N.info(2) == nil, "info() of a player without nexus is nil")
  ok(#N.players() == 1, "players()")
  ok(N.set(2, "x", { type = "rect", pw = 5, ph = 5 }) == 0, "set() to a player without nexus addresses 0 players")
  ok(not pcall(xmod.nexus.set, 1, 0, { type = "rect" }), "element id 0 is refused")
  ok(not pcall(xmod.nexus.set, 1, 5, { type = "blob" }), "unknown type is refused")
  ok(not pcall(xmod.nexus.set, 1, 5, { type = "image", image = "../secret.png" }), "image path with .. is refused")
  ok(not pcall(xmod.nexus.sound, 1, "x.mp3"), "sound must be .wav")
  ok(not pcall(xmod.nexus.music, 1, "music/a b.mp3"), "music path with a space is refused")
  ok(not pcall(xmod.nexus.send, 1, "chan", string.rep("x", 97)), "send() over 96 bytes is refused")
  ok(not pcall(N.color, "nocolour"), "unknown colour name errors")
  local c = N.color("#FF800040"); ok(c[1] == 255 and c[2] == 128 and c[3] == 0 and c[4] == 64, "hex colour")
  ok(N.id("a.b") == N.id("a.b") and N.id("a.b") ~= N.id("a.c"), "name -> id is stable")
  ok(N.keynum("F3") == 137 and N.keynum("f") == 102 and N.keynum("TAB") == 9, "key names")
end
function steps.hud(id)
  N.rect(id, "hud.panel", { anchor = "bottom", py = -70, pw = 260, ph = 34, color = { 10, 12, 18, 190 }, color2 = { 255, 255, 255, 90 }, outline = true, layer = 1 })
  N.bar(id, "hud.hp", { anchor = "bottom", py = -76, pw = 244, ph = 10, color = "#4CE06A", color2 = { 255, 255, 255, 40 }, value = 0.65, layer = 2 })
  N.text(id, "hud.hptext", { anchor = "bottom", py = -88, text = "HP 65", scale = 0.85, shadow = true, layer = 3 })
  N.text(id, "hud.hint", { anchor = "topright", px = -12, py = 10, text = "^3F3^0 shop   ^3F4^0 camera  ^1r^2g^4b ^^caret", markup = true, scale = 0.9, shadow = true })
  N.text(id, "hud.title", { anchor = "top", py = 14, text = "NEXUS — Știri de pe server", scale = 1.8, outline = true, color = "gold" })
  N.text(id, "hud.long", { anchor = "topleft", px = 16, py = 80, pw = 330, text = LONG, scale = 0.8, color2 = { 0, 0, 0, 150 }, color = { 230, 230, 230, 255 } })
  N.text(id, "hud.multi", { anchor = "right", px = -16, text = "right aligned\nsecond line is longer\nthird", scale = 1, shadow = true, color = "cyan" })
  N.bar(id, "hud.vbar", { anchor = "left", px = 16, py = 120, pw = 14, ph = 120, color = "orange", color2 = { 255, 255, 255, 40 }, value = 0.4 })
  N.image(id, "hud.logo", { anchor = "bottomleft", px = 16, py = -16, image = "gfx/nexus/logo.png", ph = 96 })
  N.image(id, "hud.missing", { anchor = "center", image = "gfx/nexus/does_not_exist.png" })
  N.set(id, "hud.hptext", { text = "HP 64" })                         -- partial update: must keep anchor/shadow/layer
  ok(N.set(id, "hud.hp", { value = 0.64 }) == 1, "partial update addresses the player")
end
function steps.menu(id)
  local r = N.menu(id, { title = "Nexus shop", cursor = true, items = { { "Health +50", "hp" }, { "Armor 100", "armor" }, { "Locked item", "x", disabled = true }, { "Third person", "cam" } },
    on_select = function(p, label, value) seen.sel[#seen.sel + 1] = value end, on_close = function(p, reason) seen.closed[#seen.closed + 1] = reason end })
  ok(r == "nexus", "menu() uses the nexus renderer for player 1"); ok(N.menu_open(id), "menu_open")
end
function steps.menu_check(id)
  ok(#seen.sel == 1 and seen.sel[1] == "armor", "key 2 selected the 2nd menu item (got " .. tostring(seen.sel[1]) .. ", n=" .. #seen.sel .. ")")
  ok(not N.menu_open(id), "menu closed after the selection")
  N.menu(id, { title = "Second", items = { { "A", 1 } }, on_close = function(p, reason) seen.closed[#seen.closed + 1] = reason end })
end
function steps.menu_check2(id)
  ok(seen.closed[#seen.closed] == "exit", "key 0 closed the menu with reason exit (got " .. tostring(seen.closed[#seen.closed]) .. ")")
  ok(#seen.sel == 1, "a disabled / absent item selects nothing")
end
function steps.world(id)
  N.label3d(id, "tag.2", 2, "Bob ^2[CT]", { markup = true })
  N.label3d(id, "tag.far", 4, "too far", { maxdist = 300 })
  N.label3d(id, "tag.wall", 5, "behind a wall")
  N.float_text(id, { 300, 60, 40 }, "-57", { color = "red", time = 5 })
  N.set(id, "world.marker", { type = "rect", pos3 = { 400, -120, 0 }, anchor = "center", pw = 14, ph = 14, color = "magenta", additive = true })
end
function steps.anim(id)
  N.text(id, "fx.slide", { anchor = "center", x = -0.3, y = 0.1, text = "sliding in", scale = 1.4, color = { 255, 255, 255, 0 }, outline = true })
  N.anim(id, "fx.slide", { x = 0, y = 0.1, time = 1, ease = "out" }); N.anim(id, "fx.slide", { alpha = 255, time = 1 })
  N.text(id, "fx.gone", { anchor = "center", y = 0.2, text = "this one fades and is removed", color = "red" })
  N.anim(id, "fx.gone", { alpha = 0, time = 0.5, remove = true })
  N.toast(id, "^2Toast:^7 round started", { markup = true, time = 4 })
end
function steps.input(id)
  N.bind(id, "f", function(p, key, down) seen.keys[#seen.keys + 1] = key .. (down and "d" or "u") end, true)
  N.set(id, 777, { type = "rect", anchor = "center", pw = 120, ph = 40, color = "blue", on_click = function(p, name, button) seen.clicks[#seen.clicks + 1] = tostring(name) .. ":" .. button end })
  N.cursor(id, true)
end
function steps.input_check(id)
  ok(#seen.keys == 2 and seen.keys[1] == "102d" and seen.keys[2] == "102u", "bound key f: down + up reached Lua (n=" .. #seen.keys .. ")")
  ok(#seen.clicks == 1 and seen.clicks[1] == "777:1", "click on element 777 reached its on_click (n=" .. #seen.clicks .. ")")
  ok(#seen.data == 1 and seen.data[1] == "hi\0there", "nexus_data with binary payload (n=" .. #seen.data .. ")")
  N.cursor(id, false); N.unbind(id, "f"); N.remove(id, 777)
end
function steps.flood(id)                                               -- 400 elements at once: must be paced, never dropped
  for i = 1, 400 do xmod.nexus.set(id, 1000 + i, { type = "rect", x = (i % 40) / 40, y = 0.5 + (i // 40) / 40, pw = 20, ph = 10, color = { (i * 7) % 256, 120, 200, 200 }, group = 9 }) end
  for i = 1, 400 do xmod.nexus.set(id, 1000 + i, { type = "rect", pw = 22, ph = 10 }) end      -- coalesces with the pending updates
  local s = xmod.nexus.stats(); ok(s.queued_ops >= 300 and s.queued_ops <= 420, "updates of the same element are merged in the queue (queued " .. s.queued_ops .. ")")
end
function steps.flood_check(id)
  local s = xmod.nexus.stats(); ok(s.queued_ops == 0 and s.dropped_ops == 0, "queue drained, nothing dropped (queued " .. s.queued_ops .. ", dropped " .. s.dropped_ops .. ")")
  N.clear(id, 9)
end
function steps.misc(id)
  ok(N.camera(id, { mode = "third", dist = 150, height = 20, time = 0 }) == 1, "camera third")
  ok(N.music(id, "sound/music/theme.mp3", true) == 1, "music loop"); ok(N.sound(id, "items/gunpickup2.wav", 0.5) == 1, "sound")
  ok(N.send(id, "fps.note", "from lua") == 1, "send data to client plugins")
  N.bind(id, "f", function(p, key, down) seen.keys[#seen.keys + 1] = key .. (down and "d" or "u") end)
end
function steps.url(id)
  ok(not pcall(xmod.nexus.open_url, id, "http://example.com/x", ""), "open_url: plain http is refused")
  ok(not pcall(xmod.nexus.open_url, id, "https://a.com/\"x", ""), "open_url: quotes are refused")
  ok(N.urlencode("AC/DC - T.N.T & more") == "AC%2FDC%20-%20T.N.T%20%26%20more", "urlencode")
  ok(N.open_url(2, "https://www.youtube.com/", "x", function(p, r) seen.url_nonexus = r end) == false and seen.url_nonexus == "no_nexus", "open_url to a player without nexus -> no_nexus")
  ok(N.youtube(id, "Daft Punk - One More Time", function(p, r) seen.url = r end) == true, "youtube() sent a request")
end
function steps.fx(id)
  ok(N.info(id).fx == true, "info().fx")
  ok(N.fov(id, 40, 0) == 1, "fov"); ok(N.shake(id, 3, 10, 30) == 1, "shake"); ok(N.punch(id, -5, 2) == 1, "punch")
  ok(N.input(id, { block = { "attack", "move", "jump" }, force = { "duck" } }) == 1, "input")
  ok(not pcall(N.input, id, { block = { "fly" } }), "unknown input name errors")
  ok(N.entity(id, 2, { hide = true }) == 1 and N.glow(id, 3, "red", 25) == 1, "entity overrides")
  ok(N.shape(id, "t.line", { type = "line", from = { 100, 0, 0 }, to = { 300, 100, 60 }, color = "red", width = 3 }) == 1, "line")
  ok(N.shape(id, "t.box", { type = "box", mins = { 200, -60, -20 }, maxs = { 260, 0, 40 }, color = { 80, 255, 120, 255 }, fill = true }) == 1, "box")
  ok(N.shape(id, "t.zone", { type = "cylinder", center = { 300, 80, -30 }, radius = 50, height = 40, color = "cyan", fill = true, additive = true }) == 1, "cylinder")
  ok(N.shape(id, "t.wall", { type = "circle", center = { 300, -200, 0 }, radius = 30, color = "yellow", xray = true }) == 1, "x-ray circle")
  ok(not pcall(N.shape, id, "t.bad", { type = "line" }), "a line without points errors")
  ok(N.light(id, "t.l", { pos = { 200, 0, 50 }, radius = 300, color = "orange" }) == 1, "light")
  ok(N.effect(id, "sparks", { pos = { 200, 0, 0 } }) == 1 and N.effect(id, "beam", { pos = { 0, 0, 0 }, to = { 100, 0, 0 } }) == 1, "effects")
  ok(not pcall(N.effect, id, "nuke", { pos = { 0, 0, 0 } }), "unknown effect errors")
  ok(N.sound3d(id, "weapons/explode3.wav", { pos = { 100, 0, 0 } }) == 1, "sound3d")
  ok(not pcall(N.sound3d, id, "../x.wav", {}), "bad sound path errors")
  ok(N.fov(2, 40) == 0, "fx functions address 0 players without nexus")
end
function steps.fx_off(id) N.input(id); N.fov(id, 0, 0); N.clear_entities(id); N.remove_shape(id, "t.line"); N.remove_light(id, "t.l") end
function steps.url_check(id) ok(seen.url == "opened", "the player's answer reached the callback (got " .. tostring(seen.url) .. ")") end
function steps.cmdflood_check(id) ok(#seen.keys <= 2 + 80 and #seen.keys >= 2 + 70, "command flood is limited to 80 per second (" .. (#seen.keys - 2) .. " of 300 got through)") end
function steps.clear(id) N.camera(id); N.music(id); N.clear(id) end
function steps.lost(id) ok(#seen.lost == 1 and seen.lost[1] == 1, "nexus_lost fired for player 1 on disconnect (n=" .. #seen.lost .. ")"); ok(not N.has(1), "has(1) false after disconnect") end
function steps.remap(id) ok(#seen.ready == 2 and N.has(1), "player 1 was greeted again after reconnect / map change (ready events: " .. #seen.ready .. ")") end
function steps.fallback(id)
  local r = N.menu(2, { title = "Classic", items = { { "One", 1 }, { "Two", 2 } }, on_select = function() end })
  ok(r == "classic", "menu() for a player without nexus falls back to the classic xmod menu (got " .. tostring(r) .. ")")
end
function steps.done() xmod.log(string.format("NEXUS LUA TEST DONE: %d passed, %d failed", pass, fail)) end

xmod.on("client_command", function(id, cmd, args)
  if cmd ~= "nxt" then return end
  local step = type(args) == "table" and args[1] or tostring(args):match("%S+")
  local fn = steps[step]; if not fn then xmod.log("NXT FAIL unknown step " .. tostring(step)); fail = fail + 1; return true end
  local good, err = pcall(fn, 1); if not good then fail = fail + 1; xmod.log("NXT FAIL step " .. step .. " raised: " .. tostring(err)) end
  return true
end)
