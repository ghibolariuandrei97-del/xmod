#include "xmod.h"
#include <cstring>
#include <cstdio>
#include <unordered_set>
#include <algorithm>

void xmod_reload();                                // main.cpp
extern bool g_any_class_hook;

struct Cmd { std::string name; int flags; Plugin *owner; int ref; std::string desc; };
static std::vector<Cmd> g_client_cmds, g_say_cmds, g_server_cmds;
static std::unordered_set<std::string> g_registered_svr;

static std::string lower(std::string s) { for (auto &c : s) c = (char)tolower((unsigned char)c); return s; }
static void drop_all(std::vector<Cmd> &v) { if (L) for (auto &c : v) luaL_unref(L, LUA_REGISTRYINDEX, c.ref); v.clear(); }
void cmd_reset() { drop_all(g_client_cmds); drop_all(g_say_cmds); drop_all(g_server_cmds); }

static void add_cmd(lua_State *l, std::vector<Cmd> &v, const char *name, const char *flags, int fnidx, const char *desc) {
	luaL_checktype(l, fnidx, LUA_TFUNCTION);
	if (!g_cur) luaL_error(l, "command registered outside of a plugin context");
	lua_pushvalue(l, fnidx);
	v.push_back(Cmd{lower(name), flags_from_string(flags), g_cur, luaL_ref(l, LUA_REGISTRYINDEX), desc ? desc : ""});
}
static int push_args_table(lua_State *l, int firstArg, const char *text) {
	lua_newtable(l); int n = 0;
	for (int a = firstArg; a < CMD_ARGC(); a++) { lua_pushstring(l, CMD_ARGV(a)); lua_rawseti(l, -2, ++n); }
	lua_pushinteger(l, n); lua_setfield(l, -2, "n");
	lua_pushstring(l, text ? text : ""); lua_setfield(l, -2, "text");
	return 1;
}
static int push_tokens_table(lua_State *l, const std::string &rest) {
	lua_newtable(l); int n = 0; size_t i = 0;
	while (i < rest.size()) {
		while (i < rest.size() && isspace((unsigned char)rest[i])) i++;
		size_t j = i; while (j < rest.size() && !isspace((unsigned char)rest[j])) j++;
		if (j > i) { lua_pushlstring(l, rest.data() + i, j - i); lua_rawseti(l, -2, ++n); }
		i = j;
	}
	lua_pushinteger(l, n); lua_setfield(l, -2, "n"); lua_pushstring(l, rest.c_str()); lua_setfield(l, -2, "text");
	return 1;
}
// returns 0 handled/no-access-ok, 1 consumed
static bool run_cmd(const Cmd &c, int id, edict_t *ent, const std::function<int(lua_State *)> &pushargs, bool is_say) {
	if (!c.owner->enabled) return false;
	if (id > 0 && c.flags && (g_players[id].access & c.flags) != c.flags) {
		CLIENT_PRINTF(ent, is_say ? print_chat : print_console, "You have no access to that command.\n"); return true;
	}
	int top = lua_gettop(L);
	lua_rawgeti(L, LUA_REGISTRYINDEX, c.ref);
	int n = 0;
	if (id > 0) { lua_pushinteger(L, id); n++; }
	n += pushargs(L);
	safe_call(c.owner, n, 1, c.name.c_str());
	bool consumed = !lua_isboolean(L, -1) || lua_toboolean(L, -1);   // default: consumed, unless returns false
	lua_settop(L, top);
	return consumed;
}

bool cmd_client_dispatch(edict_t *pEntity) {
	int id = ENTINDEX(pEntity);
	if (!L || !is_valid_player(id)) return false;
	const char *c0 = CMD_ARGV(0); if (!c0 || !*c0) return false;
	std::string cmd = lower(c0);
	bool is_say = (cmd == "say" || cmd == "say_team");
	std::string text, first, rest;
	if (is_say) {
		const char *a = CMD_ARGS(); text = a ? a : "";
		if (text.size() >= 2 && text.front() == '"' && text.back() == '"') text = text.substr(1, text.size() - 2);
		else if (!text.empty() && text.front() == '"') text.erase(0, 1);
		size_t s = text.find_first_not_of(" \t"); text = (s == std::string::npos) ? "" : text.substr(s);
		size_t sp = text.find_first_of(" \t"); first = lower(text.substr(0, sp)); rest = (sp == std::string::npos) ? "" : text.substr(sp + 1);
		static Event *ev_chat = ev_get("chat");
		if (ev_has(ev_chat)) {
			EvResult r = ev_fire(ev_chat, 3, [&](lua_State *l) { lua_pushinteger(l, id); lua_pushstring(l, text.c_str()); lua_pushboolean(l, cmd == "say_team"); return 3; });
			if (r.kind == 1) return true;
		}
	}
	static Event *ev_cc = ev_get("client_command");
	if (ev_has(ev_cc)) {
		EvResult r = ev_fire(ev_cc, 3, [&](lua_State *l) {
			lua_pushinteger(l, id); lua_pushstring(l, cmd.c_str());
			if (is_say) return 2 + push_tokens_table(l, text); return 2 + push_args_table(l, 1, CMD_ARGS()); });
		if (r.kind == 1) return true;
	}
	if (is_say) {
		for (auto &c : g_say_cmds) if (c.name == first) {
			if (run_cmd(c, id, pEntity, [&](lua_State *l) { return push_tokens_table(l, rest); }, true)) return true;
		}
	} else {
		for (auto &c : g_client_cmds) if (c.name == cmd) {
			if (run_cmd(c, id, pEntity, [&](lua_State *l) { return push_args_table(l, 1, CMD_ARGS()); }, false)) return true;
		}
	}
	return false;
}

