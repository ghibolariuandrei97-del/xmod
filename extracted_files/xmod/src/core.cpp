#include "xmod.h"
#include "hookutil.h"
#include <cstdio>
#include <cstring>
#include <ctime>
#include <chrono>
#include <fstream>
#include <algorithm>
#include <map>
#include <memory>
#include <algorithm>
#include <atomic>
#ifdef _WIN32
#else
#include <pthread.h>
#include <unistd.h>
#endif

lua_State *L = nullptr;
Plugin *g_cur = nullptr;
std::vector<Plugin *> g_plugins;
PlayerState g_players[XMOD_MAXPLAYERS + 1];
int g_maxclients = 0;
float g_time = 0;

static std::unordered_map<std::string, Event *> g_events;
bool g_stats_on = false;
static unsigned long long g_stats_t0 = 0, g_stats_frames = 0;
unsigned long long now_ns() { return (unsigned long long)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
void stats_add(Event *e, unsigned long long ns) { e->calls++; e->samples++; e->sample_ns += ns; if (ns > e->max_ns) e->max_ns = ns; }
static unsigned long long g_last_frame_ns = 0, g_frame_sum_ns = 0, g_frame_max_ns = 0;
void stats_frame() {
	g_stats_frames++; unsigned long long n = now_ns();
	if (g_last_frame_ns) { unsigned long long d = n - g_last_frame_ns; g_frame_sum_ns += d; if (d > g_frame_max_ns) g_frame_max_ns = d; }
	g_last_frame_ns = n;
}
static void stats_zero() {
	for (auto &kv : g_events) { Event *e = kv.second; e->calls = e->samples = e->sample_ns = e->max_ns = 0; }
	g_stats_t0 = now_ns(); g_stats_frames = 0; g_last_frame_ns = 0; g_frame_sum_ns = 0; g_frame_max_ns = 0;
}
// Timing is SAMPLED (every call for the first 256, then 1 in 16) so that measuring costs almost nothing; call counts are exact, totals are extrapolated.
struct StatScope {
	Event *e; unsigned long long t0;
	explicit StatScope(Event *ev) : e(ev), t0(0) { if (g_stats_on) { ev->calls++; if (ev->calls <= 256 || (ev->calls & 15) == 0) t0 = now_ns(); } }
	~StatScope() { if (t0) { unsigned long long d = now_ns() - t0; e->samples++; e->sample_ns += d; if (d > e->max_ns) e->max_ns = d; } }
};
struct HSnap {                                     // snapshot of a handler list without heap allocation for <= 8 handlers
	Handler small[8]; std::vector<Handler> big; const Handler *p; size_t n;
	explicit HSnap(const std::vector<Handler> &h) : n(h.size()) { if (n <= 8) { for (size_t i = 0; i < n; i++) small[i] = h[i]; p = small; } else { big = h; p = big.data(); } }
};
static size_t g_mem_used = 0;
static const size_t MEM_LIMIT = 96u * 1024 * 1024;
static int g_call_depth = 0;                       // main thread only
// ---- watchdog (zero cost on the normal path): the main thread only bumps a sequence number when an outermost script call
// starts/ends; a helper thread samples it and, if ONE call runs longer than the limit, installs an abort hook (like lua.c does for Ctrl-C).
static std::atomic<unsigned long long> g_call_seq{0}, g_fired_seq{0};
static std::atomic<bool> g_call_active{false}, g_wd_fired{false}, g_wd_run{false}, g_wd_lock{false};
static std::atomic<int> g_limit_ms{100};            // configured (xmod_script_timeout); 0 = off
static std::atomic<int> g_eff_ms{100};              // effective (loading gets a much larger budget)
static std::atomic<bool> g_loading{false};
static lua_State *g_wd_L = nullptr;                 // protected by g_wd_lock
static int g_max_errors = 25;
bool g_any_class_hook = false;                  // any "touch:<class>" / "think:<class>" registered
bool g_any_msg_hook = false;                    // any "message:<name>" registered

ModSet *g_curmods = nullptr;
struct Export { Plugin *owner; int ref; };
static std::unordered_map<std::string, Export> g_exports;

// ------------------------------------------------------------ logging / paths
void xlog(const char *fmt, ...) {
	char buf[1024]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
	LOG_MESSAGE(PLID, "%s", buf);
}
void xerr(const char *fmt, ...) {
	char buf[2048]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
	LOG_ERROR(PLID, "%s", buf);
	// Metamod's error log is invisible on a listen server (no dedicated console): print it there too, and always keep a file
	if (!IS_DEDICATED_SERVER()) { std::string m = std::string("[xmod] ERROR: ") + buf + "\n"; SERVER_PRINT(m.c_str()); }
	char gd[256] = {0}; GET_GAME_DIR(gd);
	if (FILE *f = fopen((std::string(gd) + "/" XMOD_DIR "/data/xmod_errors.log").c_str(), "a")) {
		time_t t = time(nullptr); char ts[32]; strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&t));
		fprintf(f, "%s  %s\n", ts, buf); fclose(f);
	}
}
std::string xpath(const char *rel) {
	char gd[256] = {0}; GET_GAME_DIR(gd);
	return std::string(gd) + "/" XMOD_DIR "/" + rel;
}
int flags_from_string(const char *s) {
	int f = 0;
	for (; s && *s; ++s) if (*s >= 'a' && *s <= 'z') f |= 1 << (*s - 'a');
	return f;
}
bool is_valid_player(int id) { return id >= 1 && id <= g_maxclients && g_players[id].connected; }
edict_t *player_edict(int id) { return is_valid_player(id) ? INDEXENT(id) : nullptr; }
edict_t *ent_edict(int idx) {
	if (idx < 0 || idx >= gpGlobals->maxEntities) return nullptr;
	edict_t *e = INDEXENT(idx);
	if (!e || e->free) return nullptr;
	if (idx > 0 && !e->pvPrivateData && idx > g_maxclients) return nullptr;
	return e;
}

