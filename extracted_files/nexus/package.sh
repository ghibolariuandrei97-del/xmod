#!/bin/bash
# Copies the built binaries + Lua files into dist/ (the layout users unpack). Run ./build.sh all first.
set -e; cd "$(dirname "$0")"; rm -rf dist; S=dist/server/cstrike; C=dist/client/cstrike
mkdir -p $S/addons/nexus/dlls $S/addons/xmod/lib $S/addons/xmod/plugins $S/gfx/nexus $C/cl_dlls $C/nexus/plugins
cp bin/nexus_mm.dll bin/nexus_mm_i386.so $S/addons/nexus/dlls/
cp lua/lib/nexus.lua $S/addons/xmod/lib/; cp lua/plugins/nexus_demo.lua lua/plugins/nexus_motd.lua lua/plugins/nexus_wheel.lua lua/plugins/nexus_song.lua $S/addons/xmod/plugins/; cp assets/gfx/nexus/logo.png $S/gfx/nexus/
cp bin/client.dll bin/client.so $C/cl_dlls/
cp bin/fps_plugin.dll bin/fps_plugin.so $C/nexus/plugins/
printf '; Nexus client plugins: one name per line (file nexus/plugins/<name>.dll on Windows, .so on Linux).\n; Only you can enable plugins - servers cannot add or switch them on.\n;fps_plugin\n' > $C/nexus/plugins.ini
cat > dist/INSTALL.txt <<'TXT'
NEXUS - quick install (details: README.md)

SERVER (needs Metamod + xmod 0.3.0 or newer)
  1. copy dist/server/cstrike/* into your cstrike folder
  2. addons/metamod/plugins.ini - xmod FIRST, nexus after it:
        win32 addons/xmod/dlls/xmod_mm.dll
        linux addons/xmod/dlls/xmod_mm_i386.so
        win32 addons/nexus/dlls/nexus_mm.dll
        linux addons/nexus/dlls/nexus_mm_i386.so
  3. addons/xmod/configs/extensions.ini - add a line:   nexus
  4. addons/xmod/configs/plugins.ini - add:   nexus_demo.lua / nexus_motd.lua / nexus_wheel.lua / nexus_song.lua   (optional demos)
  5. restart, then type "nexus" in the server console

PLAYER (optional - players without it can still play, they just do not see the Nexus HUD)
  1. in cstrike/cl_dlls rename client.dll -> client_orig.dll   (Linux: client.so -> client_orig.so)
  2. copy dist/client/cstrike/cl_dlls/client.dll (or client.so) into cstrike/cl_dlls
  3. start the game, type "nexus_info" in the console
  Undo: delete client.dll, rename client_orig.dll back (or Steam: verify integrity of game files).
  After a game update replaced client.dll, rename the new one again and put Nexus back.

TROUBLESHOOTING
  "could not link client DLL for HUD initialization" / game closes at start = Nexus could not use client_orig.dll.
  Read cstrike/cl_dlls/nexus_load.log - it logs every step (missing file, client_orig.dll.dll, Windows error code,
  last step reached before a crash). Lua errors: xmod 0.3.1+ prints them to the console and to
  addons/xmod/data/xmod_errors.log; "xmod plugins" shows why a plugin is DISABLED.
TXT
echo "dist ready"; find dist -type f | sort
