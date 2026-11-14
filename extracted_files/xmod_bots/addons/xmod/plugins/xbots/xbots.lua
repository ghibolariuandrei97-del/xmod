-- xbots.lua - the "brain" of xmod bots (needs the xmod_bots extension = xmod.bot.*)
--
-- The C++ extension is the body: it creates the fake clients and, every server frame, aims (spring model,
-- deliberate error, recoil control), walks the navigation graph (jump / duck / ladder, stuck recovery),
-- fires (tap / burst / spray, never through a teammate) and does the vision traces (FOV, smoke).
-- This plugin is the brain, running ~10 times per second per bot: what to do, where to go, whom to shoot,
-- what to buy, team plans, obeying human radio / chat orders, hearing, long-term memory and chat.
--
-- plugins.ini:   xbots/xbots.lua         (and xbots/xbots_editor.lua for the nav editor)
-- Commands:      xbots help   (console)  ·  say /bots  (admin menu)
-- Other plugins: xmod.call("xbots.add", ...) and the events listed at the end of this file / docs/XBOTS.md
xmod.register("xbots", "1.0.0", "Andrew & Claude")

local U    = require("util")
local menu = require("menu")
local json = require("json")

local B, P, E, CV = xmod.bot, xmod.player, xmod.ent, xmod.cvar   -- xmod.bot comes from the extension
local function now() return xmod.gametime() end
local rand = math.random

-- =============================================================================================
-- constants
local IN_ATTACK, IN_JUMP, IN_DUCK, IN_USE, IN_ATTACK2, IN_RELOAD = 1, 2, 4, 32, 2048, 8192
local FIRE_OFF, FIRE_TAP, FIRE_BURST, FIRE_SPRAY = 0, 1, 2, 3
local NF = { CROUCH = 1, LADDER = 2, CAMP = 4, GOAL = 8, RESCUE = 16, TSPAWN = 32, CTSPAWN = 64, SNIPER = 128,
             HOSTAGE = 256, AVOID = 512, DOOR = 1024, LEARNED = 2048, JUMP = 4096 }

-- weapons: wid (CSW_*), kind, clip size, buy alias, price
local W = {
  p228 = {1, "pistol", 13, "p228", 600},       glock18 = {17, "pistol", 20, "glock", 400},
  usp = {16, "pistol", 12, "usp", 500},        deagle = {26, "pistol", 7, "deagle", 650},
  elite = {10, "pistol", 30, "elites", 800},   fiveseven = {11, "pistol", 20, "fn57", 750},
  m3 = {21, "shotgun", 8, "m3", 1700},         xm1014 = {5, "shotgun", 7, "xm1014", 3000},
  mac10 = {7, "smg", 30, "mac10", 1400},       tmp = {23, "smg", 30, "tmp", 1250},
  mp5navy = {19, "smg", 30, "mp5", 1500},      ump45 = {12, "smg", 25, "ump45", 1700},
  p90 = {30, "smg", 50, "p90", 2350},          galil = {14, "rifle", 35, "galil", 2000},
  famas = {15, "rifle", 25, "famas", 2250},    ak47 = {28, "rifle", 30, "ak47", 2500},
  m4a1 = {22, "rifle", 30, "m4a1", 3100},      sg552 = {27, "rifle", 30, "sg552", 3500},
  aug = {8, "rifle", 30, "aug", 3500},         scout = {3, "sniper", 10, "scout", 2750},
  awp = {18, "sniper", 10, "awp", 4750},       sg550 = {13, "autosniper", 30, "sg550", 4200},
  g3sg1 = {24, "autosniper", 20, "g3sg1", 5000}, m249 = {20, "mg", 100, "m249", 5750},
  knife = {29, "knife", 0}, hegrenade = {4, "nade", 0}, flashbang = {25, "nade", 0},
  smokegrenade = {9, "nade", 0}, c4 = {6, "c4", 0},
}
local PRIMARY = { shotgun = true, smg = true, rifle = true, sniper = true, autosniper = true, mg = true }
local CSW_C4, CSW_HE = 6, 4

local PERS = {
  aggressive = { aggr = 0.85, camp = 0.10, chat = 0.60, awp = 0.05, desc = "agresiv, intra primul, nu sta pe loc" },
  normal     = { aggr = 0.55, camp = 0.30, chat = 0.45, awp = 0.10, desc = "echilibrat si de echipa" },
  careful    = { aggr = 0.30, camp = 0.60, chat = 0.30, awp = 0.15, desc = "prudent, joaca pe pozitii si pe info" },
  sniper     = { aggr = 0.35, camp = 0.75, chat = 0.30, awp = 0.90, desc = "sniper rabdator" },
  joker      = { aggr = 0.60, camp = 0.20, chat = 0.95, awp = 0.10, desc = "glumet, vorbeste mult, ironic dar prietenos" },
}
local PERS_NAMES = { "aggressive", "normal", "normal", "careful", "sniper", "joker" }
local DIFF = { [0] = {0, 20}, {20, 40}, {40, 65}, {65, 85}, {85, 100} }

-- =============================================================================================
-- cvars
local CVARS = {
  xbots_quota = "0",            -- how many bots (see xbots_quota_mode)
  xbots_quota_mode = "fill",    -- fill = bots + humans = quota · fixed = always quota bots · match = quota bots per human
  xbots_keep_slot = "1",        -- always leave this many slots free for humans
  xbots_join_team = "any",      -- any | t | ct
  xbots_difficulty = "2",       -- 0 newbie · 1 easy · 2 normal · 3 hard · 4 expert
  xbots_skill_min = "-1",       -- override the difficulty range (0..100, -1 = use difficulty)
  xbots_skill_max = "-1",
  xbots_tag = "",               -- name prefix, e.g. "[BOT] "
  xbots_think = "0.1",          -- seconds between brain ticks per bot
  xbots_reaction = "1.0",       -- reaction time multiplier (higher = slower)
  xbots_aim_head = "1.0",       -- headshot preference multiplier
  xbots_fov = "100",            -- field of view in degrees
  xbots_vision = "4000",        -- how far bots can see
  xbots_buy = "1",              -- bots buy weapons/equipment
  xbots_eco = "1",              -- team economy (save rounds)
  xbots_grenades = "1",         -- bots throw HE grenades
  xbots_restrict = "",          -- weapons bots never buy, e.g. "awp,g3sg1,sg550"
  xbots_knife_only = "0",
  xbots_peaceful = "0",         -- bots never attack
  xbots_frozen = "0",           -- bots do not move
  xbots_ffa = "0",              -- bots attack everybody (free for all / deathmatch)
  xbots_radio = "1",            -- radio messages
  xbots_obey = "1",             -- obey human radio + chat orders ("followme", "boti dupa mine", ...)
  xbots_chat = "1",             -- chat
  xbots_chat_rate = "1.0",      -- chat frequency multiplier
  xbots_chat_lang = "ro",       -- ro | en  (sections in configs/xbots_chat.ini)
  xbots_ai = "0",               -- 1 = replies through an LLM (configs/xbots.ini [ai], needs xmod_query)
  xbots_memory = "1",           -- long-term memory (data/vaults/xbots_memory.vault)
  xbots_learn = "1",            -- learn: nav nodes from humans, danger/kill spots from every death
  xbots_auto_nav = "1",         -- analyze the map automatically when it has no .nav file
  xbots_nav_max = "6000",       -- max nav nodes
  xbots_admin_flag = "h",       -- access letter for xbots commands and the /bots menu
  xbots_debug = "0",
}
for k, v in pairs(CVARS) do CV.create(k, v) end

local cfg = {}
local function read_cfg()
  cfg.quota, cfg.mode, cfg.keep = CV.int("xbots_quota"), CV.get("xbots_quota_mode"):lower(), CV.int("xbots_keep_slot")
  cfg.join, cfg.diff = CV.get("xbots_join_team"):lower(), math.max(0, math.min(4, CV.int("xbots_difficulty")))
  cfg.smin, cfg.smax, cfg.tag = CV.int("xbots_skill_min"), CV.int("xbots_skill_max"), CV.get("xbots_tag")
  cfg.think = math.max(0.05, math.min(0.5, CV.num("xbots_think")))
  cfg.react, cfg.head = math.max(0.1, CV.num("xbots_reaction")), math.max(0, CV.num("xbots_aim_head"))
  cfg.fov, cfg.vision = CV.num("xbots_fov"), CV.num("xbots_vision")
  cfg.buy, cfg.eco, cfg.nades = CV.int("xbots_buy") == 1, CV.int("xbots_eco") == 1, CV.int("xbots_grenades") == 1
  cfg.restrict = {}
  for w in CV.get("xbots_restrict"):lower():gmatch("[%w_]+") do cfg.restrict[w] = true end
  cfg.knife, cfg.peaceful, cfg.frozen = CV.int("xbots_knife_only") == 1, CV.int("xbots_peaceful") == 1, CV.int("xbots_frozen") == 1
  cfg.ffa, cfg.radio, cfg.obey = CV.int("xbots_ffa") == 1, CV.int("xbots_radio") == 1, CV.int("xbots_obey") == 1
  cfg.chat, cfg.chat_rate, cfg.lang = CV.int("xbots_chat") == 1, math.max(0, CV.num("xbots_chat_rate")), CV.get("xbots_chat_lang"):lower()
  cfg.ai, cfg.memory, cfg.learn = CV.int("xbots_ai") == 1, CV.int("xbots_memory") == 1, CV.int("xbots_learn") == 1
  cfg.auto_nav, cfg.nav_max = CV.int("xbots_auto_nav") == 1, CV.int("xbots_nav_max")
  cfg.flag, cfg.debug = CV.get("xbots_admin_flag"), CV.int("xbots_debug") == 1
  if cfg.flag == "" then cfg.flag = "h" end
end
read_cfg()

local function ext_ok() return xmod.ext.available("bot") end
local function dbg(fmt, ...) if cfg.debug then xmod.log(string.format(fmt, ...)) end end

-- =============================================================================================
-- state
local bots = {}          -- [id] = brain
local uid_seq = 0
local MAP = { kind = "dm", sites = {}, rescue = {}, tspawn = nil, ctspawn = nil }
local ROUND = { n = 0, start = -99, freeze_end = -99, planted = false, bomb = nil, dropped = nil, carrier = nil,
                t_site = 1, ended = false, kills = {} }
local ready = false      -- the map is running and the extension is there
local shot_t = {}        -- last time each player pressed attack
local hist = {}          -- recent chat lines {n = name, t = text}
local roster = {}        -- {name, pers, skill}
local AI = { url = "", key = "", model = "", provider = "openai", timeout = 8, per_min = 8, max_tokens = 60, calls = {}, prompt = nil }
local CHAT = {}          -- CHAT[lang][event] = {lines}
local MEMV = xmod.vault.open("xbots_memory")
local team_radio_t = { [1] = 0, [2] = 0 }

-- iterate our bots, dropping entries whose player left (disconnect events can be missed on shutdown / map change)
local function live()
  local ok = ext_ok()
  for id in pairs(bots) do
    if not P.connected(id) or not ok or not B.is_bot(id) then bots[id] = nil end
  end
  return pairs(bots)
end
local last_chat_t = -99

-- =============================================================================================
-- helpers
local function dist3(ax, ay, az, bx, by, bz) local dx, dy, dz = ax - bx, ay - by, az - bz; return math.sqrt(dx * dx + dy * dy + dz * dz) end
local function pdist(a, b)
  local ax, ay, az = P.origin(a); local bx, by, bz = P.origin(b)
  return dist3(ax, ay, az, bx, by, bz)