// ------------------------------------------------------------ Lua allocator, timeout hook
static void *lalloc(void *ud, void *ptr, size_t osize, size_t nsize) {
	size_t &used = *static_cast<size_t *>(ud);
	if (nsize == 0) { if (ptr) { used -= osize; free(ptr); } return nullptr; }
	size_t old = ptr ? osize : 0;
	if (used - old + nsize > MEM_LIMIT) return nullptr;
	void *p = realloc(ptr, nsize);
	if (p) used = used - old + nsize;
	return p;
}
static void abort_hook(lua_State *l, lua_Debug *) {      // runs on the main thread, installed by the watchdog only after an overrun
	lua_sethook(l, nullptr, 0, 0);
	if (g_call_depth > 0 && g_wd_fired.load() && g_call_seq.load() == g_fired_seq.load()) {
		g_wd_fired = false;
		luaL_error(l, "script timeout: a single call ran longer than %d ms (xmod_script_timeout)", g_eff_ms.load());
	}
}
static void spin_lock() { while (g_wd_lock.exchange(true, std::memory_order_acquire)) {} }
static void spin_unlock() { g_wd_lock.store(false, std::memory_order_release); }
static void sleep_ms(int ms) {
#ifdef _WIN32
	Sleep(ms);
#else
	usleep(ms * 1000);
#endif
}
static void wd_body() {
	unsigned long long last_seq = ~0ULL; auto last_change = std::chrono::steady_clock::now();
	while (g_wd_run.load()) {
		sleep_ms(5);
		int to = g_eff_ms.load();
		if (to <= 0 || !g_call_active.load(std::memory_order_acquire)) { last_seq = ~0ULL; continue; }
		unsigned long long seq = g_call_seq.load(std::memory_order_acquire); auto now = std::chrono::steady_clock::now();
		if (seq != last_seq) { last_seq = seq; last_change = now; continue; }
		if (!g_wd_fired.load() && std::chrono::duration_cast<std::chrono::milliseconds>(now - last_change).count() > to) {
			spin_lock();
			if (g_wd_L && g_call_active.load() && g_call_seq.load() == seq) { g_fired_seq = seq; g_wd_fired = true; lua_sethook(g_wd_L, abort_hook, LUA_MASKCOUNT, 1); last_change = now; }
			spin_unlock();
		}
	}
}
#ifdef _WIN32
static HANDLE g_wd_thread = nullptr;
static DWORD WINAPI wd_entry(LPVOID) { wd_body(); return 0; }
static void watchdog_start() { if (g_wd_thread) return; g_wd_run = true; g_wd_thread = CreateThread(nullptr, 0, wd_entry, nullptr, 0, nullptr); }
static void watchdog_stop() { if (!g_wd_thread) return; g_wd_run = false; WaitForSingleObject(g_wd_thread, 2000); CloseHandle(g_wd_thread); g_wd_thread = nullptr; }
#else
static pthread_t g_wd_thread; static bool g_wd_started = false;
static void *wd_entry(void *) { wd_body(); return nullptr; }
static void watchdog_start() { if (g_wd_started) return; g_wd_run = true; g_wd_started = pthread_create(&g_wd_thread, nullptr, wd_entry, nullptr) == 0; }
static void watchdog_stop() { if (!g_wd_started) return; g_wd_run = false; pthread_join(g_wd_thread, nullptr); g_wd_started = false; }
#endif
static void update_eff() { int t = g_limit_ms.load(); g_eff_ms = (t <= 0) ? 0 : (g_loading.load() ? (t * 10 > 1000 ? t * 10 : 1000) : t); }
void core_set_timeout(int ms) { g_limit_ms = ms < 0 ? 0 : ms; update_eff(); }
static int traceback(lua_State *l) {
	const char *m = lua_tostring(l, 1);
	luaL_traceback(l, l, m ? m : "(error object is not a string)", 1);
	return 1;
}

