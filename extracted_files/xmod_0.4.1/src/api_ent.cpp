#include "xmod.h"
#include <cstring>
#include <cstddef>

enum FT { T_F, T_I, T_V, T_S, T_E };
struct Field { const char *name; FT t; size_t off; };
#define FF(n) {#n, T_F, offsetof(entvars_t, n)}
#define FI(n) {#n, T_I, offsetof(entvars_t, n)}
#define FV(n) {#n, T_V, offsetof(entvars_t, n)}
#define FS(n) {#n, T_S, offsetof(entvars_t, n)}
#define FE(n) {#n, T_E, offsetof(entvars_t, n)}
static const Field FIELDS[] = {
	FF(health), FF(max_health), FF(armorvalue), FF(takedamage), FF(dmg), FF(dmgtime), FF(speed), FF(maxspeed), FF(gravity), FF(friction),
	FF(frags), FF(fov), FF(nextthink), FF(ltime), FF(animtime), FF(framerate), FF(frame), FF(scale), FF(renderamt),
	FF(fuser1), FF(fuser2), FF(fuser3), FF(fuser4),
	FI(flags), FI(movetype), FI(solid), FI(effects), FI(deadflag), FI(sequence), FI(gaitsequence), FI(skin), FI(body),
	FI(rendermode), FI(renderfx), FI(button), FI(impulse), FI(weapons), FI(team), FI(playerclass), FI(spawnflags),
	FI(waterlevel), FI(watertype), FI(modelindex), FI(colormap), FI(iuser1), FI(iuser2), FI(iuser3), FI(iuser4), FI(oldbuttons), FI(groupinfo),
	FV(origin), FV(oldorigin), FV(velocity), FV(basevelocity), FV(angles), FV(avelocity), FV(v_angle), FV(punchangle),
	FV(mins), FV(maxs), FV(absmin), FV(absmax), FV(size), FV(view_ofs), FV(rendercolor), FV(movedir),
	FV(vuser1), FV(vuser2), FV(vuser3), FV(vuser4),
	FS(classname), FS(targetname), FS(target), FS(model), FS(globalname), FS(netname), FS(message), FS(noise), FS(noise1), FS(noise2), FS(noise3),
	FE(owner), FE(enemy), FE(aiment), FE(groundentity), FE(chain), FE(dmg_inflictor), FE(euser1), FE(euser2), FE(euser3), FE(euser4),
};
static const Field *find_field(const char *n) {            // O(1): hash map built once (the table has ~90 fields)
	static std::unordered_map<std::string, const Field *> map;
	if (map.empty()) for (const Field &f : FIELDS) map[f.name] = &f;
	auto it = map.find(n); return it == map.end() ? nullptr : it->second;
}
static edict_t *E(lua_State *l, int idx = 1) {
	int i = (int)luaL_checkinteger(l, idx);
	edict_t *e = ent_edict(i);
	if (!e) luaL_error(l, "invalid entity %d", i);
	return e;
}
static int e_valid(lua_State *l) { lua_pushboolean(l, ent_edict((int)luaL_checkinteger(l, 1)) != nullptr); return 1; }
static int e_create(lua_State *l) {
	edict_t *e = CREATE_NAMED_ENTITY(MAKE_STRING(luaL_checkstring(l, 1)));
	lua_pushinteger(l, e ? ENTINDEX(e) : 0); return 1; }
static int e_spawn(lua_State *l) { MDLL_Spawn(E(l)); return 0; }
static int e_remove(lua_State *l) {
	edict_t *e = E(l); if (ENTINDEX(e) <= g_maxclients) return luaL_error(l, "cannot remove players/world");
	REMOVE_ENTITY(e); return 0; }
static int e_classname(lua_State *l) { lua_pushstring(l, STRING(E(l)->v.classname)); return 1; }
static int e_find(lua_State *l) {      // find(start_ent, "classname"[, field])
	edict_t *start = ent_edict((int)luaL_checkinteger(l, 1)); if (!start) start = INDEXENT(0);
	edict_t *e = FIND_ENTITY_BY_STRING(start, luaL_optstring(l, 3, "classname"), luaL_checkstring(l, 2));
	lua_pushinteger(l, (e && !FNullEnt(e)) ? ENTINDEX(e) : 0); return 1; }
