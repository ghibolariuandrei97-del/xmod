-- ==============================================================================================================
--  lib/translator.lua  -  simple multi-language system for xmod          local T = require("translator")
--
--    local tr = T.load("myplugin")            -- reads addons/xmod/lang/myplugin.txt (AMX Mod X dictionary format)
--    tr:chat(0, "WELCOME", name)              -- every player gets the line in HIS language
--    local s = tr(id, "KILLS", n)             -- text for one player (id 0 = server language)
--
--  Lang file (same format as AMXX data/lang/*.txt, so AMXX files work as they are):
--      [en]
--      WELCOME = {g}Welcome {t}%s{y}!
--      [ro]
--      WELCOME = {g}Bine ai venit {t}%s{y}!
--
--  Player language: his choice (say /lang, saved by SteamID) > his "setinfo lang xx" > cvar xmod_language.
--  Missing text: player language > server language > en > any language that has the key > the key itself.
--  Full documentation: xmod_skills.md, section "translator".
-- ==============================================================================================================
local M = {}
M.VERSION = "1.0"

-- =============================================  EDIT HERE  ====================================================
M.CONFIG = {
  dir          = "lang/",          -- folder of the dictionaries, relative to addons/xmod/
  say_commands = { "/lang", "/language", "/limba" },   -- chat commands that open the language menu ({} = none)
  save         = true,             -- remember each player's choice (vault "translator", by SteamID)
  warn_missing = true,             -- log every missing key once (console + data/logs via metalog if present)
}
-- ==============================================================================================================
local CFG = M.CONFIG

xmod.cvar.create("xmod_language", "en")       -- server language (AMXX: amx_language)
xmod.cvar.create("xmod_lang_players", "1")    -- 1 = players may choose their language (AMXX: amx_client_languages)
xmod.cvar.create("xmod_lang_setinfo", "1")    -- 1 = use the player's "setinfo lang xx" when he did not choose

-- native names for the language menu (any other code is shown as it is)
M.NAMES = {
  en = "English", ro = "Romana", es = "Español", de = "Deutsch", fr = "Français", pt = "Português", pt_br = "Português (BR)",
  ru = "Русский", pl = "Polski", tr = "Türkçe", it = "Italiano", hu = "Magyar", bg = "Български", uk = "Українська",
  sr = "Srpski", hr = "Hrvatski", nl = "Nederlands", cs = "Čeština", sk = "Slovenčina", el = "Ελληνικά", lt = "Lietuvių",
  lv = "Latviešu", sv = "Svenska", da = "Dansk", fi = "Suomi", no = "Norsk", mk = "Македонски", sq = "Shqip",
  ar = "العربية", zh = "中文", ja = "日本語", ko = "한국어", vi = "Tiếng Việt", id = "Indonesia", ka = "ქართული",
}

local mlog                                    -- metalog logger (optional)
do
  local ok, ML = pcall(require, "metalog")
  if ok and type(ML) == "table" and ML.new then mlog = ML.new("translator", { level = "info", location = false }) end
end
local function info(fmt, ...) local s = select("#", ...) > 0 and fmt:format(...) or fmt
  if mlog then mlog:info(s) else xmod.log("translator: " .. s) end end
local function warn(fmt, ...) local s = select("#", ...) > 0 and fmt:format(...) or fmt
  if mlog then mlog:warn(s) else xmod.log("translator: WARNING " .. s) end end

-- ------------------------------------------------------------------------------------------------- codes
--- "pt-BR" -> "pt_br"; nil if it does not look like a language code
function M.code(s)
  if type(s) ~= "string" then return nil end
  s = s:lower():gsub("%-", "_"):gsub("^%s+", ""):gsub("%s+$", "")
  if s:match("^%a%a%a?$") or s:match("^%a%a%a?_%a%a%a?%a?$") then return s end
  return nil
end
function M.lang_name(code) return M.NAMES[code] or M.NAMES[(code or ""):match("^(%a+)_") or ""] or tostring(code) end

-- ------------------------------------------------------------------------------------------------- parser
local function unescape(v) return (v:gsub("%^n", "\n"):gsub("%^t", "\t")) end

--- parse AMXX dictionary text -> { [code] = { KEY = text } }, number of keys, list of warnings
function M.parse(text)
  local langs, count, problems = {}, 0, {}
  local cur, multi_key, multi_lines
  text = (text or ""):gsub("^\239\187\191", "")              -- UTF-8 BOM
  local ln = 0
  for line in (text .. "\n"):gmatch("([^\n]*)\n") do
    ln = ln + 1
    line = line:gsub("\r$", "")
    if multi_key then                                     -- KEY: ... lines ... ':' (block ends with a line that is only ':')
      if line:match("^%s*:%s*$") then
        langs[cur][multi_key] = unescape(table.concat(multi_lines, "\n")); count = count + 1; multi_key = nil
      else multi_lines[#multi_lines + 1] = line end
    else
      local t = line:match("^%s*(.-)%s*$")
      if t == "" or t:match("^;") or t:match("^//") or t:match("^#") then
        -- comment / empty
      elseif t:match("^%[.-%]$") then
        local c = M.code(t:match("^%[(.-)%]$"))
        if c then cur = c; langs[c] = langs[c] or {} else problems[#problems + 1] = ("line %d: bad language section %s"):format(ln, t); cur = nil end
      elseif not cur then
        problems[#problems + 1] = ("line %d: text before any [language] section"):format(ln)
      else
        local k, v = t:match("^([%w_%.%-]+)%s*=%s?(.*)$")
        if k then langs[cur][k] = unescape(v); count = count + 1
        else
          local mk = t:match("^([%w_%.%-]+):$")
          if mk then multi_key, multi_lines = mk, {} else problems[#problems + 1] = ("line %d: expected KEY = text"):format(ln) end
        end
      end
    end
  end
  if multi_key then problems[#problems + 1] = "multi-line key " .. multi_key .. " is not closed with a ':' line" end
  return langs, count, problems
end

-- ------------------------------------------------------------------------------------------------- formatting
--- template + args -> text.  One table argument = named placeholders {name}; otherwise string.format (%s %d %i %f %c %x).
--- Never raises: a bad format returns the template with the arguments appended.
function M.format(tpl, ...)
  local n = select("#", ...)
  if n == 0 then return tpl end
  local first = ...
  if n == 1 and type(first) == "table" then
    return (tpl:gsub("{([%w_]+)}", function(k)
      local v = first[k]; if v == nil then v = first[tonumber(k) or k] end
      if v == nil then return nil end
      return tostring(v)
    end))
  end
  local args = table.pack(...)
  for i = 1, n do local a = args[i]; if type(a) ~= "number" and type(a) ~= "string" then args[i] = tostring(a) end end
  local ok, s = pcall(string.format, tpl, table.unpack(args, 1, n))
  if ok then return s end
  local out = { tpl }
  for i = 1, n do out[#out + 1] = tostring(args[i]) end
  return table.concat(out, " ")
end

-- ------------------------------------------------------------------------------------------------- players
local vault
local function store()
  if not CFG.save then return nil end
  if not vault then local ok, v = pcall(xmod.vault.open, "translator"); vault = ok and v or false end
  return vault or nil
end
local chosen = {}       -- id -> code chosen (menu / command / set_lang)
local from_info = {}    -- id -> code from setinfo lang

local function player_key(id)
  local ok, sid = pcall(xmod.player.steamid, id)
  if ok and type(sid) == "string" and sid:match("^STEAM_%d:%d:%d+$") then return sid end
  if ok and type(sid) == "string" and sid:match("^VALVE_%d:%d:%d+$") then return sid end
  return nil
end

function M.server_lang() return M.code(xmod.cvar.get("xmod_language")) or "en" end

--- language code of a player (0 / invalid id = server language)
function M.lang(id)
  if type(id) ~= "number" or id <= 0 then return M.server_lang() end
  return chosen[id] or (xmod.cvar.int("xmod_lang_setinfo") ~= 0 and from_info[id]) or M.server_lang()
end

local function read_setinfo(id)
  local ok, v = pcall(xmod.player.infokey, id, "lang")
  from_info[id] = ok and M.code(v) or nil
end

--- set a player's language. save (default true) = remember it for his SteamID. Fires the event "lang_changed"(id, old, new)
function M.set_lang(id, code, save)
  code = M.code(code)
  if not code or not xmod.player.connected(id) then return false end
  local old = M.lang(id)
  chosen[id] = code
  if save ~= false then
    local v, key = store(), player_key(id)
    if v and key then v:set(key, code) end
  end
  if old ~= code then xmod.emit("lang_changed", id, old, code) end
  return true
end

--- forget a player's choice (back to setinfo / server language)
function M.reset_lang(id)
  local old = M.lang(id)
  chosen[id] = nil
  local v, key = store(), player_key(id)
  if v and key then v:remove(key) end
  local new = M.lang(id)
  if old ~= new then xmod.emit("lang_changed", id, old, new) end
end

xmod.on("client_putinserver", function(id)
  chosen[id] = nil; read_setinfo(id)
  local v, key = store(), player_key(id)
  if v and key then local c = M.code(v:get(key)); if c then chosen[id] = c; v:touch(key) end end
end, 100)
xmod.on("client_infochanged", function(id)
  if not xmod.player.connected(id) then return end
  local before = M.lang(id); read_setinfo(id)
  local after = M.lang(id)
  if before ~= after then xmod.emit("lang_changed", id, before, after) end
end)
xmod.on("client_disconnect", function(id) chosen[id] = nil; from_info[id] = nil end)
for _, id in ipairs(xmod.player.all()) do read_setinfo(id) end    -- after "xmod reload"

-- ------------------------------------------------------------------------------------------------- dictionaries
local dicts, dict_order = {}, {}
local Dict = {}
Dict.__index = Dict

local function new_dict(name)
  local d = dicts[name]
  if d then return d end
  d = setmetatable({ name = name, langs = {}, order = {}, missing_seen = {} }, Dict)
  dicts[name] = d; dict_order[#dict_order + 1] = name
  return d
end

local function merge(d, langs)
  for code, keys in pairs(langs) do
    if not d.langs[code] then d.langs[code] = {}; d.order[#d.order + 1] = code end
    for k, v in pairs(keys) do d.langs[code][k] = v end
  end
end

--- tr = T.load(name [, file])  -> dictionary from addons/xmod/lang/<name>.txt (file = another path below addons/xmod/)
---   Loading the same name again re-reads the file. A missing file is not an error (add texts with tr:add).
function M.load(name, file)
  assert(type(name) == "string" and name ~= "", "translator.load(name): name expected")
  local d = new_dict(name)
  d.file = file or (CFG.dir .. name .. ".txt")
  local text = xmod.fs.read(d.file)
  if not text then
    if not d.quiet_missing then info("%s not found (dictionary '%s' starts empty)", d.file, name) end
    return d
  end
  local langs, count, problems = M.parse(text)
  merge(d, langs)
  for _, p in ipairs(problems) do warn("%s: %s", d.file, p) end
  d.loaded_keys = count
  return d
end

--- tr = T.new(name) -> empty dictionary (fill it with tr:add); returns the existing one if the name is known
function M.new(name) local d = new_dict(name); return d end
function M.dict(name) return dicts[name] end
function M.dicts() local t = {} for i, n in ipairs(dict_order) do t[i] = dicts[n] end return t end

--- tr:add(code, { KEY = "text", ... }) -> adds / replaces texts (no file needed); returns tr
function Dict:add(code, keys)
  code = assert(M.code(code), "translator: bad language code")
  merge(self, { [code] = keys }); return self
end
function Dict:set(code, key, text) return self:add(code, { [key] = text }) end
function Dict:reload() if self.file then self.langs = {}; self.order = {}; M.load(self.name, self.file) end; return self end

-- raw lookup with the fallback chain; returns text, code_used (nil if missing everywhere)
function Dict:raw(code, key)
  local L = self.langs
  local t = L[code]; if t and t[key] then return t[key], code end
  local base = code and code:match("^(%a+)_")
  if base and L[base] and L[base][key] then return L[base][key], base end
  local sv = M.server_lang()
  if L[sv] and L[sv][key] then return L[sv][key], sv end
  if L.en and L.en[key] then return L.en[key], "en" end
  for _, c in ipairs(self.order) do if L[c][key] then return L[c][key], c end end
  return nil
end

local function note_missing(d, code, key, used)
  if not CFG.warn_missing then return end
  local k = code .. "\0" .. key
  if d.missing_seen[k] then return end
  d.missing_seen[k] = true
  if used then info("[%s] key %s has no '%s' text (using '%s')", d.name, key, code, used)
  else warn("[%s] unknown key %s", d.name, key) end
end

--- text in an explicit language
function Dict:in_lang(code, key, ...)
  code = M.code(code) or M.server_lang()
  local tpl, used = self:raw(code, key)
  if used ~= code then note_missing(self, code, key, used) end
  return M.format(tpl or key, ...)
end

--- text for player `id` (0 = server language).  Same as tr(id, key, ...)
function Dict:get(id, key, ...) return self:in_lang(M.lang(id), key, ...) end
Dict.__call = function(self, id, key, ...) return self:in_lang(M.lang(id), key, ...) end

-- plural form of a language: "ONE" | "FEW" | "MANY"
local function plural_form(code, n)
  n = math.abs(math.floor(tonumber(n) or 0))
  local b = (code or ""):match("^(%a+)") or ""
  if b == "ro" then
    if n == 1 then return "ONE" end
    local r = n % 100
    if n == 0 or (r >= 1 and r <= 19) then return "FEW" end
    return "MANY"
  elseif b == "ru" or b == "uk" or b == "sr" or b == "hr" then
    local r10, r100 = n % 10, n % 100
    if r10 == 1 and r100 ~= 11 then return "ONE" end
    if r10 >= 2 and r10 <= 4 and (r100 < 12 or r100 > 14) then return "FEW" end
    return "MANY"
  elseif b == "pl" or b == "cs" or b == "sk" then
    if n == 1 then return "ONE" end
    local r10, r100 = n % 10, n % 100
    if b == "pl" then
      if r10 >= 2 and r10 <= 4 and (r100 < 12 or r100 > 14) then return "FEW" end
    elseif n >= 2 and n <= 4 then return "FEW" end
    return "MANY"
  elseif b == "fr" or b == "pt" then
    return (n <= 1) and "ONE" or "MANY"
  end
  return n == 1 and "ONE" or "MANY"
end
M.plural_form = plural_form

--- tr:plural(id, "KILLS", n [, more args]) -> picks KILLS_ONE / KILLS_FEW / KILLS_MANY (falling back to KILLS)
---   by the plural rule of the player's language; n is the first format argument:  KILLS_ONE = %d kill
function Dict:plural(id, key, n, ...)
  local code = M.lang(id)
  local form = plural_form(code, n)
  local tries = { key .. "_" .. form }
  if form == "FEW" then tries[#tries + 1] = key .. "_MANY" end
  tries[#tries + 1] = key
  for _, k in ipairs(tries) do
    local tpl, used = self:raw(code, k)
    if tpl and used == code then return M.format(tpl, n, ...) end
  end
  for _, k in ipairs(tries) do if self:raw(code, k) then return self:in_lang(code, k, n, ...) end end
  return self:in_lang(code, key, n, ...)
end

local function targets(id)
  if id and id > 0 then return xmod.player.connected(id) and { id } or {} end
  local t = {}
  for _, p in ipairs(xmod.player.all()) do if not xmod.player.is_bot(p) then t[#t + 1] = p end end
  return t
end

--- tr:chat(id|0, key, ...)  coloured chat, each player in his language (0 = all human players)
function Dict:chat(id, key, ...)
  for _, p in ipairs(targets(id)) do xmod.msg.chat(p, self:in_lang(M.lang(p), key, ...)) end
end
--- tr:print(id|0, "console"|"center"|"chat", key, ...)
function Dict:print(id, dest, key, ...)
  for _, p in ipairs(targets(id)) do xmod.msg.print(p, dest, self:in_lang(M.lang(p), key, ...)) end
end
--- tr:hud(id|0, opts, key, ...)   opts = the table of xmod.msg.hud
function Dict:hud(id, opts, key, ...)
  for _, p in ipairs(targets(id)) do xmod.msg.hud(p, self:in_lang(M.lang(p), key, ...), opts) end
end
--- tr:console(id, key, ...) -> console of a player, or the server console for id 0
function Dict:console(id, key, ...)
  if not id or id == 0 then return xmod.server_print(self:in_lang(M.server_lang(), key, ...)) end
  if xmod.player.connected(id) then xmod.msg.print(id, "console", self:in_lang(M.lang(id), key, ...) .. "\n") end
end

function Dict:has(key, code) if code then local t = self.langs[M.code(code) or ""]; return t ~= nil and t[key] ~= nil end
  return self:raw(M.server_lang(), key) ~= nil end
function Dict:langs_list() local t = {} for i, c in ipairs(self.order) do t[i] = c end return t end
function Dict:keys(code)
  local set = {}
  if code then for k in pairs(self.langs[M.code(code) or ""] or {}) do set[k] = true end
  else for _, c in ipairs(self.order) do for k in pairs(self.langs[c]) do set[k] = true end end end
  local t = {} for k in pairs(set) do t[#t + 1] = k end
  table.sort(t); return t
end
--- { [code] = {missing keys} } compared with the keys of all languages of this dictionary
function Dict:missing()
  local all, res = self:keys(), {}
  for _, c in ipairs(self.order) do
    local m = {}
    for _, k in ipairs(all) do if self.langs[c][k] == nil then m[#m + 1] = k end end
    if #m > 0 then res[c] = m end
  end
  return res
end

-- ------------------------------------------------------------------------------------------------- global helpers
--- every language code found in the loaded dictionaries: { {code, name, keys}, ... } (server language first, then by code)
function M.languages()
  local count = {}
  for _, n in ipairs(dict_order) do for c, keys in pairs(dicts[n].langs) do
    local k = 0 for _ in pairs(keys) do k = k + 1 end
    count[c] = (count[c] or 0) + k end end
  local sv, t = M.server_lang(), {}
  for c, k in pairs(count) do t[#t + 1] = { code = c, name = M.lang_name(c), keys = k } end
  table.sort(t, function(a, b)
    if (a.code == sv) ~= (b.code == sv) then return a.code == sv end
    return a.code < b.code end)
  return t
end

--- T.t(id, key, ...) -> search every dictionary (common first, then in load order); the key itself if not found
function M.t(id, key, ...)
  local code = M.lang(id)
  for _, n in ipairs(dict_order) do
    if dicts[n]:raw(code, key) then return dicts[n]:in_lang(code, key, ...) end
  end
  return M.format(key, ...)
end

function M.reload() for _, n in ipairs(dict_order) do dicts[n]:reload() end end

-- built-in dictionary: lang/common.txt (YES, NO, menu texts of this library, ...)
local common = M.load("common")
M.common = common
local function C(id, key, ...) return common:in_lang(M.lang(id), key, ...) end

-- ------------------------------------------------------------------------------------------------- menu + commands
local menu_ok, menu = pcall(require, "menu")

--- open the language menu for a player
function M.menu(id)
  if not xmod.player.connected(id) then return false end
  if xmod.cvar.int("xmod_lang_players") == 0 then xmod.msg.chat(id, C(id, "TR_DISABLED")); return false end
  if not menu_ok then return false end
  local cur, items = M.lang(id), {}
  for _, l in ipairs(M.languages()) do
    items[#items + 1] = { l.name .. (l.code == cur and "  *" or "") .. "  [" .. l.code .. "]", l.code }
  end
  menu.show(id, { title = C(id, "TR_MENU_TITLE"), items = items, on_select = function(pid, _, code)
    if M.set_lang(pid, code) then xmod.msg.chat(pid, C(pid, "TR_CHANGED", M.lang_name(code))) end
  end })
  return true
end

for _, trig in ipairs(CFG.say_commands or {}) do
  xmod.cmd.say(trig, "", function(id) M.menu(id) end, "choose your language")
end

xmod.cmd.client("xmod_lang", "", function(id, args)
  local c = args[1]
  if not c or c == "" then
    xmod.msg.print(id, "console", C(id, "TR_CURRENT", M.lang_name(M.lang(id)), M.lang(id)) .. "\n")
    local codes = {} for _, l in ipairs(M.languages()) do codes[#codes + 1] = l.code end
    xmod.msg.print(id, "console", "xmod_lang <" .. table.concat(codes, "|") .. "|reset>\n")
    return
  end
  if xmod.cvar.int("xmod_lang_players") == 0 then return xmod.msg.print(id, "console", C(id, "TR_DISABLED") .. "\n") end
  if c == "reset" then M.reset_lang(id); return xmod.msg.print(id, "console", C(id, "TR_CHANGED", M.lang_name(M.lang(id))) .. "\n") end
  if not M.code(c) then return xmod.msg.print(id, "console", C(id, "TR_UNKNOWN", c) .. "\n") end
  M.set_lang(id, c)
  xmod.msg.print(id, "console", C(id, "TR_CHANGED", M.lang_name(M.code(c))) .. "\n")
end, "xmod_lang [code|reset] - show or set your language")

xmod.cmd.server("xmod_lang_list", function()
  xmod.server_print(("translator %s: server language %s, %d dictionaries"):format(M.VERSION, M.server_lang(), #dict_order))
  for _, l in ipairs(M.languages()) do
    local pad = string.rep(" ", math.max(1, 17 - (utf8.len(l.name) or #l.name)))
    xmod.server_print(("  %-6s %s%s%d texts"):format(l.code, l.name, pad, l.keys))
  end
  for _, n in ipairs(dict_order) do local d = dicts[n]
    xmod.server_print(("  dictionary %-18s %-28s languages: %s"):format(n, d.file or "(inline)", table.concat(d.order, " "))) end
  for _, id in ipairs(xmod.player.all()) do
    xmod.server_print(("  player #%d %-24s %s%s"):format(id, xmod.player.name(id), M.lang(id), chosen[id] and " (chosen)" or (from_info[id] and " (setinfo)" or "")))
  end
end, "list languages, dictionaries and player languages")

xmod.cmd.server("xmod_lang_check", function(args)
  local any = false
  for _, n in ipairs(dict_order) do
    if not args[1] or args[1] == n then
      for code, keys in pairs(dicts[n]:missing()) do
        any = true
        xmod.server_print(("[%s] %s is missing %d: %s"):format(n, code, #keys, table.concat(keys, ", ")))
      end
    end
  end
  if not any then xmod.server_print("translator: every language has every key") end
end, "xmod_lang_check [dictionary] - keys missing per language")

xmod.cmd.server("xmod_lang_reload", function() M.reload(); xmod.server_print("translator: dictionaries reloaded") end,
  "re-read every lang/*.txt file")

return M
