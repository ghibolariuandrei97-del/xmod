#!/bin/sh
# Fetches the build dependency (Valve's HL SDK headers) into ./deps.   The server tests also need a built xmod (XMOD=<path>).
set -e
mkdir -p deps && cd deps
[ -e hlsdk ] || git clone --depth 1 https://github.com/ValveSoftware/halflife.git hlsdk
