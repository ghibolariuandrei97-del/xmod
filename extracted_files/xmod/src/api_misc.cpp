#include "xmod.h"
#include <cstring>
#include <cstdio>
#include <fstream>
#include <map>

extern bool g_precaching;                          // set by main.cpp during the precache event

// ============================================================ timers
struct Timer { int id; Plugin *owner; int fnref, argref; float next, interval; int repeats; bool dead; bool rel; };
static std::vector<Timer> g_timers; static int g_timer_id = 1;
static void timer_free(Timer &t) { if (L) { luaL_unref(L, LUA_REGISTRYINDEX, t.fnref); luaL_unref(L, LUA_REGISTRYINDEX, t.argref); } }
// Map change: game time restarts at 0, so keep timers but store their remaining time and re-base on the first frame of the new map.
void timers_map_end() { for (auto &t : g_timers) { if (!t.rel) { t.next = t.next - g_time; if (t.next < 0.01f) t.next = 0.01f; t.rel = true; } } }
void timers_reset() { for (auto &t : g_timers) timer_free(t); g_timers.clear(); }
static int add_timer(lua_State *l, float delay, float interval, int repeats, int fnidx) {
	luaL_checktype(l, fnidx, LUA_TFUNCTION); if (!g_cur) return luaL_error(l, "timer created outside of a plugin context");
	if (delay < 0.01f) delay = 0.01f;
	if (g_timers.size() > 8192) return luaL_error(l, "too many timers");
	int nargs = lua_gettop(l) - fnidx;
	lua_createtable(l, nargs, 1);
	for (int i = 1; i <= nargs; i++) { lua_pushvalue(l, fnidx + i); lua_rawseti(l, -2, i); }
	lua_pushinteger(l, nargs); lua_setfield(l, -2, "n");
	int argref = luaL_ref(l, LUA_REGISTRYINDEX);
	lua_pushvalue(l, fnidx); int fnref = luaL_ref(l, LUA_REGISTRYINDEX);
	g_timers.push_back(Timer{g_timer_id, g_cur, fnref, argref, gpGlobals->time + delay, interval, repeats, false, false});
	lua_pushinteger(l, g_timer_id++); return 1;
}
static int t_after(lua_State *l) { return add_timer(l, (float)luaL_checknumber(l, 1), 0, 1, 2); }
static int t_every(lua_State *l) { float i = (float)luaL_checknumber(l, 1); return add_timer(l, i, i, -1, 2); }
static int t_times(lua_State *l) { int n = (int)luaL_checkinteger(l, 1); float i = (float)luaL_checknumber(l, 2); return add_timer(l, i, i, n < 1 ? 1 : n, 3); }
static int t_cancel(lua_State *l) {
	int id = (int)luaL_checkinteger(l, 1);
	for (auto &t : g_timers) if (t.id == id && !t.dead) { t.dead = true; lua_pushboolean(l, 1); return 1; }
	lua_pushboolean(l, 0); return 1; }
void timers_run() {
	if (g_timers.empty() || !L) return;
	size_t n = g_timers.size();
	for (auto &t : g_timers) if (t.rel) { t.next += g_time; t.rel = false; }
	for (size_t i = 0; i < n && i < g_timers.size(); i++) {
		if (g_timers[i].dead || g_timers[i].next > g_time) continue;
		Plugin *owner = g_timers[i].owner; if (!owner->enabled) { g_timers[i].dead = true; continue; }
		int fnref = g_timers[i].fnref, argref = g_timers[i].argref;
		int top = lua_gettop(L);
		lua_rawgeti(L, LUA_REGISTRYINDEX, fnref);
		lua_rawgeti(L, LUA_REGISTRYINDEX, argref);
		lua_getfield(L, -1, "n"); int na = (int)lua_tointeger(L, -1); lua_pop(L, 1);
		for (int k = 1; k <= na; k++) lua_rawgeti(L, -k, k);       // push args above the table
		// stack: fn, argtable, a1..an  -> remove argtable
		lua_remove(L, -(na + 1));
		safe_call(owner, na, 1, "timer");
		bool cancel = lua_isboolean(L, -1) && !lua_toboolean(L, -1);
		lua_settop(L, top);
		if (i >= g_timers.size()) break;
		Timer &t = g_timers[i];
		if (cancel) t.dead = true;
		else if (t.repeats > 0 && --t.repeats == 0) t.dead = true;
		else if (t.interval > 0) t.next = g_time + t.interval; else t.dead = true;
	}
	for (size_t i = 0; i < g_timers.size();) { if (g_timers[i].dead) { timer_free(g_timers[i]); g_timers.erase(g_timers.begin() + i); } else ++i; }
}

