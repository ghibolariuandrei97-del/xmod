-- scenario: map start, 1 human admin + quota fill 8, rounds, kills, chat, radio, admin commands, editor
local ROOT = ".."   -- folder that contains addons/
local M = assert(loadfile("mock.lua"))(ROOT)
math.randomseed(7)
local function plugin(f) local fn = assert(loadfile(ROOT .. "/addons/xmod/plugins/xbots/" .. f)); fn() end
plugin("xbots.lua")
plugin("xbots_editor.lua")

xmod.emit("precache")
xmod.emit("init", 16)
-- the cfg exec: apply our cfg file
for line in io.lines(ROOT .. "/addons/xmod/configs/xbots.cfg") do
  local k, v = line:match("^(xbots_[%w_]+)%s+(%S+)")
  if k then M.cvars[k] = v:gsub('"', "") end
end
M.cvars.xbots_quota = "8"
xmod.emit("config")
M.human(1, "Andrew", 2)
M.run(4)
assert(M.analyzed, "auto analyze should start on a map without nav")
local nb = 0; for _ in pairs(M.bstate) do nb = nb + 1 end
print("bots after 4s:", nb)
M.run(20)
nb = 0; for _ in pairs(M.bstate) do nb = nb + 1 end
print("bots after 24s:", nb, "(want 7 = quota 8 - 1 human)")

local function round()
  xmod.emit("message:HLTV", "HLTV", 0, 0, { 0, 0 })
  M.spawn_all()
  M.run(5)
  -- some fights
  for i = 1, 6 do
    local all = {}
    for id, p in pairs(M.players) do if p.alive then all[#all + 1] = id end end
    if #all < 2 then break end
    local k, v = all[math.random(#all)], all[math.random(#all)]
    if k ~= v then
      M.players[v].alive = false
      xmod.emit("message:DeathMsg", "DeathMsg", 0, 0, { k, v, math.random(0, 1), "ak47" })
    end
    M.run(2)
  end
  xmod.emit("message:SendAudio", "SendAudio", 0, 0, { 0, "%!MRAD_ctwin", 100 })
  M.run(3)
end

round()
-- human talks + orders
xmod.emit("chat", 1, "salut boti, ce faceti?", false)
xmod.emit("chat", 1, "boti dupa mine", false)
xmod.emit("client_command", 1, "followme", "")
xmod.emit("client_command", 1, "holdpos", "")
M.run(6)
round()
-- flash, damage
for id in pairs(M.bstate) do
  xmod.emit("message:ScreenFade", "ScreenFade", 1, id, { 4096, 2048, 0, 255, 255, 255, 255 })
  xmod.emit("ham:takedamage:player", id, 1, 1, 20, 0)
  break
end
-- bomb planted
xmod.emit("message:SendAudio", "SendAudio", 0, 0, { 0, "%!MRAD_BOMBPL", 100 })
M.run(5)
round()

-- admin commands
local function c(...) local a = table.pack(...); M.cmdreg.server.xbots(a) end
c("list"); c("nav", "info"); c("difficulty", "4"); c("skill", "90", "all")
local someone; for id, p in pairs(M.players) do if p.bot then someone = p.name end end
c("info", someone); c("add", "t", "55", "Testerul"); c("say", someone, "salut tuturor"); c("order", "all", "follow", "Andrew")
c("kick", "t"); c("reload"); c("help")
M.cmdreg.say["/bots"](1, {})
assert(M.menu and #M.menu.items > 5)
M.menu.on_select(1, "x", "add_ct")
M.menu.on_select(1, "x", "follow")
-- editor
local e = function(...) local a = table.pack(...); M.cmdreg.client.xbots_nav(1, a) end
e("show"); e("add", "camp"); e("info"); e("from"); e("to", "jump", "both"); e("flag", "goal"); e("path"); e("go"); e("del")
M.run(3)
-- exports
local id = xmod.call("xbots.add", "Apiul", "ct", 70, "sniper")
print("export add ->", id)
assert(xmod.call("xbots.is_bot", id))
assert(xmod.call("xbots.task", id, { kind = "camp", x = 1, y = 2, z = 3, time = 10 }))
assert(xmod.call("xbots.order", 0, "rush"))
local mem = xmod.call("xbots.memory", id)
assert(type(mem) == "table")
M.run(10)
xmod.emit("map_end")

print(("-"):rep(60))
print("chat lines said:", #M.said)
for i = 1, math.min(#M.said, 14) do print("  " .. M.said[i]) end
print("bot commands (sample):")
for i = 1, math.min(#M.cmds, 12) do print("  " .. M.cmds[i]) end
local mv = 0; for k in pairs(M.vaults.xbots_memory or {}) do mv = mv + 1 end
print("memory entries:", mv)
print("errors:", #M.errors)
for _, e2 in ipairs(M.errors) do print("  " .. e2) end
local replies = 0
for _, l in ipairs(M.logs) do if l:find("^reply>") then replies = replies + 1 end end
print("console replies:", replies)
for _, l in ipairs(M.logs) do if l:find("reply> %[xbots%] [%a]") or l:find("^%[xbots%]") then print("  " .. l) end end
