#!/bin/sh
# ModCreator tests: the real ReGameDLL cs.so + xmod + ModCreator in a mini engine/Metamod (mock_mc.cpp), driven by mc_test.lua.
#   GAMEDLL=/path/to/cs.so ./run.sh        (needs g++ -m32, a Linux build of xmod in addons/xmod/dlls and of ModCreator)
set -e; cd "$(dirname "$0")"; R=$(cd ../../.. && pwd); H=$R/deps/hlsdk
GAMEDLL=${GAMEDLL:-/home/claude/deps/bins/rg/bin/linux32/cstrike/dlls/cs.so}
MCSO=${MCSO:-$R/build-zig/extensions/modcreator/modcreator_mm_i386.so}
T=${KEEP:-$(mktemp -d)}; [ -n "$KEEP" ] || trap 'rm -rf $T' EXIT; rm -rf $T/run $T/addons; mkdir -p $T
mkdir -p $T/run/gd/sprites $T/run/gd/models $T/run/gd/sound/weapons $T/run/gd/sound/mct
cp -r $R/addons $T/addons; ln -s ../../addons $T/run/gd/addons
cp $MCSO $T/addons/xmod/dlls/; FS=${FSLIB:-$(dirname $GAMEDLL)/../../../../../rh/bin/linux32/filesystem_stdio.so}; [ -f "$FS" ] && cp $FS $T/run/
rm -f $T/addons/xmod/configs/plugins-*.ini $T/addons/xmod/configs/extensions-*.ini $T/addons/xmod/data/*.kv 2>/dev/null || true
cp $R/addons/xmod/configs/extensions-modcreator.ini $T/addons/xmod/configs/ 2>/dev/null || echo mc > $T/addons/xmod/configs/extensions-modcreator.ini
if [ -n "$DEMO" ]; then printf 'modcreator/mc_demo.lua\nmodcreator/examples/mc_ex_npc.lua\nmodcreator/examples/mc_ex_weapon.lua\nmodcreator/examples/mc_ex_koth.lua\ndemo_check.lua\n' > $T/addons/xmod/configs/plugins.ini; cp demo_check.lua $T/addons/xmod/plugins/
  mkdir -p $T/run/gd/models/player/terror $T/run/gd/models/player/leet $T/run/gd/models/player/gign $T/run/gd/sound/player; for f in models/player/terror/terror.mdl models/player/leet/leet.mdl models/player/gign/gign.mdl models/w_ak47.mdl sound/weapons/m4a1_unsil-1.wav sound/weapons/m3-1.wav sound/weapons/knife_hit1.wav sound/weapons/knife_hit2.wav sound/weapons/ak47-1.wav sound/player/die1.wav sound/player/die2.wav sound/player/die3.wav; do echo x > $T/run/gd/$f; done
else echo "mc_test.lua" > $T/addons/xmod/configs/plugins.ini; cp mc_test.lua $T/addons/xmod/plugins/; fi
for f in sprites/laserbeam.spr sprites/shockwave.spr sprites/steam1.spr sprites/zerogxplode.spr models/mc_test.mdl models/v_plasma.mdl models/p_plasma.mdl models/w_plasma.mdl sound/weapons/plasma.wav sound/mct/die.wav; do echo x > $T/run/gd/$f; done
g++ -m32 -std=gnu++17 -g -w -I$R/include -I$H/dlls -I$H/engine -I$H/common -I$H/pm_shared -I$H/public mock_mc.cpp -o $T/run/mock_mc -ldl
export GAMEDLL MCLIB=../addons/xmod/dlls/modcreator_mm_i386.so; cd $T/run; RC=0; ./mock_mc > $T/out.txt 2>&1 || RC=$?
grep -E "MCT|MC TEST|ENGINE ERROR|engine errors|error|FAIL|==" $T/out.txt | grep -v "^\[log\] \[xmod\] loaded" | head -150
cp $T/out.txt ${TMPDIR:-/tmp}/mc_test_out.txt
grep -q "MC TEST DONE: [0-9]* passed, 0 failed" $T/out.txt && [ $RC = 0 ] && echo "ALL MODCREATOR TESTS PASSED" || { echo "MODCREATOR TESTS FAILED (rc=$RC, full log: ${TMPDIR:-/tmp}/mc_test_out.txt)"; exit 1; }
