#include "hookutil.h"
#include <cstring>

bool hooks_on_register(const char *ev, std::string &err) {
	std::string e = ev;
	auto strip_post = [](std::string &s) { if (s.size() > 5 && s.compare(s.size() - 5, 5, ":post") == 0) s.erase(s.size() - 5); };
	if (e.rfind("reapi:", 0) == 0) {
		std::string n = e.substr(6); strip_post(n);
		if (!reapi_available()) { err = "reapi hooks need ReGameDLL (not detected on this server) - check xmod.reapi.available() first"; return false; }
		if (!reapi_register(n)) { err = "unknown reapi hook '" + n + "' (see xmod.reapi.list())"; return false; }
		return true;
	}
	if (e.rfind("ham:", 0) == 0) {
		std::string rest = e.substr(4); strip_post(rest);
		size_t c = rest.find(':'); if (c == std::string::npos) { err = "ham event format is ham:<func>:<classname>[:post]"; return false; }
		return ham_register(rest.substr(0, c), rest.substr(c + 1), err);
	}
	return true;
}
static int h_set_arg(lua_State *l) {   // xmod.hook.set_arg(n, value): n counts the hook's own params (the entity 'this' is not counted)
	int n = (int)luaL_checkinteger(l, 1);
	if (!g_curmods) return luaL_error(l, "set_arg is only valid inside a reapi/ham pre-hook");
	if (n < 1 || n > 16) return luaL_error(l, "bad arg index");
	g_curmods->set[n - 1] = true;
	g_curmods->val[n - 1] = lua_isboolean(l, 2) ? (lua_toboolean(l, 2) ? 1 : 0) : luaL_checknumber(l, 2);
	return 0;
}
static int r_available(lua_State *l) { lua_pushboolean(l, reapi_available()); return 1; }
static int r_list(lua_State *l) { reapi_list(l); return 1; }
static int m_list(lua_State *l) { ham_list(l); return 1; }

void api_register_hooks(lua_State *l) {
	lua_getglobal(l, "xmod");
	lua_pushinteger(l, HC_CONTINUE); lua_setfield(l, -2, "HC_CONTINUE");
	lua_pushinteger(l, HC_SUPERCEDE); lua_setfield(l, -2, "HC_SUPERCEDE");
	lua_pushinteger(l, HC_BREAK); lua_setfield(l, -2, "HC_BREAK");
	lua_newtable(l); lua_pushcfunction(l, h_set_arg); lua_setfield(l, -2, "set_arg"); lua_setfield(l, -2, "hook");
	lua_newtable(l); lua_pushcfunction(l, r_available); lua_setfield(l, -2, "available"); lua_pushcfunction(l, r_list); lua_setfield(l, -2, "list"); lua_setfield(l, -2, "reapi");
	lua_newtable(l); lua_pushcfunction(l, m_list); lua_setfield(l, -2, "list"); lua_setfield(l, -2, "ham");
	lua_pop(l, 1);
}