end
local function has_weapon(id, wid) return (E.get(id, "weapons") & (1 << wid)) ~= 0 end
local function enemy_team(t) return t == 1 and 2 or 1 end

local function find_all(cls)
  local out, e = {}, 0
  while true do
    e = E.find(e, cls)
    if not e or e == 0 then break end
    out[#out + 1] = e
  end
  return out
end
local function ent_center(e)
  local x1, y1, z1 = E.get(e, "absmin"); local x2, y2, z2 = E.get(e, "absmax")
  if math.abs(x2 - x1) + math.abs(y2 - y1) < 8 then return E.get(e, "origin") end
  return (x1 + x2) / 2, (y1 + y2) / 2, (z1 + z2) / 2
end

local function best_weapons(id)   -- returns primary name, pistol name
  local w = E.get(id, "weapons")
  local prim, pist
  for name, d in pairs(W) do
    if (w & (1 << d[1])) ~= 0 then
      if PRIMARY[d[2]] then prim = name elseif d[2] == "pistol" then pist = name end
    end
  end
  return prim, pist
end

local function switch_best(id, br)
  local prim, pist = best_weapons(id)
  local want = prim or pist or "knife"
  local cur = B.weapon(id)
  if not cur or cur.name ~= want then B.cmd(id, "weapon_" .. want) end
  br.knifed = false
end

local function strip_ro(s)
  return (s:gsub("ă", "a"):gsub("â", "a"):gsub("î", "i"):gsub("ș", "s"):gsub("ş", "s"):gsub("ț", "t"):gsub("ţ", "t")
           :gsub("Ă", "A"):gsub("Â", "A"):gsub("Î", "I"):gsub("Ș", "S"):gsub("Ş", "S"):gsub("Ț", "T"):gsub("Ţ", "T"))
end

-- =============================================================================================
-- config files
local function read_file(rel)
  local ok, txt = pcall(xmod.fs.read, rel)
  if ok and type(txt) == "string" then return txt end
  return nil
end

local function read_ini(rel)
  local txt, out, sec = read_file(rel), {}, "main"
  if not txt then return nil end
  for line in (txt .. "\n"):gmatch("([^\n]*)\n") do
    line = U.trim((line:gsub("\r", "")))
    if line ~= "" and not line:match("^[;#]") then
      local s = line:match("^%[(.-)%]$")
      if s then sec = s:lower(); out[sec] = out[sec] or {}
      else out[sec] = out[sec] or {}; table.insert(out[sec], line) end
    end
  end
  return out
end

local DEFAULT_NAMES = { "Vulpea", "Bizonul", "Fulger", "Zmeul", "Mitica", "Grivei", "Turbo", "Bujor", "Viteazul",
  "Ciorba", "Sarmale", "Pescarul", "Ghost", "Raven", "Blitz", "Nova", "Viper", "Echo", "Atlas", "Rex", "Kobra",
  "Gheara", "Lupul", "Corbul", "Haiduc", "Tunetul", "Spartan", "Nebunu", "Kaiser", "Bombardieru", "Comandoul", "Smecheru" }

local function load_configs()
  -- names / profiles / ai
  roster = {}
  local ini = read_ini("configs/xbots.ini")
  if ini and ini.names then
    for _, l in ipairs(ini.names) do
      local parts = {}
      for p in (l .. "|"):gmatch("([^|]*)|") do parts[#parts + 1] = U.trim(p) end
      if parts[1] ~= "" then
        roster[#roster + 1] = { name = parts[1]:sub(1, 24), pers = PERS[parts[2] or ""] and parts[2] or nil,
                                skill = tonumber(parts[3] or "") }
      end
    end
  end
  if #roster == 0 then for _, n in ipairs(DEFAULT_NAMES) do roster[#roster + 1] = { name = n } end end
  if ini and ini.ai then
    for _, l in ipairs(ini.ai) do
      local k, v = l:match("^([%w_]+)%s*=%s*(.-)%s*$")
      if k then
        k = k:lower()
        if k == "timeout" or k == "per_minute" or k == "max_tokens" then
          AI[k == "per_minute" and "per_min" or k] = tonumber(v) or AI[k]
        elseif k == "url" or k == "key" or k == "model" or k == "provider" or k == "prompt" then
          AI[k] = v
        end
      end
    end
  end
  -- chat lines
  CHAT = {}
  local chat = read_ini("configs/xbots_chat.ini") or {}
  for sec, lines in pairs(chat) do
    local lang, ev = sec:match("^(%a+)%.([%w_]+)$")
    if lang then CHAT[lang] = CHAT[lang] or {}; CHAT[lang][ev] = lines end
  end
  if not CHAT.ro and not CHAT.en then   -- minimal built-in fallback
    CHAT.ro = { kill = { "ups", "ez", "next" }, death = { "lag...", "nu se poate", "noroc chior" },
                hello = { "salut {name}", "sal" }, win = { "gg" }, lose = { "ff ff" }, re_hi = { "salut", "sal {name}" },
                re_generic = { "da", "nu cred", "hmm", "lol" } }
  end
end

-- =============================================================================================
-- long-term memory (per bot name, in data/vaults/xbots_memory.vault)
local function mem_key(name) return "bot:" .. name:lower() end
local function load_mem(name)
  local m = MEMV:get(mem_key(name))
  if type(m) ~= "table" then m = { k = 0, d = 0, hs = 0, rounds = 0, wins = 0, since = os.time() } end
  m.w = m.w or {}; m.rel = m.rel or {}
  return m
end
local function save_mem(br) if cfg.memory and br and br.mem then MEMV:set(mem_key(br.name), br.mem) end end
local function who_key(p)
  local s = P.steamid(p) or ""
  if s == "" or s:find("BOT") or s:find("LAN") or s:find("PENDING") or s:find("VALVE_ID_LOOPBACK") then
    return "n:" .. P.name(p):lower()
  end
  return s
end
local function rel_of(br, p)
  local k = who_key(p)
  local r = br.mem.rel[k]
  if not r then
    r = { n = P.name(p), k = 0, d = 0 }
    br.mem.rel[k] = r
    local cnt, oldest, ok = 0, nil, nil
    for kk, rr in pairs(br.mem.rel) do
      cnt = cnt + 1
      if not oldest or (rr.t or 0) < oldest then oldest, ok = rr.t or 0, kk end
    end
    if cnt > 40 and ok then br.mem.rel[ok] = nil end
  end
  r.n, r.t = P.name(p), os.time()
  return r
end
local function nemesis_of(br)   -- the player who killed this bot the most (and more than it killed them)
  local best, bk
  for k, r in pairs(br.mem.rel) do
    if (r.d or 0) >= 3 and (r.d or 0) > (r.k or 0) and (not best or r.d > best.d) then best, bk = r, k end
  end
  return best, bk
end
local function victim_of(br)
  local best
  for _, r in pairs(br.mem.rel) do if (r.k or 0) >= 3 and (not best or r.k > best.k) then best = r end end
  return best
end
local function fav_weapon(br)
  local best, bn
  for name, n in pairs(br.mem.w) do if W[name] and PRIMARY[W[name][2]] and (not best or n > best) then best, bn = n, name end end
  return bn
end

-- =============================================================================================
-- chat
local function push_hist(name, text)
  hist[#hist + 1] = { n = name, t = text }
  if #hist > 12 then table.remove(hist, 1) end
end

local function say(id, text, team)
  if not P.connected(id) or not bots[id] then return end
  if xmod.emit("xbots_chat", id, text) == true then return end
  B.say(id, text, team and true or false)
  push_hist(P.name(id), text)
end

local function line_for(ev)
  local l = (CHAT[cfg.lang] or {})[ev] or (CHAT.en or {})[ev] or (CHAT.ro or {})[ev]
  if not l or #l == 0 then return nil end
  return l[rand(#l)]
end
local function fill(s, vars) return (s:gsub("{(%w+)}", function(k) return vars[k] ~= nil and tostring(vars[k]) or "" end)) end

-- an event happened: maybe say something about it (chance, cooldowns, typing delay)
local function bot_chat(id, ev, vars, opt)
  opt = opt or {}
  local br = bots[id]
  if not br or not cfg.chat then return false end
  local t = now()
  if not opt.force then
    local chance = (opt.chance or 0.3) * br.pers.chat * cfg.chat_rate * (1 + math.abs(br.mood) * 0.15)
    if rand() > chance or t < (br.chat_t or 0) or t - last_chat_t < 2.5 then return false end
  end
  local s = line_for(ev)
  if not s then return false end
  vars = vars or {}
  vars.me, vars.map = br.name, xmod.map()
  local text = fill(s, vars)
  br.chat_t = t + 8 + rand() * 14
  last_chat_t = t
  local uid = br.uid
  xmod.timer.after((opt.delay or (0.8 + rand() * 2.5)) + #text * 0.035, function()
    local b2 = bots[id]
    if b2 and b2.uid == uid then say(id, text, opt.team) end
  end)
  return true
end

-- radio with per-bot and per-team cooldowns
local function radio(id, br, cmd, prob)
  if not cfg.radio or rand() > (prob or 1) then return end
  local t, team = now(), P.team(id)
  if t < (br.radio_t or 0) or t < (team_radio_t[team] or 0) then return end
  br.radio_t, team_radio_t[team] = t + 8 + rand() * 6, t + 3
  B.cmd(id, cmd)
end

-- =============================================================================================
-- AI chat (optional): OpenAI-compatible /v1/chat/completions (OpenAI, OpenRouter, Groq, LM Studio,
-- Ollama /v1, ...) or Anthropic /v1/messages. Settings in configs/xbots.ini [ai]; the key never
-- leaves that file except in the request header.
local DEFAULT_PROMPT = "Esti {me}, un jucator de Counter-Strike 1.6 pe un server public, in echipa {team}. " ..
  "Personalitate: {pers}. Raspunzi in chat ca un gamer adevarat: foarte scurt (maxim 12 cuvinte), fara emoji, " ..
  "fara ghilimele, fara diacritice, in limba {lang}. Nu spune niciodata ca esti AI sau bot; daca te intreaba, " ..
  "glumesti. Fii jucaus, poti sa te tachinezi, dar fara injurii, fara jigniri grave si fara subiecte urate. " ..
  "Ce stii: harta {map}, scor personal {k} kill-uri / {d} morti. {memory}"

local function ai_ready() return cfg.ai and AI.url ~= "" and xmod.ext.available("query", "http_post") end

local function memory_line(br, from)
  local parts = {}
  if from and P.connected(from) then
    local r = br.mem.rel[who_key(from)]
    if r then parts[#parts + 1] = string.format("%s te-a omorat de %d ori, tu pe el de %d ori.", r.n, r.d or 0, r.k or 0) end
  end
  local nem = nemesis_of(br)
  if nem then parts[#parts + 1] = "Nemesisul tau e " .. nem.n .. "." end
  if br.mood <= -2 then parts[#parts + 1] = "Esti cam nervos, ai murit des." elseif br.mood >= 2 then parts[#parts + 1] = "Esti in forma azi." end
  return table.concat(parts, " ")
end

local function sanitize(s)
  s = s:gsub("[\r\n\t]", " "):gsub("[\"%%;]", ""):gsub("^%s*[%w%[%]_%-%.]+%s*:%s*", "", 1)
  s = U.trim(s)
  if #s > 120 then s = s:sub(1, 120) end
  return s
end

local template_reply   -- forward

local function ai_reply(bid, from, text)
  if not ai_ready() then return false end
  local t = os.time()
  local keep = {}
  for _, c in ipairs(AI.calls) do if t - c < 60 then keep[#keep + 1] = c end end
  AI.calls = keep
  if #AI.calls >= AI.per_min then return false end
  AI.calls[#AI.calls + 1] = t
  local br = bots[bid]
  local vars = { me = br.name, pers = br.pers.desc, lang = cfg.lang == "en" and "engleza" or "romana", map = xmod.map(),
                 k = br.mem.k, d = br.mem.d, team = P.team(bid) == 1 and "Terrorist" or "Counter-Terrorist",
                 memory = memory_line(br, from) }
  local system = fill(AI.prompt or DEFAULT_PROMPT, vars)
  local lines = {}
  for i = math.max(1, #hist - 7), #hist do lines[#lines + 1] = hist[i].n .. ": " .. hist[i].t end
  local user = "Chatul de pe server (ultimele mesaje):\n" .. table.concat(lines, "\n") ..
               "\n\nScrie DOAR raspunsul tau (ca " .. br.name .. ") la mesajul lui " .. P.name(from) .. "."
  local body, headers
  if AI.provider == "anthropic" then
    body = json.encode({ model = AI.model, max_tokens = AI.max_tokens, system = system, messages = { { role = "user", content = user } } })
    headers = { ["Content-Type"] = "application/json", ["x-api-key"] = AI.key, ["anthropic-version"] = "2023-06-01" }
  else
    body = json.encode({ model = AI.model, max_tokens = AI.max_tokens, temperature = 0.9, stream = false,
                         messages = { { role = "system", content = system }, { role = "user", content = user } } })
    headers = { ["Content-Type"] = "application/json" }
    if AI.key ~= "" then headers["Authorization"] = "Bearer " .. AI.key end
  end
  local uid = br.uid
  local ok = pcall(xmod.query.http_post, AI.url, body, { headers = headers, timeout = AI.timeout }, function(err, res)
    local b2 = bots[bid]
    if not b2 or b2.uid ~= uid or not P.connected(bid) then return end
    local reply
    if not err and res and res.status == 200 then
      local d = json.decode(res.body or "")
      if type(d) == "table" then
        if d.choices and d.choices[1] and d.choices[1].message then reply = d.choices[1].message.content
        elseif d.message and d.message.content then reply = d.message.content
        elseif d.content and d.content[1] then reply = d.content[1].text end
      end
    elseif cfg.debug then
      dbg("AI error: %s %s", tostring(err), res and tostring(res.status) or "")
    end
    if type(reply) == "string" and reply ~= "" then
      say(bid, sanitize(reply))
    else
      template_reply(bid, from, text)
    end
  end)
  return ok
end

-- keyword replies (no AI)
local KW = {
  { "re_bot", { "esti bot", "e bot", "is bot", "you bot", "u bot", "a bot", "robot", "aimbot", " ai?" } },
  { "re_insult", { "noob", "nub", "n00b", "prost", "ratat", "idiot", "fraier", "trash", "bad", "slab", "jeg" } },
  { "re_gg", { " gg", "gg ", "good game", "wp", "well played" } },
  { "re_praise", { "nice", "bravo", "frumos", "tare", "good", "gj", "sick", "fain", "beton" } },
  { "re_hi", { "salut", "buna", "hello", " hi ", "hey", "servus", "noroc", " sal ", "ceau", " yo " } },
  { "re_bye", { " pa ", "bye", "noapte buna", " gn ", "plec", "ma duc" } },
  { "re_question", { "?" } },
}
function template_reply(bid, from, text)
  local low = " " .. strip_ro(text:lower()) .. " "
  local ev = "re_generic"
  for _, e in ipairs(KW) do
    for _, w in ipairs(e[2]) do if low:find(w, 1, true) then ev = e[1] break end end
    if ev ~= "re_generic" then break end
  end
  local br = bots[bid]
  local nem = br and nemesis_of(br)
  bot_chat(bid, ev, { name = P.name(from), nemesis = nem and nem.n or "" }, { force = true, delay = 1.2 + rand() * 2 })
end

-- =============================================================================================
-- skill -> aim profile
local function rand_skill()
  local lo, hi = DIFF[cfg.diff][1], DIFF[cfg.diff][2]
  if cfg.smin >= 0 then lo = cfg.smin end
  if cfg.smax >= 0 then hi = math.max(lo, cfg.smax) end
  return rand(math.floor(lo), math.floor(hi))
end

local function apply_skill(id, br)
  local s = math.max(0, math.min(100, br.skill)) / 100
  B.set(id, {
    aim_speed = 30 + 370 * s ^ 1.2, max_turn = 250 + 750 * s, aim_error = 30 - 26 * s,
    recoil = 0.15 + 0.8 * s, lag = 0.14 - 0.11 * s, fov = cfg.fov, vision = cfg.vision,
    frozen = cfg.frozen,
  })
  br.react_base = (0.55 - 0.42 * s) * cfg.react
  br.head = math.min(0.95, (0.1 + 0.55 * s) * cfg.head)
end

-- =============================================================================================
-- map knowledge
local function scan_map()
  MAP = { kind = "dm", sites = {}, rescue = {}, tspawn = nil, ctspawn = nil }
  for _, cls in ipairs({ "func_bomb_target", "info_bomb_target" }) do
    for _, e in ipairs(find_all(cls)) do
      local x, y, z = ent_center(e)
      MAP.sites[#MAP.sites + 1] = { x = x, y = y, z = z, nodes = {} }
    end
  end
  for _, cls in ipairs({ "func_hostage_rescue", "info_hostage_rescue" }) do
    for _, e in ipairs(find_all(cls)) do local x, y, z = ent_center(e); MAP.rescue[#MAP.rescue + 1] = { x = x, y = y, z = z } end
  end
  local function avg(cls)
    local l = find_all(cls)
    if #l == 0 then return nil end
    local sx, sy, sz = 0, 0, 0
    for _, e in ipairs(l) do local x, y, z = E.get(e, "origin"); sx, sy, sz = sx + x, sy + y, sz + z end
    return { x = sx / #l, y = sy / #l, z = sz / #l }
  end
  MAP.tspawn, MAP.ctspawn = avg("info_player_deathmatch"), avg("info_player_start")
  if #MAP.sites > 0 then MAP.kind = "de"
  elseif #find_all("hostage_entity") > 0 then
    MAP.kind = "cs"
    if #MAP.rescue == 0 and MAP.ctspawn then MAP.rescue[1] = MAP.ctspawn end
  end
  -- GOAL nodes of the graph, grouped by the nearest bomb site
  if ext_ok() and #MAP.sites > 0 then
    for _, n in ipairs(B.nav_find(NF.GOAL)) do
      local nd = B.nav_node(n)
      local best, bd
      for _, s in ipairs(MAP.sites) do
        local d = dist3(nd.x, nd.y, nd.z, s.x, s.y, s.z)
        if not bd or d < bd then best, bd = s, d end
      end
      if best and bd < 1500 then best.nodes[#best.nodes + 1] = { x = nd.x, y = nd.y, z = nd.z } end
    end
  end
  dbg("map %s kind=%s sites=%d rescue=%d", xmod.map(), MAP.kind, #MAP.sites, #MAP.rescue)
end

local function site_point(i)
  local s = MAP.sites[i]
  if not s then return nil end
  if #s.nodes > 0 then local n = s.nodes[rand(#s.nodes)]; return n.x, n.y, n.z end
  return s.x, s.y, s.z
end

local function poll_bomb()
  ROUND.bomb, ROUND.dropped, ROUND.carrier = nil, nil, nil
  for _, e in ipairs(find_all("grenade")) do
    if (E.get(e, "model") or ""):find("w_c4") then local x, y, z = E.get(e, "origin"); ROUND.bomb = { x = x, y = y, z = z, ent = e } end
  end
  ROUND.planted = ROUND.bomb ~= nil
  if not ROUND.planted then
    for _, e in ipairs(find_all("weaponbox")) do
      if (E.get(e, "model") or ""):find("w_backpack") then local x, y, z = E.get(e, "origin"); ROUND.dropped = { x = x, y = y, z = z } end
    end
    for _, p in ipairs(P.all()) do if P.alive(p) and has_weapon(p, CSW_C4) then ROUND.carrier = p end end
  end
end

local function hostages()
  local out = {}
  for _, h in ipairs(find_all("hostage_entity")) do
    if E.get(h, "health") > 0 and (E.get(h, "effects") & xmod.EF_NODRAW) == 0 then out[#out + 1] = h end
  end
  return out
end

-- =============================================================================================
-- bots: create / remove / team join
local function used_names()
  local u = {}
  for i = 1, xmod.maxplayers() do if P.connected(i) then u[P.name(i):lower()] = true end end
  return u
end

local function pick_entry(name)
  if name then
    for _, r in ipairs(roster) do if r.name:lower() == name:lower() then return r end end
    return { name = name }
  end
  local used, free, regulars = used_names(), {}, {}
  for _, r in ipairs(roster) do
    if not used[(cfg.tag .. r.name):lower()] then
      free[#free + 1] = r
      if MEMV:has(mem_key(r.name)) then regulars[#regulars + 1] = r end
    end
  end
  if #regulars > 0 and rand() < 0.6 then return regulars[rand(#regulars)] end
  if #free > 0 then return free[rand(#free)] end
  return { name = "xBot" .. rand(100, 999) }
end

local function team_wanted(br)
  local t = br.want_team or cfg.join
  if t == 1 or t == "t" or t == "1" then return 1 elseif t == 2 or t == "ct" or t == "2" then return 2 end
  return 5
end

local function join_team(id, uid, attempt)
  local br = bots[id]
  if not br or br.uid ~= uid or not P.connected(id) then return end
  local t = P.team(id)
  if t == 1 or t == 2 then return end
  local want = team_wanted(br)
  if attempt % 2 == 1 then
    B.cmd(id, "jointeam " .. want)
    xmod.timer.after(0.3, function() if bots[id] == br then B.cmd(id, "joinclass 5") end end)
  else
    B.cmd(id, "menuselect " .. want)
    xmod.timer.after(0.3, function() if bots[id] == br then B.cmd(id, "menuselect 5") end end)
  end
  if attempt < 6 then xmod.timer.after(1.5, join_team, id, uid, attempt + 1) end
end

local function add_bot(o)
  o = o or {}
  if not ext_ok() then return nil, "the xmod_bots extension is not loaded" end
  local entry = pick_entry(o.name)
  local r = B.add(cfg.tag .. entry.name)
  if type(r) ~= "number" or r <= 0 then return nil, tostring(r) end
  local id = r
  uid_seq = uid_seq + 1
  local pname = o.pers or entry.pers or PERS_NAMES[rand(#PERS_NAMES)]
  local br = {
    id = id, uid = uid_seq, name = entry.name, skill = o.skill or entry.skill or rand_skill(),
    pers_name = pname, pers = PERS[pname] or PERS.normal, mem = load_mem(entry.name), mood = 0,
    want_team = o.team, alive = false, next_think = now() + 0.5, knife_runner = rand() < 0.5,
  }
  br.skill = math.max(0, math.min(100, br.skill))
  bots[id] = br
  apply_skill(id, br)
  xmod.timer.after(0.4, join_team, id, br.uid, 1)
  xmod.emit("xbots_added", id)
  dbg("added %s (#%d) skill %d %s", entry.name, id, br.skill, pname)
  return id
end

local function kick_bot(id)
  local br = bots[id]
  if not br then return false end
  save_mem(br)
  return B.remove(id)
end

local function count_bots(team)
  local n = 0
  for id in live() do if not team or P.team(id) == team then n = n + 1 end end
  return n
end

-- after an admin adds/kicks by hand, move the quota so the quota manager keeps the new count
local function sync_quota(delta)
  local humans = 0
  for i = 1, xmod.maxplayers() do if P.connected(i) and not P.is_bot(i) then humans = humans + 1 end end
  local n = math.max(0, count_bots() + (delta or 0))
  if cfg.mode == "fill" then CV.set("xbots_quota", tostring(n + humans))
  elseif cfg.mode == "fixed" then CV.set("xbots_quota", tostring(n)) end
end

-- =============================================================================================
-- quota
local function quota_tick()
  if not ready or not ext_ok() then return end
  read_cfg()
  local humans = 0
  for i = 1, xmod.maxplayers() do if P.connected(i) and not P.is_bot(i) then humans = humans + 1 end end
  local want = cfg.quota
  if cfg.mode == "fill" then want = cfg.quota - humans
  elseif cfg.mode == "match" then want = cfg.quota * humans end
  local others = 0                      -- bots that are not ours (e.g. ZBots)
  for i = 1, xmod.maxplayers() do if P.connected(i) and P.is_bot(i) and not bots[i] then others = others + 1 end end
  want = math.max(0, math.min(want, xmod.maxplayers() - humans - others - math.max(0, cfg.keep)))
  local have = count_bots()
  if have < want then
    add_bot()
  elseif have > want then
    -- remove from the bigger team, dead ones first
    local nt, nct = 0, 0
    for _, p in ipairs(P.all()) do local t = P.team(p); if t == 1 then nt = nt + 1 elseif t == 2 then nct = nct + 1 end end
    local from = nt > nct and 1 or (nct > nt and 2 or nil)
    local pick
    for id in live() do
      if not from or P.team(id) == from then
        if not pick or (P.alive(pick) and not P.alive(id)) then pick = id end
      end
    end
    pick = pick or next(bots)
    if pick then kick_bot(pick) end
  end
end

-- =============================================================================================
-- rounds
local function plan_round()
  ROUND.t_site = #MAP.sites > 0 and rand(#MAP.sites) or 1
  -- CTs split over the sites, a few Ts lurk on the other site
  local ct_i, t_i = 0, 0
  local ids = {}
  for id in live() do ids[#ids + 1] = id end
  table.sort(ids)
  for _, id in ipairs(ids) do
    local br, team = bots[id], P.team(id)
    br.role, br.site = "roam", nil
    if MAP.kind == "de" then
      if team == 2 then
        ct_i = ct_i + 1
        br.site = (ct_i % math.max(1, #MAP.sites)) + 1
        br.role = (rand() < br.pers.aggr * 0.4) and "hunt" or "defend"
      elseif team == 1 then
        t_i = t_i + 1
        br.site = ROUND.t_site
        if t_i % 4 == 0 and #MAP.sites > 1 then br.site = (ROUND.t_site % #MAP.sites) + 1; br.role = "lurk"
        else br.role = "rush" end
      end
    elseif MAP.kind == "cs" then
      br.role = team == 2 and "rescue" or "guard"
    else
      br.role = rand() < br.pers.camp and "camp" or "hunt"
    end
  end
end

local function new_round()
  ROUND.n = ROUND.n + 1
  ROUND.start = now()
  ROUND.freeze_end = now() + CV.num("mp_freezetime")
  ROUND.planted, ROUND.bomb, ROUND.dropped, ROUND.ended, ROUND.kills = false, nil, nil, false, {}
  for id, br in live() do
    br.task, br.order, br.enemy, br.last_enemy, br.heard, br.throwing = nil, nil, nil, nil, nil, nil
    br.bought, br.buy_at = false, now() + 0.3 + rand() * 1.5
    br.hostage_used = {}
    br.mem.rounds = (br.mem.rounds or 0) + 1
    if P.connected(id) then B.stop(id); B.fire(id, FIRE_OFF) end
  end
  poll_bomb()
  plan_round()
  -- a bit of banter between two bots
  if cfg.chat and rand() < 0.07 * cfg.chat_rate then
    local ids = {}
    for id in live() do ids[#ids + 1] = id end
    if #ids >= 2 then
      local a = table.remove(ids, rand(#ids)); local b = ids[rand(#ids)]
      if bot_chat(a, "banter_q", {}, { force = true, delay = 1 + rand() }) then
        bot_chat(b, "banter_a", { name = bots[a].name }, { force = true, delay = 4 + rand() * 2 })
      end
    end
  end
end

-- =============================================================================================
-- buying
local function team_eco(team)
  if ROUND.n <= 1 then return false end
  local sum, n = 0, 0
  for _, p in ipairs(P.all()) do if P.team(p) == team then sum, n = sum + P.money(p), n + 1 end end
  return n > 0 and sum / n < 2200
end

local function do_buy(id, br)
  br.bought = true
  if not cfg.buy or cfg.knife then return end
  local money, team = P.money(id), P.team(id)
  local list = {}
  local function buy(name)
    local w = W[name]
    local alias, price = w and w[4] or name, w and w[5] or ({ vest = 650, vesthelm = 1000, hegren = 300, flash = 200,
      sgren = 300, defuser = 200, primammo = 60, secammo = 50 })[name] or 0
    if cfg.restrict[name] or cfg.restrict[alias] or price > money then return false end
    money = money - price
    list[#list + 1] = alias
    return true
  end
  local prim = best_weapons(id)
  local pdata = prim and W[prim]
  local eco = cfg.eco and team_eco(team) and money < 3000
  local fav = fav_weapon(br)
  if not eco and (not pdata or pdata[2] == "smg" or pdata[2] == "shotgun") then
    if money >= 5700 and rand() < br.pers.awp then buy("awp")
    elseif money >= 3700 then
      local choice = fav and W[fav] and W[fav][2] == "rifle" and rand() < 0.5 and fav or (team == 1 and "ak47" or "m4a1")
      if (choice == "m4a1" or choice == "famas" or choice == "aug") and team == 1 then choice = "ak47" end
      if (choice == "ak47" or choice == "galil" or choice == "sg552") and team == 2 then choice = "m4a1" end
      if not buy(choice) then buy(team == 1 and "galil" or "famas") end
    elseif money >= 2400 and not pdata then
      buy(team == 1 and "galil" or "famas")
    elseif money >= 1800 and not pdata then
      buy(rand() < 0.5 and "mp5navy" or (team == 1 and "mac10" or "tmp"))
    end
  end
  if money >= 1000 and P.armor(id) < 50 then buy("vesthelm") elseif money >= 650 and P.armor(id) < 50 then buy("vest") end
  if ROUND.n <= 1 or (eco and money >= 650 and rand() < 0.4) then
    local _, pist = best_weapons(id)
    if pist ~= "deagle" and money >= 650 and rand() < 0.5 then buy("deagle") end
  end
  if cfg.nades and money >= 300 and rand() < 0.6 then buy("hegren") end
  if money >= 200 and rand() < 0.4 then buy("flash") end
  if team == 2 and MAP.kind == "de" and money >= 200 and rand() < 0.7 then buy("defuser") end
  buy("primammo"); buy("primammo"); buy("secammo")
  if #list > 0 then B.cmd(id, table.concat(list, ";")) end
  dbg("%s buys %s", br.name, table.concat(list, " "))
end

-- =============================================================================================
-- perception
local function perceive(id, br, t)
  if br.enemy and (not P.connected(br.enemy) or not P.alive(br.enemy)) then br.enemy = nil end
  if cfg.peaceful then br.enemy = nil; return end
  local blind = t < (br.blind_until or 0)
  local seen = blind and {} or B.scan(id, cfg.ffa)
  local best, bs, info
  for _, s in ipairs(seen) do
    if xmod.emit("xbots_target", id, s.id) ~= true then
      local score = s.dist
      if br.nemesis_id == s.id then score = score * 0.6 end
      if br.hurt_by == s.id and t - (br.hurt_t or -99) < 3 then score = score * 0.5 end
      if s.id == br.enemy then score = score * 0.7 end
      if ROUND.carrier == s.id or (br.task and br.task.kind == "defuse") then score = score * 0.8 end
      if not best or score < bs then best, bs, info = s.id, score, s end
    end
  end
  if best then
    if best ~= br.enemy then
      br.enemy, br.enemy_first = best, t
      local react = br.react_base * (0.8 + rand() * 0.5)
      if t - (br.last_contact or -99) < 3 then react = react * 0.6 end     -- already alert
      if br.task and br.task.camping then react = react * 0.8 end          -- holding an angle
      br.react = react
      br.aim_part = rand() < br.head and "head" or "body"
      local cur = B.weapon(id)
      if cur and (cur.name == "knife" or cur.name == "c4") and not cfg.knife then switch_best(id, br) end
      radio(id, br, "enemyspot", 0.35)
    end
    br.enemy_seen, br.last_contact = t, t
    br.enemy_dist, br.enemy_part = info.dist, info.part
    local x, y, z = P.origin(best)
    br.last_enemy = { x = x, y = y, z = z, t = t, id = best }
  elseif br.enemy and t - (br.enemy_seen or 0) > 0.6 then
    br.enemy = nil
  end
end

-- =============================================================================================
-- combat
local function set_fire(id, br, wname, dist, t)
  local w = W[wname]
  local s = br.skill / 100
  local maxang = math.deg(math.atan(16, math.max(dist, 1))) * (1.6 - 0.8 * s) + 0.4
  if t < (br.blind_until or 0) then    -- blinded: spray where the enemy was if close, else hold fire
    if dist < 500 then B.fire(id, FIRE_SPRAY, 0, 0, 30) else B.fire(id, FIRE_OFF) end
    return
  end
  if not w or w[2] == "knife" then
    if dist < 80 then B.fire(id, FIRE_SPRAY, 0, 0, 30) else B.fire(id, FIRE_OFF) end
    return
  end
  local k = w[2]
  if k == "pistol" then
    B.fire(id, FIRE_TAP, 0, (wname == "deagle" and 0.36 or 0.16) + (1 - s) * 0.12, maxang)
  elseif k == "shotgun" then
    if dist < 800 then B.fire(id, FIRE_TAP, 0, wname == "m3" and 0.9 or 0.3, maxang * 2.5) else B.fire(id, FIRE_OFF) end
  elseif k == "smg" or k == "mg" then
    if dist < 700 then B.fire(id, FIRE_SPRAY, 0, 0, maxang * 1.5)
    else B.fire(id, FIRE_BURST, 0.2, 0.25 + 0.15 * (1 - s), maxang) end
  elseif k == "rifle" then
    if dist < 450 then B.fire(id, FIRE_SPRAY, 0, 0, maxang * 1.3)
    elseif dist < 1400 then B.fire(id, FIRE_BURST, 0.12 + 0.1 * s, 0.35 - 0.15 * s, maxang)
    else B.fire(id, FIRE_TAP, 0, 0.35, maxang) end
  elseif k == "sniper" or k == "autosniper" then
    local zoomed = (E.get(id, "fov") or 90) < 89
    if dist > 350 and not zoomed then
      if t >= (br.zoom_t or 0) then B.press(id, IN_ATTACK2); br.zoom_t = t + 0.7 end
      B.fire(id, FIRE_OFF)
      return
    end
    local pause = wname == "awp" and 1.5 or (wname == "scout" and 1.25 or 0.3)
    B.fire(id, FIRE_TAP, 0, pause, maxang * 0.8)
  else
    B.fire(id, FIRE_OFF)
  end
end

local function combat(id, br, t)
  local e = br.enemy
  local part = br.aim_part
  if br.enemy_part == "body" and part == "head" then part = "body" elseif br.enemy_part == "feet" then part = "feet" end
  B.look_ent(id, e, part)
  local w = B.weapon(id)
  local wname = w and w.name or "knife"
  local wd = W[wname]
  local dist = br.enemy_dist or 600
  if cfg.knife and wname ~= "knife" then B.cmd(id, "weapon_knife"); wname = "knife"; wd = W.knife end
  if wd and (wd[2] == "nade" or wd[2] == "c4") and not br.throwing then switch_best(id, br) end
  -- empty magazine: pistol if close, else reload
  if w and wd and wd[3] > 0 and w.clip == 0 and t >= (br.reload_t or 0) then
    local _, pist = best_weapons(id)
    if pist and wd[2] ~= "pistol" and dist < 800 then B.cmd(id, "weapon_" .. pist) else B.press(id, IN_RELOAD) end
    br.reload_t = t + 0.8
  end
  if t - br.enemy_first >= br.react then set_fire(id, br, wname, dist, t) else B.fire(id, FIRE_OFF) end

  -- movement
  if cfg.frozen then return end
  if wname == "knife" then
    local x, y, z = P.origin(e)
    if t >= (br.next_push or 0) then br.next_push = t + 0.4; B.go(id, x, y, z, { direct = dist < 500 }) end
    return
  end
  if br.task and br.task.kind == "defuse" and br.task.defusing and dist > 900 then return end   -- keep defusing
  if t >= (br.next_dodge or 0) then
    br.next_dodge = t + 0.35 + rand() * 0.5
    if dist < 1300 and rand() < 0.4 + br.skill / 200 then B.strafe(id, rand(2) == 1 and -1 or 1, 0.2 + rand() * 0.4) end
  end
  local hp = P.health(id)
  local kind = wd and wd[2] or "rifle"
  if hp < 30 and br.pers.aggr < 0.5 and dist > 300 and br.safe then
    if not br.retreating then
      br.retreating = true
      B.go(id, br.safe.x, br.safe.y, br.safe.z)
      radio(id, br, "needbackup", 0.6)
    end
  elseif br.pers.aggr > 0.6 and dist > 380 and kind ~= "sniper" and kind ~= "autosniper" then
    if t >= (br.next_push or 0) then br.next_push = t + 1.0; local x, y, z = P.origin(e); B.go(id, x, y, z) end
  else
    if not br.retreating then B.stop(id) end
    if dist > 900 and (kind == "rifle" or kind == "mg" or kind == "smg") and br.skill > 40 then B.hold(id, IN_DUCK, 0.4) end
  end
end

-- =============================================================================================
-- tasks (what to do when no enemy is visible)
local TASKS = {}

-- (re)issue B.go only when the target moved or the bot stopped
local function travel(id, br, tk, x, y, z, opts, t)
  local g = tk.go
  local moving = B.state(id).moving
  if not g or math.abs(g.x - x) + math.abs(g.y - y) + math.abs(g.z - z) > 96 or (not moving and not tk.arrived and t - g.t > 1.2) then
    local r = B.go(id, x, y, z, opts)
    tk.go, tk.arrived = { x = x, y = y, z = z, t = t }, false
    if r == false then tk.failed = (tk.failed or 0) + 1 end
  end
  local bx, by, bz = P.origin(id)
  local d = dist3(bx, by, bz, x, y, z)
  if d < 70 then tk.arrived = true end
  -- run with the knife on long trips (faster), like real players
  if br.knife_runner and not cfg.knife and d > 1400 and t - (br.last_contact or -99) > 10 and not br.knifed
     and t > ROUND.freeze_end + 1 and not has_weapon(id, CSW_C4) then
    B.cmd(id, "weapon_knife"); br.knifed = true
  elseif br.knifed and d < 900 then
    switch_best(id, br)
  end
  return d
end

local function camp_look(id, br, tk, t)
  if t < (tk.look_t or 0) then return end
  tk.look_t = t + 1.5 + rand() * 2.5
  local x, y, z = P.origin(id)
  local near = B.nav_near(x, y, z, 900)
  local cands = {}
  for _, n in ipairs(near) do
    local nd = B.nav_node(n)
    if nd and dist3(x, y, z, nd.x, nd.y, nd.z) > 300 then cands[#cands + 1] = nd end
  end
  if #cands > 0 then
    local nd = cands[rand(#cands)]
    B.look(id, nd.x, nd.y, nd.z + 17)
  elseif tk.face then
    B.look(id, tk.face.x, tk.face.y, tk.face.z)
  end
end

function TASKS.go(id, br, t, tk)
  travel(id, br, tk, tk.x, tk.y, tk.z, { walk = tk.walk }, t)
  if tk.look then B.look(id, tk.look.x, tk.look.y, tk.look.z) end
  if tk.arrived then tk.done = true end
end

function TASKS.camp(id, br, t, tk)
  if not tk.camping then
    travel(id, br, tk, tk.x, tk.y, tk.z, nil, t)
    if tk.arrived then
      tk.camping, tk.expire = true, t + (tk.time or (10 + rand() * 20))
      B.stop(id)
      radio(id, br, "inposition", 0.25)
    end
    return
  end
  if br.pers.camp > 0.55 or tk.crouch then B.hold(id, IN_DUCK, cfg.think * 3) end
  camp_look(id, br, tk, t)
end

function TASKS.follow(id, br, t, tk)
  local L = tk.leader
  if not L or not P.connected(L) or not P.alive(L) then tk.done = true; return end
  local x, y, z = P.origin(L)
  local bx, by, bz = P.origin(id)
  local d = dist3(x, y, z, bx, by, bz)
  if d > 200 then
    if t >= (tk.next or 0) then tk.next = t + 0.8; B.go(id, x, y, z, { walk = d < 400 }) end
    B.look_path(id)
  else
    B.stop(id)
    camp_look(id, br, tk, t)
  end
end

function TASKS.plant(id, br, t, tk)
  if not has_weapon(id, CSW_C4) then tk.done = true; return end
  if not tk.planting then
    travel(id, br, tk, tk.x, tk.y, tk.z, nil, t)
    if tk.arrived then
      tk.planting, tk.expire = t, t + 6
      B.stop(id)
      B.cmd(id, "weapon_c4")
      radio(id, br, "coverme", 0.8)
    end
    return
  end
  if t - tk.planting > 0.5 then B.hold(id, IN_ATTACK | IN_DUCK, cfg.think * 3) end
end

function TASKS.defuse(id, br, t, tk)
  if not ROUND.planted or not ROUND.bomb then B.release(id, IN_USE | IN_DUCK); tk.done = true; return end
  local b = ROUND.bomb
  local d = travel(id, br, tk, b.x, b.y, b.z + 20, nil, t)
  if d < 60 then
    B.stop(id)
    B.look(id, b.x, b.y, b.z)
    B.hold(id, IN_USE | IN_DUCK, cfg.think * 3)
    tk.defusing = true
  end
end

function TASKS.pickup(id, br, t, tk)
  if not ROUND.dropped then tk.done = true; return end
  local d = ROUND.dropped
  travel(id, br, tk, d.x, d.y, d.z + 20, { direct = false }, t)
  if has_weapon(id, CSW_C4) then tk.done = true end
end

function TASKS.hostage(id, br, t, tk)
  local h = tk.ent
  if not h or E.get(h, "health") <= 0 or (E.get(h, "effects") & xmod.EF_NODRAW) ~= 0 then tk.done = true; return end
  local x, y, z = E.get(h, "origin")
  local d = travel(id, br, tk, x, y, z, nil, t)
  if d < 80 then
    B.look(id, x, y, z + 10)
    if t >= (tk.use_t or 0) then B.press(id, IN_USE); tk.use_t = t + 1.0; br.hostage_used[h] = true; tk.done = true end
  end
end

function TASKS.seek(id, br, t, tk)
  local d = travel(id, br, tk, tk.x, tk.y, tk.z, { walk = br.pers.aggr < 0.6 }, t)
  if d < 900 then B.look(id, tk.x, tk.y, tk.z + 30) end      -- pre-aim where the enemy should be
  if tk.arrived then tk.done = true; br.last_enemy, br.heard = nil, nil end
end

function TASKS.wait(id, br, t, tk)
  B.stop(id)
  camp_look(id, br, tk, t)
end

-- choose the next task
local function camp_spot(id, x, y, z, radius, team)
  local n = B.nav_best(x, y, z, radius or 900, team, "kill", 0)
  if n and n > 0 then local nd = B.nav_node(n); return nd.x, nd.y, nd.z end
  return x, y, z
end

local function decide(id, br, t)
  local team = P.team(id)
  local o = br.order
  if o and t < o.expire then
    if o.kind == "follow" or o.kind == "backup" then return { kind = "follow", leader = o.leader, expire = o.expire } end
    if o.kind == "hold" then return { kind = "camp", x = o.x, y = o.y, z = o.z, time = o.expire - t, expire = o.expire } end
    if o.kind == "goto" or o.kind == "regroup" then return { kind = "go", x = o.x, y = o.y, z = o.z, expire = t + 40 } end
  end
  br.order = nil

  if MAP.kind == "de" then
    if ROUND.planted and ROUND.bomb then
      local b = ROUND.bomb
      if team == 2 then return { kind = "defuse", expire = t + 60 } end
      local x, y, z = camp_spot(id, b.x, b.y, b.z, 600, team)
      return { kind = "camp", x = x, y = y, z = z, time = 40, face = b }
    end
    if team == 1 then
      if has_weapon(id, CSW_C4) then
        local x, y, z = site_point(br.site or ROUND.t_site)
        if x then return { kind = "plant", x = x, y = y, z = z, expire = t + 90 } end
      end
      if ROUND.dropped then
        local closest, cd
        for bid in live() do
          if P.alive(bid) and P.team(bid) == 1 then
            local bx, by, bz = P.origin(bid)
            local d = dist3(bx, by, bz, ROUND.dropped.x, ROUND.dropped.y, ROUND.dropped.z)
            if not cd or d < cd then closest, cd = bid, d end
          end
        end
        if closest == id then return { kind = "pickup", expire = t + 30 } end
      end
      if ROUND.carrier and ROUND.carrier ~= id and br.role == "rush" and rand() < 0.35 then
        return { kind = "follow", leader = ROUND.carrier, expire = t + 20 }
      end
      local x, y, z = site_point(br.site or ROUND.t_site)
      if x then
        if br.role == "lurk" then x, y, z = camp_spot(id, x, y, z, 1200, team); return { kind = "camp", x = x, y = y, z = z, time = 25 } end
        return { kind = "go", x = x, y = y, z = z, expire = t + 60 }
      end
    elseif team == 2 then
      if br.role == "defend" and br.site then
        local sx, sy, sz = site_point(br.site)
        if sx then
          local x, y, z = camp_spot(id, sx, sy, sz, 700, team)
          return { kind = "camp", x = x, y = y, z = z, time = 20 + rand() * 30 }
        end
      end
    end
  elseif MAP.kind == "cs" then
    if team == 2 then
      local hs = hostages()
      local bx, by, bz = P.origin(id)
      local best, bd
      for _, h in ipairs(hs) do
        if not br.hostage_used[h] then
          local x, y, z = E.get(h, "origin")
          local d = dist3(bx, by, bz, x, y, z)
          if not bd or d < bd then best, bd = h, d end
        end
      end
      if best then return { kind = "hostage", ent = best, expire = t + 45 } end
      if next(br.hostage_used) and #MAP.rescue > 0 then
        local r = MAP.rescue[rand(#MAP.rescue)]
        return { kind = "go", x = r.x, y = r.y, z = r.z, expire = t + 60 }
      end
    else
      local hs = hostages()
      if #hs > 0 then
        local x, y, z = E.get(hs[rand(#hs)], "origin")
        x, y, z = camp_spot(id, x, y, z, 700, team)
        return { kind = "camp", x = x, y = y, z = z, time = 20 + rand() * 25 }
      end
    end
  end

  -- hunting: last seen / heard enemy
  local le = br.last_enemy
  if le and t - le.t < 8 and rand() < 0.3 + br.pers.aggr * 0.7 then return { kind = "seek", x = le.x, y = le.y, z = le.z, expire = t + 15 } end
  local h = br.heard
  if h and t - h.t < 5 and rand() < br.pers.aggr then return { kind = "seek", x = h.x, y = h.y, z = h.z, expire = t + 12 } end

  -- camp or roam
  local bx, by, bz = P.origin(id)
  if rand() < br.pers.camp * 0.6 then
    local x, y, z = camp_spot(id, bx, by, bz, 1500, team)
    return { kind = "camp", x = x, y = y, z = z, time = 10 + rand() * 20 }
  end
  local target
  local es = team == 1 and MAP.ctspawn or MAP.tspawn
  if es and rand() < 0.35 then target = es end
  if not target then
    local n = B.nav_random(0)
    if n and n > 0 then target = B.nav_node(n) end
  end
  if target then return { kind = "go", x = target.x, y = target.y, z = target.z, expire = t + 45 } end
  return { kind = "wait", expire = t + 3 }
end

-- =============================================================================================
-- grenade throwing (simple and effective: at the place an enemy just disappeared)
local function try_grenade(id, br, t)
  local th = br.throwing
  if th then
    if th.phase == 1 and t - th.t > 0.8 then
      B.look(id, th.x, th.y, th.z + th.lift); th.phase, th.t = 2, t
    elseif th.phase == 2 and t - th.t > 0.45 then
      B.hold(id, IN_ATTACK, 0.5); th.phase, th.t = 3, t
    elseif th.phase == 3 and t - th.t > 1.2 then
      br.throwing = nil; switch_best(id, br)
    end
    return true
  end
  local le = br.last_enemy
  if not cfg.nades or not le or t < (br.nade_t or 0) or not has_weapon(id, CSW_HE) then return false end
  local age = t - le.t
  if age < 1.0 or age > 5 then return false end
  local x, y, z = P.origin(id)
  local d = dist3(x, y, z, le.x, le.y, le.z)
  if d < 350 or d > 1500 then return false end
  br.nade_t = t + 12 + rand() * 10
  br.throwing = { phase = 1, t = t, x = le.x, y = le.y, z = le.z, lift = d * 0.12 }
  B.stop(id)
  B.cmd(id, "weapon_hegrenade")
  radio(id, br, "fireinhole", 0.3)
  return true
end

-- =============================================================================================
-- one brain tick
local function on_spawn(id, br, t)
  br.alive, br.task, br.enemy, br.retreating, br.knifed, br.throwing = true, nil, nil, false, false, nil
  br.hurt_by, br.blind_until = nil, 0
  apply_skill(id, br)
  local nem, nk = nemesis_of(br)
  br.nemesis_id = nil
  if nem then for _, p in ipairs(P.all()) do if who_key(p) == nk then br.nemesis_id = p end end end
  if ROUND.n == 0 or t - ROUND.start < 20 then br.bought = false; br.buy_at = t + 0.3 + rand() * 1.2 else br.bought = true end
  if not br.role then
    local team = P.team(id)
    if MAP.kind == "de" then
      br.site = team == 1 and ROUND.t_site or (#MAP.sites > 0 and rand(#MAP.sites) or nil)
      br.role = team == 1 and "rush" or "defend"
    elseif MAP.kind == "cs" then br.role = team == 2 and "rescue" or "guard"
    else br.role = rand() < br.pers.camp and "camp" or "hunt" end
  end
  xmod.emit("xbots_spawn", id)
end

local function think(id, br, t)
  if not P.connected(id) then return end
  if not P.alive(id) then
    if br.alive then br.alive = false; br.enemy = nil; br.task = nil end
    return
  end
  if not br.alive then on_spawn(id, br, t) end
  if xmod.emit("xbots_think", id, br) == true then return end      -- another plugin drives this bot now

  if not br.bought and t >= (br.buy_at or 0) then do_buy(id, br) end
  perceive(id, br, t)

  if br.enemy then
    br.throwing = nil
    if br.task and br.task.kind == "plant" then B.release(id, IN_ATTACK) end
    combat(id, br, t)
    return
  end
  B.fire(id, FIRE_OFF)
  if br.retreating and not B.state(id).moving then br.retreating = false end
  if t >= (br.safe_t or 0) then local x, y, z = P.origin(id); br.safe = { x = x, y = y, z = z }; br.safe_t = t + 2.5 end
  if t < ROUND.freeze_end then return end
  -- calm moments: reload
  local w = B.weapon(id)
  local wd = w and W[w.name]
  if wd and wd[3] > 0 and w.clip < wd[3] * 0.5 and t >= (br.reload_t or 0) then B.press(id, IN_RELOAD); br.reload_t = t + 3 end
  if try_grenade(id, br, t) then return end

  -- where to look (overrides the default "look where I walk"): who shot me, where the enemy vanished, a noise
  local looking = false
  local le = br.last_enemy
  if br.hurt_by and t - (br.hurt_t or -99) < 1.5 and P.connected(br.hurt_by) then
    local x, y, z = P.origin(br.hurt_by); B.look(id, x, y, z + 10); looking = true
  elseif le and t - le.t < 1.5 then
    B.look(id, le.x, le.y, le.z + 20); looking = true
  elseif br.heard and t - br.heard.t < 2 then
    B.look(id, br.heard.x, br.heard.y, br.heard.z + 20); looking = true
  end
  if looking then br.look_override = true end

  local tk = br.task
  if not tk or tk.done or (tk.expire and t > tk.expire) or (tk.failed or 0) > 2 then
    tk = decide(id, br, t)
    tk.started = t
    br.task = tk
    if not looking then B.look_path(id) end
    dbg("%s -> %s", br.name, tk.kind)
  elseif not looking and br.look_override then
    br.look_override = false
    if not tk.camping then B.look_path(id) end
  end
  local fn = TASKS[tk.kind]
  if fn then fn(id, br, t, tk) end
end

local err_count, err_t = 0, 0
local function scheduler()
  if not ready or not ext_ok() then return end
  local t = now()
  for id, br in live() do
    if t >= br.next_think then
      br.next_think = t + cfg.think * (0.85 + rand() * 0.3)
      local ok, e = pcall(think, id, br, t)
      if not ok then                       -- never let one bug disable the whole plugin
        err_count = err_count + 1
        if os.time() - err_t > 10 then err_t = os.time(); xmod.log("think error (" .. err_count .. "): " .. tostring(e)) end
      end
    end
  end
end

-- hearing: footsteps (running on the ground) and gunshots, 4 times per second
local function hearing()
  if not ready then return end
  local t = now()
  for _, p in ipairs(P.all()) do
    if P.alive(p) then
      local vx, vy = E.get(p, "velocity")
      local fl = E.get(p, "flags")
      local loud = t - (shot_t[p] or -99) < 0.35
      local step = (fl & xmod.FL_ONGROUND) ~= 0 and (fl & xmod.FL_DUCKING) == 0 and (vx * vx + vy * vy) > 150 * 150
      if loud or step then
        local x, y, z = P.origin(p)
        local r, tp = loud and 2200 or 900, P.team(p)
        for bid, br in live() do
          if bid ~= p and br.alive and not br.enemy and (cfg.ffa or P.team(bid) ~= tp) then
            local bx, by, bz = P.origin(bid)
            if dist3(x, y, z, bx, by, bz) < r then br.heard = { x = x, y = y, z = z, t = t, loud = loud } end
          end
        end
      end
    end
  end
end

-- =============================================================================================
-- orders from humans (radio + chat)
local RADIO_ORDER = {
  followme = "follow", coverme = "follow", holdpos = "hold", getinpos = "hold", regroup = "regroup",
  sticktog = "regroup", stormfront = "rush", go = "rush", takepoint = "rush", fallback = "fallback",
  needbackup = "backup", takingfire = "backup",
}
local CHAT_ORDER = {
  { "follow", { "follow me", "dupa mine", "urmati-ma", "urmati ma", "urmeaza-ma", "veniti cu mine", "vino cu mine" } },
  { "hold", { "hold", "stati aici", "stai aici", "ramaneti", "campati", "pazit" } },
  { "regroup", { "regroup", "regrupare", "veniti la mine", "veniti aici", "come here" } },
  { "rush", { "rush", "go go", "atacati", "intrati", "push" } },
  { "free", { "free", "liber", "faceti ce vreti", "do your thing", "stop following" } },
}

local function give_order(leader, kind)
  local team = P.team(leader)
  local lx, ly, lz = P.origin(leader)
  local t = now()
  local list = {}
  for id, br in live() do
    if P.alive(id) and P.team(id) == team then list[#list + 1] = { id = id, br = br, d = pdist(id, leader) } end
  end
  table.sort(list, function(a, b) return a.d < b.d end)
  local maxn = (kind == "follow" or kind == "backup") and 2 or #list
  local acked = false
  for i, e in ipairs(list) do
    if i > maxn then break end
    local br = e.br
    if not has_weapon(e.id, CSW_C4) or kind == "free" then
      if kind == "free" then br.order = nil
      elseif kind == "rush" then br.order, br.role = nil, "rush"
      elseif kind == "fallback" then
        local sp = team == 1 and MAP.tspawn or MAP.ctspawn
        if sp then br.order = { kind = "goto", x = sp.x, y = sp.y, z = sp.z, expire = t + 30 } end
      else
        br.order = { kind = kind, leader = leader, x = lx, y = ly, z = lz, expire = t + (kind == "hold" and 40 or 60) }
      end
      br.task = nil
      if not acked then
        acked = true
        local uid = br.uid
        xmod.timer.after(0.6 + rand() * 0.8, function() if bots[e.id] and bots[e.id].uid == uid then B.cmd(e.id, "roger") end end)
      end
      xmod.emit("xbots_order", e.id, kind, leader)
    end
  end
  return #list
end

-- =============================================================================================
-- events
xmod.on("init", function()
  ready = false
  bots, shot_t = {}, {}
  ROUND = { n = 0, start = -99, freeze_end = -99, planted = false, t_site = 1, ended = false, kills = {} }
  xmod.exec("exec addons/xmod/configs/xbots.cfg")          -- our cvars (applied before "config")
end)

xmod.on("config", function()
  read_cfg()
  load_configs()
  if not ext_ok() then
    xmod.log("the xmod_bots extension is not loaded - add it to addons/metamod/plugins.ini AFTER xmod and 'bot' to configs/extensions.ini")
    return
  end
  B.config({ autolearn = cfg.learn, max_nodes = cfg.nav_max })
  local info = B.nav_info()
  if info.nodes == 0 and cfg.auto_nav then
    xmod.log("no navigation for " .. xmod.map() .. " - analyzing the map (a few seconds)...")
    B.nav_analyze(false, cfg.nav_max)
  end
  for id in live() do if not B.is_bot(id) then bots[id] = nil end end
  scan_map()
  ready = true
end)

xmod.on("map_end", function()
  ready = false
  for _, br in live() do save_mem(br) end
  if ext_ok() and B.nav_info().dirty then B.nav_save() end
end)

xmod.on("bot_nav_done", function(nodes, links)
  xmod.log(string.format("map analyzed: %d nodes, %d links - saved", nodes, links))
  B.nav_save()
  scan_map()
end)

xmod.on("bot_stuck", function(id)
  local br = bots[id]
  if br and br.task then br.task.failed = (br.task.failed or 0) + 1; br.task.go = nil end
end)

xmod.on("bot_arrived", function(id)
  local br = bots[id]
  if br and br.task then br.task.arrived = true end
end)

xmod.on("client_disconnect", function(id)
  local br = bots[id]
  if br then save_mem(br); bots[id] = nil; xmod.emit("xbots_removed", id) end
  shot_t[id] = nil
  for _, b in live() do
    if b.enemy == id then b.enemy = nil end
    if b.nemesis_id == id then b.nemesis_id = nil end
  end
end)

xmod.on("client_putinserver", function(id)
  if P.is_bot(id) or not cfg.chat then return end
  xmod.timer.after(4 + rand() * 5, function()
    if not P.connected(id) then return end
    local ids = {}
    for bid in live() do ids[#ids + 1] = bid end
    if #ids == 0 then return end
    local bid = ids[rand(#ids)]
    local br = bots[bid]
    local r = br.mem.rel[who_key(id)]
    local ev = (r and (r.d or 0) >= 3) and "hello_nemesis" or (r and "hello_known" or "hello")
    bot_chat(bid, ev, { name = P.name(id) }, { chance = 0.5 })
  end)
end)

xmod.on("message:HLTV", function(_, _, _, a)
  if a[1] == 0 and a[2] == 0 then new_round() end
end)

if xmod.reapi.available() then
  pcall(xmod.on, "reapi:CSGameRules_OnRoundFreezeEnd", function() ROUND.freeze_end = now() end)
end

xmod.on("message:SendAudio", function(_, _, _, a)
  local s = tostring(a[2] or "")
  local t = now()
  if s == "%!MRAD_BOMBPL" then
    xmod.timer.after(0.2, poll_bomb)
    for id, br in live() do br.task = nil; if P.team(id) == 1 and P.alive(id) then bot_chat(id, "planted", {}, { chance = 0.25 }) end end
  elseif s == "%!MRAD_BOMBDEF" then
    ROUND.planted, ROUND.bomb = false, nil
  elseif (s == "%!MRAD_terwin" or s == "%!MRAD_ctwin" or s == "%!MRAD_rounddraw") and not ROUND.ended then
    ROUND.ended = true
    local win = s == "%!MRAD_terwin" and 1 or (s == "%!MRAD_ctwin" and 2 or 0)
    for id, br in live() do
      local team = P.team(id)
      if win ~= 0 and team == win then
        br.mem.wins = (br.mem.wins or 0) + 1
        bot_chat(id, "win", {}, { chance = 0.12 })
      elseif win ~= 0 then
        bot_chat(id, br.mood <= -2 and "tilt" or "lose", {}, { chance = 0.1 })
      end
      save_mem(br)
    end
  end
end)

-- someone fired (hearing); kept tiny because cmd_start is a hot event
xmod.on("cmd_start", function(id, buttons)
  if (buttons & 1) ~= 0 then shot_t[id] = xmod.gametime() end
end)

-- damage: remember who shot us (turn towards an unseen attacker)
pcall(xmod.on, "ham:takedamage:player", function(victim, inflictor, attacker, dmg)
  local br = bots[victim]
  if br and attacker >= 1 and attacker <= xmod.maxplayers() and attacker ~= victim then
    br.hurt_by, br.hurt_t = attacker, now()
  end
end)

-- flashbangs: a white ScreenFade sent to a bot blinds it
xmod.on("message:ScreenFade", function(_, _, ent, a)
  local br = bots[ent]
  if not br or not a[7] then return end
  if (a[7] or 0) >= 200 and (a[4] or 0) >= 200 and (a[5] or 0) >= 200 and (a[6] or 0) >= 200 then
    br.blind_until = now() + ((a[1] or 0) + (a[2] or 0)) / 4096 * (1.2 - br.skill / 200)
  end
end)

xmod.on("message:DeathMsg", function(_, _, _, a)
  local k, v, hs, wname = a[1], a[2], a[3] == 1, tostring(a[4] or "")
  local t = now()
  if not v or v < 1 or not P.connected(v) then return end
  local vt = P.team(v)
  -- experience for the navigation (any death on the map)
  if cfg.learn and ext_ok() then
    local x, y, z = P.origin(v)
    B.nav_learn(x, y, z, vt, "death", 1)
    if k and k >= 1 and k ~= v and P.connected(k) then
      local kx, ky, kz = P.origin(k)
      B.nav_learn(kx, ky, kz, P.team(k), "kill", 1)
    end
  end
  if wname == "grenade" then wname = "hegrenade" end
  local killer_ok = k and k >= 1 and k ~= v and P.connected(k)
  -- killer is a bot
  local kb = killer_ok and bots[k]
  if kb then
    kb.mem.k = kb.mem.k + 1
    if hs then kb.mem.hs = (kb.mem.hs or 0) + 1 end
    if W[wname] then kb.mem.w[wname] = (kb.mem.w[wname] or 0) + 1 end
    local r = rel_of(kb, v)
    r.k = (r.k or 0) + 1
    kb.mood = math.min(3, kb.mood + 1)
    ROUND.kills[k] = (ROUND.kills[k] or 0) + 1
    kb.enemy = kb.enemy == v and nil or kb.enemy
    local vars = { name = P.name(v), weapon = wname }
    if ROUND.kills[k] == 5 then bot_chat(k, "ace", vars, { chance = 1 })
    elseif kb.nemesis_id == v then bot_chat(k, "revenge", vars, { chance = 0.8 })
    elseif wname == "knife" then bot_chat(k, "knife", vars, { chance = 0.7 })
    elseif hs then bot_chat(k, "hs", vars, { chance = 0.25 })
    else bot_chat(k, "kill", vars, { chance = 0.18 }) end
    radio(k, kb, "enemydown", 0.25)
    xmod.emit("xbots_kill", k, v)
  end
  -- victim is a bot
  local vb = bots[v]
  if vb then
    vb.mem.d = vb.mem.d + 1
    vb.mood = math.max(-3, vb.mood - 1)
    vb.alive, vb.enemy, vb.task = false, nil, nil
    if killer_ok then
      local r = rel_of(vb, k)
      r.d = (r.d or 0) + 1
      local vars = { name = P.name(k), weapon = wname }
      if P.team(k) == vt and not cfg.ffa then bot_chat(v, "tk", vars, { chance = 0.7 })
      elseif (r.d or 0) >= 3 and r.d > (r.k or 0) then
        vb.nemesis_id = k
        bot_chat(v, "nemesis", { name = P.name(k), count = r.d }, { chance = 0.5 })
      elseif hs then bot_chat(v, "death_hs", vars, { chance = 0.2 })
      elseif vb.mood <= -2 then bot_chat(v, "tilt", vars, { chance = 0.3 })
      else bot_chat(v, "death", vars, { chance = 0.15 }) end
    end
    save_mem(vb)
    xmod.emit("xbots_death", v, k)
  end
  -- teammates of the victim learn where the danger was
  local vx, vy, vz = P.origin(v)
  for id, br in live() do
    if id ~= v and br.alive and P.team(id) == vt and not br.enemy then
      local bx, by, bz = P.origin(id)
      if dist3(bx, by, bz, vx, vy, vz) < 1500 then br.heard = { x = vx, y = vy, z = vz, t = t, loud = true } end
    end
  end
end)

-- human radio orders
xmod.on("client_command", function(id, cmd)
  if not cfg.obey or bots[id] or P.is_bot(id) then return end
  local kind = RADIO_ORDER[(cmd or ""):lower()]
  if kind and P.alive(id) then give_order(id, kind) end
end)

-- human chat: orders, replies, history
xmod.on("chat", function(id, text, team_only)
  if bots[id] or not P.connected(id) then return end
  push_hist(P.name(id), text)
  local low = " " .. strip_ro(text:lower()) .. " "
  local mentions = low:find("bot", 1, true) ~= nil
  -- orders: "boti dupa mine", "bots hold", "Vulpea follow me"...
  local named
  for bid, br in live() do
    local n = br.name:lower()
    if #n >= 3 and low:find(n, 1, true) then named = bid break end
  end
  if cfg.obey and (mentions or named) and P.alive(id) then
    for _, o in ipairs(CHAT_ORDER) do
      for _, w in ipairs(o[2]) do
        if low:find(w, 1, true) then
          if named and not mentions then
            local br = bots[named]
            if P.team(named) == P.team(id) then
              local x, y, z = P.origin(id)
              if o[1] == "free" then br.order = nil
              elseif o[1] == "rush" then br.order, br.role = nil, "rush"
              else br.order = { kind = o[1], leader = id, x = x, y = y, z = z, expire = now() + 60 } end
              br.task = nil
              B.cmd(named, "roger")
            end
          else
            give_order(id, o[1])
          end
          return
        end
      end
    end
  end
  if not cfg.chat then return end
  -- replies
  local target = named
  if not target then
    if not mentions and rand() > 0.06 * cfg.chat_rate then return end
    local same, any = {}, {}
    for bid in live() do
      any[#any + 1] = bid
      if P.alive(bid) == P.alive(id) then same[#same + 1] = bid end
    end
    local pool = #same > 0 and same or any
    if #pool == 0 then return end
    target = pool[rand(#pool)]
  end
  local br = bots[target]
  if not br or now() < (br.reply_t or 0) then return end
  br.reply_t = now() + 4
  if not ai_reply(target, id, text) then template_reply(target, id, text) end
end)

-- =============================================================================================
-- admin commands and menu
local function can_admin(id) return id == 0 or P.has_access(id, cfg.flag) end

local function find_bot(arg)
  if not arg then return nil end
  local a = arg:lower()
  for id, br in live() do if br.name:lower() == a then return id end end
  for id, br in live() do if br.name:lower():find(a, 1, true) then return id end end
  local n = tonumber(arg)
  if n and bots[n] then return n end
  return nil
end

local HELP = {
  "xbots add [t|ct|any] [skill 0-100] [name]   - add a bot",
  "xbots kick <name|all|t|ct>                 - remove bots",
  "xbots quota <n> [fill|fixed|match]         - number of bots",
  "xbots difficulty <0-4> · xbots skill <0-100> [name|all]",
  "xbots list · xbots info <name> · xbots memory <name> · xbots forget <name|all>",
  "xbots say <name> <text> · xbots order <name|all> <follow <player>|hold|rush|free>",
  "xbots nav <info|analyze [clear]|save|load|clear|learn 0/1>",
  "xbots reload                               - re-read configs/xbots.ini and xbots_chat.ini",
}

local function cmd_xbots(id, args)
  if not can_admin(id) then U.reply(id, "[xbots] no access"); return end
  if not ext_ok() then U.reply(id, "[xbots] the xmod_bots extension is not loaded"); return end
  local sub = (args[1] or "help"):lower()
  if sub == "add" then
    local team, skill, name
    for i = 2, args.n do
      local a = args[i]:lower()
      if a == "t" or a == "ct" or a == "any" then team = a
      elseif tonumber(a) then skill = tonumber(a)
      else name = (name and name .. " " or "") .. args[i] end
    end
    local bid, err = add_bot({ team = team ~= "any" and team or nil, skill = skill, name = name })
    U.reply(id, bid and ("[xbots] added " .. bots[bid].name) or ("[xbots] " .. tostring(err)))
    if bid then sync_quota(0) end
  elseif sub == "kick" then
    local a = (args[2] or ""):lower()
    local n = 0
    for bid, br in live() do
      if a == "all" or (a == "t" and P.team(bid) == 1) or (a == "ct" and P.team(bid) == 2) or br.name:lower() == a then
        kick_bot(bid); n = n + 1
      end
    end
    if a == "all" then CV.set("xbots_quota", "0") else sync_quota(-n) end
    U.reply(id, "[xbots] kicked " .. n)
  elseif sub == "quota" then
    if args[2] then CV.set("xbots_quota", args[2]) end
    if args[3] then CV.set("xbots_quota_mode", args[3]) end
    U.reply(id, "[xbots] quota " .. CV.get("xbots_quota") .. " (" .. CV.get("xbots_quota_mode") .. ")")
  elseif sub == "difficulty" then
    if args[2] then CV.set("xbots_difficulty", args[2]); read_cfg()
      for bid, br in live() do br.skill = rand_skill(); apply_skill(bid, br) end end
    U.reply(id, "[xbots] difficulty " .. CV.get("xbots_difficulty"))
  elseif sub == "skill" then
    local s = tonumber(args[2] or "")
    if not s then U.reply(id, "usage: xbots skill <0-100> [name|all]"); return end
    for bid, br in live() do
      if not args[3] or args[3]:lower() == "all" or bid == find_bot(args[3]) then br.skill = math.max(0, math.min(100, s)); apply_skill(bid, br) end
    end
    U.reply(id, "[xbots] skill set")
  elseif sub == "list" then
    for bid, br in live() do
      U.reply(id, string.format("#%d %-16s team %d skill %3d %-10s task %-7s K/D %d/%d", bid, br.name, P.team(bid), br.skill,
        br.pers_name, br.task and br.task.kind or "-", br.mem.k, br.mem.d))
    end
    U.reply(id, "[xbots] " .. count_bots() .. " bots")
  elseif sub == "info" or sub == "memory" then
    local bid = find_bot(args[2])
    if not bid then U.reply(id, "[xbots] no such bot"); return end
    local br = bots[bid]
    local nem, vic, fav = nemesis_of(br), victim_of(br), fav_weapon(br)
    U.reply(id, string.format("[xbots] %s: skill %d, %s, mood %d, kills %d, deaths %d, hs %d, rounds %d, wins %d",
      br.name, br.skill, br.pers_name, br.mood, br.mem.k, br.mem.d, br.mem.hs or 0, br.mem.rounds or 0, br.mem.wins or 0))
    U.reply(id, string.format("  nemesis: %s · favourite victim: %s · best weapon: %s", nem and (nem.n .. " (" .. nem.d .. ")") or "-",
      vic and (vic.n .. " (" .. vic.k .. ")") or "-", fav or "-"))
  elseif sub == "forget" then
    local a = (args[2] or ""):lower()
    for bid, br in live() do
      if a == "all" or br.name:lower() == a then br.mem = { k = 0, d = 0, hs = 0, rounds = 0, wins = 0, w = {}, rel = {}, since = os.time() }; save_mem(br) end
    end
    if a == "all" then for _, k in ipairs(MEMV:keys("bot:")) do MEMV:remove(k) end end
    U.reply(id, "[xbots] forgotten")
  elseif sub == "say" then
    local bid = find_bot(args[2])
    if bid and args.n >= 3 then
      local parts = {}
      for i = 3, args.n do parts[#parts + 1] = args[i] end
      say(bid, table.concat(parts, " "))
    end
  elseif sub == "order" then
    local who, kind = (args[2] or ""):lower(), (args[3] or ""):lower()
    local leader = args[4] and P.find(args[4]) or (id ~= 0 and id or nil)
    local t = now()
    for bid, br in live() do
      if who == "all" or bid == find_bot(who) then
        if kind == "free" then br.order = nil
        elseif kind == "rush" then br.order, br.role = nil, "rush"
        elseif leader and P.connected(leader) then
          local x, y, z = P.origin(leader)
          br.order = { kind = kind == "hold" and "hold" or "follow", leader = leader, x = x, y = y, z = z, expire = t + 120 }
        end
        br.task = nil
      end
    end
    U.reply(id, "[xbots] ok")
  elseif sub == "nav" then
    local a = (args[2] or "info"):lower()
    if a == "analyze" then
      B.nav_analyze((args[3] or "") == "clear", cfg.nav_max); U.reply(id, "[xbots] analyzing...")
    elseif a == "save" then U.reply(id, "[xbots] save: " .. tostring(B.nav_save()))
    elseif a == "load" then U.reply(id, "[xbots] load: " .. tostring(B.nav_load())); scan_map()
    elseif a == "clear" then B.nav_clear(); U.reply(id, "[xbots] navigation cleared (not saved)")
    elseif a == "learn" then
      CV.set("xbots_learn", args[3] or "1"); read_cfg(); B.config({ autolearn = cfg.learn })
      U.reply(id, "[xbots] learning " .. (cfg.learn and "on" or "off"))
    else
      local i = B.nav_info()
      U.reply(id, string.format("[xbots] nav %s: %d nodes, %d links, analyzing %s (%d%%), unsaved %s, learning %s",
        xmod.map(), i.nodes, i.links, tostring(i.analyzing), math.floor(i.progress * 100), tostring(i.dirty), tostring(i.autolearn)))
    end
  elseif sub == "reload" then
    read_cfg(); load_configs(); U.reply(id, "[xbots] configs reloaded (" .. #roster .. " names)")
  else
    for _, l in ipairs(HELP) do U.reply(id, l) end
  end
end

xmod.cmd.client("xbots", "", function(id, args) cmd_xbots(id, args) end, "xbots help")
xmod.cmd.server("xbots", function(args) cmd_xbots(0, args) end, "xbots help")

local function open_menu(id)
  if not can_admin(id) then return end
  local d = CV.int("xbots_difficulty")
  menu.show(id, {
    title = "xmod bots (" .. count_bots() .. ")",
    items = {
      { "Adauga bot", "add" }, { "Adauga bot T", "add_t" }, { "Adauga bot CT", "add_ct" },
      { "Scoate un bot", "kick1" }, { "Scoate toti botii", "kickall" },
      { "Quota +1  (acum " .. CV.get("xbots_quota") .. ")", "q+" }, { "Quota -1", "q-" },
      { "Dificultate: " .. d .. "  (schimba)", "diff" }, { "Botii ma urmeaza", "follow" }, { "Botii liberi", "free" },
      { "Analizeaza harta (nav)", "analyze" }, { "Salveaza nav", "save" },
    },
    on_select = function(p, _, v)
      if v == "add" or v == "add_t" or v == "add_ct" then
        if add_bot({ team = v == "add_t" and "t" or (v == "add_ct" and "ct" or nil) }) then sync_quota(0) end
      elseif v == "kick1" then local b = next(bots); if b then kick_bot(b); sync_quota(-1) end
      elseif v == "kickall" then cmd_xbots(0, { "kick", "all", n = 2 })
      elseif v == "q+" then CV.set("xbots_quota", tostring(CV.int("xbots_quota") + 1))
      elseif v == "q-" then CV.set("xbots_quota", tostring(math.max(0, CV.int("xbots_quota") - 1)))
      elseif v == "diff" then cmd_xbots(0, { "difficulty", tostring((d + 1) % 5), n = 2 })
      elseif v == "follow" then if P.alive(p) then give_order(p, "follow") end
      elseif v == "free" then give_order(p, "free")
      elseif v == "analyze" then B.nav_analyze(false, cfg.nav_max)
      elseif v == "save" then B.nav_save() end
      xmod.timer.after(0.2, function() if P.connected(p) then open_menu(p) end end)
    end,
  })
end
xmod.cmd.say("/bots", "", function(id) open_menu(id) end, "xmod bots menu")

-- =============================================================================================
-- timers
xmod.timer.every(0.05, scheduler)
xmod.timer.every(0.25, hearing)
xmod.timer.every(2.0, quota_tick)
xmod.timer.every(1.0, function() if ready then poll_bomb() end end)
xmod.timer.every(300, function() if ready and ext_ok() and B.nav_info().dirty then B.nav_save() end end)

-- =============================================================================================
-- API for other plugins
--   xmod.call("xbots.add", name|nil, "t"|"ct"|nil, skill|nil, personality|nil) -> id | nil, err
--   xmod.call("xbots.kick", id|"all")                xmod.call("xbots.list") -> {ids}
--   xmod.call("xbots.is_bot", id)                    xmod.call("xbots.skill", id, 0..100)
--   xmod.call("xbots.order", id|0, "follow"|"hold"|"goto"|"rush"|"free", leader_or_x, y, z, seconds)
--   xmod.call("xbots.task", id, {kind = "go"|"camp"|"seek"|"wait", x=, y=, z=, time=, expire=})
--   xmod.call("xbots.say", id, text, team_only)      xmod.call("xbots.chat_event", id, "kill", {name = "..."})
--   xmod.call("xbots.brain", id) -> live brain table  xmod.call("xbots.memory", id) -> copy of the memory
-- Events (xmod.on): xbots_added(id) xbots_removed(id) xbots_spawn(id) xbots_kill(bot, victim) xbots_death(bot, killer)
--   xbots_order(bot, kind, leader) · xbots_think(id, brain) return true = you drive the bot this tick
--   xbots_target(id, enemy) return true = ignore that enemy · xbots_chat(id, text) return true = block the message
xmod.export("xbots.add", function(name, team, skill, pers) return add_bot({ name = name, team = team, skill = skill, pers = pers }) end)
xmod.export("xbots.kick", function(who)
  if who == "all" then for id in live() do kick_bot(id) end return true end
  return kick_bot(who)
end)
xmod.export("xbots.list", function() local l = {}; for id in live() do l[#l + 1] = id end; return l end)
xmod.export("xbots.is_bot", function(id) return bots[id] ~= nil end)
xmod.export("xbots.skill", function(id, s)
  local br = bots[id]; if not br then return false end
  br.skill = math.max(0, math.min(100, tonumber(s) or br.skill)); apply_skill(id, br); return true
end)
xmod.export("xbots.order", function(id, kind, a, y, z, secs)
  local t = now()
  local function apply(bid, br)
    if kind == "free" then br.order = nil
    elseif kind == "rush" then br.order, br.role = nil, "rush"
    elseif kind == "goto" or kind == "hold" then br.order = { kind = kind, x = a, y = y, z = z, expire = t + (secs or 60) }
    else br.order = { kind = "follow", leader = a, expire = t + (secs or 60) } end
    br.task = nil
  end
  if id == 0 then for bid, br in live() do apply(bid, br) end return true end
  if bots[id] then apply(id, bots[id]); return true end
  return false
end)
xmod.export("xbots.task", function(id, tk)
  local br = bots[id]
  if not br or type(tk) ~= "table" or not TASKS[tk.kind or ""] then return false end
  tk.expire = tk.expire or (now() + (tk.time or 30))
  br.task = tk
  return true
end)
xmod.export("xbots.say", function(id, text, team) if bots[id] then say(id, tostring(text), team) return true end return false end)
xmod.export("xbots.chat_event", function(id, ev, vars) return bot_chat(id, ev, vars or {}, { force = true }) end)
xmod.export("xbots.brain", function(id) return bots[id] end)
local function deep_copy(v) if type(v) ~= "table" then return v end; local t = {}; for k, x in pairs(v) do t[k] = deep_copy(x) end; return t end
xmod.export("xbots.memory", function(id)
  local br = bots[id]; if not br then return nil end
  return deep_copy(br.mem)
end)

load_configs()
