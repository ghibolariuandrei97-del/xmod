# Part I-b — the `metalog` and `translator` libraries (`lib/metalog.lua`, `lib/translator.lua`)

Both ship with xmod 0.4.1 in `addons/xmod/lib/` and are plain Lua (read them when in doubt). Like every library they are loaded
on the first `require` and are shared by all plugins. Their console commands (`metalog`, `xmod_lang*`) exist only after some
plugin required them. **Use them in every plugin you write for Andrew:** `metalog` instead of ad-hoc `print`/`xmod.log`
debugging, `translator` for every player-visible text when the plugin may be used on a multi-language server.

## M1. metalog — debug + logging system

### What it does
- Five levels `trace < debug < info < warn < error`, written to the **server console** and to **Markdown files**:
  `addons/xmod/data/logs/<logger>_<YYYY-MM-DD>.md` (one per logger per day) and `data/logs/errors_<date>.md`
  (warnings + errors of **all** loggers in one file). Error lines also go to `xmod.error` → `data/xmod_errors.log`.
- **Master switch** in the file itself: `M.CONFIG.enabled = false` (then `xmod reload`) turns every metalog method into an
  **empty function** (`metalog.NOOP`): no output, no files, and functions wrapped with `log:fn()` / `metalog.debug_only()`
  are replaced by an empty function, so **their code never runs**. Raising `CONFIG.level` does the same per level
  (e.g. `level = "warn"` → `log:debug`, `log:dump`, `log:fn(f)` become empty).
- Every line carries the caller's `file.lua:line` (taken from `error(…, level)`; the `debug` library is not available).
- File writes are buffered (`flush_every`, default 0.5 s); `warn`/`error` are written immediately, and everything is flushed
  on `map_end` / `shutdown`. (`xmod reload` does not fire `shutdown`: at most the last 0.5 s of info/debug lines can be lost.)
- xmod cannot create folders: `data/logs/` ships with xmod (with a README.md). If it is missing, files go to `data/logs_<name>_<date>.md`.

