#!/bin/bash
# Builds everything into bin/:  server (Metamod plugin / xmod extension) and client (client.dll proxy), Linux i386 + Windows x86.
#   ./build.sh [linux|windows|all]        needs: g++ with -m32 support (g++-multilib), and for Windows i686-w64-mingw32-g++
#   HLSDK=<path to Valve's halflife SDK>  (default: deps/hlsdk, fetched by ./fetch_deps.sh)
set -e
cd "$(dirname "$0")"
H="${HLSDK:-deps/hlsdk}"; [ -d "$H/engine" ] || { echo "HL SDK not found at $H - run ./fetch_deps.sh or set HLSDK=..."; exit 1; }
WHAT="${1:-all}"; mkdir -p bin
SRV_INC="-Ithird_party/xmod -I$H/dlls -I$H/engine -I$H/common -I$H/pm_shared -I$H/public"
CL_INC="-Iclient -I$H/engine -I$H/common -I$H/public -I$H/pm_shared -I$H/dlls -I$H/cl_dll"
CXXF="-std=gnu++17 -O2 -fno-strict-aliasing -Wall -Wno-unused-function -Wno-misleading-indentation -Wno-write-strings -Wno-attributes -Wno-unknown-pragmas -Wno-endif-labels -Wno-parentheses"
CL_SRC="client/proxy.cpp client/nexus_client.cpp client/nx_render.cpp client/nx_platform.cpp"
if [ "$WHAT" = linux ] || [ "$WHAT" = all ]; then
	g++ -m32 -shared -fPIC -fvisibility=hidden $CXXF $SRV_INC server/nexus_mm.cpp -o bin/nexus_mm_i386.so -static-libgcc -static-libstdc++ -Wl,--no-undefined -Wl,--exclude-libs,ALL
	g++ -m32 -shared -fPIC -fvisibility=hidden $CXXF $CL_INC $CL_SRC -o bin/client.so -static-libgcc -static-libstdc++ -ldl -lm -Wl,--no-undefined -Wl,--exclude-libs,ALL
	g++ -m32 -shared -fPIC -fvisibility=hidden $CXXF -Iinclude examples/client_plugin/fps_plugin.cpp -o bin/fps_plugin.so -static-libgcc -static-libstdc++ -Wl,--exclude-libs,ALL
	echo "linux: bin/nexus_mm_i386.so bin/client.so bin/fps_plugin.so"
fi
if [ "$WHAT" = windows ] || [ "$WHAT" = all ]; then
	M=i686-w64-mingw32-g++
	$M -shared $CXXF $SRV_INC server/nexus_mm.cpp -o bin/nexus_mm.dll -static -static-libgcc -static-libstdc++ -Wl,--kill-at -Wl,--enable-stdcall-fixup
	$M -shared $CXXF $CL_INC $CL_SRC -o bin/client.dll -static -static-libgcc -static-libstdc++ -lshell32
	$M -shared $CXXF -Iinclude examples/client_plugin/fps_plugin.cpp -o bin/fps_plugin.dll -static -static-libgcc -static-libstdc++
	echo "windows: bin/nexus_mm.dll bin/client.dll bin/fps_plugin.dll"
fi
