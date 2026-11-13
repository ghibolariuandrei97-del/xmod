#!/bin/sh
# Smoke test against a mock engine (needs the Linux build). Usage: ./build_linux.sh && cd test && ./run.sh
set -e; cd "$(dirname "$0")"; mkdir -p gd; ln -sfn ../../addons gd/addons
H=../deps/hlsdk; g++ -m32 -std=gnu++17 -g -w -I../include -I$H/dlls -I$H/engine -I$H/common -I$H/pm_shared -I$H/public mock_engine.cpp -o mock -ldl
./mock
