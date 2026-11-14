Tests used while writing xmod bots (not needed on the server).

engine_test.cpp  C++ side against a fake engine (room, wall with a gap, step, crate):
                 analyzer, A*, .nav save/load, walking, jumping, aim spring, vision, burst, learning.
  Build (Linux, from this folder; XMOD = xmod source tree, H = $XMOD/deps/hlsdk):
  g++ -m32 -std=c++17 -w -fpermissive -I../extensions/xmod_bots -I$XMOD/include -I$H/common -I$H/dlls \
      -I$H/engine -I$H/pm_shared -I$H/public \
      engine_test.cpp ../extensions/xmod_bots/{main,body,nav,api,link}.cpp -ldl -o engine_test && ./engine_test

mock.lua + run.lua   xbots.lua + xbots_editor.lua inside a mock of xmod (2 minutes of simulated rounds).
  lua5.4 run.lua   (edit ROOT at the top of run.lua)

bot_ext_test.lua  loads with the REAL xmod in xmod's own test/mock_engine.cpp: put it in addons/xmod/plugins,
  add "xbots/xbots.lua" + "bot_ext_test.lua" to plugins.ini and "bot" to extensions.ini, then from xmod/test:
  QTEST=1 EXTLIBS="../addons/xmod/dlls/xmod_bots_mm_i386.so" ./mock      -> "BOTTEST DONE: 18 passed, 0 failed"
  (leave xbots_editor.lua out there: the mock engine has no precache functions)
