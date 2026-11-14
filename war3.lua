--[[
  xmod Warcraft 3 (War3:FT style) for Counter-Strike 1.6                                                    version 2.0
  ------------------------------------------------------------------------------------------------------------------------
  8 races x (3 skills + 1 ultimate), hero attributes (Strength / Agility / Intelligence), configurable max level,
  XP for kills / headshots / streaks / first blood / round wins, an item shop, per-race progress saved in xmod.vault.
  Nexus front-end: HUD + WC3 command card, info above every player's head (race, level, name, weapon, HP bar),
  hero auras, killing-streak auras, death souls, level-up shockwaves, kill feed, VICTORY / DEFEAT, storm ambience,
  3D ultimates (lightning, roots, fire, explosions), damage numbers, mouse menus.
  Players without the Nexus client get classic menus, a text HUD and chat announcements - the mod plays the same.

  Chat:    /war3menu /changerace /skills /resetskills /shopmenu /level /war3help /raceinfo /playerskills /war3top
  Console: ultimate   (bind c ultimate)      Nexus players: the key in war3_ult_key (default "c"), no bind needed
  Admin:   war3_givexp <target> <xp>   war3_setlevel <target> <level>   war3_reset <target>   (access "m")
  Cvars:   war3_enabled 1 · war3_max_level 10 · war3_ult_level 6 · war3_attr_max 15 · war3_xp_mult 1.0 · war3_save 1
           war3_ult_cooldown 35 · war3_ult_key c · war3_max_items 2 · war3_kill_money 0 · war3_bots 1
           war3_hud_y 150 (HUD distance from the top, virtual px of 768) · war3_tags 1 · war3_tag_dist 1400
           war3_enemy_hp 1 (HP bars over enemies) · war3_auras 1 · war3_ambience 1 (storm flashes) · war3_killfeed 1
  Levels above 10 need 1300 + 300 per extra level more XP each; points beyond the skills go into the attributes.
]]
xmod.register("Warcraft 3", "2.0", "Andrew")
local N = require("nexus")
local U = require("util")

-- =====================================================================================================================
-- settings
-- =====================================================================================================================
xmod.cvar.create("war3_enabled", "1")
xmod.cvar.create("war3_xp_mult", "1.0")
xmod.cvar.create("war3_save", "1")
xmod.cvar.create("war3_ult_cooldown", "35")
xmod.cvar.create("war3_ult_key", "c")
xmod.cvar.create("war3_max_items", "2")
xmod.cvar.create("war3_kill_money", "0")
xmod.cvar.create("war3_bots", "1")
xmod.cvar.create("war3_max_level", "10")
xmod.cvar.create("war3_ult_level", "6")
xmod.cvar.create("war3_attr_max", "15")
xmod.cvar.create("war3_hud_y", "150")
xmod.cvar.create("war3_tags", "1")
xmod.cvar.create("war3_tag_dist", "1400")
xmod.cvar.create("war3_enemy_hp", "1")
xmod.cvar.create("war3_auras", "1")
xmod.cvar.create("war3_ambience", "1")
xmod.cvar.create("war3_killfeed", "1")

local BASE_XP = { 150, 350, 650, 1050, 1550, 2150, 2900, 3800, 4900, 6200 }          -- XP needed for levels 1..10
local XPC = {}
local function xp_need(n)                                                             -- total XP needed for level n (any n)
  if n <= 0 then return 0 elseif n <= 10 then return BASE_XP[n] end
  local v = XPC[n]; if v then return v end
  v = xp_need(n - 1) + 1300 + (n - 10) * 300; XPC[n] = v; return v
end
local SAVE = xmod.vault.open("war3")
local TAG = "{g}[War3]{y} "

local function enabled() return xmod.cvar.int("war3_enabled") ~= 0 end
local function now() return xmod.gametime() end
local function chance(p) return math.random() < p end
local function clamp(v, a, b) if v < a then return a elseif v > b then return b end return v end
local function max_level() return clamp(xmod.cvar.int("war3_max_level"), 1, 100) end
local function ult_level() return clamp(xmod.cvar.int("war3_ult_level"), 1, max_level()) end
local function attr_max() return clamp(xmod.cvar.int("war3_attr_max"), 0, 100) end

-- sounds that ship with every CS 1.6 install (cstrike + valve). Server-side ones are precached; the rest play on Nexus clients.
local SND = { soul = "debris/beamstart2.wav", level = "debris/beamstart4.wav", bell = "buttons/bell1.wav", ready = "buttons/blip1.wav",
  kill = "buttons/blip2.wav", heal = "items/medshot4.wav", thunder = "ambience/thunder_clap.wav", boom = "weapons/explode3.wav",
  pick = "items/gunpickup2.wav", suck = "ambience/port_suckin1.wav", spawn = "debris/beamstart1.wav" }
local PRE = {}
xmod.on("precache", function()
  for _, k in ipairs({ "soul", "level", "bell", "thunder", "suck" }) do PRE[SND[k]] = (xmod.precache.sound(SND[k]) or 0) > 0 end
end)

-- =====================================================================================================================
-- races. skill values are per level (1..3). ult = ultimate (1 level).
-- =====================================================================================================================
local RACES = {
  { key = "undead", name = "Undead Scourge", color = "#9B6BFF", short = "UD",
    skills = {
      { name = "Vampiric Aura",  desc = "Heal %d%% of the damage you deal", vals = { 10, 20, 30 } },
      { name = "Unholy Aura",    desc = "Move %d%% faster", vals = { 8, 16, 25 } },
      { name = "Levitation",     desc = "Gravity %d%% lower", vals = { 20, 40, 55 } } },
    ult = { name = "Suicide Bomber", desc = "Explode after 1.5 s: up to 120 damage around you" } },
  { key = "human", name = "Human Alliance", color = "#5AA0FF", short = "HU",
    skills = {
      { name = "Invisibility",   desc = "Become %d%% transparent", vals = { 40, 57, 72 } },
      { name = "Devotion Aura",  desc = "+%d health on spawn", vals = { 15, 30, 45 } },
      { name = "Bash",           desc = "%d%% chance to stun the enemy", vals = { 10, 17, 25 } } },
    ult = { name = "Teleport", desc = "Teleport to where you aim (up to 1200 units)" } },
  { key = "orc", name = "Orcish Horde", color = "#FF5A3C", short = "OR",
    skills = {
      { name = "Critical Strike", desc = "12%% chance for x%s damage", vals = { "1.5", "2", "2.5" }, mult = { 1.5, 2.0, 2.5 } },
      { name = "Critical Grenade", desc = "HE grenades deal x%s damage", vals = { "1.5", "2", "2.5" }, mult = { 1.5, 2.0, 2.5 } },
      { name = "Endurance",      desc = "Regenerate %d HP per second", vals = { 1, 2, 3 } } },
    ult = { name = "Chain Lightning", desc = "Lightning jumps between 3 enemies (60 / 40 / 25)" } },
  { key = "nightelf", name = "Night Elf", color = "#3CE0A0", short = "NE",
    skills = {
      { name = "Evasion",        desc = "%d%% chance to avoid a hit", vals = { 8, 14, 20 } },
      { name = "Thorns Aura",    desc = "Reflect %d%% of the damage you take", vals = { 8, 15, 22 } },
      { name = "Trueshot Aura",  desc = "Deal %d%% more damage", vals = { 8, 16, 25 } } },
    ult = { name = "Entangling Roots", desc = "Root the enemy you aim at for 3 seconds" } },
  { key = "bloodmage", name = "Blood Mage", color = "#FF2A55", short = "BM",
    skills = {
      { name = "Banish",         desc = "%d%% chance to blind and lift the enemy", vals = { 8, 12, 16 } },
      { name = "Siphon Mana",    desc = "Steal $%d per point of damage dealt", vals = { 2, 4, 6 } },
      { name = "Resistant Skin", desc = "Take %d%% less damage", vals = { 8, 14, 20 } } },
    ult = { name = "Immolate", desc = "Set the ground you aim at on fire for 3 s (15 dmg/s)" } },
  { key = "shadow", name = "Shadow Hunter", color = "#4CFF5A", short = "SH",
    skills = {
      { name = "Healing Wave",   desc = "Heal teammates near you for %d HP every 2 s", vals = { 4, 7, 10 } },
      { name = "Hex",            desc = "%d%% chance to slow the enemy for 3 s", vals = { 6, 10, 14 } },
      { name = "Serpent Ward",   desc = "Enemies within 250 units take %d damage per second", vals = { 2, 4, 6 } } },
    ult = { name = "Big Bad Voodoo", desc = "You cannot be hurt for 3 seconds" } },
  { key = "warden", name = "Warden", color = "#C8A0FF", short = "WA",
    skills = {
      { name = "Fan of Knives",  desc = "%d%% chance to hit enemies near your target for 12", vals = { 6, 10, 14 } },
      { name = "Blink",          desc = "%d%% chance to ignore an enemy ultimate", vals = { 33, 66, 100 } },
      { name = "Shadow Strike",  desc = "%d%% chance to poison: 4 damage/s for 5 s", vals = { 10, 15, 20 } } },
    ult = { name = "Vengeance", desc = "Heal to full and return to where you spawned" } },
  { key = "cryptlord", name = "Crypt Lord", color = "#E0C060", short = "CL",
    skills = {
      { name = "Impale",         desc = "%d%% chance to throw the enemy into the air", vals = { 8, 12, 16 } },
      { name = "Spiked Carapace", desc = "+%d armor, reflect 10%% of damage taken", vals = { 25, 50, 75 } },
      { name = "Carrion Beetles", desc = "%d%% chance for +12 damage", vals = { 10, 15, 20 } } },
    ult = { name = "Locust Swarm", desc = "Locusts hunt a random enemy for 35-55 damage" } },
}
local RACE_BY_KEY = {}; for i, r in ipairs(RACES) do r.id = i; RACE_BY_KEY[r.key] = i end
local RACE_MK = { undead = "^6", human = "^5", orc = "^1", nightelf = "^2", bloodmage = "^1", shadow = "^2", warden = "^6", cryptlord = "^3" }   -- markup colour

-- hero attributes: every level point not spent on skills can go here (capped by war3_attr_max per attribute)
local ATTRS = {
  { key = "str", name = "Strength",     short = "STR", mk = "^1", desc = "+4 max HP per point" },
  { key = "agi", name = "Agility",      short = "AGI", mk = "^2", desc = "+1% speed and +1% damage per point" },
  { key = "int", name = "Intelligence", short = "INT", mk = "^5", desc = "-2% ultimate cooldown and +2% XP per point" },
}

-- =====================================================================================================================
-- items (bought with CS money, lost on death; Tome is used at once)
-- =====================================================================================================================
local ITEMS = {
  { key = "boots",    name = "Boots of Speed",       price = 2500, desc = "Move 20% faster" },
  { key = "claws",    name = "Claws of Attack +6",   price = 1000, desc = "+6 damage on every hit" },
  { key = "cloak",    name = "Cloak of Shadows",     price = 800,  desc = "Become partly invisible" },
  { key = "mask",     name = "Mask of Death",        price = 2000, desc = "Heal 10% of the damage you deal" },
  { key = "necklace", name = "Necklace of Immunity", price = 800,  desc = "Immune to enemy ultimates" },
  { key = "frost",    name = "Orb of Frost",         price = 2000, desc = "Your hits slow the enemy" },
  { key = "periapt",  name = "Periapt of Health",    price = 1000, desc = "+50 health" },
  { key = "tome",     name = "Tome of Experience",   price = 4000, desc = "+150 XP at once" },
  { key = "sock",     name = "Sock of the Feather",  price = 1500, desc = "Lower gravity" },
  { key = "ring",     name = "Ring of Regeneration", price = 1000, desc = "Regenerate 2 HP per second" },
}
local ITEM_BY_KEY = {}; for i, it in ipairs(ITEMS) do it.id = i; ITEM_BY_KEY[it.key] = it end

