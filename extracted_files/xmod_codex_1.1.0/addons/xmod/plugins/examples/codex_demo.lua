-- ============================================================================================================
--  codex_demo.lua  -  what the Codex extension can do, on a real CS 1.6 server   (uses lib/codex.lua)
--  Enable: remove the ";" before examples/codex_demo.lua in configs/plugins-codex.ini (or add the line to configs/plugins.ini)
--
--  1. call a game function that has no API:   CBasePlayer::GiveNamedItem   (console: codex_give <#userid|name> weapon_ak47)
--  2. hook it: every item a player gets passes through Lua; cvar codex_block_c4 1 blocks the bomb
--  3. break a hard-coded limit: the 16000 money cap in CBasePlayer::AddAccount  (cvar codex_maxmoney, 0 = leave the game alone)
--  Console: codex_gamedata  -> prints the signatures of this build (to hard-code them for Windows, for example)
--
--  Nothing here can crash the server: a function that is not found stays unresolved (codex list / codex info <name>
--  say why), a failed call returns nil + the reason, and every patch is checked and undone on unload.
-- ============================================================================================================
xmod.register("Codex Demo", "1.1", "xmod")
local cx = require("codex")
xmod.cvar.create("codex_block_c4", "0")
xmod.cvar.create("codex_maxmoney", "0")

-- Gamedata: one table for both platforms. Linux binaries have symbols; the Windows mp.dll has none, but
-- GiveNamedItem prints "NULL Ent in GiveNamedItem!", so Codex finds it by that string. The first method that works
-- is used, and Codex caches a signature it generates for this build (data/codex/cache.ini).
local GAMEDATA = {
  GiveNamedItem = {
    conv = "thiscall", this = "entity",          -- a CBasePlayer method: 'this' arrives as a player index
    args = { "astring" }, ret = "void",          -- astring: the game keeps this pointer (pev->classname), so it must
                                                 -- live in the engine string pool, not in a temporary Lua string
    symbol = "CBasePlayer::GiveNamedItem",
    xref = "NULL Ent in GiveNamedItem!",
  },
  -- only its address is used (consts / set_const); it is not called or hooked: its arguments differ between the
  -- original game (int, bool) and ReGameDLL (int, RewardType, bool)
  AddAccount = {
    conv = "thiscall", this = "entity",
    symbol = { "CBasePlayer::AddAccount_OrigFunc", "CBasePlayer::AddAccount" },
    -- sig = { windows = "..." },               -- Windows (no symbols): put a signature of your mp.dll here
  },
}

-- 3. the limit. The original game clamps money in AddAccount with the constant 16000 (sometimes compiled as 15999:
-- that is what near = true is for). ReGameDLL reads mp_maxmoney instead: then there is nothing to patch.
-- Note: xmod.player.set_money still clamps to 16000; money earned in game goes through AddAccount.
local money
local function apply_money_limit()
  if money then money:restore(); money = nil end
  local max = xmod.cvar.int("codex_maxmoney")
  if max <= 0 then return end
  if not cx.has("AddAccount") then return xmod.log("money limit: " .. cx.describe("AddAccount")) end
  local h, e = cx.limit("AddAccount", 16000, max, { near = true })
  if not h then return xmod.log("money limit not changed: " .. e .. " (ReGameDLL? use mp_maxmoney)") end
  money = h
  xmod.log(("money limit raised to %d (%d places patched in AddAccount)"):format(max, #h.ids))
end

-- everything that calls Codex runs once it is attached (scripts load before extensions)
cx.ready(function()
  cx.define(GAMEDATA, { module = "game" })
  if cx.has("GiveNamedItem") then
    xmod.log("GiveNamedItem: " .. cx.describe("GiveNamedItem"))
    cx.hook("GiveNamedItem")                     -- the handler is registered below, in this plugin
  end
  apply_money_limit()
  xmod.cvar.watch("codex_maxmoney", function() apply_money_limit() end)
end)

-- 2. hook handler (an xmod event; register it in the plugin itself, at load time)
xmod.on("codex:GiveNamedItem", function(id, item)
  if item == "weapon_c4" and xmod.cvar.int("codex_block_c4") == 1 then
    return xmod.HC_SUPERCEDE                    -- the original never runs: no bomb
  end
end)

-- 1. calling it
local function find_player(who)
  local uid = who:match("^#(%d+)$")
  for _, id in ipairs(xmod.player.all()) do
    if (uid and xmod.player.userid(id) == tonumber(uid)) or (not uid and xmod.player.name(id):lower():find(who:lower(), 1, true)) then return id end
  end
end

xmod.cmd.server("codex_give", function(args)
  local who, item = args[1], args[2]
  if not (who and item) then return xmod.server_print("usage: codex_give <#userid|name> <weapon_/item_ name>") end
  local id = find_player(who)
  if not id or not xmod.player.alive(id) then return xmod.server_print("no such alive player: " .. who) end
  local ok, e = cx.try("GiveNamedItem", id, item)
  xmod.server_print(ok and ("gave " .. item .. " to " .. xmod.player.name(id)) or ("failed: " .. e))
end, "codex_give <#userid|name> <item>")

xmod.cmd.server("codex_gamedata", function() cx.dump(nil, true) end, "print Codex gamedata for this build")
