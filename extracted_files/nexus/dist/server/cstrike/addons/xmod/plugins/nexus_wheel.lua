-- Nexus "Power Wheel" test:  say /test
-- A radial menu (like modern shooters) instead of a numbered text list:
--   * 8 tiles fly out from the centre (animated on the player's PC - zero network traffic per frame)
--   * mouse hover + click, or keys 1-8; F4 or /test again closes it
--   * live cooldown bars that fill up on the client by themselves, greyed-out tiles when you can't afford / can't use them
--   * effects you can see: low gravity, glow, radar ping that marks enemies through walls for 5 seconds
-- Players without the Nexus client get the classic xmod menu with the same powers.
xmod.register("Nexus Power Wheel", "1.0", "Andrew")
local N = require("nexus")

local GROUP, R = 77, 215                  -- element group of the wheel, radius in virtual pixels (screen height = 768)
local TW, TH = 168, 70                    -- tile size
local open, cd_until, effects = {}, {}, {}   -- open[id] = true; cd_until[id][power] = time; effects[id] = {timer ids}

local WEAPONS = { "weapon_ak47", "weapon_m4a1", "weapon_awp", "weapon_famas", "weapon_galil", "weapon_mp5navy", "weapon_deagle", "weapon_xm1014" }
local POWERS  -- defined below (they use helpers)

local function now() return xmod.time() end
local function money(id) return xmod.player.money(id) end
local function cooldown_left(id, i) local t = cd_until[id] and cd_until[id][i]; return t and math.max(0, t - now()) or 0 end
local function usable(id, i)
  local p = POWERS[i]
  if p.alive and not xmod.player.alive(id) then return false, "dead" end
  if money(id) < p.price then return false, "money" end
  if cooldown_left(id, i) > 0 then return false, "cooldown" end
  return true
end
local function later(id, secs, fn) effects[id] = effects[id] or {}; table.insert(effects[id], xmod.timer.after(secs, fn, id)) end

POWERS = {
  { name = "Heal",        sub = "+50 HP",            price = 300,  cd = 10, alive = true, color = { 60, 200, 90 },
    fn = function(id) xmod.player.set_health(id, math.min(xmod.player.health(id) + 50, 150)) end },
  { name = "Armor",       sub = "100 kevlar",        price = 500,  cd = 15, alive = true, color = { 80, 140, 255 },
    fn = function(id) xmod.player.set_armor(id, 100) end },
  { name = "Low gravity", sub = "10 seconds",        price = 800,  cd = 25, alive = true, color = { 170, 100, 255 },
    fn = function(id)
      xmod.ent.set(id, "gravity", 0.35)
      later(id, 10, function(pid) if xmod.player.connected(pid) then xmod.ent.set(pid, "gravity", 1.0); N.toast(pid, "Gravity is back to normal", { time = 2 }) end end)
    end },
  { name = "Glow",        sub = "15 seconds",        price = 200,  cd = 20, alive = true, color = { 255, 210, 60 },
    fn = function(id)
      xmod.ent.set(id, "renderfx", 19); xmod.ent.set(id, "rendercolor", 255, 200, 40); xmod.ent.set(id, "renderamt", 25)   -- glow shell
      later(id, 15, function(pid) if xmod.player.connected(pid) then xmod.ent.set(pid, "renderfx", 0); xmod.ent.set(pid, "renderamt", 0) end end)
    end },
  { name = "Random gun",  sub = "surprise!",         price = 1000, cd = 30, alive = true, color = { 255, 120, 40 },
    fn = function(id) local w = WEAPONS[math.random(#WEAPONS)]; xmod.player.give_item(id, w); return w:gsub("weapon_", ""):upper() end },
  { name = "HE grenade",  sub = "boom",              price = 300,  cd = 8,  alive = true, color = { 230, 70, 70 },
    fn = function(id) xmod.player.give_item(id, "weapon_hegrenade") end },
  { name = "Radar ping",  sub = "enemies, 5 s",      price = 1500, cd = 40, alive = false, color = { 60, 230, 230 },
    fn = function(id)
      local my, n = xmod.player.team(id), 0
      for _, e in ipairs(xmod.player.all()) do
        if e ~= id and xmod.player.alive(e) and xmod.player.team(e) ~= my and xmod.player.team(e) >= 1 and xmod.player.team(e) <= 2 then
          n = n + 1
          N.set(id, N.temp_id(), { type = "text", ent = e, offset = { 0, 0, 46 }, anchor = "bottom", text = "[!] " .. xmod.player.name(e) .. "  " .. xmod.player.health(e) .. " HP",
            color = "red", outline = true, distscale = true, life = 5, fadein = 0.2, fadeout = 1, layer = 50 })
        end
      end
      return n .. " enemies marked"
    end },
  { name = "Close",       sub = "or press F4",       price = 0,    cd = 0,  alive = false, color = { 120, 120, 130 }, close = true },
}

local function slot_pos(i)                       -- tile centre, clockwise from the top
  local a = math.rad(-90 + (i - 1) * 360 / #POWERS)
  return math.floor(math.cos(a) * R * 1.25 + 0.5), math.floor(math.sin(a) * R + 0.5)   -- a little wider than tall
end

-- colours / texts of one tile for its current state (sent only when something changed)
local function tile_state(id, i)
  local p = POWERS[i]
  if p.close then return { 70, 70, 80, 220 }, "^7" .. p.name, p.sub end
  local ok, why = usable(id, i)
  local c = p.color
  if ok then return { c[1] // 3, c[2] // 3, c[3] // 3, 225 }, "^7" .. p.name, "^3$" .. p.price .. "^8   " .. p.sub end
  local sub = why == "money" and "^1need $" .. p.price or (why == "dead" and "^8only when alive" or "^8cooling down")
  return { 35, 35, 40, 200 }, "^8" .. p.name, sub
end
local last = {}                                  -- last[id][i] = state key, to avoid re-sending unchanged tiles
local function refresh(id)
  if not open[id] then return end
  last[id] = last[id] or {}
  for i = 1, #POWERS do
    local bg, title, sub = tile_state(id, i)
    local key = table.concat(bg, ",") .. title .. sub
    if last[id][i] ~= key then
      last[id][i] = key
      N.set(id, "wheel.tile" .. i, { color = bg })
      N.set(id, "wheel.name" .. i, { text = (i <= 8 and ("^9" .. i .. " ") or "") .. title })
      N.set(id, "wheel.sub" .. i, { text = sub })
    end
  end
  N.set(id, "wheel.money", { text = "^2$" .. money(id) })
end

local close_wheel
local function use(id, i)
  if not open[id] then return end
  local p = POWERS[i]; if not p then return end
  if p.close then close_wheel(id); return end
  local ok, why = usable(id, i)
  if not N.has(id) then                                     -- classic-menu player: plain chat feedback
    if ok then xmod.player.set_money(id, money(id) - p.price); local extra = p.fn(id); cd_until[id] = cd_until[id] or {}; cd_until[id][i] = now() + p.cd
      xmod.msg.chat(id, "{g}[Power] {y}" .. p.name .. (type(extra) == "string" and (" - " .. extra) or "") .. " {t}-$" .. p.price)
    else xmod.msg.chat(id, "{g}[Power] {y}" .. (why == "money" and "Not enough money" or (why == "dead" and "You must be alive" or "Cooling down"))) end
    return
  end
  if not ok then
    N.sound(id, "buttons/button10.wav", 0.7)
    N.anim(id, "wheel.tile" .. i, { px = select(1, slot_pos(i)) + 6, py = select(2, slot_pos(i)), time = 0.05 })   -- little shake
    xmod.timer.after(0.06, function(pid) if open[pid] then local x, y = slot_pos(i); N.anim(pid, "wheel.tile" .. i, { px = x, py = y, time = 0.15 }) end end, id)
    N.toast(id, why == "money" and "^1Not enough money" or (why == "dead" and "^1You must be alive" or ("^3Ready in " .. math.ceil(cooldown_left(id, i)) .. " s")), { markup = true, time = 1.5, y = 0.05 })
    return
  end
  xmod.player.set_money(id, money(id) - p.price)
  local extra = p.fn(id)
  cd_until[id] = cd_until[id] or {}; cd_until[id][i] = now() + p.cd
  -- cooldown bar: reset to empty and let the CLIENT fill it during the cooldown (no traffic while it fills)
  N.set(id, "wheel.cd" .. i, { value = 0, hidden = false })
  N.anim(id, "wheel.cd" .. i, { value = 1, time = p.cd, ease = "linear" })
  xmod.timer.after(p.cd + 0.05, function(pid) if open[pid] then refresh(pid) end end, id)
  -- feedback: tile pulse, hub flash, sound, toast
  N.anim(id, "wheel.name" .. i, { scale = 1.25, time = 0.08 })
  xmod.timer.after(0.1, function(pid) if open[pid] then N.anim(pid, "wheel.name" .. i, { scale = 1.0, time = 0.25 }) end end, id)
  N.sound(id, "items/gunpickup2.wav", 0.8)
  N.toast(id, "^2" .. p.name .. (type(extra) == "string" and ("^7  " .. extra) or "") .. "^8  -$" .. p.price, { markup = true, time = 2, y = 0.05 })
  refresh(id)
end

function close_wheel(id)
  if not open[id] then return end
  open[id] = nil; last[id] = nil
  if not N.has(id) then return end
  for i = 1, #POWERS do                                     -- fly back into the centre, then disappear
    for _, part in ipairs({ "tile", "name", "sub", "cd" }) do N.anim(id, "wheel." .. part .. i, { px = 0, py = 0, alpha = 0, time = 0.18, ease = "inout" }) end
  end
  N.anim(id, "wheel.hub", { alpha = 0, scale = 0.6, time = 0.18 }); N.anim(id, "wheel.title", { alpha = 0, time = 0.18 }); N.anim(id, "wheel.money", { alpha = 0, time = 0.18 })
  xmod.timer.after(0.2, function(pid) if not open[pid] then N.clear(pid, GROUP) end end, id)
  N.cursor(id, false)
  for k = 1, #POWERS do N.unbind(id, tostring(k)) end
  N.unbind(id, "F4")
end

local function open_wheel(id)
  if not N.has(id) then                                     -- classic menu for players without Nexus
    local items = {}
    for i, p in ipairs(POWERS) do if not p.close then items[#items + 1] = { p.name .. " ($" .. p.price .. ")", i } end end
    N.menu(id, { title = "Power Wheel", items = items, on_select = function(pid, _, i) open[pid] = true; use(pid, i); open[pid] = nil end })
    return
  end
  if open[id] then close_wheel(id); return end
  open[id] = true; last[id] = nil
  N.clear(id, GROUP)
  -- hub in the middle
  N.set(id, "wheel.hub", { type = "rect", anchor = "center", pw = 170, ph = 92, color = { 10, 12, 18, 235 }, color2 = { 255, 200, 60, 200 }, outline = true, layer = 80, group = GROUP, scale = 1 })
  N.set(id, "wheel.title", { type = "text", anchor = "center", py = -22, text = "POWER WHEEL", color = "gold", scale = 1.1, shadow = true, layer = 81, group = GROUP })
  N.set(id, "wheel.money", { type = "text", anchor = "center", py = 12, text = "", markup = true, scale = 1.5, shadow = true, layer = 81, group = GROUP })
  N.set(id, "wheel.hub", { color = { 10, 12, 18, 0 } }); N.anim(id, "wheel.hub", { color = { 10, 12, 18, 235 }, time = 0.25 })
  -- tiles: created in the centre, invisible, then animated out to their place
  for i, p in ipairs(POWERS) do
    local x, y = slot_pos(i)
    N.set(id, "wheel.tile" .. i, { type = "rect", anchor = "center", px = 0, py = 0, pw = TW, ph = TH, color = { 0, 0, 0, 0 }, color2 = { p.color[1], p.color[2], p.color[3], 200 }, outline = true,
      layer = 82, group = GROUP, on_click = function(pid, _, btn) if btn == 1 then use(pid, i) end end })
    N.set(id, "wheel.name" .. i, { type = "text", anchor = "center", px = 0, py = 0, text = "", markup = true, scale = 1.05, shadow = true, color = { 255, 255, 255, 0 }, layer = 83, group = GROUP })
    N.set(id, "wheel.sub" .. i, { type = "text", anchor = "center", px = 0, py = 0, text = "", markup = true, scale = 0.75, color = { 255, 255, 255, 0 }, layer = 83, group = GROUP })
    local left = cooldown_left(id, i)
    N.set(id, "wheel.cd" .. i, { type = "bar", anchor = "center", px = 0, py = 0, pw = TW - 16, ph = 4, color = p.color, color2 = { 255, 255, 255, 30 }, layer = 83, group = GROUP,
      value = p.cd > 0 and (1 - left / p.cd) or 1, hidden = p.close == true })
    if left > 0 then N.anim(id, "wheel.cd" .. i, { value = 1, time = left, ease = "linear" }) end
    local delay = (i - 1) * 0.03                             -- tiles come out one after another
    xmod.timer.after(delay, function(pid)
      if not open[pid] then return end
      N.anim(pid, "wheel.tile" .. i, { px = x, py = y, time = 0.28, ease = "out" })
      N.anim(pid, "wheel.name" .. i, { px = x, py = y - 12, alpha = 255, time = 0.28, ease = "out" })
      N.anim(pid, "wheel.sub" .. i, { px = x, py = y + 10, alpha = 255, time = 0.28, ease = "out" })
      N.anim(pid, "wheel.cd" .. i, { px = x, py = y + TH / 2 - 8, time = 0.28, ease = "out" })
    end, id)
  end
  refresh(id)
  N.cursor(id, true)
  for k = 1, #POWERS do N.bind(id, tostring(k), function(pid, _, down) if down then use(pid, k) end end, true) end
  N.bind(id, "F4", function(pid, _, down) if down then close_wheel(pid) end end, true)
  N.sound(id, "buttons/blip1.wav", 0.6)
end

-- keep money / alive states fresh while the wheel is open (only changed tiles are sent)
xmod.timer.every(0.5, function() for id in pairs(open) do if xmod.player.connected(id) and N.has(id) then refresh(id) end end end)

xmod.cmd.say("/test", "", function(id) open_wheel(id) end, "open the Nexus power wheel")
N.on_lost(function(id) open[id] = nil; last[id] = nil end)
xmod.on("client_disconnect", function(id) open[id] = nil; last[id] = nil; cd_until[id] = nil
  for _, t in ipairs(effects[id] or {}) do xmod.timer.cancel(t) end; effects[id] = nil end)
