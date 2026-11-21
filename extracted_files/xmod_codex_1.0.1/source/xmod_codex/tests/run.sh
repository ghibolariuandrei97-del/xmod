#!/bin/sh
# Codex self test inside xmod's mock engine (Linux, needs gcc-multilib; run ../../../build_linux.sh first)
#   ./run.sh            -> 122 checks: every convention, hooks, faults, signatures, limits, cache
#   ./run.sh mass FILE  -> trampoline/detour/signature test on a real ELF (cs.so, engine_i486.so)
#   ./run.sh pemass FILE-> the same on a real Windows PE (mp.dll, swds.dll), mapped by hand
set -e; cd "$(dirname "$0")"; R=$(cd ../../.. && pwd); H=$R/deps/hlsdk; T=${TMPDIR:-/tmp}/codex_test; mkdir -p $T
INC="-I$H/dlls -I$H/engine -I$H/common -I$H/pm_shared -I$H/public -I$R/include -I.."
SRC="../main.cpp ../api.cpp ../func.cpp ../scan.cpp ../lde.cpp ../mem.cpp ../guard.cpp ../thunks.cpp ../cache.cpp"
case "$1" in
  mass)   g++ -m32 -std=c++17 -O1 -w -fpermissive $INC mass.cpp $SRC -ldl -o $T/mass && exec $T/mass "$2" ;;
  pemass) g++ -m32 -std=c++17 -O1 -w -fpermissive $INC pemass.cpp $SRC -ldl -o $T/pemass && exec $T/pemass "$2" ;;
esac
g++ -m32 -O2 -fPIC -shared -fno-omit-frame-pointer cxtarget.cpp -o $T/libcxtarget.so
g++ -m32 -std=gnu++17 -g -w $INC mock_codex.cpp -o $R/test/mock_codex -ldl
C=$R/addons/xmod/configs; cp $C/plugins.ini $T/plugins.ini; cp $C/extensions.ini $T/extensions.ini
restore() { cp $T/plugins.ini $C/plugins.ini; cp $T/extensions.ini $C/extensions.ini; rm -f $R/addons/xmod/plugins/codex_selftest.lua $R/test/mock_codex; rm -rf $R/test/gd $R/addons/xmod/data/codex $R/addons/xmod/data/admin.log $R/addons/xmod/data/xmod_errors.log; }
trap restore EXIT
cp codex_selftest.lua $R/addons/xmod/plugins/; echo "codex_selftest.lua" >> $C/plugins.ini; grep -q "^codex" $C/extensions.ini || echo codex >> $C/extensions.ini
cd $R/test; mkdir -p gd; ln -sfn ../../addons gd/addons
QTEST=1 CXTARGET=$T/libcxtarget.so EXTLIBS="../addons/xmod/dlls/xmod_codex_mm_i386.so" ./mock_codex > $T/out.txt 2>&1 || true
grep -E "CXTEST (FAIL|DONE|PHASE)" $T/out.txt
grep -q "CXTEST DONE: [0-9]* passed, 0 failed" $T/out.txt && grep -q "CXTEST PHASE2: [0-9]* passed, 0 failed" $T/out.txt && echo "CODEX SELF TEST PASSED" || { echo "CODEX SELF TEST FAILED (see $T/out.txt)"; exit 1; }
