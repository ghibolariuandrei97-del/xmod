#!/bin/bash
# Loads the shipped 32-bit bin/client.so the way the engine does, with a fake original client library behind it.
set -e; cd "$(dirname "$0")"; ROOT=$(cd .. && pwd); H=${HLSDK:-$ROOT/deps/hlsdk}; O=out/px; rm -rf $O; mkdir -p $O/ok $O/missing $O/selfcopy
[ -f out/stream.bin ] || { echo "run test/run_server.sh first"; exit 1; }
INC="-I$ROOT/client -I$H/engine -I$H/common -I$H/public -I$H/pm_shared -I$H/dlls -I$H/cl_dll"
g++ -m32 -std=gnu++17 -g -w $INC proxy/proxy_test.cpp -o $O/proxy_test -ldl
g++ -m32 -shared -fPIC -fvisibility=hidden -w proxy/fake_orig.cpp -o $O/ok/client_orig.so
cp "$ROOT/bin/client.so" $O/ok/; cp "$ROOT/bin/client.so" $O/missing/; cp "$ROOT/bin/client.so" $O/selfcopy/; cp "$ROOT/bin/client.so" $O/selfcopy/client_orig.so
RC=0; for m in ok missing selfcopy; do $O/proxy_test $O/$m out/stream.bin $m > out/proxy_$m.log 2>&1 || RC=1; grep -hE "FAIL|TEST DONE" out/proxy_$m.log || { echo "proxy test ($m) crashed"; RC=1; }; done
[ $RC = 0 ] && echo "PROXY TESTS PASSED" || { echo "PROXY TESTS FAILED (see test/out/proxy_*.log)"; exit 1; }
