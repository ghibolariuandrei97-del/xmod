#!/bin/bash
# Server side test in a fake engine. Needs a built xmod (XMOD=<xmod folder>, default ../xmod next to this project) and ./build.sh linux.
# Output: test/out/stream.bin = every Nexus message the server sent (replayed by test/run_client.sh).
set -e; cd "$(dirname "$0")"; ROOT=$(cd .. && pwd); XMOD=${XMOD:-$ROOT/../xmod}; H=${HLSDK:-$ROOT/deps/hlsdk}
[ -f "$XMOD/addons/xmod/dlls/xmod_mm_i386.so" ] || { echo "xmod not found at $XMOD (set XMOD=...)"; exit 1; }
rm -rf out/srv; mkdir -p out/srv/gd/addons/xmod/{plugins,lib,data,configs}; W=out/srv
cp "$XMOD"/addons/xmod/lib/*.lua $W/gd/addons/xmod/lib/; cp "$ROOT"/lua/lib/nexus.lua $W/gd/addons/xmod/lib/; cp server/nexus_test.lua $W/gd/addons/xmod/plugins/
cp "$XMOD"/addons/xmod/configs/* $W/gd/addons/xmod/configs/ 2>/dev/null || true; echo "nexus_test.lua" > $W/gd/addons/xmod/configs/plugins.ini; echo nexus > $W/gd/addons/xmod/configs/extensions.ini
g++ -m32 -std=gnu++17 -g -w -I"$ROOT/third_party/xmod" -I$H/dlls -I$H/engine -I$H/common -I$H/pm_shared -I$H/public server/host.cpp -o $W/host -ldl
(cd $W && ./host "$XMOD/addons/xmod/dlls/xmod_mm_i386.so" "$ROOT/bin/nexus_mm_i386.so" ../stream.bin) > out/server.log 2>&1 || true
grep -E "FAIL|TEST DONE|flood:|longest|error|Error" out/server.log | sed 's/^\[log\] //'
grep -q "NEXUS HOST TEST DONE: [0-9]* passed, 0 failed" out/server.log && grep -q "NEXUS LUA TEST DONE: [0-9]* passed, 0 failed" out/server.log && echo "SERVER TESTS PASSED" || { echo "SERVER TESTS FAILED (see test/out/server.log)"; exit 1; }
