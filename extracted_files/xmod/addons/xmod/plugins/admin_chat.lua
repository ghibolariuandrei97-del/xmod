-- Admin Chat: say/csay/hsay/psay/chat commands, "say @text" admin chat, and gag/ungag.
xmod.register("Admin Chat", "0.2", "xmod")
local U = require("util")

local function text_from(a, i) local t = {}; for k = i, a.n do t[#t + 1] = a[k] end; return table.concat(t, " ") end

U.cmd("say", "i", function(id, a)
  local m = text_from(a, 1); if m == "" then return end
  U.say(("{g}(ADMIN) {t}%s{y}: %s"):format(U.name(id), m))
end, "say <message> - message to everybody")
U.cmd("csay", "i", function(id, a) local m = text_from(a, 1); if m ~= "" then xmod.msg.print(0, "center", m) end end, "csay <message> - centre message")
U.cmd("hsay", "i", function(id, a)
  local m = text_from(a, 1); if m ~= "" then xmod.msg.hud(0, m, { y = 0.25, hold = 6, r = 255, g = 200, b = 0, channel = 4 }) end
end, "hsay <message> - HUD message")
U.cmd("psay", "i", function(id, a)
  if a.n < 2 then U.reply(id, "usage: xmod_psay <target> <message>"); return end
  local t, err = U.target1(id, a[1], { self = true }); if not t then U.reply(id, err); return end
  local m = text_from(a, 2)
  xmod.msg.chat(t, ("{g}(PM from {t}%s{g}){y}: %s"):format(U.name(id), m))
  if id ~= 0 then xmod.msg.chat(id, ("{g}(PM to {t}%s{g}){y}: %s"):format(xmod.player.name(t), m)) end
end, "psay <target> <message> - private message")
U.cmd("chat", "i", function(id, a)
  local m = text_from(a, 1); if m == "" then return end
  for _, p in ipairs(U.admins()) do xmod.msg.chat(p, ("{g}(ADMINS) {t}%s{y}: %s"):format(U.name(id), m)) end
end, "chat <message> - message to admins only")

-- "say @text": players can ask admins for help, admins talk among themselves
xmod.on("chat", function(id, text)
  if text:sub(1, 1) ~= "@" or #text < 2 then return end
  local m = text:sub(2):gsub("^%s+", "")
  if m == "" then return end
  local isadm = U.is_admin(id)
  local line = ("{g}(%s) {t}%s{y}: %s"):format(isadm and "ADMINS" or "TO ADMINS", xmod.player.name(id), m)
  for _, p in ipairs(U.admins()) do xmod.msg.chat(p, line, id) end
  if not isadm then xmod.msg.chat(id, line) end
  return true
end)

-- ---- gag -------------------------------------------------------------------------------------------------------------------
xmod.cvar.create("xmod_gag_default", "600")
xmod.on("chat", function(id, text)
  local d = xmod.player.data(id)
  if d.gag_until and d.gag_until > os.time() then
    xmod.msg.chat(id, ("{g}[xmod] {y}You are gagged for another %s."):format(U.fmt_time(d.gag_until - os.time())))
    return true
  end
end, 100)                                              -- high priority: runs before other chat handlers
U.cmd("gag", "e", function(id, a)
  local t, err = U.target1(id, a[1]); if not t then U.reply(id, err); return end
  local secs = U.clamp(tonumber(a[2]) or xmod.cvar.int("xmod_gag_default"), 10, 86400)
  xmod.player.data(t).gag_until = os.time() + secs
  U.activity(id, ("gag {t}%s{y} for %s"):format(xmod.player.name(t), U.fmt_time(secs)))
end, "gag <target> [seconds]")
U.cmd("ungag", "e", function(id, a)
  local t, err = U.target1(id, a[1], { self = true }); if not t then U.reply(id, err); return end
  xmod.player.data(t).gag_until = nil; U.activity(id, ("ungag {t}%s"):format(xmod.player.name(t)))
end, "ungag <target>")
