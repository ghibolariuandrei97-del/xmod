xmod.register("hello test", "1", "test")
local hello = xmod.hello                      -- stable table (listed in extensions.ini during the test); the extension loads AFTER this script
local R = { pass = 0, fail = 0 }
local function check(name, cond, d) if cond then R.pass = R.pass + 1; xmod.log("PASS " .. name) else R.fail = R.fail + 1; xmod.log("FAIL " .. name .. "  " .. tostring(d or "")) end end
local frames = 0
xmod.on("frame", function() frames = frames + 1 end)
if not xmod.ext.available("hello") then       -- true only on the first run (after `xmod reload` the extension is already attached)
  local ok0, err0 = pcall(function() return hello.add(1, 2) end)
  check("calling before the extension is loaded gives a clear error", (not ok0) and tostring(err0):find("not available") ~= nil, err0)
end

xmod.on("config", function()
  check("sync add", hello.add(2, 3.5) == 5.5)
  local info = hello.info(); check("sync table result", info.name == "xmod_hello" and info.abi == 1 and info.is_main == true)
  check("extension error raises a Lua error", not pcall(hello.add))
  check("async function without callback raises", not pcall(hello.echo, 10, "x"))
  check("unsupported argument type raises", not pcall(hello.reverse, function() end, function() end))

  local f0 = frames
  hello.echo(200, "hi", function(err, res)
    check("async echo result", err == nil and res.text == "hi" and res.waited == 200 and res.on_main == false, err)
    check("callback runs on the main thread", hello.info().is_main == true)
    check("game kept running while the worker slept (frames advanced)", frames - f0 > 15, frames - f0)
  end)
  hello.echo(9999, "x", function(err, res) check("job_fail reaches the callback as err", err == "ms must be 0..5000" and res == nil, err) end)
  hello.never_replies(function(err) check("function that returns without a result gives an error", err ~= nil and err:find("without a result") ~= nil, err) end)

  local n7, ok7, n8, ok8, first7, first8 = 0, true, 0, true, nil, nil
  for i = 1, 40 do      -- the extension keeps its counters across reloads, so compare relative to the first reply
    hello.counter_add(7, 1, function(err, v) n7 = n7 + 1; first7 = first7 or v; if err or v ~= first7 + (i - 1) then ok7 = false; xmod.log("strand 7 got " .. tostring(err or v)) end end)
    hello.counter_add(8, 2, function(err, v) n8 = n8 + 1; first8 = first8 or v; if err or v ~= first8 + 2 * (i - 1) then ok8 = false end end)
  end
  hello.reverse({ 1, 2, { a = "x", b = { true, false } }, "s" }, function(err, res)
    check("nested value round trip", err == nil and res[1] == "s" and res[2].a == "x" and res[2].b[1] == false and res[2].b[2] == true and res[3] == 2 and res[4] == 1, err)
  end)
  local ticks = 0; xmod.on("hello_tick", function(i, total) ticks = ticks + 1 end)
  hello.ticker(5, 5, "hello_tick", function(err, res) check("ticker job result", res == 5, err) end)
  hello.shout("{g}[hello] {y}chat sent from a worker thread via color_chat", function(err, res) check("color_chat from a worker thread", res == true, err) end)
  local id = hello.echo(100, "cancel me", function() check("a canceled job must not call back", false) end)
  check("async.cancel returns true", xmod.async.cancel(id) == true)
  check("ext.available", xmod.ext.available("hello") and xmod.ext.available("hello", "add") and not xmod.ext.available("hello", "nope") and not xmod.ext.available("zzz"))
  local function find_ext(n) for _, e in ipairs(xmod.ext.list()) do if e.name == n then return e end end end   -- nexus (built in) is listed too
  local e = find_ext("xmod_hello"); check("ext.list", e and e.loaded == true and e.functions == 8)
  check("async.stats", type(xmod.async.stats().threads) == "number")
  check("reserved namespaces are protected", xmod.player.name ~= nil and xmod.ext.available("player") == false)
  xmod.timer.after(1.5, function()
    check("40+40 strand callbacks arrived in order", n7 == 40 and ok7 and n8 == 40 and ok8, n7 .. "/" .. tostring(ok7) .. " " .. n8 .. "/" .. tostring(ok8))
    check("5 events pushed from the worker thread", ticks == 5, ticks)
    xmod.log(("EXT TEST PART1 DONE: %d passed, %d failed"):format(R.pass, R.fail))
  end)
end)
xmod.cmd.server("t_check_add", function() check("functions still work after xmod reload", hello.add(1, 1) == 2); xmod.log(("EXT TEST FINAL: %d passed, %d failed"):format(R.pass, R.fail)) end)
xmod.cmd.server("t_slow", function() hello.echo(300, "dropped on reload", function() check("callback of a job started before a reload must be dropped", false) end) end)
xmod.cmd.server("t_check_unloaded", function()
  local ok, err = pcall(function() return hello.add(1, 2) end); check("calls after the extension unloaded fail clearly", not ok and tostring(err):find("not available") ~= nil, err)
  local function find_ext2(n) for _, e in ipairs(xmod.ext.list()) do if e.name == n then return e end end end
  check("ext.list shows it as unloaded", find_ext2("xmod_hello").loaded == false)
  xmod.log(("EXT TEST FINAL2: %d passed, %d failed"):format(R.pass, R.fail))
end)
