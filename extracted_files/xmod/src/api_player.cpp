#include "xmod.h"
#include <cstring>
#include <cstdio>

// CS 1.6 CBasePlayer private data offsets (ints). Linux builds add a 5-int shift.
static const int OFS_TEAM = 114, OFS_MONEY = 115;
#ifdef __linux__
static const int LINUX_PLAYER = 5;
#else
static const int LINUX_PLAYER = 0;
#endif

static edict_t *P(lua_State *l, int idx = 1) {
	int id = (int)luaL_checkinteger(l, idx);
	edict_t *e = player_edict(id);
	if (!e) luaL_error(l, "invalid or disconnected player id %d", id);
	return e;
}
static int *pint(edict_t *e, int ofs) {
	if (!e->pvPrivateData) return nullptr;
	return reinterpret_cast<int *>(e->pvPrivateData) + ofs + LINUX_PLAYER;
}
static int p_connected(lua_State *l) { lua_pushboolean(l, is_valid_player((int)luaL_checkinteger(l, 1))); return 1; }
static int p_ingame(lua_State *l) { int id = (int)luaL_checkinteger(l, 1); lua_pushboolean(l, is_valid_player(id) && g_players[id].putin); return 1; }
static int p_name(lua_State *l) { P(l); lua_pushstring(l, g_players[lua_tointeger(l, 1)].name.c_str()); return 1; }
static int p_ip(lua_State *l) { P(l); lua_pushstring(l, g_players[lua_tointeger(l, 1)].ip.c_str()); return 1; }
static int p_steamid(lua_State *l) {
	edict_t *e = P(l); const char *a = GETPLAYERAUTHID(e); lua_pushstring(l, a ? a : ""); return 1; }
static int p_userid(lua_State *l) { edict_t *e = P(l); lua_pushinteger(l, GETPLAYERUSERID(e)); return 1; }
static int p_bot(lua_State *l) { edict_t *e = P(l); lua_pushboolean(l, (e->v.flags & FL_FAKECLIENT) != 0); return 1; }
static int p_alive(lua_State *l) { edict_t *e = P(l); lua_pushboolean(l, e->v.deadflag == DEAD_NO && e->v.health > 0); return 1; }
static int p_team(lua_State *l) { edict_t *e = P(l); int *t = pint(e, OFS_TEAM); lua_pushinteger(l, t ? *t : 0); return 1; }   // 0 unassigned,1 T,2 CT,3 SPEC
static int p_money(lua_State *l) { edict_t *e = P(l); int *t = pint(e, OFS_MONEY); lua_pushinteger(l, t ? *t : 0); return 1; }
static int p_set_money(lua_State *l) {
	edict_t *e = P(l); int *t = pint(e, OFS_MONEY); if (!t) return 0;
	int v = (int)luaL_checkinteger(l, 2); if (v < 0) v = 0; if (v > 16000) v = 16000; *t = v;
	static int mid = 0; if (!mid) mid = GET_USER_MSG_ID(PLID, "Money", nullptr);
	if (mid && luaL_optinteger(l, 3, 1)) { MESSAGE_BEGIN(MSG_ONE, mid, nullptr, e); WRITE_LONG(v); WRITE_BYTE(1); MESSAGE_END(); }
	return 0;
}
static int p_health(lua_State *l) { lua_pushnumber(l, P(l)->v.health); return 1; }
static int p_set_health(lua_State *l) { P(l)->v.health = (float)luaL_checknumber(l, 2); return 0; }
static int p_armor(lua_State *l) { lua_pushnumber(l, P(l)->v.armorvalue); return 1; }
static int p_set_armor(lua_State *l) { P(l)->v.armorvalue = (float)luaL_checknumber(l, 2); return 0; }
static int p_frags(lua_State *l) { lua_pushnumber(l, P(l)->v.frags); return 1; }
static int p_set_frags(lua_State *l) { P(l)->v.frags = (float)luaL_checknumber(l, 2); return 0; }
static int p_origin(lua_State *l) { edict_t *e = P(l); lua_pushnumber(l, e->v.origin.x); lua_pushnumber(l, e->v.origin.y); lua_pushnumber(l, e->v.origin.z); return 3; }
static int p_set_origin(lua_State *l) {
	edict_t *e = P(l); float o[3] = {(float)luaL_checknumber(l, 2), (float)luaL_checknumber(l, 3), (float)luaL_checknumber(l, 4)};
	SET_ORIGIN(e, o); return 0; }
