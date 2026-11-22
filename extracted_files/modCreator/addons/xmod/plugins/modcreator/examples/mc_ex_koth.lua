-- ModCreator example: King of the Hill. A capture point spawns at the middle of the two spawn areas;
-- the team that captures it wins the round; the game's own round end (eliminations) still works.
plugin = { name = "mc_ex_koth", version = "1.0", author = "Andrew & Claude" }
local MC = require("modcreator")
MC.round.on("new", function()
  local tx, ty, tz = MC.spawns.random("t"); local cx, cy, cz = MC.spawns.random("ct")
  if not tx or not cx then return end
  MC.objectives.remove("hill")
  MC.objectives.add { id = "hill", name = "The Hill", type = "capture", origin = { (tx + cx) / 2, (ty + cy) / 2, math.min(tz, cz) },
    radius = 140, time = 12, round_win = true, contested = true }
end)
MC.round.on("end", function(winner, cs, reason) xmod.log("koth: round won by " .. tostring(winner) .. " (" .. reason .. ")") end)
xmod.on("mc:objective_progress", function(id, team, p)
  if id == "hill" and p > 0 and math.floor(p * 100) % 25 == 0 then MC.hud.center(0, MC.teams.name(team) .. " capturing: " .. math.floor(p * 100) .. "%") end
end)
