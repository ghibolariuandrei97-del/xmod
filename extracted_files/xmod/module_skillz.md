# module_skillz.md — how to write an xmod extension (a Metamod plugin that plugs into xmod)

> **Instructions for the AI reading this.** The user runs **xmod** (a Metamod plugin for CS 1.6 / GoldSrc that runs Lua 5.4
> plugins). He wants a **new native module**: a separate Metamod plugin in C++ that adds functions to Lua as
> `xmod.<namespace>.<function>(...)`. Follow this file exactly. Use only the API below (`include/xmod_ext.h`); do not edit
> xmod's core. Deliver: one directory `extensions/xmod_<name>/` with the `.cpp` and a `CMakeLists.txt`, the two install
> lines, and a short Lua usage example. For writing the Lua side, the user has a second file, `skillz.md`.

## 1. The model in 6 rules
1. An extension is an ordinary Metamod plugin (`xmod_<name>_mm_i386.so` / `xmod_<name>_mm.dll`), loaded **after** xmod.
   It connects with `xmod_ext_connect()` and registers functions. `include/xmod_ext_glue.h` does all of that for you.
2. **Sync function** (`register_func`): runs on the game thread when Lua calls it. Must return fast. Never block there
   (no sockets, no SQL, no big files, no sleeping).
3. **Async function** (`register_async`): runs on a **worker thread** of xmod's pool and may block as long as it needs.
   Lua passes a callback as the **last** argument; xmod calls `callback(err, result)` later **on the game thread**.
4. Inside a worker you may call **only**: `job_*`, `val_*`, `post_main`, `emit_event`, `log`, `log_error`, `color_chat`,
   `client_print`, `is_main_thread`. **Never** touch the engine (`g_engfuncs`, `MESSAGE_BEGIN`, entities, cvars) or Lua from a worker.
   Need the engine after blocking work? `post_main(fn, user)` runs `fn` on the game thread at the next frame.
5. Data crosses the boundary as **value trees** (`XmodValue`): nil, bool, int (64-bit), number, string (binary-safe),
   array, map. A Lua array table becomes an array; any other table becomes a map with string keys. No functions/userdata.
6. Handles (database, socket, file…) are **integers** you hand to Lua. Register the functions that use a handle with
   `strand_arg = <1-based position of the handle argument>`: xmod then runs all jobs with the same handle value
   **one at a time, in call order** (so `BEGIN; INSERT; COMMIT` on one connection can never interleave).

## 2. Minimal extension (complete file)
`extensions/xmod_demo/demo.cpp`
```cpp
#define XMOD_EXT_NAME    "xmod_demo"      // must be unique
#define XMOD_EXT_VERSION "1.0"
#include "xmod_ext_glue.h"                // Metamod entry points + g_xmod (the API) + g_xext (your handle)
#include <string>

// sync:  xmod.demo.add(a, b) -> number            (game thread, must be fast)
static void f_add(XmodCall *c, void *) {
    if (g_xmod->call_argc(c) < 2) { g_xmod->call_error(c, "usage: xmod.demo.add(a, b)"); return; }   // raises a Lua error
    double a = g_xmod->val_get_num(g_xmod->call_arg(c, 0), 0), b = g_xmod->val_get_num(g_xmod->call_arg(c, 1), 0);
    g_xmod->call_return(c, g_xmod->val_num(a + b));
}

// async: xmod.demo.slow_upper(text, function(err, res) end)   (worker thread, may block)
static void j_slow_upper(XmodJob *j, void *) {
    size_t n = 0; const char *s = g_xmod->val_get_str(g_xmod->job_arg(j, 0), &n);    // args are 0-based, the callback is not included
    if (!s) { g_xmod->job_fail(j, "text must be a string"); return; }                 // -> callback("text must be a string", nil)
    std::string out(s, n); for (auto &ch : out) ch = (char)toupper((unsigned char)ch);
    /* ... blocking work is fine here ... */
    XmodValue *res = g_xmod->val_map();                                               // -> callback(nil, { text = "...", length = n })
    g_xmod->val_set(res, "text", g_xmod->val_str(out.data(), out.size()));
    g_xmod->val_set(res, "length", g_xmod->val_int((int64_t)n));
    g_xmod->job_done(j, res);                                                         // xmod owns `res` now
}

static bool xext_init(const XmodApi *api, XmodExt *ext) {                             // called in Meta_Attach, game thread
    return api->register_func(ext, "demo", "add", f_add, nullptr)
        && api->register_async(ext, "demo", "slow_upper", j_slow_upper, nullptr, 0);  // last arg: strand_arg (0 = none)
}
static void xext_shutdown(void) { /* close your sockets/handles so blocked workers return; free resources */ }
```
`extensions/xmod_demo/CMakeLists.txt`
```cmake
xmod_extension(xmod_demo demo.cpp)                 # more .cpp/.c files can follow; link extra libs with target_link_libraries(xmod_demo ...)
```
Build with the normal scripts (`./build_linux.sh`, `./build_windows.sh`): every directory under `extensions/` that has a
`CMakeLists.txt` is built and copied to `addons/xmod/dlls/`. Install:
```
addons/metamod/plugins.ini      linux addons/xmod/dlls/xmod_mm_i386.so          <- xmod FIRST
                                linux addons/xmod/dlls/xmod_demo_mm_i386.so     <- then extensions   (Windows: win32 ...\xmod_demo_mm.dll)
addons/xmod/configs/extensions.ini   add a line:  demo                          <- the Lua namespace (see section 5)
```
Lua:
```lua
print(xmod.demo.add(2, 3))                                               -- 5.0
xmod.demo.slow_upper("hello", function(err, res)
  if err then xmod.error(err) return end
  xmod.log(res.text .. " (" .. res.length .. ")")                        -- runs on the game thread, a few frames later
end)
```
A complete, tested example of every feature is `extensions/xmod_hello/hello.cpp` (copy it). A large real one is
`extensions/xmod_query/` (SQL, HTTP, sockets).