// ------------------------------------------------------------ events
Event *ev_get(const char *name) {
	auto it = g_events.find(name);
	if (it != g_events.end()) return it->second;
	Event *e = new Event; e->name = name; g_events[name] = e; return e;
}
Plugin *caller_plugin(lua_State *l) {
	lua_Debug ar;
	for (int lv = 0; lv < 48 && lua_getstack(l, lv, &ar); lv++) {      // innermost Lua function that comes from a plugin or library file
		if (!lua_getinfo(l, "S", &ar) || ar.source[0] != '@') continue;
		const char *f = ar.source + 1;
		for (Plugin *p : g_plugins) if (p->file == f) return p->enabled ? p : g_cur;
	}
	return g_cur;
}
static void disable_plugin(Plugin *p, const char *why) {
	if (!p->enabled) return;
	p->enabled = false;
	xerr("plugin '%s' disabled: %s", p->name.c_str(), why);
	for (auto &kv : g_events) {
		auto &h = kv.second->handlers;
		for (size_t i = 0; i < h.size();) {
			if (h[i].owner == p) { luaL_unref(L, LUA_REGISTRYINDEX, h[i].ref); h.erase(h.begin() + i); } else ++i;
		}
	}
}
int safe_call_n(Plugin *p, int nargs, int nres, const char *what) {
	int base = lua_gettop(L) - nargs;                 // index of function
	lua_pushcfunction(L, traceback); lua_insert(L, base);
	Plugin *saved = g_cur; if (p) g_cur = p;
	if (g_call_depth++ == 0) { g_call_seq.fetch_add(1, std::memory_order_relaxed); g_call_active.store(true, std::memory_order_release); }
	int rc = lua_pcall(L, nargs, nres, base);
	if (--g_call_depth == 0) {
		g_call_active.store(false, std::memory_order_release);
		msg_script_cleanup();                          // a script that errored between xmod.msg.begin() and finish() must not leave anything half-built
		if (g_wd_fired.load()) { lua_sethook(L, nullptr, 0, 0); g_wd_fired = false; }       // overrun hook that never got to run: clear it
	}
	g_cur = saved;
	if (rc != LUA_OK) {
		const char *msg = lua_tostring(L, -1);
		xerr("[%s] error in %s: %s", p ? p->name.c_str() : "core", what, msg ? msg : "?");
		if (p) { std::string m = msg ? msg : "?"; size_t nl = m.find('\n'); if (nl != std::string::npos) m.resize(nl); p->last_error = std::string(what) + ": " + m; }
		lua_settop(L, base - 1);                       // drop error + traceback handler
		if (p && ++p->errors >= g_max_errors) disable_plugin(p, "too many script errors");
		return -1;
	}
	lua_remove(L, base);                               // drop traceback handler
	return lua_gettop(L) - base + 1;
}
bool safe_call(Plugin *p, int nargs, int nres, const char *what) {
	int top_before = lua_gettop(L) - nargs - 1;
	int n = safe_call_n(p, nargs, nres, what);
	if (n < 0) { lua_settop(L, top_before); for (int i = 0; i < nres; i++) lua_pushnil(L); return false; }
	return true;
}

