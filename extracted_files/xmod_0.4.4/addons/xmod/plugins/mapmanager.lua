-- Map Manager: nextmap / currentmap / timeleft / thetime, end-of-map map vote with "extend" option, level change on intermission.
xmod.register("Map Manager", "0.2", "xmod")
local U, vote = require("util"), require("vote")

xmod.cvar.create("xmod_nextmap", "", xmod.cvar.SERVER)
xmod.cvar.create("xmod_vote_enabled", "1")
xmod.cvar.create("xmod_vote_time", "120")        -- start the vote this many seconds before the map ends
xmod.cvar.create("xmod_vote_maps", "5")          -- number of maps in the vote
xmod.cvar.create("xmod_extend_step", "15")       -- minutes added by "extend"
xmod.cvar.create("xmod_extend_max", "90")        -- never extend beyond this many minutes

local maps, voted, extended = {}, false, 0

local function read_list(text)
  local out = {}
  if not text then return out end
  for line in (text .. "\n"):gmatch("(.-)\r?\n") do
    line = line:gsub("//.*$", ""):gsub(";.*$", ""):gsub("^%s+", ""):gsub("%s+$", "")
    if line ~= "" and xmod.map_valid(line) then out[#out + 1] = line end
  end
  return out
end
local function load_maps()
  maps = read_list(xmod.fs.read("configs/maps.ini"))
  if #maps == 0 then
    local f = xmod.cvar.get("mapcyclefile"); if f == "" then f = "mapcycle.txt" end
    maps = read_list(xmod.fs.read_game(f))
  end
end
local function cycle_next()
  local cur = xmod.map()
  for i, m in ipairs(maps) do if m == cur then return maps[i % #maps + 1] end end
  return maps[1] or cur
end
local function nextmap() local n = xmod.cvar.get("xmod_nextmap"); if n == "" then n = cycle_next() end; return n end

xmod.on("config", function() load_maps(); voted = false; extended = 0; xmod.cvar.set("xmod_nextmap", cycle_next() or "") end)
xmod.export("maps.list", function() return maps end)

xmod.cmd.say("nextmap", "", function(id) xmod.msg.chat(0, "{g}[xmod] {y}Next map: {t}" .. nextmap()) end)
xmod.cmd.say("/nextmap", "", function(id) xmod.msg.chat(0, "{g}[xmod] {y}Next map: {t}" .. nextmap()) end)
xmod.cmd.say("currentmap", "", function(id) xmod.msg.chat(id, "{g}[xmod] {y}Current map: {t}" .. xmod.map()) end)
xmod.cmd.say("timeleft", "", function(id)
  local t = U.timeleft()
  xmod.msg.chat(0, t and ("{g}[xmod] {y}Time left: {t}" .. U.fmt_time(t)) or "{g}[xmod] {y}No time limit.")
end)
xmod.cmd.say("thetime", "", function(id) xmod.msg.chat(0, "{g}[xmod] {y}Server time: {t}" .. os.date("%H:%M:%S")) end)

local function start_vote()
  voted = true
  local pool = {}
  for _, m in ipairs(maps) do if m ~= xmod.map() then pool[#pool + 1] = m end end
  U.shuffle(pool)
  local opts = {}
  for i = 1, math.min(xmod.cvar.int("xmod_vote_maps"), #pool, 6) do opts[#opts + 1] = pool[i] end
  local can_extend = (xmod.cvar.num("mp_timelimit") + xmod.cvar.num("xmod_extend_step")) <= xmod.cvar.num("xmod_extend_max")
  if can_extend then opts[#opts + 1] = ("Extend map %d min"):format(xmod.cvar.int("xmod_extend_step")) end
  if #opts == 0 then return end
  xmod.msg.chat(0, "{g}[xmod] {y}Time to choose the next map!")
  vote.start{ title = "Choose the next map", options = opts, time = 20, on_done = function(winner)
    if not winner then xmod.msg.chat(0, "{g}[xmod] {y}Nobody voted. Next map: {t}" .. nextmap()); return end
    if can_extend and winner == #opts then
      xmod.cvar.set("mp_timelimit", tostring(xmod.cvar.num("mp_timelimit") + xmod.cvar.num("xmod_extend_step")))
      voted = false; extended = extended + 1
      xmod.msg.chat(0, "{g}[xmod] {y}Map extended!")
    else
      xmod.cvar.set("xmod_nextmap", opts[winner]); xmod.msg.chat(0, "{g}[xmod] {y}Next map: {t}" .. opts[winner])
    end
  end }
end

xmod.timer.every(1.0, function()
  if voted or xmod.cvar.int("xmod_vote_enabled") == 0 or vote.running() then return end
  local t = U.timeleft()
  if t and t > 0 and t <= xmod.cvar.num("xmod_vote_time") and #maps > 1 then start_vote() end
end)

-- SVC_INTERMISSION (30): the map is over -> go to our chosen next map shortly before the game would pick its own
xmod.on("message:svc:30", function()
  local wait = math.max(1, xmod.cvar.num("mp_chattime") - 2)
  xmod.timer.after(wait, function() xmod.exec("changelevel " .. nextmap()) end)
end)
