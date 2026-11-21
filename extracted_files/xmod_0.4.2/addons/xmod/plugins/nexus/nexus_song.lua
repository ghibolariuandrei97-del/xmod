-- Nexus link test:  say /song               -> pick a song, YouTube opens in the player's browser (after they press Y)
--                   say /song <artist - title>  -> YouTube search for those words
-- Nexus never opens anything by itself: the player's client shows its own prompt with the site name, Y = open, N / ESC = no.
-- Players can switch links off completely with  nexus_urls 0  in their console.
xmod.register("Nexus Song Link", "1.0", "Andrew")
local N = require("nexus")

local SONGS = {
  { "Daft Punk - One More Time" },
  { "Eminem - Lose Yourself" },
  { "Linkin Park - Numb" },
  { "Queen - Bohemian Rhapsody" },
  { "Inna - Hot" },
  { "O-Zone - Dragostea din tei" },
  { "Smiley - Dans pe mese" },
}

local RESULT_TEXT = {
  opened   = "{g}[Song] {y}Opened in your browser. Alt+Tab to listen, then come back.",
  declined = "{g}[Song] {y}OK, not opened.",
  disabled = "{g}[Song] {y}You switched links off ({t}nexus_urls 0{y}). Type {t}nexus_urls 1{y} in the console to allow them.",
  busy     = "{g}[Song] {y}Wait a few seconds between links.",
  bad_url  = "{g}[Song] {y}That link was refused by your client.",
}

local function play(id, words)
  if not N.has(id) then
    xmod.msg.chat(id, "{g}[Song] {y}You need the Nexus client for links. Search YouTube for: {t}" .. words)
    return
  end
  local info = N.info(id)
  if info and info.urls == false then xmod.msg.chat(id, RESULT_TEXT.disabled); return end
  xmod.msg.chat(id, "{g}[Song] {y}Press {t}Y{y} to open {t}" .. words .. "{y} on YouTube, {t}N{y} to cancel.")
  N.youtube(id, words, function(pid, result)
    if xmod.player.connected(pid) then xmod.msg.chat(pid, RESULT_TEXT[result] or ("{g}[Song] {y}" .. result)) end
  end)
end

xmod.cmd.say("/song", "", function(id, args)
  local words = args and args.text and args.text:gsub("^%s+", ""):gsub("%s+$", "") or ""
  if words ~= "" then
    if #words > 80 then xmod.msg.chat(id, "{g}[Song] {y}Too long (max 80 characters)."); return end
    play(id, words); return
  end
  N.menu(id, { title = "Pick a song (opens YouTube)", items = SONGS, cursor = true,
    on_select = function(pid, label) play(pid, label) end })
end, "open a song on YouTube in your browser")