### `CONFIG` (top of `lib/metalog.lua`, edit + `xmod reload`)
| key | default | meaning |
|---|---|---|
| `enabled` | `true` | master switch (false = all methods are empty functions) |
| `level` | `"debug"` | global minimum level: `trace` `debug` `info` `warn` `error` `off` (a logger's own `level` overrides it) |
| `console` | `true` | print to the server console |
| `file` | `true` | write the .md files |
| `dir` | `"data/logs/"` | folder (must be below `data/`) |
| `rotate` | `"day"` | `"day"` = `<name>_<date>.md`, `"none"` = `<name>.md` |
| `max_file_kb` | `2048` | a file stops growing at this size (a note is written; next day = new file); `0` = no limit |
| `flush_every` | `0.5` | seconds between buffered writes |
| `combined` | `true` | copy warn/error lines into `errors_<date>.md` |
| `to_xmod` | `true` | error lines also go to `xmod.error` |
| `location` | `true` | add `file.lua:line` |
| `admins` / `admin_flag` | `false` / `"l"` | also print warn/error in the console of online admins with that flag |
| `console_min` | `"trace"` | lowest level printed in the console |

### Creating a logger
```lua
local ML  = require("metalog")
local log = ML.new("myplugin")                    -- name = console prefix + file name (default: the caller's file name)
local log2 = ML.new("myplugin_sql", { level = "info", console = false, file = true, location = false })
```
`ML.new(name, opts)` with an existing name returns the same logger (opts applied again). Use **colon syntax** for every method.

### Logger methods
| method | level | behaviour |
|---|---|---|
| `log:trace(fmt, ...)` / `debug` / `info` / `warn` / `error` | own | `fmt` like `string.format`; a bad format never raises (values are appended); tables are dumped; non-string `fmt` is dumped |
| `log:dump(label, value [, depth=4])` | debug | pretty printed value in a code block (cycles marked `<cycle>`, depth limit `{...}`) |
| `log:table(title, headers, rows)` | info | real Markdown table in the file (`rows` = lists, or maps keyed by header); `|` is escaped |
| `log:section(title)` | info | `### title` heading in the file |
| `log:once(level, fmt, ...)` | given | only the first time this exact text appears (reset every map) |
| `log:every(seconds, level, fmt, ...)` | given | rate limit per format string; the next line says how many were suppressed |
| `log:time(label)` / `log:time_end(label)` → ms | debug | CPU time (`os.clock`) between the two calls |
| `log:count(label [, by])` → value · `log:counts([reset])` | debug | counters (count always counts), `counts` writes them as a table |
| `log:bench(label, n, fn, ...)` → ms | debug | runs `fn` n times (mind `xmod_script_timeout`, 100 ms) |
| `log:player(id [, text])` | debug | `Name <#userid> <STEAM_…> <T/CT/SPEC/-> hp=N` |
| `log:codex([names])` | info | Codex status + table of its functions (name, resolved, how, addr, calls, hooks, problem); says so if Codex is not loaded |
| `log:codex_check([names])` → count | warn | one warning per Codex function that is undefined / not resolved / faulted |
| `log:assert(cond, fmt, ...)` → cond | error | logs "assertion failed: …" when cond is false/nil; **never raises** |
| `log:try(fn, ...)` → `ok, results…` | error | `pcall` that logs the error **with the arguments**; still runs when logging is off |
| `log:wrap(fn [, label])` → function | error | protected version of fn: on error logs label + error + arguments and returns nil. When the logger is off it **returns fn itself** |
| `log:fn(fn [, level="debug"])` → function | given | fn if that level is on, else **an empty function** (debug-only helpers). Decided when called — normally at load |
| `log:strip(tbl [, level="debug"])` → tbl | given | replaces every function in tbl by an empty function when the level is off |
| `log:trace_calls(tbl [, prefix])` → tbl | trace | wraps every function of tbl: logs `prefix.name(args) -> results (ms)`; unchanged when trace is off |
| `log:is(level)` → bool | – | true if that level is kept: `if log:is("debug") then <expensive> end` |
| `log:set_level(l)` · `log:get_level()` · `log:path()` · `log:flush()` | – | runtime level, current file path, write now |

Methods that become **empty** when their level is off: the five level methods, `dump`, `table`, `section`, `time`,
`time_end`, `counts`, `bench`, `player`, `codex`, `codex_check` (and `once`/`every` do nothing). Methods that **keep
working** (they only stop logging): `assert`, `try`, `wrap`, `fn`, `strip`, `trace_calls`, `count`, `is`, `set_level`, `path`, `flush`.

**Cost note:** an empty method still evaluates its arguments. Write `log:debug("x=%d hp=%d", x, hp)` (formatting happens
only when kept), not `log:debug("x=" .. x)`; guard expensive argument building with `if log:is("debug") then … end`.

### Module functions
`ML.new(name, opts)` · `ML.get(name)` · `ML.list()` · `ML.flush_all()` · `ML.set_level(name | "*", level)` ("*" = global, clears per-logger levels)
· `ML.enabled()` · `ML.set_enabled(bool)` (runtime switch; functions already returned by `fn`/`debug_only` keep their load-time state)
· `ML.debug_only(fn)` (fn or empty function from the global switch/level) · `ML.NOOP` · `ML.dump(v [, depth])` → string
· `ML.format(fmt, ...)` (safe format) · `ML.md_table(headers, rows)` → Markdown text · `ML.who(id)` → player description
· `ML.where(depth)` → `"plugins/x.lua:12"` (1 = the function that calls where) · `ML.CONFIG` · `ML.LEVELS` · `ML.log` (the library's own logger).

### Server console `metalog`
`metalog status` (switch, level and file of every logger) · `metalog level <logger|*> <level>` · `metalog on|off`
· `metalog flush` · `metalog tail <logger|errors> [lines]` · `metalog errors [lines]` (tail of `data/xmod_errors.log`)
· `metalog codex` (Codex status + check) · `metalog test` (writes a sample to `data/logs/metalog_test_<date>.md`).

### File format (what Andrew sees)
```markdown
# metalog - myplugin - 2026-11-21

Server: My Server | xmod 0.4.1 | started 11:18:09

## 11:18:09 - map de_dust2

- `11:18:09` **INFO** `plugins/myplugin.lua:44` Tester joined
- `11:18:10` **ERROR** `plugins/myplugin.lua:55` attempt to index a nil value
  ```
  arguments: 3, "ak47"
  ```

**scoreboard** (11:20:00)

| id | name | frags |
|---|---|---|
| 1 | Tester | 5 |
```

### Template
```lua
xmod.register("My Plugin", "1.0", "Andrew")
local log = require("metalog").new("myplugin")

local debug_beam = log:fn(function(id) --[[ draw debug things ]] end)   -- empty function when debug is off

xmod.on("ham:takedamage:player", log:wrap(function(victim, inflictor, attacker, dmg, bits)
  log:debug("dmg %.1f %d -> %d bits %d", dmg, attacker, victim, bits)
  debug_beam(attacker)
end, "takedamage"))

xmod.on("config", function()
  local ok, db = log:try(open_database)          -- logged with arguments if it fails, plugin keeps running
  if not log:assert(ok, "database not available") then return end
end)
```
Notes: `log:wrap` swallows the error (after logging it) — the handler returns nil, so do not wrap handlers whose return
value matters (`HC_SUPERCEDE`, `true` to block) unless returning nil on error is acceptable. Errors swallowed by
`wrap`/`try` do **not** count towards the 25-errors auto-disable.

## M2. translator — multi-language texts

### Model
- **Dictionaries** are files `addons/xmod/lang/<name>.txt` in the **AMX Mod X dictionary format** (AMXX `data/lang/*.txt`
  files work unchanged):
  ```
  ; comment (also // and #)
  [en]
  WELCOME = {g}Welcome {t}%s{y}!
  KILLS_ONE = %d kill
  KILLS = %d kills
  [ro]
  WELCOME = {g}Bine ai venit {t}%s{y}!
  ```
  `[code]` sections (`en`, `ro`, `pt_br`/`pt-BR`, case-insensitive), `KEY = text` (one space after `=` is eaten; key chars
  `[A-Za-z0-9_.-]`), `^n` = newline, `^t` = tab; colour codes stay (`{g} {y} {t} {red}` or `^1 ^3 ^4`). Multi-line text
  (xmod extension): a line `KEY:` then the lines, ended by a line containing only `:`. UTF-8 BOM is ignored.
- **Arguments:** `%s %d %i %f %x %c` (string.format), or **one table argument** for named placeholders `{name}`
  (`tr(id, "MSG", {name = "Bob", n = 3})` with `MSG = {g}{name} has {n}` — colour codes not in the table are left alone).
  AMXX `%L` is not supported. A wrong format never raises (the template + arguments are returned).
- **Player language** = his choice (`say /lang` menu, `xmod_lang <code>`, `T.set_lang`; saved by SteamID in vault
  `translator`) > his `setinfo lang xx` (cvar `xmod_lang_setinfo 1`) > server language cvar `xmod_language` (default `en`).
  Id 0 / invalid id = server language.
- **Fallback for a key:** player language → its base (`pt_br` → `pt`) → server language → `en` → any language of that
  dictionary that has it → the key itself. Missing texts are logged once (to the `translator` metalog logger: console +
  `data/logs/translator_<date>.md`).
- The built-in dictionary `lang/common.txt` (`T.common`) has en, ro, es, de, fr, pt, ru, pl, tr for: `YES NO ON OFF ENABLED
  DISABLED BACK NEXT EXIT CLOSE SECONDS MINUTES PLAYER ADMIN NO_ACCESS NOT_FOUND` and the library's own `TR_*` texts.
  Its languages are the ones offered in the `/lang` menu (plus every language of any loaded dictionary).

### API
```lua
local T  = require("translator")
local tr = T.load("myplugin")                -- lang/myplugin.txt (missing file = empty dictionary, add texts with tr:add)
tr:add("de", { WELCOME = "Willkommen %s" })   -- add/replace texts from Lua (chainable)

tr(id, "WELCOME", name)                       -- text in the player's language (also tr:get(id, key, ...))
tr:in_lang("ro", "WELCOME", name)             -- explicit language
tr:chat(id | 0, "WELCOME", name)              -- xmod.msg.chat; 0 = every human player, each in HIS language
tr:print(id | 0, "center" | "console" | "chat", key, ...)
tr:hud(id | 0, { y = 0.3, hold = 3 }, key, ...)   -- opts = xmod.msg.hud options
tr:console(id, key, ...)                      -- player console, or server console for id 0
tr:plural(id, "KILLS", n, ...)                -- KILLS_ONE / KILLS_FEW / KILLS_MANY by the language rule, fallback KILLS; n = first argument
```
Dictionary methods: `tr:add(code, tbl)` · `tr:set(code, key, text)` · `tr:has(key [, code])` · `tr:keys([code])` ·
`tr:langs_list()` · `tr:missing()` → `{ [code] = {keys missing compared with the other languages} }` · `tr:reload()` ·
`tr:raw(code, key)` → `template, code_used` (no formatting, nil if missing everywhere) · fields `tr.name`, `tr.file`, `tr.langs[code][key]`.

Module: `T.load(name [, file])` (same name again = re-read) · `T.new(name)` (empty dictionary) · `T.dict(name)` · `T.dicts()` ·
`T.lang(id)` → code · `T.set_lang(id, code [, save=true])` → bool (fires event **`lang_changed`(id, old, new)**) ·
`T.reset_lang(id)` · `T.server_lang()` · `T.languages()` → `{ {code, name, keys}, … }` · `T.lang_name(code)` (native name, `T.NAMES`)
· `T.menu(id)` (open the language menu) · `T.t(id, key, ...)` (search every dictionary, common first) · `T.format(tpl, ...)` ·
`T.parse(text)` → `langs, count, problems` · `T.code("pt-BR")` → `"pt_br"` · `T.plural_form(code, n)` → `"ONE"|"FEW"|"MANY"` ·
`T.reload()` · `T.common` · `T.CONFIG` (`dir`, `say_commands`, `save`, `warn_missing`).

Plural rules: ro (1 = ONE, 0 and 2–19 (mod 100) = FEW, else MANY → "20 **de** kill-uri"), ru/uk/sr/hr (ONE 1, 21, 31…;
FEW 2–4, 22–24…; MANY), pl (ONE 1; FEW 2–4, 22–24…; MANY), cs/sk (ONE 1; FEW 2–4; MANY), fr/pt (0 and 1 = ONE), others 1 = ONE.
FEW falls back to MANY, then to the plain key.

Cvars: `xmod_language` (server language, AMXX `amx_language`) · `xmod_lang_players 1` (players may choose; AMXX
`amx_client_languages`) · `xmod_lang_setinfo 1`. Commands: say `/lang` `/language` `/limba` (menu) · client console
`xmod_lang [code|reset]` · server `xmod_lang_list` (languages, dictionaries, each player's language) · `xmod_lang_check
[dictionary]` (missing keys per language) · `xmod_lang_reload`.

### Template
```lua
xmod.register("Shop", "1.0", "Andrew")
local tr = require("translator").load("shop")         -- addons/xmod/lang/shop.txt
xmod.cmd.say("/shop", "", function(id)
  if xmod.player.money(id) < 1000 then return tr:chat(id, "SHOP_NO_MONEY", 1000) end
  require("menu").show(id, { title = tr(id, "SHOP_TITLE"), items = { { tr(id, "SHOP_HP"), "hp" } },
    on_select = function(pid, _, v) tr:chat(pid, "SHOP_BOUGHT", tr(pid, "SHOP_HP")) end })
end)
xmod.on("lang_changed", function(id) --[[ redraw HUDs of id in the new language ]] end)
```
Rules: never build player-visible text by concatenating translated fragments in a fixed order (word order differs) — use
one key with arguments. Translate per player at send time (`tr:chat(0, …)`), never once for everybody. Keys UPPER_CASE with a
plugin prefix (`SHOP_`) so `T.t` and AMXX files do not collide. Write Romanian without diacritics unless Andrew asks.

### Porting from AMXX
| AMXX | xmod |
|---|---|
| `register_dictionary("shop.txt")` | `local tr = require("translator").load("shop")` |
| `client_print(id, print_chat, "%L", id, "KEY", a)` | `tr:print(id, "chat", "KEY", a)` / colour chat `tr:chat(id, "KEY", a)` |
| `client_print(0, …, "%L", LANG_PLAYER, "KEY")` | `tr:chat(0, "KEY")` |
| `formatex(buf, …, "%L", id, "KEY")` | `local s = tr(id, "KEY")` |
| `LANG_SERVER` | `tr(0, "KEY")` / `tr:in_lang(T.server_lang(), "KEY")` |
| `get_user_info(id, "lang", …)` | `T.lang(id)` |
| `amx_language` / `amx_client_languages` | cvars `xmod_language` / `xmod_lang_players` |
