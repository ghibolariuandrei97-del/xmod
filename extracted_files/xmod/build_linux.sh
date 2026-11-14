#!/bin/sh
# Linux 32-bit build -> addons/xmod/dlls/xmod_mm_i386.so      (needs: gcc-multilib g++-multilib cmake)
set -e; cd "$(dirname "$0")"; [ -d deps/lua ] || ./fetch_deps.sh
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j"$(nproc)"
mkdir -p addons/xmod/dlls && cp build/xmod_mm_i386.so addons/xmod/dlls/ && find build -name "*_mm_i386.so" ! -name xmod_mm_i386.so -exec cp {} addons/xmod/dlls/ \; && echo "OK: $(ls addons/xmod/dlls | tr '\n' ' ')"
