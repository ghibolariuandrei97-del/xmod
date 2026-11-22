## Appendix C — Index of every xmod Lua function and constant (generated from the running build)

Every name below exists in xmod 0.4.0 (with Nexus built in and the xmod_query extension loaded). Sections above explain each one.
`x` is an alias of `xmod`. Extension functions (`xmod.query.*`) exist only when that extension is loaded.

- **`xmod.async`**: `cancel` · `stats`
- **`xmod`**: `call` · `emit` · `error` · `exec` · `export` · `flags` · `gametime` · `has_export` · `is_dedicated` · `log` · `map` · `map_valid` · `maxentities` · `maxplayers` · `off` · `on` · `plugins` · `register` · `server_print` · `time`
- **`xmod.cmd`**: `client` · `say` · `server`
- **`xmod.cvar`**: `create` · `exists` · `get` · `int` · `num` · `query_client` · `set` · `watch`
- **`xmod.ent`**: `classname` · `create` · `find` · `find_sphere` · `get` · `remove` · `set` · `set_model` · `set_origin` · `set_size` · `spawn` · `valid`
- **`xmod.ext`**: `available` · `list`
- **`xmod.fs`**: `exists` · `exists_game` · `read` · `read_game` · `write`
- **`xmod.fullpack`**: `clear` · `get_render` · `hide` · `hide_all` · `native` · `set_effects` · `set_origin` · `set_render`
- **`xmod.ham`**: `list`
- **`xmod.hook`**: `set_arg`
- **`xmod.kv`**: `del` · `get` · `keys` · `save` · `set`
- **`xmod.mem`**: `get_float` · `get_int` · `set_float` · `set_int`
- **`xmod.msg`**: `angle` · `begin` · `block` · `byte` · `char` · `chat` · `coord` · `entity` · `fade` · `finish` · `hud` · `long` · `print` · `shake` · `short` · `show_menu` · `string`
- **`xmod.nexus`**: `anim` · `camera` · `clear` · `cursor` · `del` · `effect` · `entity` · `fov` · `has` · `info` · `input` · `keys` · `light` · `music` · `open_url` · `players` · `punch` · `reset` · `send` · `set` · `shake` · `shape` · `sound` · `sound3d` · `stats` · `sway`
- **`xmod.player`**: `access` · `alive` · `all` · `armor` · `connected` · `data` · `find` · `frags` · `give_item` · `has_access` · `health` · `infokey` · `ingame` · `ip` · `is_bot` · `kick` · `kill` · `money` · `name` · `origin` · `ping` · `set_access` · `set_armor` · `set_frags` · `set_health` · `set_infokey` · `set_money` · `set_origin` · `set_team` · `set_velocity` · `steamid` · `team` · `team_name` · `userid` · `velocity` · `view_angles`
- **`xmod.precache`**: `generic` · `model` · `sound`
- **`xmod.reapi`**: `available` · `list`
- **`xmod.sound`**: `emit`
- **`xmod.stats`**: `enable` · `get` · `info` · `reset`
- **`xmod.timer`**: `after` · `cancel` · `every` · `times`
- **`xmod.trace`**: `contents` · `hull` · `line` · `line_n`
- **`xmod.vault`**: `list` · `open`
- **`xmod.query`**: `close` · `exec` · `http` · `http_get` · `http_post` · `open` · `profiles` · `reload_config` · `resolve` · `status` · `tcp_close` · `tcp_open` · `tcp_recv` · `tcp_request` · `tcp_send` · `udp_request`
- **`(vault object v = xmod.vault.open(name))`**: `set` · `get` · `has` · `remove` · `touch` · `timestamp` · `incr` · `keys` · `count` · `prune` · `clear` · `save` · `each`

Constants:
- **`xmod`**: `EF_BRIGHTLIGHT=2` · `EF_DIMLIGHT=8` · `EF_NODRAW=128` · `FL_CLIENT=8` · `FL_DUCKING=16384` · `FL_FAKECLIENT=8192` · `FL_FROZEN=4096` · `FL_GODMODE=64` · `FL_NOTARGET=128` · `FL_ONGROUND=512` · `FL_SPECTATOR=67108864` · `FX_DISTORT=15` · `FX_GLOWSHELL=19` · `FX_NONE=0` · `FX_PULSE_FAST=2` · `FX_PULSE_SLOW=1` · `HC_BREAK=2` · `HC_CONTINUE=0` · `HC_SUPERCEDE=1` · `IN_ATTACK=1` · `IN_ATTACK2=2048` · `IN_BACK=16` · `IN_DUCK=4` · `IN_FORWARD=8` · `IN_JUMP=2` · `IN_MOVELEFT=512` · `IN_MOVERIGHT=1024` · `IN_RELOAD=8192` · `IN_USE=32` · `MOVETYPE_FLY=5` · `MOVETYPE_NOCLIP=8` · `MOVETYPE_NONE=0` · `MOVETYPE_TOSS=6` · `MOVETYPE_WALK=3` · `RENDER_GLOW=3` · `RENDER_NORMAL=0` · `RENDER_TRANSADD=5` · `RENDER_TRANSALPHA=4` · `RENDER_TRANSCOLOR=1` · `RENDER_TRANSTEXTURE=2` · `SOLID_BBOX=2` · `SOLID_NOT=0` · `SOLID_TRIGGER=1` · `TEAM_CT=2` · `TEAM_SPEC=3` · `TEAM_T=1` · `TEAM_UNASSIGNED=0` · `TRACE_IGNORE_GLASS=256` · `TRACE_IGNORE_MONSTERS=1` · `TRACE_NORMAL=0` · `gamedir=gd` · `version=0.4.0`
- **`xmod.cvar`**: `ARCHIVE=1` · `PROTECTED=32` · `SERVER=4` · `SPONLY=64`
- **`xmod.mem`**: `LINUX_PLAYER=5`
- **`xmod.msg`**: `ALL=2` · `BROADCAST=0` · `INIT=3` · `ONE=1` · `ONE_UNRELIABLE=8` · `PAS=5` · `PVS=4` · `TEMPENTITY=23`
- **`xmod.sound`**: `CHAN_AUTO=0` · `CHAN_ITEM=3` · `CHAN_VOICE=2` · `CHAN_WEAPON=1`
