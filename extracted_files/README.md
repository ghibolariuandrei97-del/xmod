# xcmd 1.1.0 — SteamCMD for xmod

One program for everything around an xmod server: install, update, mods, live stats, a live tactical map and a world server browser.
Single file, no runtime needed: `xcmd.exe` (Windows 10/11, 32 and 64 bit) · `xcmd` (Linux 64-bit) · `xcmd-i386` (Linux 32-bit).

```
xcmd                         full-screen app: Home · Monitor · Map · Mods · Browser   (tab / 1-5 to switch, q quits)
xcmd setup <profile> --dir D --zip   a folder (+ zip) ready to upload by FTP; nothing comes from Steam
                             profiles: xmod | metamod (xmod+Metamod-R) | regamedll (+ReGameDLL) | rehlds (+ReHLDS) | all (+Codex, ModCreator, xbots, query)
xcmd update --zip            newest releases; the zip has ONLY the changed files
xcmd install hlds            optional: a complete HLDS through SteamCMD
xcmd install modcreator      any package; dependencies come automatically
xcmd update                  everything to the newest release (your configs are kept)
xcmd search / info / list    the library
xcmd enable|disable|remove X
xcmd start [--map de_dust2 --port 27015 --rcon PW]
xcmd monitor [ip:port --rcon PW]   live: players, money, HP, weapons, K/D, ping, server fps, CPU, RAM, graphs
xcmd map [ip:port --rcon PW] [--maps DIR]   live top-down map (DIR = cstrike/maps of your CS 1.6 game)
xcmd browser                 every CS 1.6 server in the world that runs xmod
xcmd doctor                  checks this machine and the server
```
Every command takes `--dir <folder>` and `--os linux|windows|both` (which server binaries; default: what the folder has, else both) (default: `C:\hlds` on Windows, `~/hlds` on Linux; `xcmd dir <path>` changes it).

## Where things come from
- **HLDS / Counter-Strike 1.6**: only if you ask for it (`xcmd install hlds`), through Valve's SteamCMD.
- **ReHLDS, ReGameDLL_CS, Metamod-R**: the latest GitHub release of github.com/rehlds.
- **xmod and its packages**: the GitHub repository `ghibolariuandrei97-del/xmod`. xcmd recognises the files there
  (`xmod_0.4.4.zip`, `xmod_codex_1.1.0.zip`, `xmod_bots_1.0.3.zip`, `modCreator.zip`, `war3.lua`, any `name_1.2.zip` mod).
  Put `xcmd.json` (in this zip) in the repository root to give names, descriptions, dependencies and versions; a mod listed there appears
  in the library only once its file is in the repository. Mods that `require("modcreator")` / use `xmod.codex`, `xmod.bot`, `xmod.query`
  get that dependency installed automatically.

## Live data (monitor + map)
`xcmd setup` installs the **xcmd agent** (`addons/xmod/plugins/xcmd_agent.lua`). It writes `data/xcmd_live.json` 4×/s (read directly
when xcmd runs on the server machine) and answers `xcmd_dump` over rcon (remote servers: give the rcon password with `--rcon` or `p` in
the Monitor). Without the agent you still get name, map, players, ping (A2S). CPU and RAM are shown for a server running on the same
machine. The map is drawn from the server's `maps/<map>.bsp` (local server, `~/.xcmd/maps/`, or FastDL `sv_downloadurl`).

## Safety
Edited configs are never overwritten (the new default is saved as `*.xcmd-new`), `metamod/plugins.ini` gets one managed block,
`liblist.gam` is backed up once (`liblist.gam.xcmd-bak`), dlls locked by a running server on Windows are saved as `*.xcmd-pending`.
State per server: `<server>/.xcmd/state.json`; settings: `~/.xcmd/config.json`. GitHub API limit is 60 requests/hour without a token:
set `XCMD_GITHUB_TOKEN` if you update very often.

## Build from source
`source/`: Go 1.22+ → `go mod tidy && go build -trimpath -ldflags "-s -w" -o xcmd .`
(`GOOS=windows GOARCH=386` for xcmd.exe). Libraries: Bubble Tea, Lip Gloss, Bubbles.
