# Ham hooks available to xmod (CS 1.6 virtual functions)

Use `xmod.on("ham:<func>:<classname>", fn)` / `...:post`. Examples of classnames: `player`, `weapon_ak47`, `func_door`, `info_target`.
The first arg of `fn` is always the entity (`this`); then the args below. Pre-hooks may return `xmod.HC_SUPERCEDE, value`; `xmod.hook.set_arg(n, v)` changes numeric args (n counts after `this`). Post hooks get the return value as extra last arg.

| Function | Args | Returns |
|---|---|---|
| `spawn` | - | - |
| `precache` | - | - |
| `keyvalue` | int | - |
| `objectcaps` | - | int |
| `activate` | - | - |
| `setobjectcollisionbox` | - | - |
| `classify` | - | int |
| `deathnotice` | ent | - |
| `takedamage` | ent, ent, float, int | int |
| `takehealth` | float, int | int |
| `killed` | ent, int | - |
| `bloodcolor` | - | int |
| `istriggered` | ent | int |
| `mymonsterpointer` | - | ent |
| `mysquadmonsterpointer` | - | ent |
| `gettogglestate` | - | int |
| `addpoints` | int, int | - |
| `addpointstoteam` | int, int | - |
| `addplayeritem` | ent | int |
| `removeplayeritem` | ent | int |
| `giveammo` | int, string, int | int |
| `getdelay` | - | float |
| `ismoving` | - | int |
| `overridereset` | - | - |
| `damagedecal` | int | int |
| `settogglestate` | int | - |
| `startsneaking` | - | - |
| `stopsneaking` | - | - |
| `oncontrols` | ent | int |
| `issneaking` | - | int |
| `isalive` | - | int |
| `isbspmodel` | - | int |
| `reflectgauss` | - | int |
| `hastarget` | int | int |
| `isinworld` | - | int |
| `isplayer` | - | int |
| `isnetclient` | - | int |
| `teamid` | - | string |
| `getnexttarget` | - | ent |
| `think` | - | - |
| `touch` | ent | - |
| `use` | ent, ent, int, float | - |
| `blocked` | ent | - |
| `respawn` | - | ent |
| `updateowner` | - | - |
| `fbecomeprone` | - | int |
| `illumination` | - | int |
| `fvisible` | ent | int |
| `changeyaw` | int | float |
| `hashumangibs` | - | int |
| `hasaliengibs` | - | int |
| `fademonster` | - | - |
| `gibmonster` | - | - |
| `getdeathactivity` | - | int |
| `becomedead` | - | - |
| `irelationship` | ent | int |
| `painsound` | - | - |
| `reportaistate` | - | - |
| `monsterinitdead` | - | - |
| `look` | int | - |
| `bestvisibleenemy` | - | ent |
| `finviewcone` | ent | int |
| `player_jump` | - | - |
| `player_duck` | - | - |
| `player_prethink` | - | - |
| `player_postthink` | - | - |
| `player_shouldfadeondeath` | - | int |
| `player_impulsecommands` | - | - |
| `player_updateclientdata` | - | - |
| `item_addtoplayer` | ent | int |
| `item_addduplicate` | ent | int |
| `item_candeploy` | - | int |
| `item_deploy` | - | int |
| `item_canholster` | - | int |
| `item_holster` | int | - |
| `item_updateiteminfo` | - | - |
| `item_preframe` | - | - |
| `item_postframe` | - | - |
| `item_drop` | - | - |
| `item_kill` | - | - |
| `item_attachtoplayer` | ent | - |
| `item_primaryammoindex` | - | int |
| `item_secondaryammoindex` | - | int |
| `item_updateclientdata` | ent | int |
| `item_getweaponptr` | - | ent |
| `item_itemslot` | - | int |
| `weapon_extractammo` | ent | int |
| `weapon_extractclipammo` | ent | int |
| `weapon_addweapon` | - | int |
| `weapon_playemptysound` | - | int |
| `weapon_resetemptysound` | - | - |
| `weapon_isusable` | - | int |
| `weapon_primaryattack` | - | - |
| `weapon_secondaryattack` | - | - |
| `weapon_reload` | - | - |
| `weapon_weaponidle` | - | - |
| `weapon_retireweapon` | - | - |
| `weapon_shouldweaponidle` | - | int |
| `weapon_usedecrement` | - | int |
| `cstrike_restart` | - | - |
| `cstrike_roundrespawn` | - | - |
| `cstrike_item_candrop` | - | int |
| `cstrike_item_isweapon` | - | int |
| `cstrike_item_getmaxspeed` | - | float |
| `cstrike_weapon_sendweaponanim` | int, int | - |
| `cstrike_player_resetmaxspeed` | - | - |
| `cstrike_player_isbot` | - | int |
| `cstrike_player_ontouchingweapon` | ent | - |