-- =====================================================================================================================
-- player state
-- =====================================================================================================================
local war3_top, menu_main, menu_race, menu_skills, menu_shop, show_raceinfo, auto_skills, use_ultimate, auras_refresh, tags_refresh   -- defined further down
local P = {}          -- P[id] = { race, xp, level, skills = {s1,s2,s3,ult}, items = {}, next_race, ult_ready_at, ... }
local function newstate()
  return { race = 0, xp = 0, level = 0, skills = { 0, 0, 0, 0 }, attr = { 0, 0, 0 }, items = {}, saved = {}, ult_ready_at = 0, streak = 0,
    stun_until = 0, slow_until = 0, slow_mult = 1, voodoo_until = 0, poison = nil, spawn = nil, base_speed = 250, hud = {} }
end
local function st(id) local s = P[id]; if not s then s = newstate(); P[id] = s end; return s end
local function race_of(id) local s = P[id]; return s and RACES[s.race] or nil end
local function sk(id, n) local s = P[id]; if not s or s.race == 0 then return 0 end; return s.skills[n] or 0 end
local function has_item(id, key) local s = P[id]; if not s then return false end; for _, k in ipairs(s.items) do if k == key then return true end end; return false end
local function alive(id) return xmod.player.connected(id) and xmod.player.ingame(id) and xmod.player.alive(id) end
local function is_player(e) return type(e) == "number" and e >= 1 and e <= xmod.maxplayers() end
local function enemies(a, b) local ta, tb = xmod.player.team(a), xmod.player.team(b); return ta ~= tb and ta >= 1 and ta <= 2 and tb >= 1 and tb <= 2 end
local function level_for(xp) local l = 0; for i = 1, max_level() do if xp >= xp_need(i) then l = i else break end end; return l end
local function points_free(id) local s = st(id); local used = 0; for i = 1, 4 do used = used + s.skills[i] end; for i = 1, 3 do used = used + s.attr[i] end; return s.level - used end
local function at(id, n) local s = P[id]; if not s or s.race == 0 then return 0 end; return s.attr[n] or 0 end
local function invisible(id) return xmod.ent.get(id, "rendermode") == xmod.RENDER_TRANSALPHA and xmod.ent.get(id, "renderamt") < 200 end
local function save_key(id) if xmod.player.is_bot(id) then return nil end; local k = xmod.player.steamid(id); if not k or k == "" or k:find("PENDING") or k == "BOT" then return nil end; return "p:" .. k end

-- ---------------------------------------------------------------------------------------------------------------- saving
local function save_player(id)
  local s = P[id]; if not s or xmod.cvar.int("war3_save") == 0 then return end
  local key = save_key(id); if not key then return end
  if s.race > 0 then s.saved[RACES[s.race].key] = { xp = s.xp, s = { s.skills[1], s.skills[2], s.skills[3], s.skills[4] }, a = { s.attr[1], s.attr[2], s.attr[3] } } end
  SAVE:set(key, { race = s.race > 0 and RACES[s.race].key or "", races = s.saved, name = xmod.player.name(id) })
end
local function load_player(id)
  local s = newstate(); P[id] = s
  local key = save_key(id); if not key then return s end
  local d = SAVE:get(key)
  if type(d) == "table" then
    s.saved = type(d.races) == "table" and d.races or {}
    local r = RACE_BY_KEY[d.race or ""]
    if r then
      local rs = s.saved[RACES[r].key] or {}
      s.race = r; s.xp = tonumber(rs.xp) or 0; s.level = level_for(s.xp)
      for i = 1, 4 do s.skills[i] = (type(rs.s) == "table" and tonumber(rs.s[i])) or 0 end
      for i = 1, 3 do s.attr[i] = (type(rs.a) == "table" and tonumber(rs.a[i])) or 0 end
      if points_free(id) < 0 then s.skills = { 0, 0, 0, 0 }; s.attr = { 0, 0, 0 } end   -- lower max level / corrupted data: give the points back
    end
  end
  return s
end

-- =====================================================================================================================
-- HUD (Nexus) and text HUD (others)
-- =====================================================================================================================
local function race_color(r) return r and r.color or "#FFFFFF" end
local function xp_frac(s)
  if s.level >= max_level() then return 1 end
  local a, b = xp_need(s.level), xp_need(s.level + 1); return clamp((s.xp - a) / (b - a), 0, 1)
