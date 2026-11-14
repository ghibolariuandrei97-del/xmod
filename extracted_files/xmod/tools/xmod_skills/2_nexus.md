# Part II — Nexus 2.0 (client extension, built into xmod)

Nexus = a module **built into xmod** + a client library players install (`client/` in the xmod package). Lua plugins use it through
`local N = require("nexus")`: they draw HUD elements, world labels, menus, read keys and mouse clicks, move the camera, play music,
open links (player-confirmed), change the player's view (FOV, shake, sway, punch) and input (block / force / sensitivity), change how
entities look **for that player only**, draw 3D shapes in the world, add dynamic lights, particle effects and positional sounds, and
talk to native client plugins. Players without the client simply do not see these things.

---------------------------------------------------------------------------------------------------------------------------
## N1. Rules an AI must follow

1. Use the library: `local N = require("nexus")`. Raw `xmod.nexus.*` exists (section N6) but the library adds names, colours,
   style memory, callbacks and fallbacks.
2. **Build the HUD in `N.on_ready(function(id, info) ... end)`**. It fires when the player's client finished the handshake
   (≈2 s after joining) and **again after every map change** (the client drops all elements on map change). Never build HUDs in
   `client_putinserver` — the client is not ready yet and the calls just address 0 players.
3. Every function takes a player id; **0 = every player that has Nexus**. Players without the client are skipped silently
   (calls return the number of players addressed). Always keep a non-Nexus path for anything important (chat, classic menu).
   `N.has(id)` tells you whether a player has it.
4. Elements are **persistent**: create once, then update only what changed (`N.set(id, "hp", {value = 0.4})`). Do **not**
   re-send everything every frame. Prefer `N.anim` (runs on the client) over timers that move things.
   Update at most a few times per second per element; the server paces traffic (`nexus_rate`, default 5000 B/s per player).