static int e_find_sphere(lua_State *l) {
	float o[3] = {(float)luaL_checknumber(l, 1), (float)luaL_checknumber(l, 2), (float)luaL_checknumber(l, 3)};
	float r = (float)luaL_checknumber(l, 4); lua_newtable(l); int n = 1;
	edict_t *e = FIND_ENTITY_IN_SPHERE(INDEXENT(0), o, r);
	while (e && !FNullEnt(e)) { lua_pushinteger(l, ENTINDEX(e)); lua_rawseti(l, -2, n++); e = FIND_ENTITY_IN_SPHERE(e, o, r); if (n > 2048) break; }
	return 1; }
static int e_set_origin(lua_State *l) {
	edict_t *e = E(l); float o[3] = {(float)luaL_checknumber(l, 2), (float)luaL_checknumber(l, 3), (float)luaL_checknumber(l, 4)};
	SET_ORIGIN(e, o); return 0; }
static int e_set_model(lua_State *l) { SET_MODEL(E(l), luaL_checkstring(l, 2)); return 0; }
static int e_set_size(lua_State *l) {
	edict_t *e = E(l); float a[3], b[3];
	for (int i = 0; i < 3; i++) { a[i] = (float)luaL_checknumber(l, 2 + i); b[i] = (float)luaL_checknumber(l, 5 + i); }
	SET_SIZE(e, a, b); return 0; }
static int e_get(lua_State *l) {
	edict_t *e = E(l); const Field *f = find_field(luaL_checkstring(l, 2));
	if (!f) return luaL_error(l, "unknown entity field '%s'", lua_tostring(l, 2));
	char *base = reinterpret_cast<char *>(&e->v) + f->off;
	switch (f->t) {
	case T_F: lua_pushnumber(l, *reinterpret_cast<float *>(base)); return 1;
	case T_I: lua_pushinteger(l, *reinterpret_cast<int *>(base)); return 1;
	case T_V: { float *v = reinterpret_cast<float *>(base); lua_pushnumber(l, v[0]); lua_pushnumber(l, v[1]); lua_pushnumber(l, v[2]); return 3; }
	case T_S: lua_pushstring(l, STRING(*reinterpret_cast<int *>(base))); return 1;
	case T_E: { edict_t *o = *reinterpret_cast<edict_t **>(base); lua_pushinteger(l, (o && !FNullEnt(o)) ? ENTINDEX(o) : 0); return 1; }
	}
	return 0;
}
static int e_set(lua_State *l) {
	edict_t *e = E(l); const Field *f = find_field(luaL_checkstring(l, 2));
	if (!f) return luaL_error(l, "unknown entity field '%s'", lua_tostring(l, 2));
	char *base = reinterpret_cast<char *>(&e->v) + f->off;
	switch (f->t) {
	case T_F: *reinterpret_cast<float *>(base) = (float)luaL_checknumber(l, 3); break;
	case T_I: *reinterpret_cast<int *>(base) = (int)luaL_checkinteger(l, 3); break;
	case T_V: { float *v = reinterpret_cast<float *>(base); v[0] = (float)luaL_checknumber(l, 3); v[1] = (float)luaL_checknumber(l, 4); v[2] = (float)luaL_checknumber(l, 5); break; }
	case T_S: *reinterpret_cast<int *>(base) = ALLOC_STRING(luaL_checkstring(l, 3)); break;
	case T_E: { int i = (int)luaL_checkinteger(l, 3); *reinterpret_cast<edict_t **>(base) = i > 0 ? ent_edict(i) : nullptr; break; }
	}
	return 0;
}
// ---- raw private-data access (CS offsets), bounds-checked
static char *mem_ptr(lua_State *l, int bytes) {
	edict_t *e = E(l); int ofs = (int)luaL_checkinteger(l, 2);
#ifdef __linux__
	ofs += (int)luaL_optinteger(l, 3, 0);
#endif
	if (!e->pvPrivateData || ofs < 0 || ofs * 4 + bytes > 16384) { luaL_error(l, "bad private data access"); return nullptr; }
	return reinterpret_cast<char *>(e->pvPrivateData) + ofs * 4;
}
static int m_get_int(lua_State *l) { lua_pushinteger(l, *reinterpret_cast<int *>(mem_ptr(l, 4))); return 1; }
static int m_get_float(lua_State *l) { lua_pushnumber(l, *reinterpret_cast<float *>(mem_ptr(l, 4))); return 1; }
static int m_set_int(lua_State *l) { char *p = mem_ptr(l, 4); *reinterpret_cast<int *>(p) = (int)luaL_checkinteger(l, 4); return 0; }
static int m_set_float(lua_State *l) { char *p = mem_ptr(l, 4); *reinterpret_cast<float *>(p) = (float)luaL_checknumber(l, 4); return 0; }
// ---- traces
static int t_common(lua_State *l, TraceResult &tr) {
	lua_newtable(l);
	lua_pushnumber(l, tr.flFraction); lua_setfield(l, -2, "fraction");
	lua_pushinteger(l, (tr.pHit && !FNullEnt(tr.pHit)) ? ENTINDEX(tr.pHit) : 0); lua_setfield(l, -2, "ent");
	lua_pushnumber(l, tr.vecEndPos.x); lua_setfield(l, -2, "x"); lua_pushnumber(l, tr.vecEndPos.y); lua_setfield(l, -2, "y");
	lua_pushnumber(l, tr.vecEndPos.z); lua_setfield(l, -2, "z");
	lua_pushnumber(l, tr.vecPlaneNormal.x); lua_setfield(l, -2, "nx"); lua_pushnumber(l, tr.vecPlaneNormal.y); lua_setfield(l, -2, "ny");
	lua_pushnumber(l, tr.vecPlaneNormal.z); lua_setfield(l, -2, "nz");
	lua_pushboolean(l, tr.fStartSolid); lua_setfield(l, -2, "startsolid"); lua_pushboolean(l, tr.fAllSolid); lua_setfield(l, -2, "allsolid");
	return 1;
}
static int t_line(lua_State *l) {      // line(x1,y1,z1, x2,y2,z2 [, flags [, ignore_ent]])
	float a[3], b[3]; for (int i = 0; i < 3; i++) { a[i] = (float)luaL_checknumber(l, 1 + i); b[i] = (float)luaL_checknumber(l, 4 + i); }
	TraceResult tr; edict_t *ig = ent_edict((int)luaL_optinteger(l, 8, 0));
	TRACE_LINE(a, b, (int)luaL_optinteger(l, 7, 0), ig, &tr); return t_common(l, tr); }