static int p_velocity(lua_State *l) { edict_t *e = P(l); lua_pushnumber(l, e->v.velocity.x); lua_pushnumber(l, e->v.velocity.y); lua_pushnumber(l, e->v.velocity.z); return 3; }
static int p_set_velocity(lua_State *l) {
	edict_t *e = P(l); e->v.velocity = Vector((float)luaL_checknumber(l, 2), (float)luaL_checknumber(l, 3), (float)luaL_checknumber(l, 4)); return 0; }
static int p_view_angles(lua_State *l) { edict_t *e = P(l); lua_pushnumber(l, e->v.v_angle.x); lua_pushnumber(l, e->v.v_angle.y); lua_pushnumber(l, e->v.v_angle.z); return 3; }
static int p_kill(lua_State *l) { edict_t *e = P(l); MDLL_ClientKill(e); return 0; }
static int p_kick(lua_State *l) {
	edict_t *e = P(l); const char *why = luaL_optstring(l, 2, "Kicked");
	char cmd[512]; snprintf(cmd, sizeof cmd, "kick #%d \"%s\"\n", GETPLAYERUSERID(e), why);
	for (char *c = cmd + 6; *c; ++c) if (*c == ';') *c = ',';       // no command injection
	SERVER_COMMAND(cmd); return 0; }
static int p_give(lua_State *l) {
	edict_t *e = P(l); const char *cls = luaL_checkstring(l, 2);
	if (strncmp(cls, "weapon_", 7) && strncmp(cls, "item_", 5) && strncmp(cls, "ammo_", 5)) return luaL_error(l, "give_item only accepts weapon_/item_/ammo_ classes");
	edict_t *it = CREATE_NAMED_ENTITY(MAKE_STRING(cls)); if (!it) { lua_pushboolean(l, 0); return 1; }
	it->v.origin = e->v.origin; it->v.spawnflags |= (1 << 30);        // SF_NORESPAWN
	MDLL_Spawn(it); int solid = it->v.solid; MDLL_Touch(it, e);
	if (it->v.solid == solid && !(it->v.flags & FL_KILLME) && it->v.owner != e) REMOVE_ENTITY(it);
	lua_pushboolean(l, 1); return 1; }
static int p_access(lua_State *l) { P(l); lua_pushinteger(l, g_players[lua_tointeger(l, 1)].access); return 1; }
static int p_set_access(lua_State *l) { P(l); g_players[lua_tointeger(l, 1)].access = (int)luaL_checkinteger(l, 2); return 0; }
static int p_has_access(lua_State *l) {
	P(l); int req = lua_isinteger(l, 2) ? (int)lua_tointeger(l, 2) : flags_from_string(luaL_checkstring(l, 2));
	lua_pushboolean(l, req == 0 || (g_players[lua_tointeger(l, 1)].access & req) == req); return 1; }
static int p_infokey(lua_State *l) {
	edict_t *e = P(l); char *buf = GET_INFOKEYBUFFER(e); lua_pushstring(l, INFOKEY_VALUE(buf, const_cast<char *>(luaL_checkstring(l, 2)))); return 1; }
static int p_set_infokey(lua_State *l) {
	edict_t *e = P(l); char *buf = GET_INFOKEYBUFFER(e);
	SET_CLIENT_KEY_VALUE(ENTINDEX(e), buf, const_cast<char *>(luaL_checkstring(l, 2)), const_cast<char *>(luaL_checkstring(l, 3))); return 0; }
static int p_all(lua_State *l) {      // player.all([only_alive_or_filter])
	lua_newtable(l); int n = 1;
	for (int i = 1; i <= g_maxclients; i++) if (g_players[i].connected && g_players[i].putin) { lua_pushinteger(l, i); lua_rawseti(l, -2, n++); }
	return 1; }
static int p_pdata(lua_State *l) {   // plugin-private per-player Lua table
	int id = (int)luaL_checkinteger(l, 1);
	if (!g_cur || id < 1 || id > XMOD_MAXPLAYERS) return luaL_error(l, "bad player id");
	lua_rawgeti(l, LUA_REGISTRYINDEX, g_cur->pdata_ref);
	if (lua_rawgeti(l, -1, id) != LUA_TTABLE) { lua_pop(l, 1); lua_newtable(l); lua_pushvalue(l, -1); lua_rawseti(l, -3, id); }
	return 1; }