EvResult ev_fire(Event *e, int, const std::function<int(lua_State *)> &push_args) {
	EvResult res;
	if (e->handlers.empty() || !L) return res;
	StatScope ss(e); HSnap snap(e->handlers);
	for (size_t i = 0; i < snap.n; i++) {
		const Handler &h = snap.p[i];
		if (!h.owner->enabled) continue;
		int top = lua_gettop(L);
		lua_rawgeti(L, LUA_REGISTRYINDEX, h.ref);
		int n = push_args ? push_args(L) : 0;
		if (!safe_call(h.owner, n, 2, e->name.c_str())) { lua_settop(L, top); continue; }
		if (lua_isboolean(L, -2)) {
			if (lua_toboolean(L, -2)) { res.kind = 1; }
			else { res.kind = 2; if (lua_isstring(L, -1)) res.reason = lua_tostring(L, -1); }
			lua_settop(L, top);
			break;
		}
		lua_settop(L, top);
	}
	return res;
}
EvResult ev_fire_nums(Event *e, const double *v, int n, unsigned intmask) {
	EvResult res;
	if (e->handlers.empty() || !L) return res;
	StatScope ss(e); HSnap snap(e->handlers);
	for (size_t i = 0; i < snap.n; i++) {
		const Handler &h = snap.p[i];
		if (!h.owner->enabled) continue;
		int top = lua_gettop(L);
		lua_rawgeti(L, LUA_REGISTRYINDEX, h.ref);
		for (int k = 0; k < n; k++) { if (intmask & (1u << k)) lua_pushinteger(L, (lua_Integer)v[k]); else lua_pushnumber(L, v[k]); }
		if (!safe_call(h.owner, n, 2, e->name.c_str())) { lua_settop(L, top); continue; }
		if (lua_isboolean(L, -2)) {
			if (lua_toboolean(L, -2)) res.kind = 1; else { res.kind = 2; if (lua_isstring(L, -1)) res.reason = lua_tostring(L, -1); }
			lua_settop(L, top); break;
		}
		lua_settop(L, top);
	}
	return res;
}

HookOut ev_fire_hook(Event *e, const std::vector<AV> &av, ModSet &mods) {
	HookOut out; if (e->handlers.empty() || !L) return out;
	StatScope ss(e);
	ModSet *saved = g_curmods; g_curmods = &mods;
	std::vector<Handler> snapshot = e->handlers;
	for (auto &h : snapshot) {
		if (!h.owner->enabled) continue;
		int top = lua_gettop(L);
		lua_rawgeti(L, LUA_REGISTRYINDEX, h.ref);
		for (const AV &a : av) {
			switch (a.kind) {
			case 0: lua_pushinteger(L, (lua_Integer)a.num); break;
			case 1: lua_pushnumber(L, a.num); break;
			case 2: if (a.str) lua_pushstring(L, a.str); else lua_pushnil(L); break;
			case 3: lua_pushinteger(L, (lua_Integer)a.num); break;
			case 4: lua_createtable(L, 0, 3); lua_pushnumber(L, a.v[0]); lua_setfield(L, -2, "x"); lua_pushnumber(L, a.v[1]); lua_setfield(L, -2, "y");
				lua_pushnumber(L, a.v[2]); lua_setfield(L, -2, "z"); break;
			default: lua_pushnil(L);
			}
		}
		if (!safe_call(h.owner, (int)av.size(), 2, e->name.c_str())) { lua_settop(L, top); continue; }
		int code = lua_isnumber(L, -2) ? (int)lua_tointeger(L, -2) : HC_CONTINUE;
		if (code == HC_SUPERCEDE || code == HC_BREAK) {
			out.code = code;
			if (lua_isboolean(L, -1)) { out.has = true; out.num = lua_toboolean(L, -1); }
			else if (lua_type(L, -1) == LUA_TNUMBER) { out.has = true; out.num = lua_tonumber(L, -1); }
			else if (lua_type(L, -1) == LUA_TSTRING) { out.hasstr = true; out.str = lua_tostring(L, -1); }
			lua_settop(L, top); break;
		}
		lua_settop(L, top);
	}
	g_curmods = saved;
	return out;
}