## 3. API reference (`const XmodApi *g_xmod`)
| Group | Functions |
|---|---|
| Register (game thread, in `xext_init`) | `register_func(ext, ns, name, fn, user)` · `register_async(ext, ns, name, fn, user, strand_arg)` → 1 ok / 0 failed (reserved namespace, duplicate name, bad characters: only `[A-Za-z0-9_]`) |
| Sync call | `call_argc(c)` · `call_arg(c, i)` · `call_return(c, value)` · `call_error(c, "msg")` |
| Async job | `job_argc(j)` · `job_arg(j, i)` · `job_done(j, value)` · `job_fail(j, "msg")` — call exactly one of done/fail; returning without either gives the callback an error |
| Build values | `val_nil()` `val_bool(b)` `val_int(i64)` `val_num(d)` `val_str(ptr, len)` `val_cstr(z)` `val_array()` `val_map()` · `val_push(array, v)` · `val_set(map, "key", v)` (both take ownership of `v`) · `val_free(v)` only for values you never handed over |
| Read values | `val_type(v)` → `XV_NIL/BOOL/INT/NUM/STR/ARRAY/MAP` · `val_get_int(v, default)` · `val_get_num(v, default)` · `val_get_bool(v, default)` · `val_get_str(v, &len)` (NULL if not a string) · `val_len(v)` · `val_at(array, i)` · `val_get(map, "key")` · `val_key(map, i)` / `val_val(map, i)` to iterate. All accept NULL and return the default. |
| From any thread | `post_main(fn, user)` · `emit_event("event_name", args_array)` (fires `xmod.on("event_name", function(a, b, ...) end)`; takes ownership) · `log(msg)` · `log_error(msg)` · `is_main_thread()` |
| Game helpers, any thread | `color_chat(player_id, text)` — coloured chat, `0` = all human players, tags `{y}` default, `{t}` team colour, `{g}` green, max ~188 chars · `client_print(player_id, "console"\|"center"\|"chat", text)` |

Argument values (`call_arg` / `job_arg`) belong to xmod: read them, never free or keep them after the function returns
(copy strings you need later). `user` is the pointer you passed at registration.
Reserved namespaces (cannot be used): `core player ent mem trace msg cmd cvar timer kv fs sound precache fullpack hook reapi ham stats ext async`.

