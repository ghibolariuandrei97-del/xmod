#include "hookutil.h"
#include <cstring>
#include <entity_state.h>
extern bool g_attach_done, g_atfp_pre_installed, g_atfp_post_installed;
extern entity_state_t *g_fp_state;
extern unsigned char g_fp_hide[33][2048]; extern unsigned char g_fp_hide_all[2048]; extern bool g_fp_native;

bool hooks_on_register(const char *ev, std::string &err) {
	std::string e = ev;
	auto strip_post = [](std::string &s) { if (s.size() > 5 && s.compare(s.size() - 5, 5, ":post") == 0) s.erase(s.size() - 5); };
	if (e == "add_to_full_pack" || e == "add_to_full_pack:post") {
		bool inst = (e == "add_to_full_pack") ? g_atfp_pre_installed : g_atfp_post_installed;
		if (g_attach_done && !inst) { err = "'" + e + "' is a very hot hook: it must be registered at the top level of a plugin while the server starts (restart the server or `meta reload xmod` after adding it)"; return false; }
		return true;
	}
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
// xmod.fullpack.*: modify the entity_state being sent to the current client (only valid inside add_to_full_pack:post handlers)
static entity_state_t *fp(lua_State *l) { if (!g_fp_state) luaL_error(l, "xmod.fullpack.* is only valid inside an add_to_full_pack:post handler"); return g_fp_state; }
static int fp_set_render(lua_State *l) {   // set_render(mode [, amount [, fx [, r, g, b]]])  (nil keeps the value)
	entity_state_t *s = fp(l);
	if (!lua_isnoneornil(l, 1)) s->rendermode = (int)luaL_checkinteger(l, 1);
	if (!lua_isnoneornil(l, 2)) s->renderamt = (int)luaL_checkinteger(l, 2);
	if (!lua_isnoneornil(l, 3)) s->renderfx = (int)luaL_checkinteger(l, 3);
	if (!lua_isnoneornil(l, 4)) s->rendercolor.r = (byte)luaL_checkinteger(l, 4);
	if (!lua_isnoneornil(l, 5)) s->rendercolor.g = (byte)luaL_checkinteger(l, 5);
	if (!lua_isnoneornil(l, 6)) s->rendercolor.b = (byte)luaL_checkinteger(l, 6);
	return 0; }
// ---- native hide table (zero Lua calls per entity): Lua decides at a low rate, the engine hook just looks the pair up.
static int fp_native(lua_State *l) {   // xmod.fullpack.native(): call at plugin load time to install the hook
	if (g_attach_done && !g_atfp_pre_installed) return luaL_error(l, "xmod.fullpack.native() must be called at plugin load while the server starts (restart the server after adding it)");
	g_fp_native = true; return 0; }
static void need_native(lua_State *l) { if (!g_fp_native || !g_atfp_pre_installed && g_attach_done) luaL_error(l, "call xmod.fullpack.native() at plugin load first"); }
static int fp_hide(lua_State *l) {     // hide(host, ent, true|false)  - hide entity `ent` from client `host`
	need_native(l); int h = (int)luaL_checkinteger(l, 1), e = (int)luaL_checkinteger(l, 2);
	if (h < 1 || h > 32 || e < 0 || e >= 2048) return luaL_error(l, "bad host/ent"); g_fp_hide[h][e] = lua_toboolean(l, 3) ? 1 : 0; return 0; }
static int fp_hide_all(lua_State *l) { // hide_all(ent, true|false) - hide entity from every client
	need_native(l); int e = (int)luaL_checkinteger(l, 1); if (e < 0 || e >= 2048) return luaL_error(l, "bad ent"); g_fp_hide_all[e] = lua_toboolean(l, 2) ? 1 : 0; return 0; }
static int fp_clear(lua_State *l) { memset(g_fp_hide, 0, sizeof g_fp_hide); memset(g_fp_hide_all, 0, sizeof g_fp_hide_all); return 0; }
static int fp_set_effects(lua_State *l) { fp(l)->effects = (int)luaL_checkinteger(l, 1); return 0; }
static int fp_set_origin(lua_State *l) { entity_state_t *s = fp(l); s->origin[0] = (float)luaL_checknumber(l, 1); s->origin[1] = (float)luaL_checknumber(l, 2); s->origin[2] = (float)luaL_checknumber(l, 3); return 0; }
static int fp_get_render(lua_State *l) { entity_state_t *s = fp(l); lua_pushinteger(l, s->rendermode); lua_pushinteger(l, s->renderamt); lua_pushinteger(l, s->renderfx); return 3; }
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
	lua_newtable(l);
	static const luaL_Reg fpl[] = { {"native", fp_native}, {"hide", fp_hide}, {"hide_all", fp_hide_all}, {"clear", fp_clear}, {"set_render", fp_set_render}, {"get_render", fp_get_render}, {"set_effects", fp_set_effects}, {"set_origin", fp_set_origin}, {nullptr, nullptr} };
	luaL_setfuncs(l, fpl, 0); lua_setfield(l, -2, "fullpack");
	lua_newtable(l); lua_pushcfunction(l, m_list); lua_setfield(l, -2, "list"); lua_setfield(l, -2, "ham");
	lua_pop(l, 1);
}
