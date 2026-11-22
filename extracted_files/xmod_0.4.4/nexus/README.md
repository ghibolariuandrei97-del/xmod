# Nexus 2.0 — the client half of xmod (CS 1.6 / GoldSrc)

Nexus lets xmod Lua plugins work **on the player's PC**: real HUDs (any font size, UTF-8, boxes, bars, PNG/TGA/JPG images),
labels glued to players and places, client-side animations, a mouse cursor with clickable UI, keys without `bind`, camera
control, per-player MP3 music, player-confirmed links, and in Nexus 2: FOV / shake / sway / punch, input control (block, force,
sensitivity), per-viewer entity looks (hide, glow, colour), 3D lines/boxes/zones in the world, dynamic lights, particle effects,
positional sounds, screen tints, plus native client plugins with a data channel. Full API: **`xmod_skills.md`, Part II** (in the xmod root).

| part | where |
|---|---|
| server side | **built into xmod** (`src/nexus_server.cpp`) — nothing to install, Lua: `xmod.nexus.*` / `require("nexus")` |
| Lua library + demo plugins | `addons/xmod/lib/nexus.lua`, `addons/xmod/plugins/nexus/` |
| client (players) | `client/cstrike/cl_dlls/client.dll` (`client.so`) in the xmod package, built from `nexus/client/` |
| native client plugins (optional) | `client/cstrike/nexus/` — `plugins.ini` + `plugins/*.dll` |

Players without the client simply don't see Nexus things; nothing is sent to them and the library falls back where it can.

## Player install
Rename `cstrike/cl_dlls/client.dll` → `client_orig.dll` (Linux `client.so` → `client_orig.so`), copy the Nexus `client.dll` next to it.
Console: `nexus_info`. Problems: `cl_dlls/nexus_load.log` (rewritten every start) says exactly what happened.
Undo: delete the Nexus `client.dll` and rename `client_orig.dll` back (or Steam → Verify integrity of game files).
After a game update replaced `client.dll`, rename the new one again and put Nexus back.
Player switches: `nexus_enable 0`, `nexus_keys 0` (never report keys), `nexus_urls 0` (never show link prompts), `nexus_scale`.

## Build and test (from the xmod root)
```
./fetch_deps.sh && ./build_linux.sh          # xmod (with the Nexus server inside)
nexus/build_client.sh                         # client.dll + client.so + example client plugin -> nexus/bin/
nexus/test/run_all.sh                         # fake HLDS + real xmod, Mesa off-screen render (PNG snapshots), proxy tests (+ Wine)
```

## Layout
```
shared/nexus_proto.h      wire protocol: the ONLY things a server can make a client do
client/proxy.cpp          client.dll/.so proxy: loads client_orig, forwards all 43 SDK functions, hooks 13 of them
client/nexus_client.cpp   messages, elements, layout, world anchoring, input, cursor, camera, Nexus 2 effects, client plugins
client/nx_render.cpp      OpenGL 1.1 renderer (2D and 3D, state fully saved/restored), baked font, stb_image
client/nx_platform.cpp    Windows / Linux differences
include/nexus_plugin.h    C ABI for native client plugins (examples/client_plugin/fps_plugin.cpp)
test/                     host.cpp (fake HLDS), client/ (render test), proxy/ (proxy tests), nexus_test.lua
tools/gen_font.py         font atlas generator
```

## Honest status
* Tested here: the server module inside the real xmod in a fake HLDS; the client on Mesa off-screen OpenGL (with screenshots)
  and without OpenGL; the shipped 32-bit `client.so` and Windows `client.dll` loaded like the engine does (the latter under Wine).
* Confirmed in game by the user: Nexus 1 (HUD, menus, cursor, MOTD, wheel, links). **Nexus 2 effects are new and not yet tried in
  a real game.**
* Nexus does no memory patching or signature scanning (public SDK interface only), but how VAC treats a replaced client
  library is unknown — players install it at their own risk.
* Everything Nexus 2 does is client-side (feel and visuals). Rules that must hold (freeze, speed, who may shoot) must also be
  enforced on the server.

Third-party: stb_image (public domain / MIT, `client/third_party/STB_LICENSE.txt`), DejaVu Sans Bold glyphs baked into
`client/font_data.h` (DejaVu font license), Valve HL SDK headers (fetched by `fetch_deps.sh`).
