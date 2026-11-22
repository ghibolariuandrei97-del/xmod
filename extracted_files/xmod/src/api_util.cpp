#include "xmod.h"
#include <cstring>

static int u_log(lua_State *l) { Plugin *c = caller_plugin(l); xlog("[%s] %s", c ? c->name.c_str() : "core", luaL_checkstring(l, 1)); return 0; }
static int u_error(lua_State *l) { Plugin *c = caller_plugin(l); xerr("[%s] %s", c ? c->name.c_str() : "core", luaL_checkstring(l, 1)); return 0; }
static int u_server_print(lua_State *l) { std::string s = std::string(luaL_checkstring(l, 1)) + "\n"; SERVER_PRINT(s.c_str()); return 0; }
static int u_map(lua_State *l) { lua_pushstring(l, STRING(gpGlobals->mapname)); return 1; }
static int u_maxplayers(lua_State *l) { lua_pushinteger(l, g_maxclients); return 1; }
static int u_dedicated(lua_State *l) { lua_pushboolean(l, IS_DEDICATED_SERVER()); return 1; }
static int u_gametime(lua_State *l) { lua_pushnumber(l, gpGlobals->time); return 1; }

static int u_maxentities(lua_State *l) { lua_pushinteger(l, gpGlobals->maxEntities); return 1; }
static int u_map_valid(lua_State *l) { lua_pushboolean(l, IS_MAP_VALID(const_cast<char *>(luaL_checkstring(l, 1)))); return 1; }

void api_register_util(lua_State *l) {
	lua_getglobal(l, "xmod");
	static const luaL_Reg fns[] = { {"log", u_log}, {"error", u_error}, {"server_print", u_server_print}, {"map", u_map},
		{"maxplayers", u_maxplayers}, {"is_dedicated", u_dedicated}, {"gametime", u_gametime}, {"map_valid", u_map_valid}, {"maxentities", u_maxentities}, {nullptr, nullptr} };
	luaL_setfuncs(l, fns, 0);
	lua_pop(l, 1);
}