end
local function items_line(id)
  local s = st(id); if #s.items == 0 then return "^8no items - ^3/shopmenu" end
  local n = {}; for _, k in ipairs(s.items) do n[#n + 1] = "^9" .. ITEM_BY_KEY[k].name end
  return table.concat(n, "^8, ")
end
local function attr_line(id)
  local s = st(id); local t = {}
  for i, a in ipairs(ATTRS) do t[#t + 1] = a.mk .. a.short .. " ^7" .. s.attr[i] end
  return table.concat(t, "   ")
end
local function ult_key() local k = xmod.cvar.get("war3_ult_key"); return (k == "" and "c" or k):upper() end

-- left panel (anchored top-left, war3_hud_y below the top) + a WC3 style command card at the bottom centre
local SLOT_W, SLOT_GAP, G = 128, 8, 31
local function hy(v) return clamp(xmod.cvar.int("war3_hud_y"), 0, 700) + v end
local function slot_x(i) return math.floor((i - 2.5) * (SLOT_W + SLOT_GAP)) end
local function hud_build(id)
  if not N.has(id) then return end
  local s = st(id); s.hud = {}
  N.clear(id, G)
  N.rect(id, "war3.bg", { anchor = "topleft", px = 12, py = hy(0), pw = 360, ph = 94, color = { 8, 10, 16, 185 }, color2 = { 255, 255, 255, 32 }, outline = true, layer = 1, group = G })
  N.rect(id, "war3.accent", { anchor = "topleft", px = 12, py = hy(0), pw = 360, ph = 3, color = "#FFC83C", layer = 2, group = G })
  N.text(id, "war3.race", { anchor = "topleft", px = 22, py = hy(10), text = "", markup = true, scale = 1.05, shadow = true, layer = 2, group = G })
  N.bar(id, "war3.xp", { anchor = "topleft", px = 22, py = hy(35), pw = 340, ph = 7, color = "#FFC83C", color2 = { 255, 255, 255, 30 }, value = 0, layer = 2, group = G })
  N.text(id, "war3.xptext", { anchor = "topleft", px = 22, py = hy(46), text = "", markup = true, scale = 0.68, layer = 2, group = G })
  N.text(id, "war3.attr", { anchor = "topleft", px = 22, py = hy(61), text = "", markup = true, scale = 0.68, layer = 2, group = G })
  N.text(id, "war3.items", { anchor = "topleft", px = 22, py = hy(76), pw = 344, text = "", markup = true, scale = 0.66, layer = 2, group = G })
  for i = 1, 4 do
    local cx = slot_x(i)
    N.rect(id, "war3.slot" .. i, { anchor = "bottom", px = cx, py = -54, pw = SLOT_W, ph = 54, color = { 8, 10, 16, 190 }, color2 = { 255, 255, 255, 40 }, outline = true, layer = 1, group = G, hidden = true })
    N.text(id, "war3.sname" .. i, { anchor = "bottom", px = cx, py = -82, pw = SLOT_W - 8, text = "", markup = true, scale = 0.74, shadow = true, layer = 2, group = G })
    N.text(id, "war3.spip" .. i, { anchor = "bottom", px = cx, py = -60, text = "", markup = true, scale = 0.9, shadow = true, layer = 2, group = G })
  end
  N.bar(id, "war3.ult", { anchor = "bottom", px = slot_x(4), py = -56, pw = SLOT_W - 6, ph = 4, color = "#6FD0FF", color2 = { 255, 255, 255, 25 }, value = 1, layer = 3, group = G, hidden = true })
end
local function ult_ready(s) return s.skills[4] > 0 and now() >= s.ult_ready_at end
local function hud_update(id, force)
  if not xmod.player.connected(id) then return end
  local s = st(id); local r = RACES[s.race]
  if N.has(id) then
    local h = s.hud
    local free = points_free(id)
    local race = r and (RACE_MK[r.key] .. r.name .. "  ^7Level ^3" .. s.level .. (s.level >= max_level() and " ★" or "")
      .. (free > 0 and ("  ^2+" .. free .. " point" .. (free > 1 and "s" or "")) or "")) or "^8No race - type ^3/changerace"
    if force or h.race ~= race then h.race = race; N.set(id, "war3.race", { text = race }); N.set(id, "war3.accent", { color = race_color(r) }) end
    local frac = xp_frac(s)
    if force or h.frac ~= frac then h.frac = frac; N.anim(id, "war3.xp", { value = frac, time = 0.4 }) end
    local xpt = s.level >= max_level() and ("^7" .. s.xp .. " XP  ^3(max level)") or ("^7" .. s.xp .. " ^8/ " .. xp_need(s.level + 1) .. " XP")
    if force or h.xpt ~= xpt then h.xpt = xpt; N.set(id, "war3.xptext", { text = xpt }) end
    local al = attr_line(id); if force or h.al ~= al then h.al = al; N.set(id, "war3.attr", { text = al }) end
    local il = items_line(id); if force or h.il ~= il then h.il = il; N.set(id, "war3.items", { text = "^8Items: " .. il }) end
    -- command card
    for i = 1, 4 do
      local name, pip, lit
      if not r then name, pip, lit = "", "", false
      elseif i < 4 then
        local lv = s.skills[i]; name = (lv > 0 and "^7" or "^8") .. r.skills[i].name
        pip = "^3" .. string.rep("●", lv) .. "^8" .. string.rep("●", 3 - lv); lit = lv > 0
      else
        lit = s.skills[4] > 0; name = (lit and "^1" or "^8") .. r.ult.name
        pip = not lit and (s.level >= ult_level() and "^2learn: /skills" or ("^8level " .. ult_level())) or (ult_ready(s) and ("^2READY ^7[" .. ult_key() .. "]") or ("^8[" .. ult_key() .. "]"))
      end
      local key = (r and r.key or "") .. name .. pip
      if force or h["slot" .. i] ~= key then
        h["slot" .. i] = key
        N.set(id, "war3.slot" .. i, { hidden = not r, color2 = lit and N.color(r.color, 200) or { 255, 255, 255, 40 } })
        N.set(id, "war3.sname" .. i, { text = name }); N.set(id, "war3.spip" .. i, { text = pip })
      end
    end
    local ult = s.skills[4] > 0
    if force or h.ult ~= ult then h.ult = ult; N.set(id, "war3.ult", { hidden = not ult }) end
  elseif not xmod.player.is_bot(id) then
    local txt = r and string.format("%s  Lv %d  XP %d/%s  %s", r.name, s.level, s.xp, s.level >= max_level() and "max" or tostring(xp_need(s.level + 1)),
      points_free(id) > 0 and ("[" .. points_free(id) .. " pts: /skills]") or "") or "No race: say /changerace"
    xmod.msg.hud(id, txt, { r = 255, g = 200, b = 60, x = 0.012, y = 0.22, hold = 2.2, fadein = 0, fadeout = 0, channel = 3 })
  end
end
local function take_race(id, rid)
  local s = st(id); local r = RACES[rid]; local rs = s.saved[r.key] or {}
  s.race = rid; s.xp = tonumber(rs.xp) or 0; s.level = level_for(s.xp)
  s.skills = { 0, 0, 0, 0 }; s.attr = { 0, 0, 0 }
  if type(rs.s) == "table" then for i = 1, 4 do s.skills[i] = tonumber(rs.s[i]) or 0 end end
  if type(rs.a) == "table" then for i = 1, 3 do s.attr[i] = tonumber(rs.a[i]) or 0 end end
  if s.level < ult_level() then s.skills[4] = 0 end
  if points_free(id) < 0 then s.skills = { 0, 0, 0, 0 }; s.attr = { 0, 0, 0 } end
end
local function ult_cooldown_visual(id, secs)
  if not N.has(id) then return end
  N.set(id, "war3.ult", { value = 0, color = "#6FD0FF" })
  N.anim(id, "war3.ult", { value = 1, time = math.max(0.05, secs), ease = "linear" })
  hud_update(id)
end

-- =====================================================================================================================
-- effects helpers (shared by everybody who has Nexus)
-- =====================================================================================================================
local fxn = 0
local function fxname() fxn = (fxn % 300) + 1; return "war3.fx" .. fxn end
local function eye(id) local x, y, z = xmod.player.origin(id); local _, _, vz = xmod.ent.get(id, "view_ofs"); return x, y, z + (vz or 17) end
local function aim_dir(id) local p, y = xmod.player.view_angles(id); p, y = math.rad(p), math.rad(y); return math.cos(p) * math.cos(y), math.cos(p) * math.sin(y), -math.sin(p) end
local function aim_trace(id, dist)
  local x, y, z = eye(id); local dx, dy, dz = aim_dir(id)
  return xmod.trace.line(x, y, z, x + dx * dist, y + dy * dist, z + dz * dist, 0, id)
end
local function visible(a, b)
  local x1, y1, z1 = eye(a); local x2, y2, z2 = xmod.player.origin(b)
  local tr = xmod.trace.line(x1, y1, z1, x2, y2, z2, xmod.TRACE_IGNORE_MONSTERS, a)
  return tr.fraction >= 0.97
end
local function dist(a, b) local x1, y1, z1 = xmod.player.origin(a); local x2, y2, z2 = xmod.player.origin(b); return math.sqrt((x1 - x2) ^ 2 + (y1 - y2) ^ 2 + (z1 - z2) ^ 2) end
local function pos(id, dz) local x, y, z = xmod.player.origin(id); return { math.floor(x), math.floor(y), math.floor(z + (dz or 0)) } end
local function float_dmg(attacker, victim, amount, color)
  if not N.has(attacker) or not alive(victim) then return end
  local p = pos(victim, 34); p[1] = p[1] + math.random(-10, 10); p[2] = p[2] + math.random(-10, 10)
  N.float_text(attacker, p, tostring(math.floor(amount + 0.5)), { color = color or "yellow", time = 0.9, rise = 36 })
end
local function lightning(a, b, color)          -- a jagged 3D line from player a to player b, seen by every Nexus player
  local p1, p2 = pos(a, 10), pos(b, 10)
  local prev = p1
  for i = 1, 4 do
    local t = i / 4; local nx = { math.floor(p1[1] + (p2[1] - p1[1]) * t + (i < 4 and math.random(-18, 18) or 0)),
      math.floor(p1[2] + (p2[2] - p1[2]) * t + (i < 4 and math.random(-18, 18) or 0)), math.floor(p1[3] + (p2[3] - p1[3]) * t + (i < 4 and math.random(-14, 14) or 0)) }
    N.shape(0, fxname(), { type = "line", from = prev, to = nx, color = color or { 140, 200, 255, 255 }, width = 4, additive = true, life = 0.6 })
    prev = nx
  end
  N.light(0, fxname(), { pos = p2, radius = 180, color = color or { 120, 180, 255 }, life = 0.4 })
end
local function plain(text) return (text:gsub("%^%d", ""):gsub("★", "*")) end
local function announce(id, text) if N.has(id) then N.toast(id, text, { markup = true, time = 2.2, y = 0.18 }) else xmod.msg.chat(id, TAG .. plain(text)) end end
-- to everybody: a big Nexus banner, chat for players without Nexus
local function announce_all(text, o)
  o = o or {}
  N.toast(0, text, { markup = true, time = o.time or 2.6, y = o.y or 0.12, scale = o.scale or 1.45 })
  for _, p in ipairs(xmod.player.all()) do if not N.has(p) and not xmod.player.is_bot(p) then xmod.msg.chat(p, TAG .. plain(text)) end end
end
-- a sound everybody hears from an entity (server sound when precached, else Nexus 3D sound)
local function sfx(ent, sample, vol)
  if PRE[sample] then xmod.sound.emit(ent, xmod.sound.CHAN_AUTO, sample, vol or 1, 0.8)
  else N.sound3d(0, sample, { ent = ent, volume = vol or 1 }) end
end
local function ring(viewer, center, radius, color, life, o)   -- a flat circle in the world
  o = o or {}
  N.shape(viewer, fxname(), { type = "circle", center = center, radius = radius, color = color, width = o.width or 3, additive = true, pulse = o.pulse, fill = o.fill, life = life, ent = o.ent })
end

-- =====================================================================================================================
-- movement / visuals that depend on race + items (re-applied on spawn, on CS speed reset, and when they change)
-- =====================================================================================================================
local function speed_mult(id)
  local m = 1
  if sk(id, 2) > 0 and race_of(id).key == "undead" then m = m * (1 + RACES[1].skills[2].vals[sk(id, 2)] / 100) end
  if has_item(id, "boots") then m = m * 1.2 end
  m = m * (1 + at(id, 2) / 100)
  local s = P[id]
  if s and now() < s.slow_until then m = m * s.slow_mult end
  return m
end
local function apply_speed(id)
  if not alive(id) then return end
  local s = st(id)
  if now() < s.stun_until then xmod.ent.set(id, "maxspeed", 1); xmod.player.set_velocity(id, 0, 0, 0); return end
  local base = s.base_speed or 250
  if base <= 1 then return end                                  -- CS freeze time
  xmod.ent.set(id, "maxspeed", base * speed_mult(id))
end
local function apply_gravity(id)
  if not alive(id) then return end
  local g = 1
  local r = race_of(id)
  if r and r.key == "undead" and sk(id, 3) > 0 then g = 1 - r.skills[3].vals[sk(id, 3)] / 100 end
  if has_item(id, "sock") then g = math.min(g, 0.5) end
  xmod.ent.set(id, "gravity", g)
end
local function apply_visibility(id)
  if not alive(id) then return end
  local amt = 255
  local r = race_of(id)
  if r and r.key == "human" and sk(id, 1) > 0 then amt = math.floor(255 * (1 - r.skills[1].vals[sk(id, 1)] / 100)) end
  if has_item(id, "cloak") then amt = math.min(amt, 150) end
  local s = P[id]
  if amt < 255 then
    xmod.ent.set(id, "rendermode", xmod.RENDER_TRANSALPHA); xmod.ent.set(id, "renderamt", amt); xmod.ent.set(id, "renderfx", xmod.FX_NONE)
  elseif s and (s.streak or 0) >= 5 and xmod.cvar.int("war3_auras") ~= 0 then        -- rampage: a burning shell everybody sees
    xmod.ent.set(id, "rendermode", xmod.RENDER_NORMAL); xmod.ent.set(id, "renderfx", xmod.FX_GLOWSHELL)
    xmod.ent.set(id, "rendercolor", 255, 70, 20); xmod.ent.set(id, "renderamt", 12)
  else
    xmod.ent.set(id, "rendermode", xmod.RENDER_NORMAL); xmod.ent.set(id, "renderamt", 255); xmod.ent.set(id, "renderfx", xmod.FX_NONE)
  end
end
local function max_hp(id)
  local hp = 100; local r = race_of(id)
  if r and r.key == "human" and sk(id, 2) > 0 then hp = hp + r.skills[2].vals[sk(id, 2)] end
  if has_item(id, "periapt") then hp = hp + 50 end
  return hp + at(id, 1) * 4
end
local function apply_all(id) apply_speed(id); apply_gravity(id); apply_visibility(id) end

-- =====================================================================================================================
-- XP and levels
-- =====================================================================================================================
local function level_up_fx(id)
  local s = st(id); local maxed = s.level >= max_level()
  if N.has(id) then
    N.toast(id, (maxed and "^3★ MAX LEVEL ★  " or "^3LEVEL UP!  ") .. "^7You are now level ^3" .. s.level .. (points_free(id) > 0 and "  ^2(/skills)" or ""), { markup = true, time = 3, y = 0.16, scale = 1.6 })
    N.sound(id, SND.bell, 0.8); N.shake(id, 1.2, 8, 0.6); N.tint(id, { 255, 215, 90, 45 }, 0.1, 0.4); N.fov(id, 100, 0.12)
    xmod.timer.after(0.25, function(p) if N.has(p) then N.fov(p, 0, 0.6) end end, id)
  end
  if alive(id) then
    local feet = pos(id, -34)
    N.effect(0, "teleport", { pos = pos(id) })
    N.shape(0, fxname(), { type = "cylinder", center = feet, radius = 38, height = 220, color = { 255, 210, 60, 150 }, additive = true, fill = true, pulse = true, width = 2, life = 1.4 })
    N.light(0, fxname(), { ent = id, radius = 260, color = { 255, 210, 60 }, life = 1.4 })
    for k = 0, 3 do   -- golden shockwave
      xmod.timer.after(k * 0.12, function() ring(0, feet, 40 + k * 55, { 255, 210, 60, 230 - k * 45 }, 0.45, { width = 4 }) end)
    end
    N.float_text(0, pos(id, 56), maxed and "MAX LEVEL" or "LEVEL UP", { color = "gold", scale = 1.5, time = 1.6, rise = 60 })
    sfx(id, SND.level, 0.9)
  end
  if maxed then announce_all("^3★ ^7" .. N.escape(xmod.player.name(id)) .. " ^3reached MAX LEVEL " .. s.level .. " ★", { time = 3.5 })
  else xmod.msg.chat(0, TAG .. "{t}" .. xmod.player.name(id) .. "{y} reached level {g}" .. s.level .. "{y} (" .. (race_of(id) and race_of(id).name or "?") .. ")", id) end
end
local function give_xp(id, amount, why)
  if not enabled() or not xmod.player.connected(id) then return end
  local s = st(id); if s.race == 0 or amount == 0 then return end
  amount = amount * xmod.cvar.num("war3_xp_mult"); if amount > 0 then amount = amount * (1 + at(id, 3) * 0.02) end
  amount = math.floor(amount + 0.5)
  local before = s.level
  s.xp = math.max(0, s.xp + amount); s.level = level_for(s.xp)
  if why and amount > 0 and not xmod.player.is_bot(id) then
    if N.has(id) then
      local t = N.temp_id()
      N.set(id, t, { type = "text", anchor = "topleft", px = 380, py = hy(32), text = "^2+" .. amount .. " XP ^8" .. why, markup = true, scale = 0.85, shadow = true, life = 1.8, fadeout = 0.6, layer = 3 })
      N.anim(id, t, { px = 380, py = hy(14), time = 1.8 })
    else xmod.msg.chat(id, TAG .. "{g}+" .. amount .. " XP{y} " .. why) end
  end
  if s.level > before then level_up_fx(id); if xmod.player.is_bot(id) then auto_skills(id) end; save_player(id) end
  hud_update(id)
end
local function kill_xp(killer, victim, headshot)
  local vl = P[victim] and P[victim].level or 0
  local xp = 20 + vl * 6 + (headshot and 15 or 0)
  give_xp(killer, xp, headshot and "headshot kill" or "kill")
  local money = xmod.cvar.int("war3_kill_money")
  if money > 0 then xmod.player.set_money(killer, math.min(16000, xmod.player.money(killer) + money)) end
end

-- ---------------------------------------------------------------------------------------------------------------- kill feed
local WNAME = { [1] = "P228", [3] = "Scout", [4] = "HE Grenade", [5] = "XM1014", [6] = "C4", [7] = "MAC-10", [8] = "AUG", [9] = "Smoke", [10] = "Dual Elites",
  [11] = "Five-SeveN", [12] = "UMP45", [13] = "SG550", [14] = "Galil", [15] = "FAMAS", [16] = "USP", [17] = "Glock", [18] = "AWP", [19] = "MP5",
  [20] = "M249", [21] = "M3", [22] = "M4A1", [23] = "TMP", [24] = "G3SG1", [25] = "Flashbang", [26] = "Deagle", [27] = "SG552", [28] = "AK-47", [29] = "Knife", [30] = "P90" }
local CW = {}                                                  -- current weapon id of every player (from CurWeapon)
local SKILL_KILL = { suicide_bomber = "Suicide Bomber", chain_lightning = "Chain Lightning", immolate = "Immolate", locust_swarm = "Locust Swarm",
  serpent_ward = "Serpent Ward", poison = "Shadow Strike", thorns = "Thorns", knife = "Fan of Knives" }
local FEED, feed_text = {}, ""
local function hero(id)
  local s = P[id]; local r = s and RACES[s.race]
  return "^7" .. N.escape(xmod.player.name(id)) .. (r and (" ^8[" .. RACE_MK[r.key] .. r.short .. " ^8" .. s.level .. "]") or "")
end
local function feed_send(to)
  if xmod.cvar.int("war3_killfeed") == 0 then return end
  N.text(to or 0, "war3.feed", { anchor = "topright", px = -14, py = 168, text = feed_text, markup = true, scale = 0.68, outline = true, layer = 5, group = 33 })
end
local function feed_add(killer, victim, hs, weapon)
  local how = SKILL_KILL[weapon or ""] or WNAME[CW[killer or 0] or 0] or weapon or ""
  local line = killer and (hero(killer) .. " ^1» " .. hero(victim) .. " ^8" .. how .. (hs and " ^1(HS)" or "")) or (hero(victim) .. " ^8died")
  table.insert(FEED, 1, { t = now(), text = line }); while #FEED > 5 do table.remove(FEED) end
  local l = {}; for _, f in ipairs(FEED) do l[#l + 1] = f.text end
  feed_text = table.concat(l, "\n"); feed_send()
end
local function feed_expire()
  local ch = false
  while #FEED > 0 and now() - FEED[#FEED].t > 8 do table.remove(FEED); ch = true end
  if ch then local l = {}; for _, f in ipairs(FEED) do l[#l + 1] = f.text end; feed_text = table.concat(l, "\n"); feed_send() end
end

-- ---------------------------------------------------------------------------------------------------------------- deaths
local STREAK = { [3] = "^9KILLING SPREE", [4] = "^9DOMINATING", [5] = "^1MEGA KILL", [6] = "^1UNSTOPPABLE", [7] = "^6WICKED SICK",
  [8] = "^6MONSTER KILL", [9] = "^3GODLIKE", [10] = "^3BEYOND GODLIKE" }
local first_blood = false
local function death_fx(killer, victim, hs)
  local r = race_of(victim)
  local c = r and N.color(r.color) or { 200, 210, 255, 255 }
  local p = pos(victim, -12)
  -- the soul leaves the body: a coloured beam to the sky, a ring on the ground, a light and a whoosh
  N.shape(0, fxname(), { type = "line", from = p, to = { p[1], p[2], p[3] + 360 }, color = { c[1], c[2], c[3], 210 }, width = 9, additive = true, life = 1.3 })
  N.shape(0, fxname(), { type = "cylinder", center = { p[1], p[2], p[3] - 22 }, radius = 22, height = 90, color = { c[1], c[2], c[3], 120 }, fill = true, additive = true, life = 0.9 })
  ring(0, { p[1], p[2], p[3] - 22 }, 46, { c[1], c[2], c[3], 220 }, 1.1)
  N.effect(0, "implosion", { pos = p, size = 40, count = 60, life = 0.8 })
  N.light(0, fxname(), { pos = { p[1], p[2], p[3] + 30 }, radius = 200, color = { c[1], c[2], c[3] }, life = 1.1 })
  sfx(victim, SND.soul, 0.75)
  if N.has(victim) then N.tint(victim, { 90, 0, 0, 120 }, 0.25, 1.8); N.fov(victim, 72, 1.5); N.shake(victim, 2, 6, 0.8) end
  if not is_player(killer) or killer == victim or not xmod.player.connected(killer) then return end
  if alive(killer) then N.effect(0, "beam", { pos = p, to = pos(killer), color = { 255, 210, 60 }, life = 0.45 }) end     -- the XP flows to the killer
  if N.has(killer) then
    N.sound(killer, SND.kill, 0.7)
    if hs then N.float_text(killer, pos(victim, 50), "HEADSHOT", { color = "#FF3030", scale = 1.4, time = 1.1 }) end
  end
  if N.has(victim) then
    local kr = race_of(killer)
    N.toast(victim, "^8Slain by " .. hero(killer) .. (alive(killer) and ("   ^1♥ ^7" .. xmod.player.health(killer) .. " HP left") or ""), { markup = true, time = 3.5, y = 0.30, scale = 1.15 })
  end
end
local function on_kill(killer, victim, hs, weapon)
  if not is_player(victim) or not xmod.player.connected(victim) then return end
  local real = is_player(killer) and killer ~= victim and xmod.player.connected(killer)
  death_fx(killer, victim, hs)
  local vs = st(victim); local vstreak = vs.streak or 0; vs.streak = 0
  if enabled() and real then
    kill_xp(killer, victim, hs)
    local ks = st(killer); ks.streak = (ks.streak or 0) + 1
    if not first_blood then
      first_blood = true; give_xp(killer, 15, "first blood")
      announce_all("^1FIRST BLOOD!  " .. hero(killer))
    end
    if vstreak >= 3 then give_xp(killer, 25, "ended a streak"); announce_all(hero(killer) .. " ^7ended " .. hero(victim) .. "^7's streak (" .. vstreak .. ")", { y = 0.2, scale = 1.1 })
    elseif ks.streak >= 3 then
      announce_all(STREAK[math.min(ks.streak, 10)] .. "!  " .. hero(killer) .. " ^8(" .. ks.streak .. " kills)", { scale = 1.5 })
      N.sound(0, SND.thunder, 0.35)
    end
    apply_visibility(killer)
  end
  feed_add(real and killer or nil, victim, hs, weapon)
  if auras_refresh then auras_refresh() end
end

-- =====================================================================================================================
-- dealing extra damage (skills / ultimates) with kill credit
-- =====================================================================================================================
local expect_death = {}     -- [victim] = { attacker, weapon }  our own kills: the suicide DeathMsg is replaced by one with the right killer
local function ult_immune(attacker, victim)
  if has_item(victim, "necklace") then return true end
  local r = race_of(victim)
  if r and r.key == "warden" and sk(victim, 2) > 0 and chance(r.skills[2].vals[sk(victim, 2)] / 100) then return true end
  return false
end
local function war_damage(attacker, victim, amount, weapon)
  if not alive(victim) or amount <= 0 then return 0 end
  local vs = st(victim)
  if now() < vs.voodoo_until then return 0 end
  amount = math.floor(amount + 0.5)
  local hp = xmod.player.health(victim) - amount
  if hp > 0 then
    xmod.player.set_health(victim, hp)
    if N.has(victim) then N.punch(victim, -2, math.random(-2, 2)) end
    if attacker ~= victim then float_dmg(attacker, victim, amount, "#FF9040") end
    return amount
  end
  -- lethal: kill the victim, then give the kill to the attacker (frags, DeathMsg, XP)
  local vfr = xmod.player.frags(victim)
  expect_death[victim] = { attacker = attacker, weapon = weapon or "world", t = now() }
  xmod.player.set_health(victim, 1)                            -- if the game refuses the kill (suicide cooldown) the victim is left at 1 HP
  xmod.player.kill(victim)
  xmod.player.set_frags(victim, vfr)
  if is_player(attacker) and attacker ~= victim and xmod.player.connected(attacker) then
    xmod.player.set_frags(attacker, xmod.player.frags(attacker) + 1)
    pcall(function()
      xmod.msg.begin(xmod.msg.ALL, "DeathMsg"); xmod.msg.byte(attacker); xmod.msg.byte(victim); xmod.msg.byte(0); xmod.msg.string(weapon or "world"); xmod.msg.finish()
    end)
  end
  on_kill(attacker, victim, false, weapon)
  return amount
end

-- =====================================================================================================================
-- damage hooks
-- =====================================================================================================================
local DMG_GRENADE = 16777216                       -- 1<<24
local last_dmg = {}
xmod.on("ham:takedamage:player", function(victim, inflictor, attacker, dmg, bits)
  if not enabled() or not is_player(attacker) or attacker == victim or not xmod.player.connected(attacker) or not enemies(attacker, victim) then return end
  local vs, as = st(victim), st(attacker)
  if now() < vs.voodoo_until then return xmod.HC_SUPERCEDE, 0 end
  local vr, ar = race_of(victim), race_of(attacker)
  -- victim: evasion
  if vr and vr.key == "nightelf" and sk(victim, 1) > 0 and chance(vr.skills[1].vals[sk(victim, 1)] / 100) then
    if N.has(victim) then N.float_text(victim, pos(victim, 40), "evaded", { color = "#3CE0A0", time = 0.8 }) end
    if N.has(attacker) then N.float_text(attacker, pos(victim, 40), "miss", { color = "gray", time = 0.8 }) end
    return xmod.HC_SUPERCEDE, 0
  end
  local d, crit = dmg, false
  -- attacker bonuses
  if ar then
    if ar.key == "orc" then
      if sk(attacker, 1) > 0 and chance(0.12) then d = d * ar.skills[1].mult[sk(attacker, 1)]; crit = true end
      if sk(attacker, 2) > 0 and (bits & DMG_GRENADE) ~= 0 then d = d * ar.skills[2].mult[sk(attacker, 2)]; crit = true end
    elseif ar.key == "nightelf" and sk(attacker, 3) > 0 then d = d * (1 + ar.skills[3].vals[sk(attacker, 3)] / 100)
    elseif ar.key == "cryptlord" and sk(attacker, 3) > 0 and chance(ar.skills[3].vals[sk(attacker, 3)] / 100) then d = d + 12; crit = true end
  end
  if has_item(attacker, "claws") then d = d + 6 end
  d = d * (1 + at(attacker, 2) / 100)
  -- victim reductions
  if vr then
    if vr.key == "bloodmage" and sk(victim, 3) > 0 then d = d * (1 - vr.skills[3].vals[sk(victim, 3)] / 100) end
  end
  last_dmg[victim] = { d = d, crit = crit, attacker = attacker }
  if d ~= dmg then xmod.hook.set_arg(3, d) end
end)

xmod.on("ham:takedamage:player:post", function(victim, inflictor, attacker, dmg, bits)
  local info = last_dmg[victim]; last_dmg[victim] = nil
  if not info or info.attacker ~= attacker or not xmod.player.connected(attacker) then return end
  local d = info.d
  local vr, ar = race_of(victim), race_of(attacker)
  float_dmg(attacker, victim, d, info.crit and "#FF3030" or "yellow")
  if info.crit and N.has(attacker) then N.punch(attacker, -1.2, 0) end
  -- attacker on-hit effects
  local heal = 0
  if ar then
    local k = ar.key
    if k == "undead" and sk(attacker, 1) > 0 then heal = heal + d * ar.skills[1].vals[sk(attacker, 1)] / 100 end
    if alive(victim) then
      if k == "human" and sk(attacker, 3) > 0 and chance(ar.skills[3].vals[sk(attacker, 3)] / 100) then
        st(victim).stun_until = now() + 0.6; apply_speed(victim)
        xmod.timer.after(0.65, function(v) apply_speed(v) end, victim)
        if N.has(victim) then N.shake(victim, 2, 15, 0.5) end
        if N.has(attacker) then N.float_text(attacker, pos(victim, 50), "BASH", { color = "#5AA0FF", time = 0.8 }) end
      elseif k == "bloodmage" then
        if sk(attacker, 1) > 0 and chance(ar.skills[1].vals[sk(attacker, 1)] / 100) then
          xmod.msg.fade(victim, 1.0, 0.8, 0, 0, 0, 0, 255)
          local vx, vy = xmod.player.velocity(victim); xmod.player.set_velocity(victim, vx, vy, 320)
          N.effect(0, "implosion", { pos = pos(victim), size = 30, count = 40, life = 0.8 })
          if N.has(attacker) then N.float_text(attacker, pos(victim, 50), "BANISH", { color = "#FF2A55", time = 0.9 }) end
        end
        if sk(attacker, 2) > 0 then
          local steal = math.min(xmod.player.money(victim), math.floor(d * ar.skills[2].vals[sk(attacker, 2)]))
          if steal > 0 then xmod.player.set_money(victim, xmod.player.money(victim) - steal); xmod.player.set_money(attacker, math.min(16000, xmod.player.money(attacker) + steal)) end
        end
      elseif k == "shadow" and sk(attacker, 2) > 0 and chance(ar.skills[2].vals[sk(attacker, 2)] / 100) then
        local vs = st(victim); vs.slow_until = now() + 3; vs.slow_mult = 0.5; apply_speed(victim)
        xmod.timer.after(3.05, function(v) apply_speed(v) end, victim)
        N.glow(0, victim, "#4CFF5A", 25); xmod.timer.after(3, function(v) N.glow(0, v) end, victim)
        if N.has(attacker) then N.float_text(attacker, pos(victim, 50), "HEX", { color = "#4CFF5A", time = 0.9 }) end
      elseif k == "warden" then
        if sk(attacker, 1) > 0 and chance(ar.skills[1].vals[sk(attacker, 1)] / 100) then
          local vx, vy, vz = xmod.player.origin(victim)
          for _, e in ipairs(xmod.player.all()) do
            if e ~= victim and alive(e) and enemies(attacker, e) and dist(e, victim) < 220 then war_damage(attacker, e, 12, "knife"); lightning(victim, e, { 200, 160, 255, 255 }) end
          end
          N.effect(0, "sparks", { pos = { math.floor(vx), math.floor(vy), math.floor(vz) } })
        end
        if sk(attacker, 3) > 0 and chance(ar.skills[3].vals[sk(attacker, 3)] / 100) then
          st(victim).poison = { attacker = attacker, ticks = 5 }
          N.glow(0, victim, "#60FF60", 15); xmod.timer.after(5, function(v) N.glow(0, v) end, victim)
          if N.has(attacker) then N.float_text(attacker, pos(victim, 50), "POISON", { color = "#60FF60", time = 0.9 }) end
        end
      elseif k == "cryptlord" and sk(attacker, 1) > 0 and chance(ar.skills[1].vals[sk(attacker, 1)] / 100) then
        local vx, vy = xmod.player.velocity(victim); xmod.player.set_velocity(victim, vx * 0.3, vy * 0.3, 520)
        N.effect(0, "sparks", { pos = pos(victim, -30) })
        if N.has(attacker) then N.float_text(attacker, pos(victim, 50), "IMPALE", { color = "#E0C060", time = 0.9 }) end
      end
    end
  end
  if has_item(attacker, "mask") then heal = heal + d * 0.10 end
  if heal > 0 and alive(attacker) then
    local hp = xmod.player.health(attacker); local cap = math.max(max_hp(attacker), 100) + 50
    if hp < cap then xmod.player.set_health(attacker, math.min(cap, hp + math.floor(heal + 0.5))) end
  end
  if has_item(attacker, "frost") and alive(victim) then
    local vs = st(victim); vs.slow_until = now() + 1; vs.slow_mult = math.min(vs.slow_mult < 1 and vs.slow_mult or 1, 0.6); apply_speed(victim)
    xmod.timer.after(1.05, function(v) local s2 = P[v]; if s2 and now() >= s2.slow_until then s2.slow_mult = 1 end; apply_speed(v) end, victim)
    if N.has(victim) then N.tint(victim, { 120, 190, 255, 50 }, 0.1, 0.8) end
  end
  -- victim reflections
  if vr and alive(attacker) then
    local refl = 0
    if vr.key == "nightelf" and sk(victim, 2) > 0 then refl = vr.skills[2].vals[sk(victim, 2)] / 100 end
    if vr.key == "cryptlord" and sk(victim, 2) > 0 then refl = 0.10 end
    if refl > 0 then war_damage(victim, attacker, d * refl, "thorns") end
  end
end)

-- =====================================================================================================================
-- deaths, XP for kills and round wins
-- =====================================================================================================================
xmod.on("message:DeathMsg", function(_, _, _, a)
  local killer, victim, hs, weapon = a[1], a[2], a[3] == 1, a[4]
  local exp = expect_death[victim]
  if exp and killer == victim then return true end              -- our own kill: we send a DeathMsg with the real killer
  if exp and killer == exp.attacker then expect_death[victim] = nil; return end
  on_kill(killer, victim, hs, weapon)
end)
xmod.on("message:CurWeapon", function(_, _, ent, a) if a[1] == 1 and is_player(ent) then CW[ent] = a[2] end end)
xmod.on("ham:killed:player:post", function(victim)
  local s = P[victim]; if not s then return end
  s.items = {}; s.poison = nil; s.voodoo_until = 0; s.stun_until = 0; s.slow_until = 0
  if N.has(victim) then N.input(victim) end
  apply_visibility(victim)
  xmod.timer.after(0.5, function(v) expect_death[v] = nil end, victim)
  hud_update(victim)
end)
-- new round (HLTV 0 0 is sent at every round start, also during freeze time)
local round_no = 0
xmod.on("message:HLTV", function(_, _, _, a)
  if a[1] ~= 0 or a[2] ~= 0 then return end
  round_no = round_no + 1; first_blood = false
  for _, s in pairs(P) do s.streak = 0 end
  if not enabled() then return end
  N.set(0, "war3.round", { type = "text", anchor = "top", py = 118, text = "^9ROUND " .. round_no .. "\n^8prepare for battle", markup = true, scale = 1.9,
    outline = true, life = 3.5, fadein = 0.3, fadeout = 1.2, layer = 70 })
  N.sound(0, SND.thunder, 0.3)
end)
local WIN_T = { ["#Terrorists_Win"] = true, ["#Target_Bombed"] = true, ["#Hostages_Not_Rescued"] = true, ["#VIP_Assassinated"] = true }
local WIN_CT = { ["#CTs_Win"] = true, ["#Bomb_Defused"] = true, ["#Target_Saved"] = true, ["#All_Hostages_Rescued"] = true, ["#VIP_Escaped"] = true }
xmod.on("message:TextMsg", function(_, _, _, a)
  local m = a[2]; if type(m) ~= "string" then return end
  local team = WIN_T[m] and 1 or (WIN_CT[m] and 2 or nil); if not team or not enabled() then return end
  xmod.timer.after(0.1, function()
    for _, id in ipairs(xmod.player.all()) do
      local t = xmod.player.team(id)
      if t == team then give_xp(id, alive(id) and 35 or 20, "round won") end
      if N.has(id) and (t == 1 or t == 2) then
        local win = t == team
        N.set(id, N.temp_id(), { type = "text", anchor = "top", py = 150, text = win and "VICTORY" or "DEFEAT", color = win and "#FFD24A" or "#FF3A3A", scale = 3.2,
          outline = true, shadow = true, life = 3.2, fadein = 0.25, fadeout = 1.2, layer = 70 })
        if win then N.tint(id, { 255, 210, 80, 35 }, 0.3, 1.2) else N.tint(id, { 40, 0, 0, 70 }, 0.3, 1.2) end
        N.sound(id, win and SND.bell or SND.suck, 0.6)
      end
    end
    for _, id in ipairs(xmod.player.all()) do save_player(id) end
    SAVE:save()
  end)
end)

-- =====================================================================================================================
-- spawning
-- =====================================================================================================================
xmod.on("ham:spawn:player:post", function(id)
  if not alive(id) then return end
  local s = st(id)
  if s.next_race then
    local r = s.next_race; s.next_race = nil
    if s.race > 0 then save_player(id) end
    take_race(id, r)
    if xmod.player.is_bot(id) then auto_skills(id) end
    announce(id, "^7You are now ^9" .. RACES[r].name .. " ^7(level ^3" .. s.level .. "^7)")
    save_player(id)
  end
  s.spawn = pos(id); s.stun_until = 0; s.slow_until = 0; s.slow_mult = 1; s.voodoo_until = 0; s.poison = nil
  if N.has(id) then N.tint(id); N.fov(id, 0, 0.3) end
  local r0 = race_of(id)
  if r0 then                                                      -- summoned: a short flash in the race colour
    local c = N.color(r0.color)
    N.effect(0, "teleport", { pos = s.spawn })
    N.light(0, fxname(), { ent = id, radius = 150, color = { c[1], c[2], c[3] }, life = 0.7 })
    ring(0, { s.spawn[1], s.spawn[2], s.spawn[3] - 34 }, 34, { c[1], c[2], c[3], 220 }, 0.8)
  end
  s.ult_ready_at = math.max(s.ult_ready_at, now() + 4)          -- no ultimate in the first seconds of a round
  xmod.timer.after(0.15, function(pid)
    if not alive(pid) then return end
    local hp = max_hp(pid); if hp > 100 then xmod.player.set_health(pid, hp); xmod.ent.set(pid, "max_health", hp) end
    local r = race_of(pid)
    if r and r.key == "cryptlord" and sk(pid, 2) > 0 then xmod.player.set_armor(pid, math.max(xmod.player.armor(pid), r.skills[2].vals[sk(pid, 2)])) end
    apply_all(pid); hud_update(pid, true); if auras_refresh then auras_refresh() end
    if P[pid].skills[4] > 0 then ult_cooldown_visual(pid, math.max(0, P[pid].ult_ready_at - now())) end
    if P[pid].race == 0 and not xmod.player.is_bot(pid) then xmod.timer.after(1, function(q) if xmod.player.connected(q) and P[q] and P[q].race == 0 then menu_race(q) end end, pid) end
    if points_free(pid) > 0 and not xmod.player.is_bot(pid) then xmod.msg.chat(pid, TAG .. "You have {g}" .. points_free(pid) .. "{y} skill point(s) - type {t}/skills") end
  end, id)
end)
local ok_speed = pcall(xmod.on, "ham:cstrike_player_resetmaxspeed:player:post", function(id)
  local s = P[id]; if not s then return end
  s.base_speed = xmod.ent.get(id, "maxspeed"); apply_speed(id)
end)
if not ok_speed then xmod.log("war3: speed hook not available - speed skills disabled") end

-- =====================================================================================================================
-- once per second: regeneration, healing wave, serpent ward, poison, ult ready, HUD
-- =====================================================================================================================
local tick = 0
local next_storm = 60
xmod.timer.every(1.0, function()
  if not enabled() then return end
  tick = tick + 1
  for _, id in ipairs(xmod.player.all()) do
    local s = P[id]
    if s and alive(id) then
      local r = race_of(id)
      local regen = 0
      if r and r.key == "orc" and sk(id, 3) > 0 then regen = regen + r.skills[3].vals[sk(id, 3)] end
      if has_item(id, "ring") then regen = regen + 2 end
      if regen > 0 then local hp, m = xmod.player.health(id), max_hp(id); if hp < m then xmod.player.set_health(id, math.min(m, hp + regen)) end end
      if N.has(id) and tick % 2 == 0 and xmod.player.health(id) <= 25 then N.tint(id, { 170, 0, 0, 55 }, 0.15, 0.25) end     -- low HP pulse
      if r and r.key == "shadow" then
        if sk(id, 1) > 0 and tick % 2 == 0 then
          for _, t in ipairs(xmod.player.all()) do
            if t ~= id and alive(t) and xmod.player.team(t) == xmod.player.team(id) and dist(id, t) < 300 then
              local hp, m = xmod.player.health(t), max_hp(t)
              if hp < m then
                xmod.player.set_health(t, math.min(m, hp + r.skills[1].vals[sk(id, 1)]))
                if N.has(t) then N.float_text(t, pos(t, 40), "+" .. r.skills[1].vals[sk(id, 1)], { color = "#4CFF5A", time = 0.8 }); N.sound(t, SND.heal, 0.4) end
                ring(0, pos(t, -34), 30, { 76, 255, 90, 200 }, 0.6); N.effect(0, "beam", { pos = pos(id), to = pos(t), color = { 76, 255, 90 }, life = 0.3 })
              end
            end
          end
        end
        if sk(id, 3) > 0 then
          local hit = false
          for _, e in ipairs(xmod.player.all()) do
            if alive(e) and enemies(id, e) and dist(id, e) < 250 and visible(id, e) then war_damage(id, e, r.skills[3].vals[sk(id, 3)], "serpent_ward"); hit = true end
          end
          if hit and N.has(id) then N.shape(id, "war3.ward", { type = "circle", ent = id, center = { 0, 0, -34 }, radius = 250, color = { 76, 255, 90, 120 }, width = 2, life = 1.1 }) end
        end
      end
      if s.poison and s.poison.ticks > 0 then
        s.poison.ticks = s.poison.ticks - 1
        if xmod.player.connected(s.poison.attacker) then war_damage(s.poison.attacker, id, 4, "poison") end
        if s.poison and s.poison.ticks <= 0 then s.poison = nil end
      end
      if xmod.player.is_bot(id) and r and s.skills[4] > 0 and now() >= s.ult_ready_at and chance(0.25) then   -- bots use their ultimate now and then
        local k = r.key; local near = false
        for _, e in ipairs(xmod.player.all()) do if alive(e) and enemies(id, e) and dist(id, e) < (k == "undead" and 220 or 800) and visible(id, e) then near = true; break end end
        if (near and k ~= "human" and k ~= "warden" and k ~= "bloodmage") or (k == "warden" and xmod.player.health(id) < 35) then use_ultimate(id) end
      end
      if s.skills[4] > 0 and s.ult_ready_at > 0 and now() >= s.ult_ready_at and not s.ult_announced then
        s.ult_announced = true
        if N.has(id) then
          N.set(id, "war3.ult", { color = "#6FFF8F" }); N.sound(id, SND.ready, 0.5); hud_update(id)
          ring(id, pos(id, -34), 44, { 111, 255, 143, 230 }, 0.6, { width = 4 })
          N.toast(id, "^2" .. (r and r.ult.name or "Ultimate") .. " ^7is ready ^8[" .. ult_key() .. "]", { markup = true, time = 1.4, y = 0.72, scale = 1.0, box = false })
        end
      end
    end
    if s and tick % 2 == 0 and not N.has(id) then hud_update(id) end
  end
  feed_expire()
  -- storm ambience: now and then lightning flashes over the map, then the thunder
  if xmod.cvar.int("war3_ambience") ~= 0 and now() >= next_storm then
    next_storm = now() + math.random(70, 140)
    N.tint(0, { 215, 230, 255, 70 }, 0.04, 0.06)
    xmod.timer.after(0.22, function() N.tint(0, { 215, 230, 255, 45 }, 0.04, 0.05) end)
    xmod.timer.after(1.3, function() N.sound(0, SND.thunder, 0.45) end)
  end
end)

-- =====================================================================================================================
-- ultimates
-- =====================================================================================================================
local function aimed_enemy(id, range)
  local tr = aim_trace(id, range)
  if is_player(tr.ent) and alive(tr.ent) and enemies(id, tr.ent) then return tr.ent end
  -- forgiving aim: the enemy closest to the crosshair within 10 degrees
  local best, bd = nil, math.cos(math.rad(10))
  local x, y, z = eye(id); local dx, dy, dz = aim_dir(id)
  for _, e in ipairs(xmod.player.all()) do
    if alive(e) and enemies(id, e) and dist(id, e) < range then
      local ex, ey, ez = xmod.player.origin(e); local vx, vy, vz = ex - x, ey - y, ez - z; local l = math.sqrt(vx * vx + vy * vy + vz * vz)
      local c = (vx * dx + vy * dy + vz * dz) / (l > 0 and l or 1)
      if c > bd and visible(id, e) then best, bd = e, c end
    end
  end
  return best
end
local ULT = {}
ULT.undead = function(id)
  announce(id, "^1Suicide Bomber armed!")
  N.light(0, fxname(), { ent = id, radius = 200, color = { 255, 60, 40 }, life = 1.5 })
  N.shape(0, fxname(), { type = "circle", ent = id, center = { 0, 0, -34 }, radius = 300, color = { 255, 60, 40, 220 }, width = 3, pulse = true, life = 1.5, xray = true })
  xmod.timer.after(1.5, function(pid)
    if not alive(pid) then return end
    local x, y, z = xmod.player.origin(pid)
    N.effect(0, "explosion", { pos = { math.floor(x), math.floor(y), math.floor(z) } })
    xmod.sound.emit(pid, xmod.sound.CHAN_AUTO, "weapons/explode3.wav", 1, 0.5)
    for _, e in ipairs(xmod.player.all()) do
      if alive(e) and enemies(pid, e) then
        local dd = dist(pid, e)
        if dd < 300 and not ult_immune(pid, e) then war_damage(pid, e, 120 * (1 - dd / 300) + 15, "suicide_bomber"); if N.has(e) then N.shake(e, 5, 15, 1) end end
      end
    end
    war_damage(pid, pid, 999, "suicide_bomber")
  end, id)
  return true
end
ULT.human = function(id)
  local tr = aim_trace(id, 1200)
  local x, y, z = eye(id)
  local tx, ty, tz = tr.x - (tr.x - x) * 0.04 + tr.nx * 36, tr.y - (tr.y - y) * 0.04 + tr.ny * 36, tr.z + tr.nz * 36 + 4
  local chk = xmod.trace.hull(tx, ty, tz, tx, ty, tz + 1, 1, 0, id)
  if chk.startsolid or chk.allsolid then announce(id, "^1Cannot teleport there"); return false end
  local ox, oy, oz = xmod.player.origin(id)
  N.effect(0, "teleport", { pos = { math.floor(ox), math.floor(oy), math.floor(oz) } })
  xmod.player.set_origin(id, tx, ty, tz)
  N.effect(0, "teleport", { pos = { math.floor(tx), math.floor(ty), math.floor(tz) } })
  if N.has(id) then N.fov(id, 120, 0); N.fov(id, 0, 0.5) end
  return true
end
ULT.orc = function(id)
  local target = aimed_enemy(id, 900)
  if not target then announce(id, "^1No enemy in sight"); return false end
  local hit, cur, dmgs = { [target] = true }, id, { 60, 40, 25 }
  local chain = { target }
  for i = 2, 3 do
    local best, bd = nil, 500
    for _, e in ipairs(xmod.player.all()) do if not hit[e] and alive(e) and enemies(id, e) and dist(chain[#chain], e) < bd and visible(chain[#chain], e) then best, bd = e, dist(chain[#chain], e) end end
    if not best then break end
    hit[best] = true; chain[#chain + 1] = best
  end
  for i, e in ipairs(chain) do
    lightning(cur, e)
    if not ult_immune(id, e) then war_damage(id, e, dmgs[i], "chain_lightning"); if N.has(e) then N.shake(e, 3, 18, 0.5); N.tint(e, { 160, 200, 255, 60 }, 0.05, 0.3) end
    elseif N.has(id) then N.float_text(id, pos(e, 40), "immune", { color = "gray" }) end
    cur = e
  end
  N.sound3d(0, "ambience/thunder_clap.wav", { ent = id, volume = 0.9 })
  return true
end
ULT.nightelf = function(id)
  local t = aimed_enemy(id, 1000)
  if not t then announce(id, "^1No enemy in sight"); return false end
  if ult_immune(id, t) then announce(id, "^8" .. xmod.player.name(t) .. " is immune"); return true end
  local s = st(t); s.stun_until = now() + 3; apply_speed(t)
  xmod.timer.after(3.05, function(v) apply_speed(v) end, t)
  if N.has(t) then N.freeze(t, true); xmod.timer.after(3, function(v) if N.has(v) then N.freeze(v, false) end end, t); N.toast(t, "^2You are entangled!", { markup = true, time = 2 }) end
  N.shape(0, fxname(), { type = "cylinder", ent = t, center = { 0, 0, -36 }, radius = 26, height = 60, color = { 60, 220, 90, 220 }, width = 3, fill = true, life = 3 })
  lightning(id, t, { 80, 255, 120, 255 })
  return true
end
ULT.bloodmage = function(id)
  local tr = aim_trace(id, 1000)
  if tr.fraction >= 1 then announce(id, "^1Aim at the ground"); return false end
  local c = { math.floor(tr.x), math.floor(tr.y), math.floor(tr.z + 4) }
  N.shape(0, fxname(), { type = "cylinder", center = c, radius = 160, height = 70, color = { 255, 90, 30, 200 }, fill = true, additive = true, pulse = true, width = 3, life = 3 })
  N.light(0, fxname(), { pos = { c[1], c[2], c[3] + 40 }, radius = 300, color = { 255, 110, 30 }, life = 3 })
  N.effect(0, "lava", { pos = c })
  xmod.timer.times(3, 1.0, function(pid)
    if not xmod.player.connected(pid) then return false end
    for _, e in ipairs(xmod.player.all()) do
      if alive(e) and enemies(pid, e) then
        local ex, ey, ez = xmod.player.origin(e)
        if math.sqrt((ex - c[1]) ^ 2 + (ey - c[2]) ^ 2) < 160 and math.abs(ez - c[3]) < 100 and not ult_immune(pid, e) then war_damage(pid, e, 15, "immolate"); if N.has(e) then N.tint(e, { 255, 90, 20, 70 }, 0.05, 0.4) end end
      end
    end
  end, id)
  return true
end
ULT.shadow = function(id)
  local s = st(id); s.voodoo_until = now() + 3
  N.glow(0, id, "#FFD040", 35); xmod.timer.after(3, function(pid) N.glow(0, pid) end, id)
  N.light(0, fxname(), { ent = id, radius = 220, color = { 255, 210, 60 }, life = 3 })
  if N.has(id) then N.tint(id, { 255, 210, 60, 40 }, 0.2, 2.8) end
  announce(id, "^3Big Bad Voodoo: invulnerable for 3 seconds")
  return true
end
ULT.warden = function(id)
  local s = st(id); if not s.spawn then return false end
  local ox, oy, oz = xmod.player.origin(id)
  N.effect(0, "implosion", { pos = { math.floor(ox), math.floor(oy), math.floor(oz) }, size = 40, count = 80, life = 0.6 })
  xmod.player.set_origin(id, s.spawn[1], s.spawn[2], s.spawn[3] + 2)
  xmod.player.set_health(id, math.max(xmod.player.health(id), max_hp(id)))
  N.effect(0, "teleport", { pos = s.spawn })
  announce(id, "^5Vengeance: healed and back at your spawn")
  return true
end
ULT.cryptlord = function(id)
  local list = {}
  for _, e in ipairs(xmod.player.all()) do if alive(e) and enemies(id, e) then list[#list + 1] = e end end
  if #list == 0 then announce(id, "^1No enemy alive"); return false end
  local t = list[math.random(#list)]
  if ult_immune(id, t) then announce(id, "^8" .. xmod.player.name(t) .. " is immune to the swarm"); return true end
  lightning(id, t, { 230, 200, 90, 255 })
  N.effect(0, "burst", { pos = pos(t), size = 24, color = { 120, 0, 0 }, life = 1 })
  war_damage(id, t, math.random(35, 55), "locust_swarm")
  if N.has(t) then N.shake(t, 2, 20, 1) end
  announce(id, "^9Locust Swarm hit ^7" .. xmod.player.name(t))
  return true
end
function use_ultimate(id)
  if not enabled() or not alive(id) then return end
  local s = st(id); local r = RACES[s.race]
  if not r then return xmod.msg.chat(id, TAG .. "Pick a race first: {t}/changerace") end
  if s.skills[4] == 0 then return xmod.msg.chat(id, TAG .. "You have not learned your ultimate (from level " .. ult_level() .. ", {t}/skills{y})") end
  local left = s.ult_ready_at - now()
  if left > 0 then
    if N.has(id) then N.float_text(id, pos(id, 50), string.format("%.0fs", math.ceil(left)), { color = "gray", time = 0.7 }) else xmod.msg.print(id, "center", string.format("Ultimate ready in %d s", math.ceil(left))) end
    return
  end
  if ULT[r.key](id) then
    N.float_text(0, pos(id, 62), r.ult.name, { color = r.color, scale = 1.35, time = 1.4, rise = 50 })
    local cd = math.max(5, xmod.cvar.num("war3_ult_cooldown") * (1 - math.min(0.6, at(id, 3) * 0.02)))
    s.ult_ready_at = now() + cd; s.ult_announced = false
    ult_cooldown_visual(id, cd)
  end
end
xmod.cmd.client("ultimate", "", function(id) use_ultimate(id) end, "use your War3 ultimate")
xmod.cmd.client("war3_ultimate", "", function(id) use_ultimate(id) end, "use your War3 ultimate")

-- =====================================================================================================================
-- menus (Nexus window with mouse for Nexus players, classic menus for everyone else - N.menu does both)
-- =====================================================================================================================
local function race_label(id, r)
  local s = st(id); local saved = s.saved[r.key]; local lv = level_for(saved and tonumber(saved.xp) or 0)
  if s.race == r.id then lv = s.level end
  return r.name .. "  (lv " .. lv .. ")" .. (s.race == r.id and "  *" or "")
end
function menu_race(id)
  local items = {}
  for _, r in ipairs(RACES) do items[#items + 1] = { race_label(id, r), r.id } end
  N.menu(id, { title = "Choose your race", items = items, cursor = true, y = 0.09, width = 340,
    on_select = function(pid, _, rid)
      local s = st(pid); local r = RACES[rid]
      if s.race == rid then xmod.msg.chat(pid, TAG .. "You already are {t}" .. r.name); return end
      if s.race == 0 or not alive(pid) then
        s.next_race = rid
        if alive(pid) then
          -- first race of the session while alive: take it now
          s.next_race = nil
          take_race(pid, rid)
          apply_all(pid); hud_update(pid, true); save_player(pid)
          announce(pid, "^7You are now ^9" .. r.name .. " ^7(level ^3" .. s.level .. "^7)")
          if points_free(pid) > 0 then xmod.timer.after(0.3, function(q) if xmod.player.connected(q) then menu_skills(q) end end, pid) end
        else
          xmod.msg.chat(pid, TAG .. "You will be {t}" .. r.name .. "{y} when you spawn.")
        end
      else
        s.next_race = rid
        xmod.msg.chat(pid, TAG .. "You will become {t}" .. r.name .. "{y} next round.")
      end
      show_raceinfo(pid, rid)
    end })
end

function menu_skills(id)
  local s = st(id); local r = RACES[s.race]
  if not r then return menu_race(id) end
  local free = points_free(id); local amax = attr_max()
  local items = {}
  for i = 1, 3 do
    local lvl = s.skills[i]
    items[#items + 1] = { r.skills[i].name .. " [" .. lvl .. "/3]" .. (lvl >= 3 and " (max)" or ""), i, disabled = not (free > 0 and lvl < 3) }
  end
  local ucan = free > 0 and s.skills[4] == 0 and s.level >= ult_level()
  items[#items + 1] = { "ULTIMATE: " .. r.ult.name .. (s.skills[4] > 0 and " (learned)" or (s.level < ult_level() and " (level " .. ult_level() .. ")" or "")), 4, disabled = not ucan }
  for i, a in ipairs(ATTRS) do
    items[#items + 1] = { a.name .. " [" .. s.attr[i] .. "/" .. amax .. "]", 4 + i, disabled = not (free > 0 and s.attr[i] < amax) }
  end
  N.menu(id, { title = r.name .. " - skills (" .. free .. " point" .. (free == 1 and "" or "s") .. " free)", items = items, cursor = true, y = 0.09, width = 420,
    on_select = function(pid, _, n)
      local st2 = st(pid)
      if points_free(pid) <= 0 then return end
      if n >= 5 then
        if st2.attr[n - 4] >= attr_max() then return end
        st2.attr[n - 4] = st2.attr[n - 4] + 1
        if n == 5 and alive(pid) then xmod.player.set_health(pid, xmod.player.health(pid) + 4) end
      elseif n == 4 then
        if st2.level < ult_level() or st2.skills[4] > 0 then return end
        st2.skills[4] = 1; st2.ult_ready_at = math.max(st2.ult_ready_at, now() + 5); ult_cooldown_visual(pid, 5)
      else
        if st2.skills[n] >= 3 then return end
        st2.skills[n] = st2.skills[n] + 1
      end
      apply_all(pid); hud_update(pid, true); save_player(pid)
      if N.has(pid) then N.sound(pid, SND.pick, 0.6); if alive(pid) then ring(pid, pos(pid, -34), 36, N.color(st2.race > 0 and RACES[st2.race].color or "#FFFFFF", 230), 0.5) end end
      if points_free(pid) > 0 then menu_skills(pid); return true end   -- re-opened with the new state (true = the library must not close it)
    end })
end

function menu_shop(id)
  local s = st(id); local maxi = xmod.cvar.int("war3_max_items")
  local items = {}
  for _, it in ipairs(ITEMS) do
    local owned = has_item(id, it.key)
    local afford = xmod.player.money(id) >= it.price
    local full = #s.items >= maxi and it.key ~= "tome"
    items[#items + 1] = { it.name .. "  $" .. it.price .. (owned and "  (owned)" or ""), it.key, disabled = owned or not afford or full or not alive(id) }
  end
  N.menu(id, { title = "Item shop  ($" .. xmod.player.money(id) .. ", " .. #s.items .. "/" .. maxi .. " items)", items = items, cursor = true, y = 0.09, width = 400,
    on_select = function(pid, _, key)
      local it = ITEM_BY_KEY[key]; local s2 = st(pid)
      if not alive(pid) then return end
      if xmod.player.money(pid) < it.price then return xmod.msg.chat(pid, TAG .. "Not enough money") end
      if key ~= "tome" and (#s2.items >= xmod.cvar.int("war3_max_items") or has_item(pid, key)) then return xmod.msg.chat(pid, TAG .. "No free item slot") end
      xmod.player.set_money(pid, xmod.player.money(pid) - it.price)
      if key == "tome" then give_xp(pid, 150, "tome")
      else
        s2.items[#s2.items + 1] = key
        if key == "periapt" then xmod.player.set_health(pid, xmod.player.health(pid) + 50) end
        apply_all(pid)
      end
      if N.has(pid) then N.sound(pid, "items/gunpickup2.wav", 0.7); N.toast(pid, "^2Bought ^7" .. it.name .. " ^8- " .. it.desc, { markup = true, time = 2, y = 0.18 }) else xmod.msg.chat(pid, TAG .. "Bought {t}" .. it.name) end
      hud_update(pid)
    end })
end

function show_raceinfo(id, rid)
  local r = RACES[rid or st(id).race]
  if not r then return end
  local lines = { "^9" .. r.name }
  for i = 1, 3 do local sv = r.skills[i]; lines[#lines + 1] = "^3" .. sv.name .. "^7: " .. string.format(sv.desc, sv.vals[1]) .. " ^8(lv1) .. " .. string.format(sv.desc, sv.vals[3]) .. " (lv3)" end
  lines[#lines + 1] = "^1Ultimate - " .. r.ult.name .. "^7: " .. r.ult.desc .. " ^8(from level " .. ult_level() .. ")"
  local al = {}; for _, a in ipairs(ATTRS) do al[#al + 1] = a.mk .. a.name .. "^8 " .. a.desc end
  lines[#lines + 1] = table.concat(al, "^8  ·  ")
  if N.has(id) then
    N.set(id, "war3.info", { type = "text", anchor = "top", py = 90, pw = 620, text = table.concat(lines, "\n"), markup = true, scale = 0.9, color = { 230, 230, 230, 255 },
      color2 = { 8, 10, 16, 215 }, shadow = true, layer = 60, life = 9, fadein = 0.2, fadeout = 0.8 })
  else
    for _, l in ipairs(lines) do xmod.msg.print(id, "console", (l:gsub("%^%d", ""))) end
    xmod.msg.chat(id, TAG .. "{t}" .. r.name .. "{y}: details printed in your console")
  end
end

local function show_level(id)
  local s = st(id); local r = RACES[s.race]
  if not r then return xmod.msg.chat(id, TAG .. "You have no race yet: {t}/changerace") end
  xmod.msg.chat(id, TAG .. "{t}" .. r.name .. "{y} level {g}" .. s.level .. "/" .. max_level() .. "{y}, " .. s.xp .. (s.level < max_level() and (" / " .. xp_need(s.level + 1)) or "") .. " XP, " .. points_free(id) .. " free point(s)")
end
local function reset_skills(id)
  local s = st(id); if s.race == 0 then return end
  s.skills = { 0, 0, 0, 0 }; s.attr = { 0, 0, 0 }; apply_all(id); hud_update(id, true); save_player(id)
  xmod.msg.chat(id, TAG .. "Your skills and attributes were reset. You have {g}" .. points_free(id) .. "{y} points: {t}/skills")
  menu_skills(id)
end
local function player_skills(id)
  for _, p in ipairs(xmod.player.all()) do
    local s = P[p]; local r = s and RACES[s.race]
    xmod.msg.print(id, "console", string.format("%-24s %-16s lv %2d  %s", xmod.player.name(p), r and r.name or "-", s and s.level or 0, s and #s.items > 0 and ("items: " .. table.concat(s.items, ",")) or ""))
  end
  xmod.msg.chat(id, TAG .. "Player races printed in your console")
end
local function war3_help(id)
  local lines = {
    "^9Warcraft 3 on xmod",
    "^7Kill enemies (^3+20 XP^7, more for higher levels, headshots, streaks, first blood) and win rounds to level up (max ^3" .. max_level() .. "^7).",
    "^7Every level gives a ^3point^7: 3 skills per race (3 levels each), an ^1ultimate^7 from level " .. ult_level() .. ", and ^1STR ^2AGI ^5INT^7 attributes.",
    "^7Kill 3+ in a row for a ^9streak aura^7, 5+ and you burn. Ending a streak pays ^3+25 XP^7.",
    "^7Your progress is saved for every race separately.",
    "^3/war3menu^7 main menu   ^3/changerace^7   ^3/skills^7   ^3/shopmenu^7   ^3/resetskills^7   ^3/level^7   ^3/raceinfo",
    "^7Ultimate: console ^3ultimate^7 (bind c ultimate)" .. "   Nexus players: key ^3" .. xmod.cvar.get("war3_ult_key"):upper(),
  }
  if N.has(id) then
    N.set(id, "war3.info", { type = "text", anchor = "top", py = 90, pw = 640, text = table.concat(lines, "\n"), markup = true, scale = 0.9, color2 = { 8, 10, 16, 215 }, shadow = true, layer = 60, life = 12, fadein = 0.2, fadeout = 0.8 })
  else for _, l in ipairs(lines) do xmod.msg.chat(id, TAG .. (l:gsub("%^%d", ""))) end end
end

-- the 10 best saved players (sum of the levels of all their races)
function war3_top(id)
  local list = {}
  SAVE:each(function(_, v)
    if type(v) == "table" and type(v.races) == "table" then
      local lv, best, bl = 0, "", -1
      for k, rs in pairs(v.races) do local l = level_for(tonumber(rs.xp) or 0); lv = lv + l; if l > bl and RACE_BY_KEY[k] then best, bl = RACES[RACE_BY_KEY[k]].name, l end end
      list[#list + 1] = { name = tostring(v.name or "?"), lv = lv, best = best, bl = bl }
    end
  end, "p:")
  table.sort(list, function(x, y) return x.lv > y.lv end)
  local lines = { "^9Warcraft 3 - top heroes  ^8(total level of all races)" }
  for i = 1, math.min(10, #list) do local e = list[i]; lines[#lines + 1] = string.format("^3%2d. ^7%s  ^2%d  ^8%s", i, N.escape(e.name), e.lv, e.bl >= 0 and ("best: " .. e.best .. " " .. e.bl) or "no race yet") end
  if #list == 0 then lines[#lines + 1] = "^8nobody yet" end
  if N.has(id) then
    N.set(id, "war3.info", { type = "text", anchor = "top", py = 90, pw = 560, text = table.concat(lines, "\n"), markup = true, scale = 0.9, color2 = { 8, 10, 16, 215 }, shadow = true, layer = 60, life = 10, fadein = 0.2, fadeout = 0.8 })
  else for _, l in ipairs(lines) do xmod.msg.print(id, "console", plain(l)) end; xmod.msg.chat(id, TAG .. "Top list printed in your console") end
end

function menu_main(id)
  local s = st(id)
  N.menu(id, { title = "Warcraft 3" .. (s.race > 0 and (" - " .. RACES[s.race].name .. " lv " .. s.level) or ""), cursor = true, y = 0.09, width = 320,
    items = { { "Choose race", "race" }, { "Skills (" .. points_free(id) .. " free)", "skills", disabled = s.race == 0 }, { "Item shop", "shop" },
      { "Race information", "info", disabled = s.race == 0 }, { "Top heroes", "top" }, { "Reset skills", "reset", disabled = s.race == 0 }, { "Help", "help" } },
    on_select = function(pid, _, v)
      if v == "race" then menu_race(pid); return true elseif v == "skills" then menu_skills(pid); return true elseif v == "shop" then menu_shop(pid); return true
      elseif v == "info" then show_raceinfo(pid) elseif v == "top" then war3_top(pid) elseif v == "reset" then reset_skills(pid); return true elseif v == "help" then war3_help(pid) end
    end })
end

local SAY = { { "/war3menu", menu_main, "Warcraft 3 menu" }, { "/war3", menu_main }, { "/changerace", menu_race, "choose your War3 race" },
  { "/skills", menu_skills, "spend War3 skill points" }, { "/selectskill", menu_skills }, { "/shopmenu", menu_shop, "War3 item shop" }, { "/shop", menu_shop },
  { "/level", show_level, "your War3 level" }, { "/resetskills", reset_skills, "reset your War3 skills" }, { "/war3help", war3_help, "War3 help" },
  { "/raceinfo", function(id) show_raceinfo(id) end, "your race's skills" }, { "/playerskills", player_skills, "everybody's race and level" },
  { "/war3top", function(id) war3_top(id) end, "best War3 players" } }
for _, c in ipairs(SAY) do xmod.cmd.say(c[1], "", function(id) c[2](id) end, c[3]) end
for _, c in ipairs({ { "war3menu", menu_main }, { "changerace", menu_race }, { "skillsmenu", menu_skills }, { "shopmenu", menu_shop } }) do
  xmod.cmd.client(c[1], "", function(id) c[2](id) end)
end

-- =====================================================================================================================
-- admin commands
-- =====================================================================================================================
U.cmd("war3_givexp", "m", function(id, args)
  local list, err = U.target(id, args[1], { multi = true }); if not list then return U.reply(id, err) end
  local n = tonumber(args[2]); if not n then return U.reply(id, "usage: war3_givexp <target> <xp>") end
  for _, t in ipairs(list) do give_xp(t, n, "from admin") end
  U.activity(id, "gave " .. n .. " War3 XP to " .. #list .. " player(s)")
end, "<target> <xp>")
U.cmd("war3_setlevel", "m", function(id, args)
  local list, err = U.target(id, args[1], { multi = true }); if not list then return U.reply(id, err) end
  local lv = clamp(tonumber(args[2]) or 0, 0, max_level())
  for _, t in ipairs(list) do
    local s = st(t)
    if s.race > 0 then
      local before = s.level
      s.xp = xp_need(lv); s.level = lv; if points_free(t) < 0 then s.skills = { 0, 0, 0, 0 }; s.attr = { 0, 0, 0 } end
      if lv < ult_level() then s.skills[4] = 0 end
      if xmod.player.is_bot(t) then auto_skills(t) end
      apply_all(t); hud_update(t, true); save_player(t)
      if lv > before then level_up_fx(t) end
    end
  end
  U.activity(id, "set the War3 level of " .. #list .. " player(s) to " .. lv)
end, "<target> <level>")
U.cmd("war3_reset", "m", function(id, args)
  local list, err = U.target(id, args[1], { multi = false }); if not list then return U.reply(id, err) end
  for _, t in ipairs(list) do local key = save_key(t); if key then SAVE:remove(key) end; P[t] = newstate(); hud_update(t, true) end
  U.activity(id, "reset the War3 progress of " .. U.name(list[1]))
end, "<target>   (deletes all saved War3 progress)")

-- =====================================================================================================================
-- bots: a random race, points spent automatically
-- =====================================================================================================================
auto_skills = function(id)
  local s = st(id); if s.race == 0 then return end
  local guard = 0
  while points_free(id) > 0 and guard < 20 do
    guard = guard + 1
    if s.level >= ult_level() and s.skills[4] == 0 then s.skills[4] = 1
    else
      local opts = {}; for i = 1, 3 do if s.skills[i] < 3 then opts[#opts + 1] = i end end
      if #opts > 0 then local n = opts[math.random(#opts)]; s.skills[n] = s.skills[n] + 1
      else
        local ao = {}; for i = 1, 3 do if s.attr[i] < attr_max() then ao[#ao + 1] = i end end
        if #ao == 0 then break end
        local n = ao[math.random(#ao)]; s.attr[n] = s.attr[n] + 1
      end
    end
  end
end

-- =====================================================================================================================
-- info above every head (race, level, name, weapon + HP bar) and hero auras - Nexus players see them
-- =====================================================================================================================
local TAGS = {}        -- TAGS[viewer][p] = what that viewer was last sent about p
local function hp_color(f) if f > 0.6 then return { 70, 230, 90, 255 } elseif f > 0.3 then return { 240, 200, 50, 255 } end return { 240, 60, 50, 255 } end
local function tag_text(p)
  local s = P[p]; local r = s and RACES[s.race]
  local l1 = r and (RACE_MK[r.key] .. r.name .. " ^7Lv ^3" .. s.level .. (s.level >= max_level() and " ★" or "")) or "^8no race"
  if s and (s.streak or 0) >= 3 then l1 = l1 .. "  ^1" .. s.streak .. " kills" end
  local w = WNAME[CW[p] or 0]
  return l1 .. "\n^7" .. N.escape(xmod.player.name(p)) .. (w and ("\n^8" .. w) or "")
end
function tags_refresh()
  local on = xmod.cvar.int("war3_tags") ~= 0
  local md = clamp(xmod.cvar.int("war3_tag_dist"), 200, 8000); local ehp = xmod.cvar.int("war3_enemy_hp") ~= 0
  local players = xmod.player.all()
  local txt, hpf = {}, {}
  for _, p in ipairs(players) do
    if alive(p) then txt[p] = tag_text(p); hpf[p] = clamp(math.floor(xmod.player.health(p) / math.max(1, max_hp(p)) * 20 + 0.5) / 20, 0, 1) end
  end
  for _, v in ipairs(N.players()) do
    local T = TAGS[v]; if not T then T = {}; TAGS[v] = T end
    for _, p in ipairs(players) do
      local c = T[p]; if not c then c = {}; T[p] = c end
      local en = enemies(v, p)
      local vis = on and p ~= v and txt[p] ~= nil and not (en and invisible(p))
      local tn, bn = "war3.tag." .. p, "war3.hpb." .. p
      if vis then
        local rel = en and 1 or 2
        if c.rel ~= rel or c.md ~= md then
          c.rel, c.md = rel, md
          N.label3d(v, tn, p, txt[p], { offset = { 0, 0, 50 }, markup = true, scale = 0.62, maxdist = md, occlude = en, layer = -10, group = 32 })
          N.bar(v, bn, { ent = p, offset = { 0, 0, 44 }, anchor = "bottom", pw = 56, ph = 5, value = hpf[p], color = hp_color(hpf[p]), color2 = { 0, 0, 0, 170 },
            maxdist = md, occlude = en, distscale = true, layer = -11, group = 32 })
          c.text, c.hp, c.hidden, c.barhidden = txt[p], hpf[p], true, nil
        end
        if c.hidden then c.hidden = false; N.show(v, tn) end
        local bh = en and not ehp
        if c.barhidden ~= bh then c.barhidden = bh; N.set(v, bn, { hidden = bh }) end
        if c.text ~= txt[p] then c.text = txt[p]; N.set(v, tn, { text = txt[p] }) end
        if c.hp ~= hpf[p] then c.hp = hpf[p]; N.set(v, bn, { value = hpf[p], color = hp_color(hpf[p]) }) end
      elseif c.rel and not c.hidden then
        c.hidden = true; c.barhidden = true; N.hide(v, tn); N.hide(v, bn)
      end
    end
  end
end

local AURA = {}        -- AURA[p] = the aura everybody was sent for player p
local function aura_kind(p)
  if xmod.cvar.int("war3_auras") == 0 or not alive(p) then return nil end
  local s = P[p]; if not s or s.race == 0 or invisible(p) then return nil end
  local base = s.level >= max_level() and "max" or (s.level >= ult_level() and "hero" or "")
  local fire = (s.streak or 0) >= 5 and "inferno" or ((s.streak or 0) >= 3 and "fire" or "")
  local k = base .. "/" .. fire .. "/" .. s.race
  return k ~= "//" .. s.race and k or nil
end
local function aura_send(viewer, p, kind)
  for _, n in ipairs({ "a", "b", "c" }) do N.remove_shape(viewer, "war3.au" .. n .. p) end
  N.remove_light(viewer, "war3.aul" .. p)
  if not kind then return end
  local base, fire, rid = kind:match("^(%a*)/(%a*)/(%d+)$")
  local c = N.color(RACES[tonumber(rid)].color)
  if base == "hero" then          -- a hero ring in the race colour under the feet
    N.shape(viewer, "war3.aua" .. p, { type = "circle", ent = p, center = { 0, 0, -35 }, radius = 26, color = { c[1], c[2], c[3], 170 }, width = 2, additive = true })
  elseif base == "max" then       -- max level: a golden pulsing double ring and a soft light
    N.shape(viewer, "war3.aua" .. p, { type = "circle", ent = p, center = { 0, 0, -35 }, radius = 30, color = { 255, 210, 60, 220 }, width = 3, additive = true, pulse = true })
    N.shape(viewer, "war3.aub" .. p, { type = "circle", ent = p, center = { 0, 0, -34 }, radius = 40, color = { c[1], c[2], c[3], 150 }, width = 2, additive = true })
    N.light(viewer, "war3.aul" .. p, { ent = p, radius = 110, color = { 255, 200, 90 } })
  end
  if fire ~= "" then               -- killing streak: a burning column (taller and red for 5+ kills)
    local inf = fire == "inferno"
    N.shape(viewer, "war3.auc" .. p, { type = "cylinder", ent = p, center = { 0, 0, -36 }, radius = inf and 28 or 24, height = inf and 80 or 30,
      color = inf and { 255, 50, 20, 110 } or { 255, 150, 30, 110 }, fill = true, additive = true, pulse = true, width = 2 })
    N.light(viewer, "war3.aul" .. p, { ent = p, radius = inf and 170 or 120, color = inf and { 255, 60, 20 } or { 255, 150, 40 } })
  end
end
function auras_refresh()
  for _, p in ipairs(xmod.player.all()) do
    local k = aura_kind(p)
    if AURA[p] ~= k then AURA[p] = k; aura_send(0, p, k) end
  end
end
xmod.timer.every(0.5, function() tags_refresh(); auras_refresh() end)

-- =====================================================================================================================
-- connect / disconnect / Nexus
-- =====================================================================================================================
xmod.on("client_putinserver", function(id)
  local s = load_player(id)
  if xmod.player.is_bot(id) then
    if xmod.cvar.int("war3_bots") ~= 0 then s.race = math.random(#RACES); s.xp = xp_need(math.random(0, 4)); s.level = level_for(s.xp); auto_skills(id) end
    return
  end
  xmod.timer.after(3, function(pid)
    if not xmod.player.connected(pid) then return end
    local r = race_of(pid)
    if r then xmod.msg.chat(pid, TAG .. "Welcome back, {t}" .. r.name .. "{y} level {g}" .. st(pid).level .. "{y}. Menu: {t}/war3menu")
    else xmod.msg.chat(pid, TAG .. "This server runs {g}Warcraft 3{y}! Pick a race: {t}/changerace{y}  -  help: {t}/war3help") end
  end, id)
end)
xmod.on("client_disconnect", function(id)
  save_player(id); SAVE:save(); P[id] = nil; last_dmg[id] = nil; expect_death[id] = nil; CW[id] = nil
  if AURA[id] then aura_send(0, id, nil); AURA[id] = nil end
  N.remove(0, "war3.tag." .. id); N.remove(0, "war3.hpb." .. id)
  TAGS[id] = nil; for _, T in pairs(TAGS) do T[id] = nil end
end)
N.on_lost(function(id) TAGS[id] = nil end)
xmod.on("map_end", function() for id in pairs(P) do if xmod.player.connected(id) then save_player(id) end end; SAVE:save() end)
xmod.on("shutdown", function() for id in pairs(P) do if xmod.player.connected(id) then save_player(id) end end; SAVE:save() end)

N.on_ready(function(id)
  hud_build(id); hud_update(id, true)
  TAGS[id] = {}                                                  -- the client lost every element: send all tags again
  for p, k in pairs(AURA) do if k and xmod.player.connected(p) then aura_send(id, p, k) end end
  if feed_text ~= "" then feed_send(id) end
  local s = st(id)
  if s.skills[4] > 0 then ult_cooldown_visual(id, math.max(0, s.ult_ready_at - now())) end
  local key = xmod.cvar.get("war3_ult_key"); if key == "" then key = "c" end
  local okk, err = pcall(N.bind, id, key, function(pid, _, down) if down then use_ultimate(pid) end end, true)
  if not okk then xmod.log("war3: bad war3_ult_key '" .. key .. "': " .. tostring(err)) end
end)

-- players already connected when the plugin loads (xmod reload)
for _, id in ipairs(xmod.player.all()) do load_player(id) end
xmod.log("Warcraft 3 loaded: " .. #RACES .. " races, " .. #ITEMS .. " items, " .. SAVE:count() .. " saved players")
