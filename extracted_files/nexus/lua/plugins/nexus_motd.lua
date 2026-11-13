-- Nexus MOTD: an interactive, multi-page welcome window drawn by Nexus (mouse + keyboard), with a chat fallback.
--   say /motd  opens it.  Shown once per connection when the player's Nexus client is ready.
--   Mouse: click the buttons.  Keys while open: LEFT/RIGHT arrows = pages, F4 = close (ESC can never be captured).
xmod.register("Nexus MOTD", "1.1", "Andrew")
local N = require("nexus")

local PAGES = {
  { title = "Welcome to the Server!",
    body = "This is ^3not^0 a normal MOTD.\n\nThis window is drawn by Nexus on your PC. You can click the buttons, "
        .. "go back and forth between pages and close it - without leaving the game.\n\nPress ^3NEXT^0 to continue." },
  { title = "Server Rules",
    body = "^31.^0 No cheating / wallhack / aimbot\n^32.^0 No insulting other players\n^33.^0 No camping with the AWP 10 rounds in a row\n"
        .. "^34.^0 Respect the admins\n\nBreaking the rules gets you ^1kicked^0." },
  { title = "Shop & Features",
    body = "The button below runs real game code on the server - an old MOTD can only show a web page.\n\n"
        .. "Click it to get a free ^3AK-47^0 (you must be alive).",
    action = { label = "GET A FREE AK-47", fn = function(id)
      if not xmod.player.alive(id) then N.toast(id, "^1You must be alive", { markup = true }); return end
      xmod.player.give_item(id, "weapon_ak47")
      N.toast(id, "^2You got an AK-47 from the MOTD!", { markup = true })
      N.sound(id, "items/gunpickup2.wav")
    end } },
}

local W, H = 640, 440                  -- window size in virtual pixels (screen height = 768)
local GROUP = 100                      -- every element of the window is in this group: N.clear(id, GROUP) removes it
local open, shown = {}, {}             -- open[id] = current page while the window is open; shown[id] = auto-shown this connection
local used, prev = {}, {}              -- element names drawn on the current / previous page (to remove only what disappeared: no flicker)

-- one element, positioned by its top-left corner inside the window (dx, dy)
local function put(id, name, props, dx, dy)
  props.anchor, props.x, props.y = "topleft", 0.5, 0.5
  props.px, props.py = -W / 2 + dx, -H / 2 + dy
  props.group = GROUP
  N.set(id, name, props); used[name] = true
end
-- a button = clickable rect + a label on top (the label is not clickable, so the click lands on the rect)
local function button(id, name, label, dx, dy, w, h, color, fn)
  put(id, name, { type = "rect", pw = w, ph = h, color = color, color2 = { 255, 255, 255, 60 }, outline = true, layer = 92,
    on_click = function(pid, _, btn) if btn == 1 and open[pid] then fn(pid) end end }, dx, dy)
  N.set(id, name .. ".label", { type = "text", text = label, anchor = "center", x = 0, y = 0,
    px = -W / 2 + dx + w / 2, py = -H / 2 + dy + h / 2, scale = 1.05, shadow = true, layer = 93, group = GROUP })
  used[name .. ".label"] = true
end

local close_motd, show_page
local function turn(id, delta)
  local p = open[id]; if not p then return end
  local np = p + delta
  if np < 1 or np > #PAGES then return end
  N.sound(id, "buttons/lightswitch2.wav", 0.6)
  show_page(id, np)
end

function close_motd(id)
  if not open[id] then return end
  open[id] = nil; prev[id] = nil
  if N.has(id) then
    N.clear(id, GROUP)
    N.cursor(id, false)
    N.unbind(id, "LEFT"); N.unbind(id, "RIGHT"); N.unbind(id, "F4")      -- only our keys: other plugins may have binds too
  end
end

function show_page(id, page)
  if page < 1 or page > #PAGES then page = 1 end
  local P = PAGES[page]
  if not N.has(id) then                                     -- fallback: chat, one page per /motd call
    xmod.msg.chat(id, "{g}[MOTD] {t}" .. P.title .. " {y}(" .. page .. "/" .. #PAGES .. ")")
    for line in (P.body:gsub("%^%d", "") .. "\n"):gmatch("(.-)\n") do if line ~= "" then xmod.msg.chat(id, "{y}" .. line) end end
    return
  end
  local first = not open[id]
  open[id] = page
  if first then
    N.cursor(id, true)
    N.bind(id, "LEFT", function(pid, _, down) if down then turn(pid, -1) end end, true)
    N.bind(id, "RIGHT", function(pid, _, down) if down then turn(pid, 1) end end, true)
    N.bind(id, "F4", function(pid, _, down) if down then close_motd(pid) end end, true)
  end
  used = {}                                                 -- filled by put() / button() below

  -- frame
  put(id, "motd.bg", { type = "rect", pw = W, ph = H, color = { 14, 16, 24, 228 }, color2 = { 255, 200, 60, 220 }, outline = true, layer = 90 }, 0, 0)
  put(id, "motd.head", { type = "rect", pw = W - 2, ph = 50, color = { 255, 200, 60, 40 }, layer = 91 }, 1, 1)
  put(id, "motd.title", { type = "text", text = P.title, color = "gold", scale = 1.6, shadow = true, layer = 92 }, 22, 11)
  button(id, "motd.close", "X", W - 46, 10, 32, 30, { 170, 30, 30, 235 }, close_motd)

  -- body (word-wrapped to the window width)
  put(id, "motd.body", { type = "text", text = P.body, markup = true, pw = W - 48, color = { 235, 235, 235, 255 }, scale = 1.1, shadow = true, layer = 92 }, 24, 72)

  -- action button (last page)
  if P.action then button(id, "motd.action", P.action.label, W / 2 - 130, H - 130, 260, 40, { 150, 100, 0, 235 }, P.action.fn) end

  -- footer: BACK  ● ● ●  NEXT
  if page > 1 then button(id, "motd.back", "< BACK", 22, H - 60, 120, 38, { 60, 60, 70, 235 }, function(pid) turn(pid, -1) end) end
  if page < #PAGES then button(id, "motd.next", "NEXT >", W - 142, H - 60, 120, 38, { 30, 120, 50, 235 }, function(pid) turn(pid, 1) end) end
  for i = 1, #PAGES do
    put(id, "motd.dot" .. i, { type = "rect", pw = 12, ph = 12, color = i == page and { 255, 200, 60, 255 } or { 255, 255, 255, 70 }, layer = 92 },
      W / 2 - (#PAGES * 22) / 2 + (i - 1) * 22 + 5, H - 47)
  end
  put(id, "motd.hint", { type = "text", text = "arrows: pages     F4: close", color = { 255, 255, 255, 110 }, scale = 0.75, layer = 92 }, 22, H - 18)

  for name in pairs(prev[id] or {}) do if not used[name] then N.remove(id, name) end end   -- e.g. BACK on page 1
  prev[id] = used
end

N.on_ready(function(id)
  if shown[id] then return end                              -- once per connection (on_ready fires again after every map change)
  shown[id] = true
  xmod.timer.after(1.5, function(pid) if xmod.player.connected(pid) and N.has(pid) then show_page(pid, 1) end end, id)
end)
N.on_lost(function(id) open[id] = nil; prev[id] = nil end)                  -- the client already dropped everything
xmod.on("client_disconnect", function(id) open[id] = nil; prev[id] = nil; shown[id] = nil end)

xmod.cmd.say("/motd", "", function(id)
  if open[id] then close_motd(id) else show_page(id, 1) end
end, "open the server's MOTD window")
