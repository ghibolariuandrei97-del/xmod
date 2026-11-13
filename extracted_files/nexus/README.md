# Nexus 1.1.0 — a server-scriptable client extension for xmod (CS 1.6 / GoldSrc)

Nexus lets an **xmod Lua plugin draw and control things on the player's screen**: real HUDs (text with any font size,
UTF-8 incl. ăîâșț, boxes, progress bars, PNG/TGA/JPG images), labels glued to players and points in the world, animations
that run on the client (no traffic per frame), a mouse cursor with clickable UI, key capture without `bind`, a third-person
or fixed camera, per-player MP3 music, player-confirmed links (e.g. a YouTube song in their browser), and a data channel to
native **client plugins**.

It has two halves:

| part | file | where |
|---|---|---|
| server: Metamod plugin + xmod extension | `nexus_mm.dll` / `nexus_mm_i386.so` | `addons/nexus/dlls/` |
| Lua library + demo | `nexus.lua`, `nexus_demo.lua` | `addons/xmod/lib/`, `addons/xmod/plugins/` |
| client: proxy in front of the game's client library | `client.dll` / `client.so` | player's `cstrike/cl_dlls/` |
| optional native client plugins | `*.dll` / `*.so` + `nexus/plugins.ini` | player's `cstrike/nexus/` |

Players **without** the client simply don't see Nexus elements; nothing is ever sent to them, and the library falls back where it can
(menus become classic xmod menus). Full API for people and AI assistants: **[nexus.md](nexus.md)**.

## Install
See `dist/INSTALL.txt` (server: Metamod order *xmod first, then nexus*, plus a line `nexus` in `addons/xmod/configs/extensions.ini`;
player: rename `cl_dlls/client.dll` → `client_orig.dll` and drop the Nexus `client.dll` next to it).

## Build
```
./fetch_deps.sh            # Valve HL SDK headers into deps/hlsdk
./build.sh all             # Linux i386 (g++ -m32) + Windows x86 (i686-w64-mingw32-g++)  ->  bin/
./package.sh               # bin/ + lua/ -> dist/ (the layout users unpack)
XMOD=../xmod test/run_all.sh   # server test (fake HLDS + real xmod), client test (OSMesa, PNG snapshots), proxy test
```

## Layout
```
shared/nexus_proto.h       wire protocol (the ONLY things a server can make a client do)
server/nexus_mm.cpp        Metamod plugin + xmod extension (xmod.nexus.*), detection, per-player queues, rate limiting
client/proxy.cpp           client.dll/.so proxy: loads client_orig, forwards all 43 SDK functions, hooks 10 of them
client/nexus_client.cpp    message parser, elements, layout, world anchoring, input, cursor, camera, client plugin host
client/nx_render.cpp       OpenGL 1.1 renderer (state fully saved/restored), baked font atlas, stb_image loader
client/nx_platform.cpp     Windows/Linux differences
include/nexus_plugin.h     C ABI for native client plugins     examples/client_plugin/fps_plugin.cpp
lua/lib/nexus.lua          the Lua library                     lua/plugins/nexus_demo.lua
test/                      server/, client/, proxy/ tests      tools/gen_font.py (font atlas generator)
```

## Troubleshooting the client
`cl_dlls/nexus_load.log` is rewritten at every game start and says exactly what happened (folder, which file was loaded,
Windows error code, slots found, what the original `Initialize` returned).
* **"could not link client DLL for HUD initialization"** (engine error) = the original client library could not be used.
  Nexus shows its own message box first (it can end up behind the game window) and writes the reason to `nexus_load.log`.
  Usual causes: `client_orig.dll` missing, or Windows hid the extension and the file became `client_orig.dll.dll`
  (Nexus 1.0.1+ accepts that too), or `client_orig.dll` is itself a copy of Nexus.
* Undo: delete the Nexus `client.dll`, rename `client_orig.dll` back to `client.dll` (or Steam → Verify integrity).

## Honest status
* Tested here: the server extension inside a fake HLDS/Metamod with the **real xmod 0.3.0** (handshake, limits, pacing, events, menus,
  fallback); the client core + renderer on an off-screen OpenGL (Mesa) with PNG snapshots; the shipped 32-bit `client.so`
  loaded like the engine does, with a fake original client library behind it; the shipped Windows `client.dll` the same way
  under Wine (fake engine + fake original `client_orig.dll`); 30 000 random/hostile messages.
* **Not tested here:** a real HLDS, a real CS 1.6 client (Windows or Linux) with the real `client.dll`, Steam/VAC.
  `nexus_mm.dll` was never run. Camera, cursor and world labels are checked only in the fake engine — treat them as experimental until you try them in game.
* Replacing `client.dll` is something players do at their own risk. Nexus does no memory patching, no signature scanning and touches
  no engine code (only the public SDK interface), but I cannot tell you how VAC or any anti-cheat treats a replaced client library.
* About MetaHookSv: it is a client-side framework (plugin loader, new renderer, engine hooks) that players install for themselves.
  Nexus is a different kind of thing: a client extension **driven by the server's Lua plugins**, deliberately limited to a small,
  validated set of operations so a server can never run commands, change cvars, read files or load code on a player's PC.
  It does not replace the engine renderer.

Third-party: stb_image (public domain / MIT, `client/third_party/STB_LICENSE.txt`), DejaVu Sans Bold glyphs baked into
`client/font_data.h` (DejaVu font license, free to embed), Valve HL SDK headers (not included; fetched by `fetch_deps.sh`).