// ------------------------------------------------------------ xmod.* core API
static Plugin *find_plugin(const char *name) {
	for (auto *p : g_plugins) if (p->name == name) return p;
	return nullptr;
}
static int l_register(lua_State *l) {
	if (!g_cur) return 0;
	g_cur->name = luaL_checkstring(l, 1);
	g_cur->version = luaL_optstring(l, 2, "");
	g_cur->author = luaL_optstring(l, 3, "");
	return 0;
}
static int l_on(lua_State *l) {
	const char *ev = luaL_checkstring(l, 1);
	luaL_checktype(l, 2, LUA_TFUNCTION);
	int prio = (int)luaL_optinteger(l, 3, 0);
	Plugin *owner = caller_plugin(l);
	if (!owner) return luaL_error(l, "xmod.on called outside of a plugin context");
	{ std::string herr; if (!hooks_on_register(ev, herr)) return luaL_error(l, "%s", herr.c_str()); }
	lua_pushvalue(l, 2);
	Handler h{owner, luaL_ref(l, LUA_REGISTRYINDEX), prio};
	Event *e = ev_get(ev);
	auto pos = std::find_if(e->handlers.begin(), e->handlers.end(), [&](const Handler &x) { return x.prio < prio; });
	e->handlers.insert(pos, h);
	if (!strncmp(ev, "touch:", 6) || !strncmp(ev, "think:", 6)) g_any_class_hook = true;
	if (!strncmp(ev, "message:", 8)) g_any_msg_hook = true;
	return 0;
}
static int l_off(lua_State *l) {
	Event *e = ev_get(luaL_checkstring(l, 1));
	luaL_checktype(l, 2, LUA_TFUNCTION);
	for (size_t i = 0; i < e->handlers.size(); ++i) {
		lua_rawgeti(l, LUA_REGISTRYINDEX, e->handlers[i].ref);
		bool same = lua_rawequal(l, -1, 2); lua_pop(l, 1);
		if (same) { luaL_unref(l, LUA_REGISTRYINDEX, e->handlers[i].ref); e->handlers.erase(e->handlers.begin() + i); lua_pushboolean(l, 1); return 1; }
	}
	lua_pushboolean(l, 0); return 1;
}
static int l_emit(lua_State *l) {
	Event *e = ev_get(luaL_checkstring(l, 1));
	int argc = lua_gettop(l) - 1;
	EvResult r = ev_fire(e, argc, [&](lua_State *s) { for (int i = 0; i < argc; i++) lua_pushvalue(s, 2 + i); return argc; });
	if (r.kind == 0) { lua_pushnil(l); return 1; }
	lua_pushboolean(l, r.kind == 1);
	if (r.kind == 2) { lua_pushstring(l, r.reason.c_str()); return 2; }
	return 1;
}
static int l_export(lua_State *l) {
	const char *name = luaL_checkstring(l, 1);
	luaL_checktype(l, 2, LUA_TFUNCTION);
	Plugin *owner = caller_plugin(l); if (!owner) return 0;
	lua_pushvalue(l, 2);
	auto it = g_exports.find(name);
	if (it != g_exports.end()) luaL_unref(l, LUA_REGISTRYINDEX, it->second.ref);
	g_exports[name] = Export{owner, luaL_ref(l, LUA_REGISTRYINDEX)};
	return 0;
}
static int l_call(lua_State *l) {
	const char *name = luaL_checkstring(l, 1);
	auto it = g_exports.find(name);
	if (it == g_exports.end() || !it->second.owner->enabled) return luaL_error(l, "no exported function '%s'", name);
	int argc = lua_gettop(l) - 1;
	int top = lua_gettop(l);
	lua_rawgeti(l, LUA_REGISTRYINDEX, it->second.ref);
	for (int i = 0; i < argc; i++) lua_pushvalue(l, 2 + i);
	int n = safe_call_n(it->second.owner, argc, LUA_MULTRET, name);
	if (n < 0) { lua_settop(l, top); lua_pushnil(l); return 1; }
	return lua_gettop(l) - top;
}
static int l_has_export(lua_State *l) {
	auto it = g_exports.find(luaL_checkstring(l, 1));
	lua_pushboolean(l, it != g_exports.end() && it->second.owner->enabled); return 1;
}
static int l_plugins(lua_State *l) {
	lua_newtable(l); int i = 1;
	for (auto *p : g_plugins) {
		lua_newtable(l);
		lua_pushstring(l, p->name.c_str()); lua_setfield(l, -2, "name");
		lua_pushstring(l, p->version.c_str()); lua_setfield(l, -2, "version");
		lua_pushstring(l, p->author.c_str()); lua_setfield(l, -2, "author");
		lua_pushstring(l, p->file.c_str()); lua_setfield(l, -2, "file");
		lua_pushboolean(l, p->enabled); lua_setfield(l, -2, "enabled");
		lua_rawseti(l, -2, i++);
	}
	return 1;
}
static int l_time(lua_State *l) { lua_pushnumber(l, gpGlobals ? gpGlobals->time : 0); return 1; }
static int l_exec(lua_State *l) {
	std::string c = std::string(luaL_checkstring(l, 1)) + "\n";
	SERVER_COMMAND(const_cast<char *>(c.c_str())); return 0;
}
static int l_print(lua_State *l) {
	int n = lua_gettop(l); std::string out;
	for (int i = 1; i <= n; i++) { size_t len; const char *s = luaL_tolstring(l, i, &len); if (i > 1) out += "\t"; out.append(s, len); lua_pop(l, 1); }
	{ Plugin *c = caller_plugin(l); xlog("[%s] %s", c ? c->name.c_str() : "core", out.c_str()); }
	return 0;
}
static int st_enable(lua_State *l) { bool on = lua_toboolean(l, 1); if (on && !g_stats_on) stats_zero(); g_stats_on = on; return 0; }
static int st_reset(lua_State *) { stats_zero(); return 0; }
static double ev_total_us(const Event *e) { return e->samples ? (e->sample_ns / 1000.0) * ((double)e->calls / (double)e->samples) : 0.0; }
static int st_get(lua_State *l) {        // list of {name, calls, total_us (extrapolated), avg_us, max_us (largest SAMPLED call), handlers}, sorted by total time
	std::vector<Event *> v; for (auto &kv : g_events) if (kv.second->calls) v.push_back(kv.second);
	std::sort(v.begin(), v.end(), [](Event *a, Event *b) { return ev_total_us(a) > ev_total_us(b); });
	lua_createtable(l, (int)v.size(), 0); int i = 1;
	for (Event *e : v) {
		lua_createtable(l, 0, 6);
		lua_pushstring(l, e->name.c_str()); lua_setfield(l, -2, "name");
		lua_pushinteger(l, (lua_Integer)e->calls); lua_setfield(l, -2, "calls");
		lua_pushnumber(l, ev_total_us(e)); lua_setfield(l, -2, "total_us");
		lua_pushnumber(l, e->samples ? e->sample_ns / 1000.0 / (double)e->samples : 0); lua_setfield(l, -2, "avg_us");
		lua_pushnumber(l, e->max_ns / 1000.0); lua_setfield(l, -2, "max_us");
		lua_pushinteger(l, (lua_Integer)e->handlers.size()); lua_setfield(l, -2, "handlers");
		lua_rawseti(l, -2, i++);
	}
	return 1; }
