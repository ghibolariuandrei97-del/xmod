#!/bin/bash
# Builds the Nexus CLIENT (players' files): client.dll / client.so proxy + the example client plugin  ->  nexus/bin/
#   needs g++-multilib (Linux, -m32) and i686-w64-mingw32-g++ (Windows). HL SDK: ../deps/hlsdk (../fetch_deps.sh)
set -e; cd "$(dirname "$0")"; H="${HLSDK:-../deps/hlsdk}"; [ -d "$H/engine" ] || { echo "HL SDK not found at $H - run ../fetch_deps.sh"; exit 1; }
WHAT="${1:-all}"; mkdir -p bin
CL_INC="-Iclient -I$H/engine -I$H/common -I$H/public -I$H/pm_shared -I$H/dlls -I$H/cl_dll"
CXXF="-std=gnu++17 -O2 -fno-strict-aliasing -Wall -Wno-unused-function -Wno-misleading-indentation -Wno-write-strings -Wno-attributes -Wno-unknown-pragmas -Wno-endif-labels -Wno-parentheses"
SRC="client/proxy.cpp client/nexus_client.cpp client/nx_render.cpp client/nx_platform.cpp"
if [ "$WHAT" = linux ] || [ "$WHAT" = all ]; then
	g++ -m32 -shared -fPIC -fvisibility=hidden $CXXF $CL_INC $SRC -o bin/client.so -static-libgcc -static-libstdc++ -ldl -lm -Wl,--no-undefined -Wl,--exclude-libs,ALL
	g++ -m32 -shared -fPIC -fvisibility=hidden $CXXF -Iinclude examples/client_plugin/fps_plugin.cpp -o bin/fps_plugin.so -static-libgcc -static-libstdc++ -Wl,--exclude-libs,ALL
fi
if [ "$WHAT" = windows ] || [ "$WHAT" = all ]; then
	M=i686-w64-mingw32-g++
	$M -shared $CXXF $CL_INC $SRC -o bin/client.dll -static -static-libgcc -static-libstdc++ -lshell32
	$M -shared $CXXF -Iinclude examples/client_plugin/fps_plugin.cpp -o bin/fps_plugin.dll -static -static-libgcc -static-libstdc++
fi
echo "OK: $(ls bin | tr '\n' ' ')"