static int t_line_n(lua_State *l) {    // line_n(x1,y1,z1,x2,y2,z2 [,flags [,ignore]]) -> fraction, ent, endx, endy, endz, nx, ny, nz   (no table allocation: much cheaper)
	float a[3], b[3]; for (int i = 0; i < 3; i++) { a[i] = (float)luaL_checknumber(l, 1 + i); b[i] = (float)luaL_checknumber(l, 4 + i); }
	TraceResult tr; edict_t *ig = ent_edict((int)luaL_optinteger(l, 8, 0));
	TRACE_LINE(a, b, (int)luaL_optinteger(l, 7, 0), ig, &tr);
	lua_pushnumber(l, tr.flFraction); lua_pushinteger(l, (tr.pHit && !FNullEnt(tr.pHit)) ? ENTINDEX(tr.pHit) : 0);
	lua_pushnumber(l, tr.vecEndPos.x); lua_pushnumber(l, tr.vecEndPos.y); lua_pushnumber(l, tr.vecEndPos.z);
	lua_pushnumber(l, tr.vecPlaneNormal.x); lua_pushnumber(l, tr.vecPlaneNormal.y); lua_pushnumber(l, tr.vecPlaneNormal.z); return 8; }
static int t_hull(lua_State *l) {      // hull(x1,y1,z1, x2,y2,z2, hull [, flags [, ignore_ent]])
	float a[3], b[3]; for (int i = 0; i < 3; i++) { a[i] = (float)luaL_checknumber(l, 1 + i); b[i] = (float)luaL_checknumber(l, 4 + i); }
	TraceResult tr; edict_t *ig = ent_edict((int)luaL_optinteger(l, 9, 0));
	TRACE_HULL(a, b, (int)luaL_optinteger(l, 8, 0), (int)luaL_checkinteger(l, 7), ig, &tr); return t_common(l, tr); }