static int st_info(lua_State *l) {       // {frames, wall_s, fps, lua_kb, on, frame_ms_avg, frame_ms_max}
	double wall = g_stats_on ? (now_ns() - g_stats_t0) / 1e9 : 0; unsigned long long fr = g_stats_frames;
	lua_createtable(l, 0, 7);
	lua_pushinteger(l, (lua_Integer)fr); lua_setfield(l, -2, "frames");
	lua_pushnumber(l, wall); lua_setfield(l, -2, "wall_s");
	lua_pushnumber(l, wall > 0 ? fr / wall : 0); lua_setfield(l, -2, "fps");
	lua_pushnumber(l, lua_gc(l, LUA_GCCOUNT) + lua_gc(l, LUA_GCCOUNTB) / 1024.0); lua_setfield(l, -2, "lua_kb");
	lua_pushboolean(l, g_stats_on); lua_setfield(l, -2, "on");
	lua_pushnumber(l, fr > 1 ? g_frame_sum_ns / 1e6 / (double)(fr - 1) : 0); lua_setfield(l, -2, "frame_ms_avg");
	lua_pushnumber(l, g_frame_max_ns / 1e6); lua_setfield(l, -2, "frame_ms_max");
	return 1; }
static int l_flags(lua_State *l) { lua_pushinteger(l, flags_from_string(luaL_checkstring(l, 1))); return 1; }