// ============================================================ key/value store
struct KV { std::map<std::string, std::string> d; bool dirty = false; };
static std::unordered_map<std::string, KV> g_kv;
static bool good_name(const char *n) { if (!*n) return false; for (; *n; ++n) if (!(isalnum((unsigned char)*n) || *n == '_' || *n == '-')) return false; return true; }
static std::string esc(const std::string &s) { std::string o; for (char c : s) { if (c == '\\') o += "\\\\"; else if (c == '\n') o += "\\n"; else if (c == '\t') o += "\\t"; else if (c != '\r') o += c; } return o; }
static std::string unesc(const std::string &s) { std::string o; for (size_t i = 0; i < s.size(); i++) { if (s[i] == '\\' && i + 1 < s.size()) { char c = s[++i]; o += c == 'n' ? '\n' : c == 't' ? '\t' : c; } else o += s[i]; } return o; }
static KV &kv_store(lua_State *l, const char *name) {
	if (!good_name(name)) luaL_error(l, "bad store name '%s'", name);
	auto it = g_kv.find(name); if (it != g_kv.end()) return it->second;
	KV &kv = g_kv[name]; std::ifstream f(xpath((std::string("data/") + name + ".kv").c_str())); std::string line;
	while (std::getline(f, line)) { size_t t = line.find('\t'); if (t == std::string::npos) continue; kv.d[unesc(line.substr(0, t))] = unesc(line.substr(t + 1)); }
	return kv;
}
static void kv_save(const std::string &name, KV &kv) {
	std::ofstream f(xpath((std::string("data/") + name + ".kv").c_str()), std::ios::trunc);
	if (!f) { xerr("cannot write data/%s.kv", name.c_str()); return; }
	for (auto &p : kv.d) f << esc(p.first) << '\t' << esc(p.second) << '\n';
	kv.dirty = false;
}
void kv_flush_all() { for (auto &p : g_kv) if (p.second.dirty) kv_save(p.first, p.second); }
void kv_reset() { g_kv.clear(); }
static int k_get(lua_State *l) { KV &kv = kv_store(l, luaL_checkstring(l, 1)); auto it = kv.d.find(luaL_checkstring(l, 2));
	if (it == kv.d.end()) { if (lua_gettop(l) >= 3) lua_pushvalue(l, 3); else lua_pushnil(l); } else lua_pushstring(l, it->second.c_str()); return 1; }
static int k_set(lua_State *l) { KV &kv = kv_store(l, luaL_checkstring(l, 1)); size_t n; const char *v = luaL_tolstring(l, 3, &n);
	kv.d[luaL_checkstring(l, 2)] = std::string(v, n); kv.dirty = true; return 0; }
static int k_del(lua_State *l) { KV &kv = kv_store(l, luaL_checkstring(l, 1)); kv.dirty |= kv.d.erase(luaL_checkstring(l, 2)) > 0; return 0; }
static int k_save(lua_State *l) { const char *n = luaL_checkstring(l, 1); kv_save(n, kv_store(l, n)); return 0; }
static int k_keys(lua_State *l) { KV &kv = kv_store(l, luaL_checkstring(l, 1)); const char *pre = luaL_optstring(l, 2, ""); lua_newtable(l); int i = 1;
	for (auto &p : kv.d) if (!strncmp(p.first.c_str(), pre, strlen(pre))) { lua_pushstring(l, p.first.c_str()); lua_rawseti(l, -2, i++); } return 1; }

// ============================================================ sound / precache
static int s_emit(lua_State *l) {   // emit(ent, channel, sample [, vol, attn, flags, pitch])
	edict_t *e = ent_edict((int)luaL_checkinteger(l, 1)); if (!e) return luaL_error(l, "invalid entity");
	g_engfuncs.pfnEmitSound(e, (int)luaL_checkinteger(l, 2), luaL_checkstring(l, 3), (float)luaL_optnumber(l, 4, 1.0), (float)luaL_optnumber(l, 5, 0.8),
		(int)luaL_optinteger(l, 6, 0), (int)luaL_optinteger(l, 7, 100)); return 0; }
static int guard(lua_State *l) { if (!g_precaching) { luaL_error(l, "precache is only allowed inside the 'precache' event"); } return 0; }
static int pc_sound(lua_State *l) { guard(l); lua_pushinteger(l, PRECACHE_SOUND(const_cast<char *>(luaL_checkstring(l, 1)))); return 1; }
static int pc_model(lua_State *l) { guard(l); lua_pushinteger(l, PRECACHE_MODEL(const_cast<char *>(luaL_checkstring(l, 1)))); return 1; }
static int pc_generic(lua_State *l) { guard(l); lua_pushinteger(l, PRECACHE_GENERIC(const_cast<char *>(luaL_checkstring(l, 1)))); return 1; }