static int p_find(lua_State *l) {      // find by (partial, case-insens.) name or #userid or steamid
	std::string key = luaL_checkstring(l, 1); int found = 0, hits = 0;
	for (int i = 1; i <= g_maxclients; i++) {
		if (!g_players[i].connected) continue;
		bool m = false;
		if (!key.empty() && key[0] == '#') m = (g_players[i].userid == atoi(key.c_str() + 1));
		else if (!strncmp(key.c_str(), "STEAM_", 6)) m = (g_players[i].authid == key);
		else { std::string a = g_players[i].name, b = key; for (auto &c : a) c = tolower((unsigned char)c); for (auto &c : b) c = tolower((unsigned char)c); m = a.find(b) != std::string::npos; }
		if (m) { found = i; hits++; }
	}
	lua_pushinteger(l, hits == 1 ? found : 0); lua_pushinteger(l, hits); return 2; }

static int p_ping(lua_State *l) { edict_t *e = P(l); int ping = 0, loss = 0; g_engfuncs.pfnGetPlayerStats(e, &ping, &loss); lua_pushinteger(l, ping); lua_pushinteger(l, loss); return 2; }
static int p_set_team(lua_State *l) {   // set_team(id, 1=T|2=CT|3=SPEC)  (pdata + TeamInfo + model key)
	edict_t *e = P(l); int team = (int)luaL_checkinteger(l, 2); if (team < 1 || team > 3) return luaL_error(l, "team must be 1(T), 2(CT) or 3(SPEC)");
	int *t = pint(e, OFS_TEAM); if (!t) return 0; *t = team;
	static const char *names[] = {"UNASSIGNED", "TERRORIST", "CT", "SPECTATOR"};
	static int mid = 0; if (!mid) mid = GET_USER_MSG_ID(PLID, "TeamInfo", nullptr);
	if (mid) { MESSAGE_BEGIN(MSG_ALL, mid, nullptr, nullptr); WRITE_BYTE(ENTINDEX(e)); WRITE_STRING(names[team]); MESSAGE_END(); }
	if (team != 3) { char *buf = GET_INFOKEYBUFFER(e); SET_CLIENT_KEY_VALUE(ENTINDEX(e), buf, const_cast<char *>("model"), const_cast<char *>(team == 1 ? "terror" : "urban")); }
	return 0; }
static int p_team_name(lua_State *l) { static const char *n[] = {"UNASSIGNED", "TERRORIST", "CT", "SPECTATOR"}; int t = (int)luaL_checkinteger(l, 1); lua_pushstring(l, (t >= 0 && t <= 3) ? n[t] : "?"); return 1; }

void clear_player_pdata(int id) {
	if (!L) return;
	for (auto *p : g_plugins) { lua_rawgeti(L, LUA_REGISTRYINDEX, p->pdata_ref); lua_pushnil(L); lua_rawseti(L, -2, id); lua_pop(L, 1); }
}

void api_register_player(lua_State *l) {
	lua_getglobal(l, "xmod"); lua_newtable(l);
	static const luaL_Reg fns[] = {
		{"connected", p_connected}, {"ingame", p_ingame}, {"name", p_name}, {"ip", p_ip}, {"steamid", p_steamid}, {"userid", p_userid},
		{"is_bot", p_bot}, {"alive", p_alive}, {"team", p_team}, {"money", p_money}, {"set_money", p_set_money},
		{"health", p_health}, {"set_health", p_set_health}, {"armor", p_armor}, {"set_armor", p_set_armor},
		{"frags", p_frags}, {"set_frags", p_set_frags}, {"origin", p_origin}, {"set_origin", p_set_origin},
		{"velocity", p_velocity}, {"set_velocity", p_set_velocity}, {"view_angles", p_view_angles},
		{"kill", p_kill}, {"kick", p_kick}, {"give_item", p_give}, {"access", p_access}, {"set_access", p_set_access},
		{"has_access", p_has_access}, {"infokey", p_infokey}, {"set_infokey", p_set_infokey}, {"all", p_all},
		{"data", p_pdata}, {"ping", p_ping}, {"set_team", p_set_team}, {"team_name", p_team_name}, {"find", p_find}, {nullptr, nullptr} };
	luaL_setfuncs(l, fns, 0);
	lua_setfield(l, -2, "player"); lua_pop(l, 1);
}