// ------------------------------------------------------------ plugin loading
static std::string read_text(const std::string &path) {
	std::ifstream f(path, std::ios::binary); if (!f) return "";
	return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
static void make_env(Plugin *p) {
	lua_newtable(L);                                   // env
	lua_newtable(L);                                   // mt
	lua_pushglobaltable(L); lua_setfield(L, -2, "__index");
	lua_setmetatable(L, -2);
	lua_pushvalue(L, -1); p->env_ref = luaL_ref(L, LUA_REGISTRYINDEX);
	lua_newtable(L); p->pdata_ref = luaL_ref(L, LUA_REGISTRYINDEX);
}
static bool run_file(Plugin *p, const std::string &path, int nresults) {
	std::string src = read_text(path);
	if (src.empty()) { xerr("cannot read '%s'", path.c_str()); p->last_error = "cannot read " + p->file; return false; }
	std::string chunk = "@" + p->file;
	if (luaL_loadbufferx(L, src.data(), src.size(), chunk.c_str(), "t") != LUA_OK) {
		xerr("syntax error: %s", lua_tostring(L, -1)); p->last_error = std::string("syntax error: ") + lua_tostring(L, -1); lua_pop(L, 1); return false;
	}
	lua_rawgeti(L, LUA_REGISTRYINDEX, p->env_ref);
	lua_setupvalue(L, -2, 1);                          // _ENV = plugin env
	int top = lua_gettop(L) - 1;
	if (!safe_call(p, 0, nresults, "load")) { lua_settop(L, top); return false; }
	return true;
}
static int l_require(lua_State *l) {
	const char *name = luaL_checkstring(l, 1);
	for (const char *c = name; *c; ++c) if (!(isalnum((unsigned char)*c) || *c == '_' || *c == '-')) return luaL_error(l, "bad library name '%s'", name);
	lua_getfield(l, LUA_REGISTRYINDEX, "xmod.libs");
	lua_getfield(l, -1, name);
	if (!lua_isnil(l, -1)) return 1;
	lua_pop(l, 1);
	Plugin *lib = new Plugin; lib->file = std::string("lib/") + name + ".lua"; lib->name = std::string("lib:") + name;
	lib->version = "lib"; g_plugins.push_back(lib); make_env(lib); lua_pop(l, 1);   // make_env leaves env on stack
	int top = lua_gettop(l);
	if (!run_file(lib, xpath(lib->file.c_str()), 1)) { lua_settop(l, top); return luaL_error(l, "failed to load library '%s'", name); }
	lua_pushvalue(l, -1); lua_setfield(l, top, name);  // libs[name] = result
	return 1;
}

void core_load_plugins() {
	g_loading = true; update_eff();
	struct LoadGuard { ~LoadGuard() { g_loading = false; update_eff(); } } load_guard;
	std::string ini = read_text(xpath("configs/plugins.ini"));
	if (ini.empty()) { xerr("configs/plugins.ini missing or empty"); return; }
	size_t pos = 0;
	while (pos <= ini.size()) {
		size_t nl = ini.find('\n', pos); if (nl == std::string::npos) nl = ini.size();
		std::string line = ini.substr(pos, nl - pos); pos = nl + 1;
		size_t c = line.find_first_of(";#"); if (c != std::string::npos) line.erase(c);
		if (line.rfind("//", 0) == 0) continue;
		size_t a = line.find_first_not_of(" \t\r"), b = line.find_last_not_of(" \t\r");
		if (a == std::string::npos) { if (nl >= ini.size()) break; continue; }
		line = line.substr(a, b - a + 1);
		for (char ch : line) if (!(isalnum((unsigned char)ch) || ch == '_' || ch == '-' || ch == '.' || ch == '/')) { xerr("skipping bad plugin entry '%s'", line.c_str()); line.clear(); break; }
		if (line.find("..") != std::string::npos || line[0] == '/') { xerr("skipping bad plugin entry '%s'", line.c_str()); line.clear(); }
		if (line.empty()) continue;
		Plugin *p = new Plugin; p->file = "plugins/" + line;
		{ size_t sl = line.rfind('/'); p->name = line.substr(sl == std::string::npos ? 0 : sl + 1, line.rfind('.') - (sl == std::string::npos ? 0 : sl + 1)); }
		g_plugins.push_back(p); make_env(p); lua_pop(L, 1);
		int top = lua_gettop(L);
		if (!run_file(p, xpath(p->file.c_str()), 0)) { p->enabled = false; lua_settop(L, top); continue; }
		// optional global `plugin = {name=,version=,author=}`
		lua_rawgeti(L, LUA_REGISTRYINDEX, p->env_ref);
		if (lua_getfield(L, -1, "plugin") == LUA_TTABLE) {
			auto fld = [&](const char *k, std::string &out) { if (lua_getfield(L, -1, k) == LUA_TSTRING) out = lua_tostring(L, -1); lua_pop(L, 1); };
			fld("name", p->name); fld("version", p->version); fld("author", p->author);
		}
		lua_settop(L, top);
		xlog("loaded plugin %s %s", p->name.c_str(), p->version.c_str());
		if (nl >= ini.size()) break;
	}
}

static void open_libs() {
	static const luaL_Reg libs[] = { {"_G", luaopen_base}, {LUA_TABLIBNAME, luaopen_table}, {LUA_STRLIBNAME, luaopen_string},
		{LUA_MATHLIBNAME, luaopen_math}, {LUA_UTF8LIBNAME, luaopen_utf8}, {nullptr, nullptr} };
	for (const luaL_Reg *l = libs; l->func; ++l) { luaL_requiref(L, l->name, l->func, 1); lua_pop(L, 1); }
	for (const char *n : {"dofile", "loadfile", "collectgarbage"}) { lua_pushnil(L); lua_setglobal(L, n); }
	lua_pushcfunction(L, l_print); lua_setglobal(L, "print");
	lua_pushcfunction(L, l_require); lua_setglobal(L, "require");
	// tiny safe os
	lua_newtable(L);
	lua_pushcfunction(L, [](lua_State *l) { lua_pushinteger(l, (lua_Integer)time(nullptr)); return 1; }); lua_setfield(L, -2, "time");
	lua_pushcfunction(L, [](lua_State *l) { lua_pushnumber(l, (double)clock() / CLOCKS_PER_SEC); return 1; }); lua_setfield(L, -2, "clock");
	lua_pushcfunction(L, [](lua_State *l) {
		char buf[256]; time_t t = (time_t)luaL_optinteger(l, 2, (lua_Integer)time(nullptr)); struct tm *tmv = localtime(&t);
		strftime(buf, sizeof buf, luaL_optstring(l, 1, "%Y-%m-%d %H:%M:%S"), tmv); lua_pushstring(l, buf); return 1; }); lua_setfield(L, -2, "date");
	lua_setglobal(L, "os");
	lua_newtable(L); lua_setfield(L, LUA_REGISTRYINDEX, "xmod.libs");
}

bool core_init() {
	g_mem_used = 0;
	L = lua_newstate(lalloc, &g_mem_used);
	if (!L) return false;
	spin_lock(); g_wd_L = L; spin_unlock(); watchdog_start();
	open_libs();
	lua_newtable(L);                                   // xmod
	static const luaL_Reg core[] = { {"register", l_register}, {"on", l_on}, {"off", l_off}, {"emit", l_emit},
		{"export", l_export}, {"call", l_call}, {"has_export", l_has_export}, {"plugins", l_plugins},
		{"time", l_time}, {"exec", l_exec}, {"flags", l_flags}, {nullptr, nullptr} };
	luaL_setfuncs(L, core, 0);
	lua_newtable(L);
	static const luaL_Reg stl[] = { {"enable", st_enable}, {"reset", st_reset}, {"get", st_get}, {"info", st_info}, {nullptr, nullptr} };
	luaL_setfuncs(L, stl, 0); lua_setfield(L, -2, "stats");
	lua_pushstring(L, XMOD_VERSION); lua_setfield(L, -2, "version");
	char gd[256] = {0}; GET_GAME_DIR(gd); lua_pushstring(L, gd); lua_setfield(L, -2, "gamedir");
	lua_setglobal(L, "xmod");
	api_register_util(L); api_register_player(L); api_register_ent(L);
	api_register_msg(L); api_register_cmd(L); api_register_misc(L); api_register_hooks(L);
	ext_lua_init();
	lua_getglobal(L, "xmod"); lua_pushvalue(L, -1); lua_setglobal(L, "x"); lua_pop(L, 1);
	return true;
}
static void core_teardown() {
	if (L) {
		kv_flush_all();
		for (auto &kv : g_events) kv.second->handlers.clear();
		for (auto &kv : g_exports) (void)kv;
		g_exports.clear();
		timers_reset(); cmd_reset(); msg_reset(); cvar_watch_reset(); kv_reset();
		ext_lua_closing();
		spin_lock(); g_wd_L = nullptr; spin_unlock(); g_wd_fired = false;
		lua_close(L); L = nullptr;
	}
	for (auto *p : g_plugins) delete p;
	g_plugins.clear(); g_cur = nullptr; g_call_depth = 0; g_any_class_hook = false; g_any_msg_hook = false;
}
void core_shutdown() { core_teardown(); ext_shutdown(); watchdog_stop(); }
void core_reload() {
	core_teardown();
	if (core_init()) core_load_plugins();
}
