-- xmod vote library:  local vote = require("vote")
-- vote.start{ title="Change map?", options={"Yes","No"}, time=15, voters=function(id) return true end,
--             on_done=function(winner_index, counts, total) ... end }   -> true | false, reason
local menu = require("menu")
local V = {}
local active

function V.running() return active ~= nil end

function V.finish()
  if not active then return end
  local st = active; active = nil
  if st.timer then xmod.timer.cancel(st.timer) end
  for id in pairs(st.menus) do if xmod.player.connected(id) then menu.close(id) end end
  local best, ties, total = -1, {}, 0
  for i, c in ipairs(st.counts) do
    total = total + c
    if c > best then best, ties = c, { i } elseif c == best then ties[#ties + 1] = i end
  end
  local winner = (total > 0) and ties[math.random(#ties)] or nil
  if st.def.on_done then st.def.on_done(winner, st.counts, total) end
end

function V.start(def)
  if active then return false, "A vote is already running" end
  local st = { def = def, counts = {}, voted = {}, menus = {} }
  for i = 1, #def.options do st.counts[i] = 0 end
  active = st
  local items = {}
  for i, o in ipairs(def.options) do items[i] = { o, i } end
  local secs = def.time or 15
  local any = false
  for _, id in ipairs(xmod.player.all()) do
    if not xmod.player.is_bot(id) and (not def.voters or def.voters(id)) then
      any = true; st.menus[id] = true
      menu.show(id, { title = def.title, items = items, timeout = secs, on_select = function(pid, _, idx)
        if st.voted[pid] or active ~= st then return end
        st.voted[pid] = idx; st.counts[idx] = st.counts[idx] + 1
        xmod.msg.chat(pid, "{g}[vote] {y}You voted for {t}" .. def.options[idx])
        local all = true
        for vid in pairs(st.menus) do if xmod.player.connected(vid) and not st.voted[vid] then all = false end end
        if all then xmod.timer.after(0.5, V.finish) end
      end })
    end
  end
  if not any then active = nil; return false, "No players to vote" end
  st.timer = xmod.timer.after(secs + 0.5, V.finish)
  return true
end
xmod.on("map_end", function() active = nil end)
return V