## 4. Patterns
```cpp
// (a) options table:  xmod.demo.fetch(url, { timeout = 3, headers = { ["X-A"] = "1" } }, cb)
const XmodValue *o = g_xmod->job_arg(j, 1);
double timeout = (g_xmod->val_type(o) == XV_MAP) ? g_xmod->val_get_num(g_xmod->val_get(o, "timeout"), 10.0) : 10.0;

// (b) handles + strand:  open -> handle;  use(handle, ...);  close(handle)
static std::mutex g_mu; static std::map<int64_t, std::shared_ptr<Thing>> g_things; static std::atomic<int64_t> g_next{1};
static void j_open(XmodJob *j, void *) { auto t = std::make_shared<Thing>(/* blocking connect */); int64_t h = g_next++;
    { std::lock_guard<std::mutex> l(g_mu); g_things[h] = t; } g_xmod->job_done(j, g_xmod->val_int(h)); }
static void j_use(XmodJob *j, void *)  { std::shared_ptr<Thing> t; { std::lock_guard<std::mutex> l(g_mu); auto it = g_things.find(g_xmod->val_get_int(g_xmod->job_arg(j, 0), 0)); if (it != g_things.end()) t = it->second; }
    if (!t) { g_xmod->job_fail(j, "invalid or closed handle"); return; } /* work with t */ }
api->register_async(ext, "demo", "open", j_open, nullptr, 0);
api->register_async(ext, "demo", "use",  j_use,  nullptr, 1);     // strand on argument #1 = the handle: ordered, never concurrent per handle
api->register_async(ext, "demo", "close", j_close, nullptr, 1);

// (c) push events to Lua from a long-running worker (progress, incoming data):
XmodValue *a = g_xmod->val_array(); g_xmod->val_push(a, g_xmod->val_int(percent)); g_xmod->emit_event("demo_progress", a);

// (d) engine work after blocking work: collect data in the worker, then
struct Ctx { int id; std::string text; };
static void on_main(void *u) { Ctx *c = (Ctx *)u; /* game thread: engine calls are allowed here */ delete c; }
g_xmod->post_main(on_main, new Ctx{id, text});
```

## 5. What the Lua side sees (tell the user)
- Lua scripts are loaded **before** extensions attach. So extension functions exist from the `config` / `init` /
  gameplay events on, **not** at the top level of a script. List your namespace in `configs/extensions.ini`: then
  `local d = xmod.demo` is valid at load time (a stable table) and fills in when the extension attaches.
- Calling a function of a missing/unloaded extension raises a clear Lua error. Check with `xmod.ext.available("demo")`
  or `xmod.ext.available("demo", "add")`. `xmod.ext.list()` and console `xmod ext` list loaded extensions.
- An async call returns a job id; `xmod.async.cancel(id)` drops the callback (the work itself is not interrupted).
  `xmod.async.stats()` → `{threads, max_threads, idle, running, queued, pending, ...}`.
- Callbacks are never called after `xmod reload` or after the calling plugin was disabled; the player a job was about
  may have left — callbacks must re-check `xmod.player.connected(id)`.
- Server cvars: `xmod_async_threads` (4, max 16), `xmod_async_budget_ms` (2: ms per frame spent delivering results),
  `xmod_async_queue_max` (4096 pending jobs).

## 6. Checklist before you hand the code over
- [ ] Every sync function is non-blocking; everything that can wait is async.
- [ ] No engine / Lua / game-state access in worker code; shared C++ state is protected by a mutex.
- [ ] Every code path of an async function ends in exactly one `job_done` or `job_fail`.
- [ ] Arguments are validated (`val_get_*` return your default on a wrong type; send a useful `job_fail`/`call_error` text).
- [ ] Functions taking a handle use `strand_arg`; handle tables use `shared_ptr` so closing while a job runs is safe.
- [ ] Limits: cap sizes, timeouts and counts that scripts control. Secrets (passwords, tokens, hosts) come from a config
      file read by the extension, not from Lua.
- [ ] `xext_shutdown` unblocks workers (close sockets/handles). xmod then waits up to 5 s for your running jobs.
- [ ] Third-party libraries: link statically, or load at run time (`dlopen` / `LoadLibrary`) and fail with a clear message.
      GoldSrc servers are **32-bit**: a 64-bit `.so`/`.dll` cannot be loaded.
- [ ] Portable: Linux (gcc, `-m32`) and Windows (MinGW, as used by `build_windows.sh`; `std::mutex`/`std::atomic` compile there).
- [ ] Tested: add checks to `test/` like `test/hello_test.lua` and run `test/run_ext.sh` (mock engine).

## 7. Limits you should know
- A value passed from Lua may nest at most 8 levels and hold ~200 000 items; functions, userdata and coroutines cannot be passed.
- Lua integers are 64-bit and survive unchanged; floats are doubles. `val_get_int` also accepts floats with an integral value.
- xmod delivers results within a per-frame time budget, so thousands of results arriving at once spread over a few frames.
- If you also need engine or game hooks in the same plugin, don't include `xmod_ext_glue.h`; write the Metamod entry
  points yourself (see xmod's `src/main.cpp`) and call `xmod_ext_connect()` / `ext_register()` / `ext_unregister()` yourself.
- The API struct only grows at the end. `g_xmod->abi_version` is 1; check `g_xmod->size` before using a field added later.
