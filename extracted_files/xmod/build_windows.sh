#!/bin/sh
# Windows DLL cross-compiled on Linux with MinGW -> addons/xmod/dlls/xmod_mm.dll   (needs: g++-mingw-w64-i686 cmake)
# On a Windows PC with Visual Studio instead run:  cmake -B build-msvc -A Win32 && cmake --build build-msvc --config Release
set -e; cd "$(dirname "$0")"; [ -d deps/lua ] || ./fetch_deps.sh
cmake -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-i686.cmake -DCMAKE_BUILD_TYPE=Release && cmake --build build-win -j"$(nproc)"
mkdir -p addons/xmod/dlls && cp build-win/xmod_mm.dll addons/xmod/dlls/ && find build-win -name "*_mm.dll" ! -name xmod_mm.dll -exec cp {} addons/xmod/dlls/ \; && echo "OK: $(ls addons/xmod/dlls | tr '\n' ' ')"