static int t_contents(lua_State *l) {
	float o[3] = {(float)luaL_checknumber(l, 1), (float)luaL_checknumber(l, 2), (float)luaL_checknumber(l, 3)};
	lua_pushinteger(l, POINT_CONTENTS(o)); return 1; }

void api_register_ent(lua_State *l) {
	lua_getglobal(l, "xmod");
	lua_newtable(l);
	static const luaL_Reg ent[] = { {"valid", e_valid}, {"create", e_create}, {"spawn", e_spawn}, {"remove", e_remove},
		{"classname", e_classname}, {"find", e_find}, {"find_sphere", e_find_sphere}, {"set_origin", e_set_origin},
		{"set_model", e_set_model}, {"set_size", e_set_size}, {"get", e_get}, {"set", e_set}, {nullptr, nullptr} };
	luaL_setfuncs(l, ent, 0); lua_setfield(l, -2, "ent");
	lua_newtable(l);
	static const luaL_Reg mem[] = { {"get_int", m_get_int}, {"get_float", m_get_float}, {"set_int", m_set_int}, {"set_float", m_set_float}, {nullptr, nullptr} };
	luaL_setfuncs(l, mem, 0); lua_pushinteger(l, 5); lua_setfield(l, -2, "LINUX_PLAYER"); lua_setfield(l, -2, "mem");
	lua_newtable(l);
	static const luaL_Reg tr[] = { {"line", t_line}, {"line_n", t_line_n}, {"hull", t_hull}, {"contents", t_contents}, {nullptr, nullptr} };
	luaL_setfuncs(l, tr, 0); lua_setfield(l, -2, "trace");
	// useful constants
	struct K { const char *n; int v; };
	static const K ks[] = { {"FL_ONGROUND", FL_ONGROUND}, {"FL_DUCKING", FL_DUCKING}, {"FL_FROZEN", FL_FROZEN}, {"FL_GODMODE", FL_GODMODE},
		{"FL_NOTARGET", FL_NOTARGET}, {"FL_FAKECLIENT", FL_FAKECLIENT}, {"FL_SPECTATOR", FL_SPECTATOR}, {"FL_CLIENT", FL_CLIENT},
		{"MOVETYPE_NONE", MOVETYPE_NONE}, {"MOVETYPE_WALK", MOVETYPE_WALK}, {"MOVETYPE_NOCLIP", MOVETYPE_NOCLIP}, {"MOVETYPE_FLY", MOVETYPE_FLY},
		{"MOVETYPE_TOSS", MOVETYPE_TOSS}, {"SOLID_NOT", SOLID_NOT}, {"SOLID_BBOX", SOLID_BBOX}, {"SOLID_TRIGGER", SOLID_TRIGGER},
		{"IN_ATTACK", IN_ATTACK}, {"IN_JUMP", IN_JUMP}, {"IN_DUCK", IN_DUCK}, {"IN_FORWARD", IN_FORWARD}, {"IN_BACK", IN_BACK},
		{"IN_USE", IN_USE}, {"IN_ATTACK2", IN_ATTACK2}, {"IN_RELOAD", IN_RELOAD}, {"IN_MOVELEFT", IN_MOVELEFT}, {"IN_MOVERIGHT", IN_MOVERIGHT},
		{"TRACE_NORMAL", 0}, {"TRACE_IGNORE_MONSTERS", 1}, {"TRACE_IGNORE_GLASS", 0x100},
		{"RENDER_NORMAL", 0}, {"RENDER_TRANSCOLOR", 1}, {"RENDER_TRANSTEXTURE", 2}, {"RENDER_GLOW", 3}, {"RENDER_TRANSALPHA", 4}, {"RENDER_TRANSADD", 5},
		{"FX_NONE", 0}, {"FX_GLOWSHELL", 19}, {"FX_PULSE_SLOW", 1}, {"FX_PULSE_FAST", 2}, {"FX_DISTORT", 15}, {"EF_NODRAW", 128}, {"EF_BRIGHTLIGHT", 2}, {"EF_DIMLIGHT", 8},
		{"TEAM_UNASSIGNED", 0}, {"TEAM_T", 1}, {"TEAM_CT", 2}, {"TEAM_SPEC", 3}, {nullptr, 0} };
	for (const K *k = ks; k->n; ++k) { lua_pushinteger(l, k->v); lua_setfield(l, -2, k->n); }
	lua_pop(l, 1);
}
