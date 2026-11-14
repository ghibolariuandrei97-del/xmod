#!/bin/sh
# Fetches build dependencies into ./deps:  HLSDK headers + Lua 5.4.   (add --regen to also fetch ReGameDLL for tools/gen_reapi.py)
set -e
mkdir -p deps && cd deps
[ -d hlsdk ] || git clone --depth 1 https://github.com/ValveSoftware/halflife.git hlsdk
[ -d lua ]   || git clone --depth 1 --branch v5.4.6 https://github.com/lua/lua.git lua
if [ "$1" = "--regen" ]; then [ -d regamedll ] || git clone --depth 1 https://github.com/rehlds/ReGameDLL_CS.git regamedll; fi
