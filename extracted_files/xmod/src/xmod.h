// xmod - next-generation scripting platform for GoldSrc (Metamod plugin, Lua 5.4)
#pragma once
#include "mm_sdk.h"
extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>

#define XMOD_VERSION "0.3.1"
#ifdef _MSC_VER
#define strcasecmp _stricmp
#endif
#define XMOD_MAXPLAYERS 32
#define XMOD_DIR "addons/xmod"

struct Plugin;

// ---- access flags (letters a..z -> bits 0..25, AMXX compatible) ----
int  flags_from_string(const char *s);

// ---- per-player state kept natively ----
struct PlayerState {
	bool connected = false, putin = false, bot = false;
	int  access = 0;
	int  userid = 0;
	std::string name, ip, authid;
};
extern PlayerState g_players[XMOD_MAXPLAYERS + 1];
extern int g_maxclients;

// ---- logging / helpers ----
void xlog(const char *fmt, ...);
void xerr(const char *fmt, ...);
std::string xpath(const char *rel);               // relative to <gamedir>/addons/xmod
bool is_valid_player(int id);                      // 1..maxclients && connected
edict_t *player_edict(int id);
edict_t *ent_edict(int idx);                       // validated edict or nullptr

// ---- plugin / event system ----
struct Handler { Plugin *owner; int ref; int prio; };
struct Plugin {
	std::string file, name, version, author;
	bool enabled = true;
	int  env_ref = LUA_NOREF, pdata_ref = LUA_NOREF;
	int  errors = 0;
	std::string last_error;                        // first line of the last script error (shown by "xmod plugins")
};
extern lua_State *L;
extern Plugin *g_cur;                              // plugin whose handler is currently running
Plugin *caller_plugin(lua_State *l);               // plugin whose CODE called the current native (a plugin closure called through a library = that plugin)
extern std::vector<Plugin *> g_plugins;

struct Event { std::string name; std::vector<Handler> handlers; unsigned long long calls = 0, samples = 0, sample_ns = 0, max_ns = 0; };
Event *ev_get(const char *name);                   // create-on-demand
inline bool ev_has(Event *e) { return !e->handlers.empty(); }

// Fires event. Handler args must already be pushed via the lambda. Returns:
//   result_kind: 0 = nobody returned anything, 1 = returned true/handled (blocks), 2 = returned false (+optional string reason)
struct EvResult { int kind = 0; std::string reason; };
EvResult ev_fire(Event *e, int nargs_hint, const std::function<int(lua_State *)> &push_args);
// Allocation-free fast path for hot events: pushes n numbers (bit k of intmask set => pushed as Lua integer, else float).
EvResult ev_fire_nums(Event *e, const double *v, int n, unsigned intmask);
extern bool g_stats_on;                            // per-event timing (xmod.stats.enable)
unsigned long long now_ns();
void stats_add(Event *e, unsigned long long ns);
void stats_frame();
bool     safe_call(Plugin *p, int nargs, int nres, const char *what);   // pcall with traceback, timeout & error accounting

// ---- subsystem registration (each api_*.cpp) ----
void api_register_util(lua_State *L);
void api_register_player(lua_State *L);
void api_register_ent(lua_State *L);
void api_register_msg(lua_State *L);
void api_register_cmd(lua_State *L);
void api_register_misc(lua_State *L);

// ---- hooks implemented in api files and called from main.cpp ----
bool cmd_client_dispatch(edict_t *pEntity);        // true = supercede
void cmd_reset();                                  // on reload: drop registered script commands
void timers_run();                                 // per frame
void timers_reset();
void timers_map_end();
void cvar_watch_run();
void cvar_watch_reset();
void msg_reset();
bool msg_busy();                                   // a pass-through game message is open right now (sending would crash the engine)
void msg_defer(const std::function<void()> &f);   // run f right after that message ends
void msg_end_post();                               // engine post-hook of MessageEnd
void msg_frame_reset();
void msg_script_cleanup();
bool msg_begin(int dest, int type, const float *org, edict_t *ed);
bool msg_write(int kind, double num, const char *str);
bool msg_end();                                    // true = supercede
void kv_flush_all();
void kv_reset();
void cvarquery_result(const edict_t *e, int req, const char *name, const char *value);

// ---- core ----
bool core_init();
void core_shutdown();
void core_reload();
void core_set_timeout(int ms);                     // xmod_script_timeout (0 = off); loading gets 10x (min 1000 ms)
void core_load_plugins();
extern float g_time;

// ---- extension API (src/ext_api.cpp, contract in include/xmod_ext.h)
void ext_lua_init();                               // after the Lua state + xmod table exist, before scripts run
void ext_lua_closing();
void ext_pump();                                   // every frame: run posted main-thread work, deliver async results (budgeted)
void ext_shutdown();                               // stop and join the worker threads
void ext_set_limits(int threads, int budget_ms, int queue_max);
void ext_console_list();
void xmod_color_chat(int id, const std::string &text);                       // api_msg.cpp (main thread only)
void xmod_client_print(int id, const std::string &kind, const std::string &text);