static void svr_builtin() {
	std::string sub = CMD_ARGC() > 1 ? lower(CMD_ARGV(1)) : "help";
	char buf[512];
	if (sub == "version") { snprintf(buf, sizeof buf, "xmod %s (Lua %s.%s) - %zu plugins\n", XMOD_VERSION, "5", "4", g_plugins.size()); SERVER_PRINT(buf); }
	else if (sub == "plugins") {
		SERVER_PRINT("  # name                 version    author           status\n"); int i = 0;
		for (auto *p : g_plugins) { snprintf(buf, sizeof buf, "%3d %-20s %-10s %-16s %s\n", ++i, p->name.c_str(), p->version.c_str(), p->author.c_str(), p->enabled ? "running" : "DISABLED"); SERVER_PRINT(buf); }
	} else if (sub == "cmds") {
		for (auto &c : g_server_cmds) { snprintf(buf, sizeof buf, "  [srv] %-24s %s\n", c.name.c_str(), c.desc.c_str()); SERVER_PRINT(buf); }
		for (auto &c : g_client_cmds) { snprintf(buf, sizeof buf, "  [cl ] %-24s %s\n", c.name.c_str(), c.desc.c_str()); SERVER_PRINT(buf); }
		for (auto &c : g_say_cmds) { snprintf(buf, sizeof buf, "  [say] %-24s %s\n", c.name.c_str(), c.desc.c_str()); SERVER_PRINT(buf); }
	} else if (sub == "reload") { SERVER_PRINT("xmod: reloading scripts...\n"); xmod_reload(); }
	else SERVER_PRINT("usage: xmod <version|plugins|cmds|reload>\n");
}
static void svr_dispatch() {
	std::string name = lower(CMD_ARGV(0));
	if (name == "xmod") { svr_builtin(); return; }
	if (!L) return;
	for (auto &c : g_server_cmds) if (c.name == name) {
		if (run_cmd(c, 0, nullptr, [&](lua_State *l) { return push_args_table(l, 1, CMD_ARGS()); }, false)) return;
	}
}
static void register_svr(const char *name) {
	std::string n = lower(name);
	if (g_registered_svr.insert(n).second) REG_SVR_COMMAND(strdup(n.c_str()), svr_dispatch);   // engine keeps the pointer
}
void cmd_register_builtin() { register_svr("xmod"); }

static int c_client(lua_State *l) { add_cmd(l, g_client_cmds, luaL_checkstring(l, 1), luaL_optstring(l, 2, ""), 3, luaL_optstring(l, 4, "")); return 0; }
static int c_say(lua_State *l) { add_cmd(l, g_say_cmds, luaL_checkstring(l, 1), luaL_optstring(l, 2, ""), 3, luaL_optstring(l, 4, "")); return 0; }
static int c_server(lua_State *l) {
	const char *n = luaL_checkstring(l, 1); if (!strcasecmp(n, "xmod")) return luaL_error(l, "'xmod' is reserved");
	add_cmd(l, g_server_cmds, n, "", 2, luaL_optstring(l, 3, "")); register_svr(n); return 0; }

// ------------------------------------------------------------ cvars
static int cv_create(lua_State *l) {
	const char *name = luaL_checkstring(l, 1), *def = luaL_optstring(l, 2, "0"); int flags = (int)luaL_optinteger(l, 3, 0);
	if (!CVAR_GET_POINTER(name)) {
		cvar_t *cv = new cvar_t; cv->name = strdup(name); cv->string = strdup(def); cv->flags = flags; cv->value = 0; cv->next = nullptr;
		CVAR_REGISTER(cv);
	}
	return 0; }
static int cv_get(lua_State *l) { lua_pushstring(l, CVAR_GET_STRING(luaL_checkstring(l, 1))); return 1; }
static int cv_num(lua_State *l) { lua_pushnumber(l, CVAR_GET_FLOAT(luaL_checkstring(l, 1))); return 1; }
static int cv_int(lua_State *l) { lua_pushinteger(l, (lua_Integer)CVAR_GET_FLOAT(luaL_checkstring(l, 1))); return 1; }
static int cv_set(lua_State *l) {
	const char *n = luaL_checkstring(l, 1); luaL_checkany(l, 2); size_t len; const char *v = luaL_tolstring(l, 2, &len);
	if (strchr(v, '"') || strchr(v, ';') || strchr(v, '\n')) return luaL_error(l, "illegal character in cvar value");
	CVAR_SET_STRING(n, v); return 0; }
