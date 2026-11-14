#!/bin/bash
# Nexus server test in a fake HLDS with the real xmod (Nexus built in). Needs ../../build_linux.sh first.
# Output: out/stream.bin = every Nexus message sent to player 1 (replayed by run_client.sh / run_proxy.sh).
set -e; cd "$(dirname "$0")"; NX=$(cd .. && pwd); XM=$(cd ../.. && pwd); H=${HLSDK:-$XM/deps/hlsdk}
[ -f "$XM/addons/xmod/dlls/xmod_mm_i386.so" ] || { echo "build xmod first (build_linux.sh)"; exit 1; }
rm -rf out/srv; mkdir -p out/srv/gd/addons/xmod/{plugins,lib,data,configs}; W=out/srv
cp "$XM"/addons/xmod/lib/*.lua $W/gd/addons/xmod/lib/; cp nexus_test.lua $W/gd/addons/xmod/plugins/
cp "$XM"/addons/xmod/configs/* $W/gd/addons/xmod/configs/ 2>/dev/null || true; echo "nexus_test.lua" > $W/gd/addons/xmod/configs/plugins.ini; : > $W/gd/addons/xmod/configs/extensions.ini
g++ -m32 -std=gnu++17 -g -w -I"$XM/include" -I$H/dlls -I$H/engine -I$H/common -I$H/pm_shared -I$H/public host.cpp -o $W/host -ldl
(cd $W && ./host "$XM/addons/xmod/dlls/xmod_mm_i386.so" - ../stream.bin) > out/server.log 2>&1 || true
grep -E "FAIL|TEST DONE|flood:|longest|ERROR" out/server.log | sed 's/^\[log\] //'
grep -q "NEXUS HOST TEST DONE: [0-9]* passed, 0 failed" out/server.log && grep -q "NEXUS LUA TEST DONE: [0-9]* passed, 0 failed" out/server.log && echo "SERVER TESTS PASSED" || { echo "SERVER TESTS FAILED (see nexus/test/out/server.log)"; exit 1; }