5. Positions: `x, y` are fractions of the screen (0..1), `px, py` are virtual pixels (the screen is 768 virtual pixels high,
   × the player's `nexus_scale`). Use `anchor` to stick things to corners/edges so they work on every resolution.
6. Element names are global across all plugins (one library instance). Prefix them with your plugin name: `"shop.bg"`.
7. Paths (images, sounds, music) are relative, `[A-Za-z0-9_./-]`, no `..`, max 63 chars. Files must exist on the client:
   precache/download them (`xmod.precache.generic("gfx/myplugin/icon.png")` in the `precache` event; sounds with
   `xmod.precache.sound`). Music: `.mp3` relative to the game dir (e.g. `sound/myplugin/theme.mp3`, precache_generic it).
8. Text is UTF-8, max 1024 bytes per element, max 64 lines. Escape player-provided text with `N.escape()` when `markup = true`.
9. Keys/cursor change how the game feels: only capture keys while a feature needs them, release them after (`N.unbind`,
   `N.cursor(id, false)`). ESC and the console key can never be captured; players can turn key reporting off (`nexus_keys 0`).
10. Keep a plugin's UI state in **local tables of the plugin** (`local open = {}`), not `xmod.player.data`: with xmod 0.3.0,
   code called back by the library (`on_click`, binds, `on_ready`) runs as `lib:nexus`, so `xmod.player.data` there returns the
   library's table. xmod 0.3.1 fixes that (ownership follows the code), local tables work with both.
11. `N.unbind(id)` without a key removes **every** bind of that player, also other plugins' binds: unbind your own keys by name.
12. Errors in your callbacks are caught by the library and logged as `[lib:nexus] error in on_click callback: plugins/x.lua:LINE: ...`.
13. Nexus 2 effects are **client-side**: perfect for feel and visuals, but a modified client can ignore them. Anything that must be
   enforced (a freeze, a speed limit, who can shoot) must ALSO be enforced on the server (e.g. `xmod.ent.set(id, "maxspeed", 1)`).
   Check `N.info(id).fx` before relying on them (older clients ignore them; the calls just address 0 players).
14. Never trust client data: `nexus_data` / click / key events come from the player's PC. Validate everything.

---------------------------------------------------------------------------------------------------------------------------
## N2. Install (summary)
Server: nothing to do — Nexus is part of xmod 0.4+ (`nexus.lua` ships in `addons/xmod/lib/`). If an older standalone
`nexus_mm` line is still in `addons/metamod/plugins.ini`, remove it. Console: `nexus` shows every player's state.
Player: rename `cstrike/cl_dlls/client.dll` → `client_orig.dll` (Linux `client.so` → `client_orig.so`), copy Nexus
`client.dll`/`client.so` there. Console: `nexus_info`. Load problems: read `cl_dlls/nexus_load.log` (rewritten every start).

---------------------------------------------------------------------------------------------------------------------------
## N3. Lua library — `local N = require("nexus")`

### Players
| function | returns |
|---|---|
| `N.available()` | the server extension is loaded |
| `N.has(id)` | player has a ready Nexus client |
| `N.info(id)` | `{version, proto, w, h, gl, cursor, world, images, urls, caps}` or nil. `urls=false` → the player switched links off (or client < 1.1). `gl=false` → software renderer: **images are not drawn**, text uses the engine font |
| `N.players()` | list of ids with Nexus |
| `N.on_ready(fn(id, info))` | register; also called at once for players already ready |
| `N.on_lost(fn(id))` | disconnect / map change / client gone |

### Elements
`N.set(id, name, props)` creates or updates; only given props change. `name` = string (→ id 20001..60000) or a number
(raw id 1..20000 is yours; ids never collide with names). Shortcuts set the type: `N.text`, `N.rect`, `N.image`, `N.bar`
(`(id, name, props)`). `N.remove(id, name)`, `N.clear(id [, group])`, `N.hide(id, name)`, `N.show(id, name)`.
Limit: 512 elements per player on the client.

| prop | meaning |
|---|---|
| `type` | `"text"` `"rect"` `"image"` `"bar"` (needed the first time; shortcuts set it) |
| `x, y` | position, fraction of screen width/height, measured from the anchor point |
| `px, py` | extra offset in virtual pixels (768 = screen height) |
| `anchor` | `"topleft"` (default) `"top"` `"topright"` `"left"` `"center"` `"right"` `"bottomleft"` `"bottom"` `"bottomright"` — the screen point **and** the point of the element placed there. `anchor="bottomright", px=-10, py=-10` → 10 px from that corner |
| `w, h` / `pw, ph` | size (fraction / virtual px). rect & bar need a size. **text:** width = wrap width (word wrap). **image:** omitted → natural size; only one given → keeps aspect |
| `color` | `"#RRGGBB"`, `"#RRGGBBAA"`, name (`white black red green blue yellow orange cyan magenta gray gold ct t spec`), `{r,g,b[,a]}` |
| `color2` | text: background box (padded) · bar: track · rect with `outline=true`: border colour |
| `scale` | text/image size factor; text is 16 virtual px high at 1 |
| `text` | UTF-8, `\n` = new line |
| `markup` | `true`: `^1` red `^2` green `^3` yellow `^4` blue `^5` cyan `^6` magenta `^7` white `^8` gray `^9` orange `^0` element colour, `^^` = `^` |
| `shadow`, `outline` | text drop shadow / 8-way outline (rect: border, uses color2) |
| `image` | `.png .tga .bmp .jpg` path below the game dir (≤2048×2048; 256 different images per client) |
| `value` | bar fill 0..1 (horizontal if w ≥ h, else vertical, filling upwards) |
| `layer` | -127..127, higher draws on top (default 0; menus use 90+) |
| `group` | 0..254, for `N.clear(id, group)`. **250 is used by N.menu** |
| `additive` | additive blending (images) |
| `hidden` | keep but don't draw |
| `on_click` | `function(id, name, button)` (button 1 left, 2 right, 3 middle). Needs the cursor on (`N.cursor`). Hover is highlighted. `false` removes it |
| `ent` + `offset={x,y,z}` | glue to an entity (players: their id). Hidden when the entity is not sent to that client (out of PVS / dead / gone) |
| `pos3={x,y,z}` | glue to a fixed world point |
| `maxdist` | world elements: hidden beyond this distance (fades the last 15 %) |
| `occlude` | world elements: hidden behind world geometry (trace from the eye) |
| `distscale` | world elements: smaller when far (×0.35..1.6) |
| `life` | seconds, then the element deletes itself on the client |
| `fadein`, `fadeout` | seconds (fadeout = last part of `life`) |

World elements still use `anchor` for alignment around the projected point (`"bottom"` = centred above it).

### Animation (runs on the client)
`N.anim(id, name, {target..., time = 0.5, ease = "out"|"linear"|"inout", remove = false})`
targets: `x`+`y` (both), `px`+`py` (both), `alpha` 0..255, `scale`, `value`, `color`. Several targets in one call run together.
A new animation of the same property replaces the old one; setting `x/y/px/py` with `N.set` cancels position animations.
`remove = true` deletes the element when done. Max 8 running animations per element.

### Ready-made
| function | |
|---|---|
| `N.toast(id, text [, {time=3, color, y=0.22, scale=1.3, markup, box=true}])` | centred message, slides in, fades out |
| `N.label3d(id, name, ent, text [, {offset={0,0,40}, color, maxdist=1500, occlude=true, distscale=true, scale, markup, layer, group}])` | name tag above an entity |
| `N.float_text(id, {x,y,z}, text [, {color="yellow", time=1, rise=40, scale=1.2, maxdist=2500}])` | damage-number style text that rises and fades |
| `N.menu(id, menu)` | see below; returns `"nexus"`, `"classic"` (fallback) or `false` |
| `N.menu_close(id)`, `N.menu_open(id)` | |

Examples: `nexus_motd.lua` (paged window with buttons), `nexus_wheel.lua` (radial menu, cooldown bars, world markers), `nexus_song.lua` (YouTube links). A complete windowed UI (pages, buttons, keyboard, fallback) is in `lua/plugins/nexus_motd.lua` — copy its `put()` / `button()` helpers for your own windows.

`menu = { title, items = { {label, value [, disabled=true]}, ... }, on_select = fn(id, label, value) (return true to keep it open),
on_close = fn(id, reason) ("exit" | "replaced" | "lost" | "timeout"), cursor = false (true = mouse + clickable rows),
timeout, anchor = "left", x, y, px = 16, width = 300, bg, border, color, title_color }`.
7 items per page, keys 1-7 pick, 8/9 page, 0 exit. While open, digit keys are captured (weapon slots don't switch).
Players without Nexus get xmod's classic menu (`require("menu")`) with the same `on_select`.

### Input, camera, sound
| function | |
|---|---|
| `N.bind(id, key, fn(id, keynum, down) [, block])` | key: `"f"`, `"F3"`, `"TAB"`, `"MOUSE4"`, `"MWHEELUP"`… or a number. `block=true`: the game doesn't see it. Max 48 keys per player. Keys typed in chat/console are not seen |
| `N.unbind(id [, key])` | one key or all |
| `N.keynum(key)`, `N.KEY` | key numbers (letters = lower-case ASCII; `F1`=135…`F12`=146, `MOUSE1`=241) |
| `N.cursor(id, on)` | mouse cursor; while on, the view is frozen, attack is blocked, clicks go to `on_click` |
| `N.camera(id, {mode="third", dist=120, height=0, side=0, collide=true, time=0.4})` | third person (experimental) |
| `N.camera(id, {mode="fixed", pos={x,y,z}, angles={pitch,yaw,roll}, time})` | fixed camera; `N.camera(id)` = back to normal |
| `N.sound(id, "path.wav" [, vol 0..1])` | local sound, path below `sound/` |
| `N.music(id, "sound/x/theme.mp3" [, loop])` / `N.music(id)` stop | per-player MP3 |
| `N.send(id, channel, data)` | to native client plugins; channel ≤31 chars, data ≤96 bytes (binary ok) |
| `N.open_url(id, url, label [, cb(id, result)])` | ask to open an **https** link in the player's browser. The CLIENT shows its own prompt (host name big, Y = open, N/ESC = no, 20 s timeout); the server can't answer it. `result`: `opened` `declined` `disabled` `busy` `bad_url` `no_nexus`. Max 200 chars, one prompt at a time, 5 s apart. Fullscreen players must Alt+Tab to the browser |
| `N.youtube(id, words [, cb])` | `open_url` of a YouTube search for `words` |
| `N.urlencode(text)` | for query strings (`%20` etc.) |
| `N.on_data(channel, fn(id, data))` | from client plugins |

### Nexus 2 — view, input, entities, world (client-side; need `N.info(id).fx`)
| function | |
|---|---|
| `N.fov(id, fov [, seconds=0.25])` / `N.fov(id, 0 [, s])` | smooth zoom / widen; 0 = back to the game's fov (the game's own zoom still works underneath) |
| `N.shake(id [, degrees=2, frequency=12, seconds=0.8])` | camera shake that fades out |
| `N.sway(id, degrees [, speed=1])` / `N.sway(id, 0)` | slow drunk / underwater sway |
| `N.punch(id, pitch, yaw)` | view kick that settles by itself (recoil / hit feedback) |
| `N.input(id, {block = {...}, force = {...}, sens = 1, invert_x, invert_y})` / `N.input(id)` | `block`: `attack attack2 jump duck move strafe use reload look`; `force`: `duck walk jump attack attack2 use`; `sens` mouse multiplier (0..10); `look` freezes the view |
| `N.freeze(id, on)` · `N.drunk(id, strength)` | presets of the above (`N.drunk(id, 0)` = sober) |
| `N.entity(viewer, ent, {hide = true} / {rendermode=, renderamt=, color=, renderfx=, scale=})` | how one entity looks **for this viewer only**. `rendermode`: `normal color texture glow alpha add`; `renderfx`: `glowshell hologram pulse_fast distort ...`; `N.entity(viewer, ent)` removes; `N.clear_entities(viewer)` |
| `N.glow(viewer, ent, color [, thickness=20])` / `N.glow(viewer, ent)` | glowing outline (team highlights, objectives) |
| `N.shape(id, name, {type="line", from=, to=} / {type="box"|"quad", mins=, maxs=} / {type="circle"|"cylinder", center=, radius=, height=, segments=}, color=, width=2, fill, xray (through walls), additive, pulse, ent=e (coords become offsets, follows it), life=s)` | 3D lines / boxes / zones in the world (OpenGL only), max 256 |
| `N.remove_shape(id, name)` | |
| `N.light(id, name, {pos= | ent=, offset=, radius=200, color=, life=s})` / `N.remove_light(id, name)` | coloured dynamic light, can follow an entity, max 32 |
| `N.effect(id, kind, {pos=, to=, color=, size=, count=, life=})` | particles: `explosion sparks teleport lava blob burst implosion beam` (beam needs `to`; burst `color` = palette index in r) |
| `N.sound3d(id, "path.wav", {pos= | ent=, volume=1, attn=0.8, pitch=100})` | sound heard from a place, only by this player |
| `N.tint(id, color [, fade=0.3, hold])` / `N.tint(id)` | full-screen colour (night vision, damage, underwater) |

Example: `nexus_fx.lua` (say /fx) uses every one of these.

### Helpers
`N.color(c [, alpha])` → `{r,g,b,a}` · `N.escape(text)` (for markup) · `N.id(name)` · `N.temp_id()` (60001..65535, for elements
with `life`).

---------------------------------------------------------------------------------------------------------------------------
## N4. Examples

```lua
xmod.register("myhud", "1.0", "me")
local N = require("nexus")

N.on_ready(function(id)
  N.rect(id, "myhud.bg",  { anchor = "bottomleft", px = 16, py = -16, pw = 220, ph = 40, color = {0, 0, 0, 160}, color2 = "gold", outline = true })
  N.text(id, "myhud.lvl", { anchor = "bottomleft", px = 28, py = -44, text = "Level 1", color = "gold", shadow = true, layer = 1 })
  N.bar(id,  "myhud.xp",  { anchor = "bottomleft", px = 28, py = -24, pw = 196, ph = 6, value = 0, color = "#4CE06A", color2 = {255,255,255,40}, layer = 1 })
end)

local function set_xp(id, level, frac)                     -- call when it changes, not every frame
  N.set(id, "myhud.lvl", { text = "Level " .. level })
  N.anim(id, "myhud.xp", { value = frac, time = 0.4 })
end

-- label over every player's head for every Nexus player
N.on_ready(function(viewer)
  for _, p in ipairs(xmod.player.all()) do
    if p ~= viewer then N.label3d(viewer, "myhud.tag." .. p, p, N.escape(xmod.player.name(p))) end
  end
end)
xmod.on("client_disconnect", function(p) N.remove(0, "myhud.tag." .. p) end)

-- a clickable menu with the mouse
xmod.cmd.say("/shop", "", function(id)
  N.menu(id, { title = "Shop", cursor = true, items = { {"AK-47", "ak"}, {"Armor", "armor"} },
    on_select = function(p, label, value) if value == "ak" then xmod.player.give_item(p, "weapon_ak47") end end })
end)

-- key without a bind
N.on_ready(function(id) N.bind(id, "F3", function(p, _, down) if down then N.toast(p, "F3 pressed") end end, true) end)
```

---------------------------------------------------------------------------------------------------------------------------
## N5. Events (also usable directly with `xmod.on`)
| event | args |
|---|---|
| `nexus_ready` | id, info |
| `nexus_lost` | id |
| `nexus_key` | id, keynum, down (bool) — only keys you subscribed |
| `nexus_click` | id, elem_id (0 = clicked on nothing), button 1..3 |
| `nexus_data` | id, channel, data (string, may contain zero bytes) |
| `nexus_url` | id, request, result (1 opened, 0 declined/timeout, 2 disabled, 3 busy, 4 bad url) — the library turns this into the `open_url` callback |

---------------------------------------------------------------------------------------------------------------------------
## N6. Raw extension API `xmod.nexus.*` (all synchronous; they only queue bytes)
`has(id)` · `info(id)` · `players()` · `set(id|0, elem 1..65535, props)` (props as the library, but `type` always required,
colours only as `{r,g,b,a}` tables, `click=true` instead of `on_click`; **the style fields `anchor layer group` + flags are one
block: if you send any of them, send all** — the library does that for you) · `del(id|0, elem)` · `clear(id|0 [, group])` ·
`anim(id|0, elem, {...})` · `keys(id|0, { keynum | {key=, block=} ... })` (replaces the list) · `cursor(id|0, bool)` ·
`sound(id|0, path [, vol])` · `music(id|0 [, path, loop])` · `camera(id|0, {...})` · `send(id|0, channel, data)` · `reset(id|0)` ·
`open_url(id, url, label)` → request id (0 = no Nexus, -1 = links not available) · `stats()` → `{ready, without_nexus, sent_bytes, sent_messages, queued_ops, dropped_ops, version, proto}`.
Errors (bad type, bad path, too long) raise Lua errors. Returns = number of players addressed.

---------------------------------------------------------------------------------------------------------------------------
## N7. Cvars and commands
Server: `nexus_rate` (bytes/s per player, default 5000, min 500) · `nexus_debug 1` (log handshakes) · console `nexus`.
Client: `nexus_enable 0/1` · `nexus_scale` (0.5..3, UI size) · `nexus_keys 0` (never report keys) · `nexus_cursor_speed` ·
`nexus_cl_debug` · `nexus_urls 0` (never show link prompts) · `nexus_version` (read by the server to detect Nexus) · command `nexus_info`.

---------------------------------------------------------------------------------------------------------------------------
## N8. Native client plugins (C/C++)
`include/nexus_plugin.h`. A plugin exports `int NexusPlugin_Init(const NexusApi *api, NexusPlugin *out)` and returns
`NEXUS_PLUGIN_ABI`. It is loaded only if the **player** lists it in `cstrike/nexus/plugins.ini`; the file is
`cstrike/nexus/plugins/<name>.dll` (`.so`). Servers can never install or enable plugins.
Callbacks: `on_frame`, `on_draw` (after Nexus elements; use `api->rect/text/text_width/image/image_draw/world_to_screen`,
real pixels, colours `0xRRGGBBAA`), `on_key` (return 1 = swallow), `on_data(channel, data, len)`, `on_session(ready)`,
`on_shutdown`. `api->send(channel, data, len)` → Lua `N.on_data`. `api->engine` = the SDK `cl_enginefunc_t`.
Example: `examples/client_plugin/fps_plugin.cpp` (FPS counter; answers channel `fps.query` with `fps.value`).

---------------------------------------------------------------------------------------------------------------------------
## N9. How it works / limits
* Detection: ~1.5 s after a player takes a slot the server queries the client cvar `nexus_version`; if present it sends HELLO with
  a random token, the client answers `nexus_hello <proto> <version> <w> <h> <caps> <token>`. Then `nexus_ready` fires.
  Old protocol → ignored (logged). Engines without client cvar queries → Nexus stays off.
* Server → client: user message `Nexus` (≤190 bytes, several ops each), per-player queue (max 4096 ops, updates of the same
  element are merged), token-bucket pacing so the reliable channel never overflows. Client → server: `nexus_*` console
  commands, limited to 80/s per player on the server and 60/s on the client.
* The client only does what `shared/nexus_proto.h` lists. Sound/music names are validated on both sides (no `;`, quotes,
  spaces, `..`, absolute paths); there is no op that executes commands or changes cvars.
* Drawing: after the game's HUD, OpenGL 1.1 on the engine's context with all state saved/restored; fallback to the engine's
  2D functions (software mode: rects + console font, no images).
* Client library: proxy `client.dll` that loads `client_orig.dll` from the same folder and forwards everything (all 43 SDK
  functions, `F`, `CreateInterface`); it hooks Initialize, HUD_Init, HUD_VidInit, HUD_Redraw, HUD_Frame, HUD_Key_Event,
  CL_CreateMove, CL_IsThirdPerson, V_CalcRefdef, HUD_Shutdown. No memory patching or signature scanning.
* Map change: server and client both drop the session; HELLO again → `nexus_ready` again.

## N10. Test status (be honest with users)
Tested in this repo: the server module inside xmod in a fake HLDS (72 checks incl. every Nexus 2 function and its error cases), the client core + renderer
on Mesa off-screen OpenGL and the engine fallback (~230 checks incl. 30 000 random messages, FOV / shake / input / entity overrides /
lights / particles / 3D sound, and a 3D render of shapes with an x-ray shape behind a wall), the shipped 32-bit `client.so` loaded like the engine,
and the shipped Windows `client.dll` the same way under Wine. **Not tested on a real game client or a real HLDS.** Nexus 1 features were
confirmed working by the user in game; Nexus 2 features (FOV, input control, entity overrides, shapes, lights, effects) are new and
need an in-game check. VAC behaviour of a replaced client.dll is unknown.
