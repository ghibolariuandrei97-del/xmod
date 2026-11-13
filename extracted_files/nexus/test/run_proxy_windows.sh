#!/bin/bash
# Same as run_proxy.sh, but for the shipped Windows bin/client.dll, under Wine (needs wine32 + i686-w64-mingw32-g++).
set -e; cd "$(dirname "$0")"; ROOT=$(cd .. && pwd); H=${HLSDK:-$ROOT/deps/hlsdk}; O=out/pxw; rm -rf $O; mkdir -p $O/ok $O/missing $O/selfcopy $O/dblext
[ -f out/stream.bin ] || { echo "run test/run_server.sh first"; exit 1; }
M=i686-w64-mingw32-g++; INC="-I$ROOT/client -I$H/engine -I$H/common -I$H/public -I$H/pm_shared -I$H/dlls -I$H/cl_dll"
$M -std=gnu++17 -O1 -w $INC proxy/proxy_test.cpp -o $O/proxy_test.exe -static
$M -shared -w proxy/fake_orig.cpp -o $O/ok/client_orig.dll -static -static-libgcc
for d in ok missing selfcopy dblext; do cp "$ROOT/bin/client.dll" $O/$d/; done
cp "$ROOT/bin/client.dll" $O/selfcopy/client_orig.dll; cp $O/ok/client_orig.dll $O/dblext/client_orig.dll.dll
export WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml="
RC=0; for m in ok missing selfcopy dblext; do mode=$m; [ $m = dblext ] && mode=ok
  ( cd $O && timeout 120 wine proxy_test.exe $m ../stream.bin $mode ) > out/proxyw_$m.log 2>&1 || RC=1
  grep -ahE "FAIL|TEST DONE" out/proxyw_$m.log || { echo "windows proxy test ($m) crashed:"; tail -5 out/proxyw_$m.log; RC=1; }; done
[ $RC = 0 ] && echo "WINDOWS PROXY TESTS PASSED (Wine)" || { echo "WINDOWS PROXY TESTS FAILED (see test/out/proxyw_*.log)"; exit 1; }
