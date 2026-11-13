-- Nexus library for xmod:  local N = require("nexus")
-- A friendly layer over xmod.nexus.* (the raw functions of the Nexus extension):
--   names instead of numeric element ids, colours as "#RRGGBB" / names, partial updates that keep the style,
--   click and key callbacks, menus with a fallback for players without the Nexus client, toasts, 3D labels.
-- Every function takes a player id; 0 = every player that has Nexus. Nothing here errors when the extension is missing:
-- N.available() is false and the functions do nothing (N.has returns false), so one plugin serves both kinds of players.
-- Setup: "nexus" must be listed in addons/xmod/configs/extensions.ini (that makes xmod.nexus exist while plugins load).
local N = {}
local X = xmod.nexus                                                 -- a stable table when "nexus" is in extensions.ini; filled when the extension attaches
if not X then xmod.log("nexus.lua: add a line \"nexus\" to addons/xmod/configs/extensions.ini (and load nexus_mm after xmod in metamod/plugins.ini)") end
local function up() return X ~= nil and xmod.ext.available("nexus") end
N.available = up                                                     -- is the server side extension loaded right now?
-- plugin callbacks run protected: a bug in one plugin's on_click / bind / on_ready must not count as an error of this library
-- (25 errors would disable lib:nexus and with it Nexus for every plugin). The message names the plugin file and line.
local function call(what, fn, ...)
  local ok, err = pcall(fn, ...)
  if not ok then xmod.error("error in " .. what .. " callback: " .. tostring(err)) end
end
N._call = call

-- ---------------------------------------------------------------- keys (engine key numbers)
N.KEY = { TAB = 9, ENTER = 13, SPACE = 32, BACKSPACE = 127, UP = 128, DOWN = 129, LEFT = 130, RIGHT = 131, ALT = 132, CTRL = 133, SHIFT = 134,
  INS = 147, DEL = 148, PGDN = 149, PGUP = 150, HOME = 151, END = 152, MWHEELDOWN = 239, MWHEELUP = 240, MOUSE1 = 241, MOUSE2 = 242, MOUSE3 = 243, MOUSE4 = 244, MOUSE5 = 245, PAUSE = 255 }
for i = 1, 12 do N.KEY["F" .. i] = 134 + i end
local function keynum(k)
  if type(k) == "number" then return math.floor(k) end
  if type(k) ~= "string" or k == "" then error("nexus: bad key " .. tostring(k), 3) end
  if #k == 1 then return k:lower():byte() end                       -- letters, digits, punctuation: their ASCII code (lower case)
  local n = N.KEY[k:upper()]; if not n then error("nexus: unknown key name '" .. k .. "'", 3) end
  return n
end
N.keynum = keynum

-- ---------------------------------------------------------------- colours
N.COLORS = { white = {255, 255, 255}, black = {0, 0, 0}, red = {255, 64, 64}, green = {80, 255, 80}, blue = {90, 140, 255}, yellow = {255, 230, 50},
  orange = {255, 153, 38}, cyan = {76, 255, 255}, magenta = {255, 102, 255}, gray = {153, 153, 153}, grey = {153, 153, 153}, gold = {255, 200, 60},
  ct = {153, 204, 255}, t = {255, 63, 63}, spec = {204, 204, 204} }