static int cv_exists(lua_State *l) { lua_pushboolean(l, CVAR_GET_POINTER(luaL_checkstring(l, 1)) != nullptr); return 1; }

struct Watch { std::string name, last; Plugin *owner; int ref; };
static std::vector<Watch> g_watches;
void cvar_watch_reset() { if (L) for (auto &w : g_watches) luaL_unref(L, LUA_REGISTRYINDEX, w.ref); g_watches.clear(); }
static int cv_watch(lua_State *l) {
	const char *n = luaL_checkstring(l, 1); luaL_checktype(l, 2, LUA_TFUNCTION); if (!g_cur) return 0;
	lua_pushvalue(l, 2); const char *cur = CVAR_GET_STRING(n);
	g_watches.push_back(Watch{n, cur ? cur : "", g_cur, luaL_ref(l, LUA_REGISTRYINDEX)}); return 0; }
void cvar_watch_run() {
	static float next = 0; if (g_time < next) return; next = g_time + 0.5f;
	for (size_t i = 0; i < g_watches.size(); i++) {
		const char *cur = CVAR_GET_STRING(g_watches[i].name.c_str()); std::string now = cur ? cur : "";
		if (now == g_watches[i].last || !g_watches[i].owner->enabled) continue;
		std::string old = g_watches[i].last; g_watches[i].last = now;
		int top = lua_gettop(L); lua_rawgeti(L, LUA_REGISTRYINDEX, g_watches[i].ref);
		lua_pushstring(L, g_watches[i].name.c_str()); lua_pushstring(L, old.c_str()); lua_pushstring(L, now.c_str());
		safe_call(g_watches[i].owner, 3, 0, "cvar.watch"); lua_settop(L, top);
	}
}
// client cvar queries
struct Query { Plugin *owner; int ref; };
static std::unordered_map<int, Query> g_queries; static int g_qid = 1000;
static int cv_query_client(lua_State *l) {
	int id = (int)luaL_checkinteger(l, 1); const char *n = luaL_checkstring(l, 2); luaL_checktype(l, 3, LUA_TFUNCTION);
	edict_t *e = player_edict(id); if (!e || !g_cur) return luaL_error(l, "invalid player");
	if (!g_engfuncs.pfnQueryClientCvarValue2) return luaL_error(l, "engine does not support QueryClientCvarValue2");
	lua_pushvalue(l, 3); int req = ++g_qid; g_queries[req] = Query{g_cur, luaL_ref(l, LUA_REGISTRYINDEX)};
	g_engfuncs.pfnQueryClientCvarValue2(e, n, req); return 0; }
void cvarquery_result(const edict_t *e, int req, const char *name, const char *value) {
	auto it = g_queries.find(req); if (it == g_queries.end() || !L) return;
	Query q = it->second; g_queries.erase(it);
	int top = lua_gettop(L); lua_rawgeti(L, LUA_REGISTRYINDEX, q.ref); luaL_unref(L, LUA_REGISTRYINDEX, q.ref);
	lua_pushinteger(L, ENTINDEX(const_cast<edict_t *>(e))); lua_pushstring(L, name); lua_pushstring(L, value);
	if (q.owner->enabled) safe_call(q.owner, 3, 0, "cvar.query_client"); lua_settop(L, top);
}

void api_register_cmd(lua_State *l) {
	lua_getglobal(l, "xmod");
	lua_newtable(l);
	static const luaL_Reg cmd[] = { {"client", c_client}, {"say", c_say}, {"server", c_server}, {nullptr, nullptr} };
	luaL_setfuncs(l, cmd, 0); lua_setfield(l, -2, "cmd");
	lua_newtable(l);
	static const luaL_Reg cv[] = { {"create", cv_create}, {"get", cv_get}, {"num", cv_num}, {"int", cv_int}, {"set", cv_set},
		{"exists", cv_exists}, {"watch", cv_watch}, {"query_client", cv_query_client}, {nullptr, nullptr} };
	luaL_setfuncs(l, cv, 0);
	lua_pushinteger(l, FCVAR_SERVER); lua_setfield(l, -2, "SERVER"); lua_pushinteger(l, FCVAR_ARCHIVE); lua_setfield(l, -2, "ARCHIVE");
	lua_pushinteger(l, FCVAR_PROTECTED); lua_setfield(l, -2, "PROTECTED"); lua_pushinteger(l, FCVAR_SPONLY); lua_setfield(l, -2, "SPONLY");
	lua_setfield(l, -2, "cvar"); lua_pop(l, 1);
}
