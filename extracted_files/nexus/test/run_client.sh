#!/bin/bash
# Client tests. Needs test/out/stream.bin (run_server.sh), libosmesa6-dev and python3.
#   1. the client core + renderer on an off-screen OpenGL (PNG snapshots in test/out/png)   2. the same without OpenGL   3. with a client plugin
set -e; cd "$(dirname "$0")"; ROOT=$(cd .. && pwd); H=${HLSDK:-$ROOT/deps/hlsdk}; O=out/cl; rm -rf $O out/png; mkdir -p $O/game $O/game_plug/nexus/plugins out/png
[ -f out/stream.bin ] || { echo "run test/run_server.sh first"; exit 1; }
INC="-I$ROOT/client -Iclient -I$H/engine -I$H/common -I$H/public -I$H/pm_shared -I$H/dlls -I$H/cl_dll"
g++ -std=gnu++17 -g -O1 -w $INC client/render_test.cpp "$ROOT"/client/nexus_client.cpp "$ROOT"/client/nx_render.cpp "$ROOT"/client/nx_platform.cpp -o $O/render_test -lOSMesa -ldl -lm
g++ -shared -fPIC -std=gnu++17 -w -I"$ROOT/include" "$ROOT"/examples/client_plugin/fps_plugin.cpp -o $O/game_plug/nexus/plugins/fps_plugin.so
cp -r "$ROOT"/assets/gfx $O/game/; cp -r "$ROOT"/assets/gfx $O/game_plug/; echo "fps_plugin" > $O/game_plug/nexus/plugins.ini
RC=0
$O/render_test out/stream.bin $O/game out/png > out/client_gl.log 2>&1 || RC=1
NOGL=1 $O/render_test out/stream.bin $O/game out/png > out/client_nogl.log 2>&1 || RC=1
mkdir -p out/png_plugin; EXPECT_PLUGIN=1 $O/render_test out/stream.bin $O/game_plug out/png_plugin > out/client_plugin.log 2>&1 || RC=1
grep -hE "FAIL|TEST DONE" out/client_gl.log out/client_nogl.log out/client_plugin.log
[ $RC = 0 ] && echo "CLIENT TESTS PASSED" || { echo "CLIENT TESTS FAILED (see test/out/client_*.log)"; exit 1; }