-- N.color("#FF8800"), N.color("#FF8800CC"), N.color("red"), N.color("red", 128), N.color({r, g, b [, a]})  ->  {r, g, b, a}
function N.color(c, alpha)
  local out
  if type(c) == "table" then out = { c[1] or 255, c[2] or 255, c[3] or 255, c[4] or 255 }
  elseif type(c) == "string" then
    local hex = c:match("^#(%x+)$")
    if hex and (#hex == 6 or #hex == 8) then
      out = { tonumber(hex:sub(1, 2), 16), tonumber(hex:sub(3, 4), 16), tonumber(hex:sub(5, 6), 16), #hex == 8 and tonumber(hex:sub(7, 8), 16) or 255 }
    else
      local n = N.COLORS[c:lower()]; if not n then error("nexus: unknown colour '" .. c .. "'", 2) end
      out = { n[1], n[2], n[3], 255 }
    end
  else error("nexus: bad colour", 2) end
  if alpha then out[4] = alpha end
  return out
end
-- text for an element with markup = true: ^1 red ^2 green ^3 yellow ^4 blue ^5 cyan ^6 magenta ^7 white ^8 gray ^9 orange ^0 back to the element colour
function N.escape(s) return (tostring(s):gsub("%^", "^^")) end       -- use for player names / chat inside a markup text

-- ---------------------------------------------------------------- element ids
local ids, next_id, next_tmp = {}, 20001, 60001
-- name -> element id (20001..60000). Numbers pass through unchanged: raw numeric ids 1..20000 are yours and never collide with names.
-- The same name always gives the same id (in every plugin that uses this library).
function N.id(name)
  if type(name) == "number" then return math.floor(name) end
  local id = ids[name]
  if not id then
    if next_id > 60000 then error("nexus: more than 40000 element names (use N.temp_id() for one-shot elements)", 2) end
    id = next_id; next_id = next_id + 1; ids[name] = id
  end
  return id
end
-- an id for a short-lived element (give it a life=): ids 60001..65535 are handed out in a circle
function N.temp_id() local id = next_tmp; next_tmp = next_tmp + 1; if next_tmp > 65535 then next_tmp = 60001 end; return id end

-- ---------------------------------------------------------------- players
function N.has(id) return up() and X.has(id) or false end
function N.info(id) return up() and X.info(id) or nil end
function N.players() return up() and X.players() or {} end
local function targets(id) if id == 0 then return N.players() end; if N.has(id) then return { id } end; return {} end

-- ---------------------------------------------------------------- elements
local STYLE = { "anchor", "layer", "group", "shadow", "outline", "click", "occlude", "additive", "hidden", "distscale", "markup" }
local state = {}                                                     -- [player][elem id] = { type =, style = {...}, on_click = fn }
local function pstate(p) local s = state[p]; if not s then s = {}; state[p] = s end; return s end
local function prepare(p, eid, props)                                -- fills in what the wire format needs: type, full style, colour tables
  local st = pstate(p)[eid]; local out = {}
  for k, v in pairs(props) do out[k] = v end
  if not st then st = { style = {} }; pstate(p)[eid] = st end
  out.type = out.type or st.type; if not out.type then error("nexus: element needs a type the first time it is set", 3) end
  st.type = out.type
  if out.on_click ~= nil then st.on_click = out.on_click or nil; out.click = out.on_click and true or false; out.on_click = nil end
  local touched = false
  for _, k in ipairs(STYLE) do if out[k] ~= nil then st.style[k] = out[k]; touched = true end end
  if touched then for _, k in ipairs(STYLE) do if st.style[k] ~= nil then out[k] = st.style[k] end end end   -- style travels as one block: send all of it
  if out.color ~= nil then out.color = N.color(out.color) end
  if out.color2 ~= nil then out.color2 = N.color(out.color2) end
  if out.ent ~= nil and out.offset == nil then out.offset = { 0, 0, 0 } end
  return out
end
-- N.set(player, name, props)  creates or updates an element; only the given props change.  Returns how many players got it.
--   type     "text" | "rect" | "image" | "bar"      (needed the first time)
--   x, y     position as a fraction of the screen (0..1), measured from the anchor;  px, py  extra offset in virtual pixels (768 = screen height)
--   w, h     size as a fraction of the screen;  pw, ph  size in virtual pixels.   text: w/pw = wrap width.  image: natural size when omitted
--   anchor   "topleft" "top" "topright" "left" "center" "right" "bottomleft" "bottom" "bottomright": corner of the screen the position is
--            measured from AND the point of the element that sits there (anchor = "bottomright", px = -10, py = -10 -> 10 px from that corner)
--   color, color2   "#RRGGBB[AA]" | name | {r, g, b, a}.   color2: text = background box, bar = track, rect + outline = border
--   scale    text / image size factor (text is 16 virtual pixels high at 1)        value   bar fill 0..1
--   text, image ("gfx/nexus/logo.png": png tga bmp jpg below the game dir)
--   layer (-127..127, higher is drawn later), group (0..254, see N.clear)
--   shadow, outline, additive, hidden, markup (colour codes ^1..^9), on_click = function(player, name, button)
--   ent = entity index + offset = {x, y, z}  |  pos3 = {x, y, z}:  glue the element to a point in the world;  maxdist, occlude, distscale
--   life (seconds, then it deletes itself), fadein, fadeout (seconds)
function N.set(id, name, props)
  if not up() then return 0 end
  local eid, n = N.id(name), 0
  for _, p in ipairs(targets(id)) do n = n + X.set(p, eid, prepare(p, eid, props)) end
  return n
end
local function typed(t) return function(id, name, props) props = props or {}; props.type = t; return N.set(id, name, props) end end
N.text, N.rect, N.image, N.bar = typed("text"), typed("rect"), typed("image"), typed("bar")
function N.remove(id, name)
  if not up() then return 0 end
  local eid = N.id(name)
  for _, p in ipairs(targets(id)) do pstate(p)[eid] = nil end
  return X.del(id, eid)
end
-- N.clear(player)  removes everything;  N.clear(player, group)  removes one group
function N.clear(id, group)
  if not up() then return 0 end
  for _, p in ipairs(targets(id)) do
    if group == nil then state[p] = {} else for eid, st in pairs(pstate(p)) do if (st.style.group or 0) == group then state[p][eid] = nil end end end
  end
  if group == nil then return X.clear(id) end
  return X.clear(id, group)
end
-- N.anim(player, name, { x=, y= | px=, py= | alpha= (0..255) | scale= | value= | color=,  time = 0.5, ease = "out"|"linear"|"inout", remove = true })
function N.anim(id, name, a)
  if not up() then return 0 end
  local out = {}; for k, v in pairs(a) do out[k] = v end
  if out.color ~= nil then out.color = N.color(out.color) end
  local eid = N.id(name)
  if out.remove then for _, p in ipairs(targets(id)) do pstate(p)[eid] = nil end end
  return X.anim(id, eid, out)
end
function N.hide(id, name) return N.set(id, name, { hidden = true }) end
function N.show(id, name) return N.set(id, name, { hidden = false }) end

-- ---------------------------------------------------------------- small ready-made pieces
-- N.toast(player, "text" [, { time = 3, color =, y = 0.22, scale = 1.3 }])   a centred message that slides in and fades out
function N.toast(id, text, o)
  o = o or {}; local eid, t = N.temp_id(), o.time or 3
  local n = N.set(id, eid, { type = "text", text = text, anchor = "top", x = 0, y = (o.y or 0.22) - 0.03, scale = o.scale or 1.3, color = o.color or "white",
    color2 = o.box == false and { 0, 0, 0, 0 } or { 0, 0, 0, 150 }, shadow = true, markup = o.markup, layer = 100, life = t, fadein = 0.2, fadeout = 0.6 })
  if n > 0 then X.anim(id, eid, { x = 0, y = o.y or 0.22, time = 0.35, ease = "out" }) end
  return n
end
-- N.label3d(player, name, entity, "text" [, { offset = {0, 0, 40}, color =, maxdist = 1500, occlude = true, scale = 1 }])   text glued to an entity
function N.label3d(id, name, ent, text, o)
  o = o or {}
  return N.set(id, name, { type = "text", text = text, ent = ent, offset = o.offset or { 0, 0, 40 }, anchor = "bottom", color = o.color or "white", scale = o.scale or 1,
    maxdist = o.maxdist or 1500, occlude = o.occlude ~= false, distscale = o.distscale ~= false, outline = true, markup = o.markup, layer = o.layer or -10, group = o.group })
end
-- N.float_text(player, {x, y, z}, "text" [, { color =, time = 1, rise = 40 }])   e.g. damage numbers: appears at a world position, rises and fades
function N.float_text(id, pos, text, o)
  o = o or {}; local eid, t = N.temp_id(), o.time or 1
  local n = N.set(id, eid, { type = "text", text = text, pos3 = pos, anchor = "center", color = o.color or "yellow", scale = o.scale or 1.2, outline = true, distscale = true,
    maxdist = o.maxdist or 2500, life = t, fadeout = t * 0.5, layer = -5 })
  if n > 0 then X.anim(id, eid, { px = 0, py = -(o.rise or 40), time = t, ease = "out" }) end
  return n
end

-- ---------------------------------------------------------------- keys
local binds, overlay = {}, {}                                        -- [player][keynum] = { fn =, block = };  overlay = keys of an open menu (wins over binds)
local function push_keys(p)
  if not N.has(p) then return end
  local list, seen = {}, {}
  for k, b in pairs(overlay[p] or {}) do list[#list + 1] = { key = k, block = b.block and true or false }; seen[k] = true end
  for k, b in pairs(binds[p] or {}) do if not seen[k] then list[#list + 1] = { key = k, block = b.block and true or false } end end
  X.keys(p, list)
end
-- N.bind(player, key, function(player, key_number, down) [, block])   key: "f", "F3", "TAB", "MOUSE4" or a key number.
--   block = true: the game does not see the key while it is bound.  The player needs the Nexus client; ESC and the console key cannot be bound.
function N.bind(id, key, fn, block)
  for _, p in ipairs(targets(id)) do binds[p] = binds[p] or {}; binds[p][keynum(key)] = { fn = fn, block = block }; push_keys(p) end
end
function N.unbind(id, key)
  for _, p in ipairs(targets(id)) do if binds[p] then if key == nil then binds[p] = {} else binds[p][keynum(key)] = nil end; push_keys(p) end end
end
function N.cursor(id, on) return up() and X.cursor(id, on and true or false) or 0 end
function N.sound(id, file, vol) return up() and X.sound(id, file, vol or 1) or 0 end
function N.music(id, file, loop) if not up() then return 0 end; if file then return X.music(id, file, loop and true or false) end; return X.music(id) end
-- N.camera(player, { mode = "third", dist = 120, height = 10, side = 0, time = 0.4 })  |  { mode = "fixed", pos = {x, y, z}, angles = {pitch, yaw, roll} }  |  N.camera(player) = off
function N.camera(id, o) return up() and X.camera(id, o or { mode = "off" }) or 0 end
function N.send(id, channel, data) return up() and X.send(id, channel, data) or 0 end

-- ---------------------------------------------------------------- menus
local menus = {}                                                     -- [player] = open menu
local MENU_GROUP, PER_PAGE = 250, 7
local fallback_ok, fallback = pcall(require, "menu")
local function menu_close(p, reason)
  local m = menus[p]; if not m then return end
  menus[p] = nil; overlay[p] = nil
  if N.has(p) then N.clear(p, MENU_GROUP); if m.cursor then X.cursor(p, false) end; push_keys(p) end
  if reason ~= "select" and m.on_close then call("menu on_close", m.on_close, p, reason) end
end
local function menu_pick(p, n)
  local m = menus[p]; if not m then return end
  local pages = math.max(1, math.ceil(#m.items / PER_PAGE))
  if n == 0 then menu_close(p, "exit")
  elseif n == 8 and m.page < pages then m.page = m.page + 1; m.render(p)
  elseif n == 9 and m.page > 1 then m.page = m.page - 1; m.render(p)
  elseif n >= 1 and n <= PER_PAGE then
    local it = m.items[(m.page - 1) * PER_PAGE + n]
    if it and not it.disabled then
      local keep; if m.on_select then local ok, r = pcall(m.on_select, p, it[1], it[2]); if ok then keep = r else xmod.error("error in menu on_select callback: " .. tostring(r)) end end   -- return true to keep the menu open
      if keep == true then if menus[p] == m then m.render(p) end else menu_close(p, "select") end
    end
  end
end
local function menu_render(p)
  local m = menus[p]; if not m or not N.has(p) then return end
  local first = (m.page - 1) * PER_PAGE; local pages = math.max(1, math.ceil(#m.items / PER_PAGE))
  local rows = {}
  for i = 1, PER_PAGE do local it = m.items[first + i]; if it then rows[#rows + 1] = { i, it[1], it.disabled } end end
  rows[#rows + 1] = false
  if m.page < pages then rows[#rows + 1] = { 8, "Next" } end
  if m.page > 1 then rows[#rows + 1] = { 9, "Back" } end
  rows[#rows + 1] = { 0, "Exit" }
  local lh, w, pad = 24, m.width or 300, 12
  local h = pad * 2 + 34 + #rows * lh
  N.clear(p, MENU_GROUP)
  m.piece(p, "nexus.menu.bg", { type = "rect", pw = w, ph = h, color = m.bg or { 12, 14, 20, 215 }, color2 = m.border or { 255, 200, 60, 200 }, outline = true, layer = 90 }, 0, 0, w, h)
  m.piece(p, "nexus.menu.title", { type = "text", text = (m.title or "Menu") .. (pages > 1 and string.format("  ^8%d/%d", m.page, pages) or ""), color = m.title_color or "gold", scale = 1.15, markup = true, shadow = true, layer = 91 }, pad, pad, w, h)
  local row = 0
  for _, r in ipairs(rows) do
    if r then
      local n, label, dis = r[1], r[2], r[3]
      m.piece(p, "nexus.menu.i" .. n, { type = "text", text = (dis and "^8" or "^3") .. n .. ". " .. (dis and "^8" or "^0") .. label, color = m.color or "white", markup = true, shadow = true, layer = 91,
        pw = w - pad * 2, on_click = (not dis) and function(pl) menu_pick(pl, n) end or false }, pad, pad + 34 + row * lh, w, h)
    end
    row = row + 1
  end
end
-- N.menu(player, { title = "Shop", items = { {"AK-47", "ak"}, {"Armor", "armor", disabled = true}, ... },
--                  on_select = function(player, label, value) end,     -- return true to keep the menu open
--                  on_close = function(player, reason) end,            -- reason: "exit" | "replaced" | "lost" | "timeout"
--                  cursor = false,      -- true: show a mouse cursor, the rows can be clicked (the player cannot aim while it is open)
--                  timeout = nil, anchor = "left", x = 0, y = 0, px = 16, width = 300, bg =, border =, color =, title_color = })
-- Keys 1-7 pick, 8/9 turn pages, 0 closes. Players without the Nexus client get the classic xmod menu (require("menu")) with the same callbacks.
function N.menu(id, m)
  if not N.has(id) then
    if fallback_ok and fallback and fallback.show then
      local items = {}; for i, it in ipairs(m.items) do items[i] = { it[1], it[2] } end
      fallback.show(id, { title = m.title or "Menu", items = items, timeout = m.timeout or -1, on_select = function(p, label, value) if m.on_select then m.on_select(p, label, value) end end })
      return "classic"
    end
    return false
  end
  if menus[id] then menu_close(id, "replaced") end
  local st = {}; for k, v in pairs(m) do st[k] = v end
  st.page = 1; st.render = menu_render
  -- one piece of the menu: (dx, dy) = its top-left corner inside the panel, whatever the panel's screen anchor is
  st.piece = function(p, name, props, dx, dy, w, h)
    local anchor = st.anchor or "left"; local col, row = 0, 0
    if anchor:find("right") then col = 2 elseif anchor == "top" or anchor == "center" or anchor == "bottom" then col = 1 end
    if anchor:find("bottom") then row = 2 elseif anchor == "left" or anchor == "center" or anchor == "right" then row = 1 end
    props.anchor, props.group = "topleft", MENU_GROUP
    props.x, props.y = col * 0.5 + (st.x or 0), row * 0.5 + (st.y or 0)
    props.px = (st.px or 16) * (1 - col) - w * col / 2 + dx; props.py = -h * row / 2 + dy
    N.set(p, name, props)
  end
  menus[id] = st
  local ov = {}; for n = 0, 9 do ov[48 + n] = { block = true } end; overlay[id] = ov
  push_keys(id); menu_render(id)
  if st.cursor then X.cursor(id, true) end
  if st.timeout and st.timeout > 0 then xmod.timer.after(st.timeout, function() if menus[id] == st then menu_close(id, "timeout") end end) end
  return "nexus"
end
function N.menu_close(id) menu_close(id, "exit") end
function N.menu_open(id) return menus[id] ~= nil end

-- ---------------------------------------------------------------- events
local ready_cbs, lost_cbs, data_cbs = {}, {}, {}
-- N.on_ready(function(player, info) end)   the player's Nexus client finished the handshake (after every map change too): build the HUD here
function N.on_ready(fn) ready_cbs[#ready_cbs + 1] = fn; for _, p in ipairs(N.players()) do fn(p, N.info(p)) end end
function N.on_lost(fn) lost_cbs[#lost_cbs + 1] = fn end
-- N.on_data("channel", function(player, data) end)   data sent by a client plugin (api->send)
function N.on_data(channel, fn) data_cbs[channel] = data_cbs[channel] or {}; table.insert(data_cbs[channel], fn) end
local function forget(p) state[p] = nil; binds[p] = nil; overlay[p] = nil end
if X then
  xmod.on("nexus_ready", function(p, info) forget(p); menus[p] = nil; for _, fn in ipairs(ready_cbs) do call("on_ready", fn, p, info) end end)
  xmod.on("nexus_lost", function(p) local m = menus[p]; menus[p] = nil; forget(p); if m and m.on_close then call("menu on_close", m.on_close, p, "lost") end; for _, fn in ipairs(lost_cbs) do call("on_lost", fn, p) end end)
  xmod.on("nexus_key", function(p, key, down)
    if menus[p] and key >= 48 and key <= 57 then if down then menu_pick(p, key - 48) end; return end
    local b = binds[p] and binds[p][key]; if b and b.fn then call("bind", b.fn, p, key, down) end
  end)
  xmod.on("nexus_click", function(p, eid, button)
    local st = state[p] and state[p][eid]; if not st or not st.on_click then return end
    local name = eid; for k, v in pairs(ids) do if v == eid then name = k; break end end
    call("on_click", st.on_click, p, name, button)
  end)
  xmod.on("nexus_data", function(p, channel, data) for _, fn in ipairs(data_cbs[channel] or {}) do call("on_data", fn, p, data) end end)
end
return N
