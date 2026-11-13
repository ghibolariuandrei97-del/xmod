xmod.on("ham:takedamage:player", function(victim, inf, att, dmg, bits)
  xmod.log(("PRE victim=%d attacker=%d dmg=%.1f bits=%d"):format(victim, att, dmg, bits))
  xmod.hook.set_arg(3, dmg * 2)                  -- args after 'this': 1=inflictor 2=attacker 3=damage 4=bits
end)
xmod.on("ham:takedamage:player:post", function(victim, inf, att, dmg, bits, ret) xmod.log(("POST dmg=%.1f ret=%d"):format(dmg, ret)) end)
xmod.on("ham:spawn:player", function(id) xmod.log("spawn hook, blocking"); return xmod.HC_SUPERCEDE end)
xmod.log("reapi available: " .. tostring(xmod.reapi.available()) .. ", ham funcs: " .. #xmod.ham.list())
local ok, err = pcall(xmod.on, "ham:nosuchfunc:player", function() end); xmod.log("bad hook -> " .. tostring(err))
local ok2, err2 = pcall(xmod.on, "reapi:CBasePlayer_Spawn", function() end); xmod.log("reapi without ReGameDLL -> " .. tostring(err2))
