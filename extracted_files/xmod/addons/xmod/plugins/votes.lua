-- Votes: xmod_votemap, xmod_votekick, xmod_vote (custom question), xmod_cancelvote.  Needs access flag j.
xmod.register("Votes", "0.2", "xmod")
local U, vote = require("util"), require("vote")
xmod.cvar.create("xmod_vote_ratio", "0.5")         -- share of votes "Yes" needs

local function yesno(title, on_pass, id)
  local ok, err = vote.start{ title = title, options = { "Yes", "No" }, time = 15, on_done = function(winner, counts, total)
    local ratio = xmod.cvar.num("xmod_vote_ratio")
    if total > 0 and counts[1] / total > ratio then
      xmod.msg.chat(0, ("{g}[vote] {y}Passed ({t}%d{y}/%d)."):format(counts[1], total)); on_pass()
    else xmod.msg.chat(0, ("{g}[vote] {y}Failed ({t}%d{y} yes / %d total)."):format(counts[1], total)) end
  end }
  if not ok then U.reply(id, err) end
  return ok
end

U.cmd("votemap", "j", function(id, a)
  if a.n < 1 then U.reply(id, "usage: xmod_votemap <map> [map2 map3 ...]"); return end
  local list = {}
  for i = 1, math.min(a.n, 5) do if xmod.map_valid(a[i]) then list[#list + 1] = a[i] else U.reply(id, "Invalid map: " .. a[i]); return end end
  U.activity(id, "started a map vote")
  if #list == 1 then
    yesno("Change map to " .. list[1] .. "?", function() xmod.timer.after(3.0, function() xmod.exec("changelevel " .. list[1]) end) end, id)
  else
    local ok, err = vote.start{ title = "Vote for the next map", options = list, time = 15, on_done = function(w)
      if not w then xmod.msg.chat(0, "{g}[vote] {y}Nobody voted."); return end
      xmod.msg.chat(0, "{g}[vote] {y}Changing map to {t}" .. list[w]); xmod.timer.after(3.0, function() xmod.exec("changelevel " .. list[w]) end)
    end }
    if not ok then U.reply(id, err) end
  end
end, "votemap <map> [map2 ...]")

U.cmd("votekick", "j", function(id, a)
  local t, err = U.target1(id, a[1]); if not t then U.reply(id, err); return end
  local name = xmod.player.name(t); U.activity(id, "started a kick vote for {t}" .. name)
  yesno("Kick " .. name .. "?", function() if xmod.player.connected(t) then xmod.player.kick(t, "Vote kicked") end end, id)
end, "votekick <target>")

U.cmd("vote", "j", function(id, a)
  if a.n < 3 then U.reply(id, 'usage: xmod_vote "question" "option 1" "option 2" ...'); return end
  local q, opts = a[1], {}
  for i = 2, math.min(a.n, 7) do opts[#opts + 1] = a[i] end
  local ok, err = vote.start{ title = q, options = opts, time = 20, on_done = function(w, counts, total)
    if not w then xmod.msg.chat(0, "{g}[vote] {y}Nobody voted."); return end
    local parts = {}; for i, o in ipairs(opts) do parts[#parts + 1] = ("%s: %d"):format(o, counts[i]) end
    xmod.msg.chat(0, ("{g}[vote] {y}%s -> {t}%s{y}  (%s)"):format(q, opts[w], table.concat(parts, ", ")))
  end }
  if not ok then U.reply(id, err) end
end, "vote \"question\" \"opt1\" \"opt2\" ...")

U.cmd("cancelvote", "j", function(id) if vote.running() then vote.finish(); U.activity(id, "ended the vote") end end, "cancelvote")
