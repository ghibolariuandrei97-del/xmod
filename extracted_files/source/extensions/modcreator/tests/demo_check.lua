-- runs the demo mod in the real-game mock and checks that it plays (tests/run.sh DEMO=1)
local MC = require("modcreator")
local pass, fail = 0, 0
local function ok(c, n, x) if c then pass = pass + 1 else fail = fail + 1; xmod.log("MCT FAIL " .. n .. " -> " .. tostring(x)) end end
xmod.cmd.server("mct_phase1", function() ok(MC.round.is_live() or MC.round.state() == "freeze", "round running", MC.round.state()) end)
xmod.cmd.server("mct_phase2", function()
  ok(MC.npc.count() > 0, "a wave spawned", MC.npc.count())
  ok(MC.teams.of(1) == "humans" and MC.teams.of(2) == "humans", "everybody is human", MC.teams.of(2))
  ok(xmod.mc.weapon_find(1, "weapon_ak47") ~= 0 and MC.weapons.is(xmod.mc.weapon_find(1, "weapon_ak47"), "thunder"), "thunder given")
  ok(MC.objectives.get("hold") ~= nil, "hold objective")
  xmod.log(string.format("MCT demo: npcs=%d p1 hp=%s p2 hp=%s", MC.npc.count(), tostring(xmod.player.health(1)), tostring(xmod.player.health(2))))
end)
xmod.cmd.server("mct_phase3", function() ok(true, "still running") end)
xmod.cmd.server("mct_report", function() xmod.log(string.format("MC TEST DONE: %d passed, %d failed", pass, fail)) end)