// ============================================================ sandboxed file access
static bool safe_rel(const char *p) {
	if (!*p || strstr(p, "..") || p[0] == '/' || p[0] == '\\' || strchr(p, ':')) return false;
	return true;
}
static std::string game_path(const char *rel) { char gd[256] = {0}; GET_GAME_DIR(gd); return std::string(gd) + "/" + rel; }
static bool slurp(const std::string &path, std::string &out) {
	std::ifstream f(path, std::ios::binary); if (!f) return false;
	out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); return true;
}
static int fs_read(lua_State *l) {        // fs.read("configs/x.ini") -> string|nil   (relative to addons/xmod)
	const char *r = luaL_checkstring(l, 1); std::string o;
	if (!safe_rel(r) || !slurp(xpath(r), o)) { lua_pushnil(l); return 1; }
	lua_pushlstring(l, o.data(), o.size()); return 1; }
static int fs_read_game(lua_State *l) {   // fs.read_game("mapcycle.txt") -> string|nil (relative to the mod dir, read-only)
	const char *r = luaL_checkstring(l, 1); std::string o;
	if (!safe_rel(r) || !slurp(game_path(r), o)) { lua_pushnil(l); return 1; }
	lua_pushlstring(l, o.data(), o.size()); return 1; }
static int fs_write(lua_State *l) {       // fs.write("data/x.txt", text [, append]) - only data/ and configs/ are writable
	const char *r = luaL_checkstring(l, 1); size_t n; const char *t = luaL_checklstring(l, 2, &n);
	if (!safe_rel(r) || (strncmp(r, "data/", 5) && strncmp(r, "configs/", 8))) return luaL_error(l, "fs.write: only data/ and configs/ are writable");
	std::ofstream f(xpath(r), lua_toboolean(l, 3) ? std::ios::app : std::ios::trunc); if (!f) { lua_pushboolean(l, 0); return 1; }
	f.write(t, (std::streamsize)n); lua_pushboolean(l, 1); return 1; }
static int fs_exists(lua_State *l) { const char *r = luaL_checkstring(l, 1); std::ifstream f(safe_rel(r) ? xpath(r) : std::string()); lua_pushboolean(l, (bool)f); return 1; }
static int fs_exists_game(lua_State *l) { const char *r = luaL_checkstring(l, 1); std::ifstream f(safe_rel(r) ? game_path(r) : std::string()); lua_pushboolean(l, (bool)f); return 1; }

void api_register_misc(lua_State *l) {
	lua_getglobal(l, "xmod");
	lua_newtable(l); static const luaL_Reg tm[] = { {"after", t_after}, {"every", t_every}, {"times", t_times}, {"cancel", t_cancel}, {nullptr, nullptr} };
	luaL_setfuncs(l, tm, 0); lua_setfield(l, -2, "timer");
	lua_newtable(l); static const luaL_Reg kv[] = { {"get", k_get}, {"set", k_set}, {"del", k_del}, {"save", k_save}, {"keys", k_keys}, {nullptr, nullptr} };
	luaL_setfuncs(l, kv, 0); lua_setfield(l, -2, "kv");
	lua_newtable(l); static const luaL_Reg sn[] = { {"emit", s_emit}, {nullptr, nullptr} };
	luaL_setfuncs(l, sn, 0);
	lua_pushinteger(l, CHAN_AUTO); lua_setfield(l, -2, "CHAN_AUTO"); lua_pushinteger(l, CHAN_WEAPON); lua_setfield(l, -2, "CHAN_WEAPON");
	lua_pushinteger(l, CHAN_VOICE); lua_setfield(l, -2, "CHAN_VOICE"); lua_pushinteger(l, CHAN_ITEM); lua_setfield(l, -2, "CHAN_ITEM");
	lua_setfield(l, -2, "sound");
	lua_newtable(l); static const luaL_Reg fsr[] = { {"read", fs_read}, {"read_game", fs_read_game}, {"write", fs_write}, {"exists", fs_exists}, {"exists_game", fs_exists_game}, {nullptr, nullptr} };
	luaL_setfuncs(l, fsr, 0); lua_setfield(l, -2, "fs");
	lua_newtable(l); static const luaL_Reg pc[] = { {"sound", pc_sound}, {"model", pc_model}, {"generic", pc_generic}, {nullptr, nullptr} };
	luaL_setfuncs(l, pc, 0); lua_setfield(l, -2, "precache");
	lua_pop(l, 1);
}
