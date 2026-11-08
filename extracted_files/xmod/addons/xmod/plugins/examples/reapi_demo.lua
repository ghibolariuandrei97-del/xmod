-- REAPI EXAMPLE: ReGameDLL hookchains (needs ReGameDLL_CS on the server). Full list: docs/REAPI_HOOKS.md
-- Event name:  reapi:<Hook>[:post]    return xmod.HC_SUPERCEDE[, value] to skip the original, xmod.HC_BREAK to stop other plugins.
xmod.register("ReAPI demo", "0.1", "xmod")

if not xmod.reapi.available() then
  xmod.log("ReGameDLL not detected - this example does nothing. (Use examples/ham_demo.lua instead.)")
  return
end


xmod.on("reapi:CBasePlayer_TakeDamage", function(victim, inflictor, attacker, damage, bits)
  if attacker == victim then return xmod.HC_SUPERCEDE, 0 end     -- no self damage: skip original, return 0
  xmod.hook.set_arg(3, damage * 1.25)                            -- +25% damage for everybody
end)

xmod.on("reapi:CBasePlayer_Jump", function(id)
  -- fires every time a player presses jump while on ground
end)

xmod.on("reapi:CSGameRules_RestartRound", function() xmod.msg.chat(0, "{g}[demo] {y}New round!") end)

-- Block buying: HasRestrictItem(item_id, item_type) -> return nonzero to forbid
xmod.on("reapi:CBasePlayer_HasRestrictItem", function(id, item, itype)
  -- return xmod.HC_SUPERCEDE, 1
end)
