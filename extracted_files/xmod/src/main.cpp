// xmod - Metamod glue: exports, hook tables, lifecycle events
#include "xmod.h"
#include "hookutil.h"
#include <usercmd.h>
#include <cstring>

enginefuncs_t g_engfuncs;
globalvars_t *gpGlobals;
meta_globals_t *gpMetaGlobals;
mutil_funcs_t *gpMetaUtilFuncs;
gamedll_funcs_t *gpGamedllFuncs;
bool g_precaching = false;
extern bool g_any_class_hook;
void cmd_register_builtin();
void clear_player_pdata(int id);

plugin_info_t Plugin_info = { META_INTERFACE_VERSION, "xmod", XMOD_VERSION, __DATE__, "Andrew", "https://github.com/ghibolariuandrei97", "XMOD",
	PT_ANYTIME, PT_ANYTIME };

static bool g_map_active = false, g_need_config = false, g_world_spawned = false, g_cfg_exec = false;
static Event *ev_precache, *ev_init, *ev_config, *ev_map_end, *ev_shutdown, *ev_frame, *ev_connect, *ev_putin, *ev_disc, *ev_info,
	*ev_pre, *ev_post, *ev_cmdstart, *ev_touch, *ev_think, *ev_spawn, *ev_emit;

static void cache_events() {
	ev_precache = ev_get("precache"); ev_init = ev_get("init"); ev_config = ev_get("config"); ev_map_end = ev_get("map_end");
	ev_shutdown = ev_get("shutdown"); ev_frame = ev_get("frame"); ev_connect = ev_get("client_connect"); ev_putin = ev_get("client_putinserver");
	ev_disc = ev_get("client_disconnect"); ev_info = ev_get("client_infochanged"); ev_pre = ev_get("prethink"); ev_post = ev_get("postthink");
	ev_cmdstart = ev_get("cmd_start"); ev_touch = ev_get("touch"); ev_think = ev_get("think"); ev_spawn = ev_get("spawn"); ev_emit = ev_get("emit_sound");
}
static void fire_simple(Event *e) { if (ev_has(e)) ev_fire(e, 0, nullptr); }
static void fire_init() {
	g_maxclients = gpGlobals->maxClients;
	if (ev_has(ev_init)) ev_fire(ev_init, 1, [](lua_State *l) { lua_pushinteger(l, g_maxclients); return 1; });
}
void xmod_reload() {          // `xmod reload`
	core_reload(); cache_events();
	if (g_map_active) { xlog("note: 'precache' event does not run on reload (map already loaded)"); fire_init(); g_need_config = true; }
}

// ------------------------------------------------------------ DLL API (pre)
static void Pre_GameInit() { RETURN_META(MRES_IGNORED); }
static int Pre_Spawn(edict_t *pent) {
	if (!g_world_spawned && ENTINDEX(pent) == 0) {                // world spawn => precache phase
		g_world_spawned = true; g_precaching = true; fire_simple(ev_precache); g_precaching = false;
	}
	RETURN_META_VALUE(MRES_IGNORED, 0);
}
static void Post_Spawn(edict_t *pent) {                           // Post: entity fully spawned
	if (ev_has(ev_spawn) && pent && !pent->free) {
		int idx = ENTINDEX(pent); ev_fire(ev_spawn, 2, [&](lua_State *l) { lua_pushinteger(l, idx); lua_pushstring(l, STRING(pent->v.classname)); return 2; });
	}
}
static int Pre_ClientConnect(edict_t *pEntity, const char *pszName, const char *pszAddress, char szRejectReason[128]) {
	int id = ENTINDEX(pEntity);
	if (id >= 1 && id <= XMOD_MAXPLAYERS) {
		PlayerState &p = g_players[id]; p = PlayerState(); p.connected = true; p.name = pszName ? pszName : ""; p.ip = pszAddress ? pszAddress : "";
		p.bot = (pEntity->v.flags & FL_FAKECLIENT) != 0 || (pszAddress && !strcmp(pszAddress, "127.0.0.1") && false);
		clear_player_pdata(id);
		if (ev_has(ev_connect)) {
			EvResult r = ev_fire(ev_connect, 3, [&](lua_State *l) { lua_pushinteger(l, id); lua_pushstring(l, p.name.c_str()); lua_pushstring(l, p.ip.c_str()); return 3; });
			if (r.kind == 2) { strncpy(szRejectReason, r.reason.empty() ? "Rejected by server" : r.reason.c_str(), 127); szRejectReason[127] = 0; p = PlayerState(); RETURN_META_VALUE(MRES_SUPERCEDE, 0); }
		}
	}
	RETURN_META_VALUE(MRES_IGNORED, 1);
}
static void Post_ClientPutInServer(edict_t *pEntity) {
	int id = ENTINDEX(pEntity); if (id < 1 || id > XMOD_MAXPLAYERS) return;
	PlayerState &p = g_players[id]; p.connected = true; p.putin = true; p.bot = (pEntity->v.flags & FL_FAKECLIENT) != 0;
	p.userid = GETPLAYERUSERID(pEntity); const char *a = GETPLAYERAUTHID(pEntity); p.authid = a ? a : "";
	char *ib = GET_INFOKEYBUFFER(pEntity); p.name = INFOKEY_VALUE(ib, "name");
	if (ev_has(ev_putin)) ev_fire(ev_putin, 1, [&](lua_State *l) { lua_pushinteger(l, id); return 1; });
}
static void Pre_ClientDisconnect(edict_t *pEntity) {
	int id = ENTINDEX(pEntity); if (id < 1 || id > XMOD_MAXPLAYERS) RETURN_META(MRES_IGNORED);
	if (g_players[id].connected && ev_has(ev_disc)) ev_fire(ev_disc, 1, [&](lua_State *l) { lua_pushinteger(l, id); return 1; });
	g_players[id] = PlayerState(); clear_player_pdata(id);
	RETURN_META(MRES_IGNORED);
}
static void Pre_ClientUserInfoChanged(edict_t *pEntity, char *infobuffer) {
	int id = ENTINDEX(pEntity);
	if (id >= 1 && id <= XMOD_MAXPLAYERS && g_players[id].connected) {
		std::string nn = INFOKEY_VALUE(infobuffer, "name"); std::string old = g_players[id].name; g_players[id].name = nn;
		if (ev_has(ev_info)) ev_fire(ev_info, 3, [&](lua_State *l) { lua_pushinteger(l, id); lua_pushstring(l, old.c_str()); lua_pushstring(l, nn.c_str()); return 3; });
	}
	RETURN_META(MRES_IGNORED);
}
static void Pre_ClientCommand(edict_t *pEntity) {
	if (cmd_client_dispatch(pEntity)) RETURN_META(MRES_SUPERCEDE);
	RETURN_META(MRES_IGNORED);
}
static void Post_ServerActivate(edict_t *, int, int maxClients) {
	g_maxclients = maxClients; g_map_active = true; g_need_config = true;
	ham_apply_pending();
	fire_init();
	RETURN_META(MRES_IGNORED);
}
static void Pre_ServerDeactivate() {
	fire_simple(ev_map_end); timers_map_end(); kv_flush_all();
	g_map_active = false; g_world_spawned = false;
	for (int i = 0; i <= XMOD_MAXPLAYERS; i++) g_players[i] = PlayerState();
	RETURN_META(MRES_IGNORED);
}
static void Pre_StartFrame() {
	g_time = gpGlobals->time;
	if (g_need_config) {          // frame 1: exec our cfg (async), frame 2: fire 'config' so cvars from it are already applied
		if (!g_cfg_exec) { g_cfg_exec = true; SERVER_COMMAND(const_cast<char *>("exec addons/xmod/configs/xmod.cfg\n")); }
		else { g_cfg_exec = false; g_need_config = false; fire_simple(ev_config); }
	}
	timers_run(); cvar_watch_run();
	if (ev_has(ev_frame)) ev_fire(ev_frame, 0, nullptr);
	RETURN_META(MRES_IGNORED);
}
static void Pre_PlayerPreThink(edict_t *e) {
	if (ev_has(ev_pre)) { int id = ENTINDEX(e); if (is_valid_player(id)) ev_fire(ev_pre, 1, [&](lua_State *l) { lua_pushinteger(l, id); return 1; }); }
	RETURN_META(MRES_IGNORED);
}
static void Pre_PlayerPostThink(edict_t *e) {
	if (ev_has(ev_post)) { int id = ENTINDEX(e); if (is_valid_player(id)) ev_fire(ev_post, 1, [&](lua_State *l) { lua_pushinteger(l, id); return 1; }); }
	RETURN_META(MRES_IGNORED);
}
static void Pre_CmdStart(const edict_t *player, const struct usercmd_s *cmd, unsigned int) {
	if (ev_has(ev_cmdstart)) {
		int id = ENTINDEX(const_cast<edict_t *>(player));
		if (is_valid_player(id)) ev_fire(ev_cmdstart, 6, [&](lua_State *l) {
			lua_pushinteger(l, id); lua_pushinteger(l, cmd->buttons); lua_pushinteger(l, cmd->impulse);
			lua_pushnumber(l, cmd->forwardmove); lua_pushnumber(l, cmd->sidemove); lua_pushnumber(l, cmd->upmove); return 6; });
	}
	RETURN_META(MRES_IGNORED);
}
static bool class_hook(const char *prefix, edict_t *e, Event *generic, const std::function<int(lua_State *)> &push, bool &consumed) {
	consumed = false;
	if (ev_has(generic)) { EvResult r = ev_fire(generic, 2, push); if (r.kind == 1) { consumed = true; return true; } }
	if (g_any_class_hook && e) {
		Event *ce = ev_get((std::string(prefix) + STRING(e->v.classname)).c_str());
		if (ev_has(ce)) { EvResult r = ev_fire(ce, 2, push); if (r.kind == 1) { consumed = true; return true; } }
	}
	return false;
}
static void Pre_Touch(edict_t *a, edict_t *b) {
	if (!ev_has(ev_touch) && !g_any_class_hook) RETURN_META(MRES_IGNORED);
	int ia = ENTINDEX(a), ib = ENTINDEX(b); bool c;
	class_hook("touch:", a, ev_touch, [&](lua_State *l) { lua_pushinteger(l, ia); lua_pushinteger(l, ib); return 2; }, c);
	if (c) RETURN_META(MRES_SUPERCEDE);
	RETURN_META(MRES_IGNORED);
}
static void Pre_Think(edict_t *e) {
	if (!ev_has(ev_think) && !g_any_class_hook) RETURN_META(MRES_IGNORED);
	int i = ENTINDEX(e); bool c;
	class_hook("think:", e, ev_think, [&](lua_State *l) { lua_pushinteger(l, i); return 1; }, c);
	if (c) RETURN_META(MRES_SUPERCEDE);
	RETURN_META(MRES_IGNORED);
}
static void Pre_CvarValue2(const edict_t *e, int req, const char *name, const char *value) {
	cvarquery_result(e, req, name, value); RETURN_META(MRES_IGNORED);
}

// ------------------------------------------------------------ engine API (pre)
static void E_MessageBegin(int d, int t, const float *o, edict_t *e) { if (msg_begin(d, t, o, e)) RETURN_META(MRES_SUPERCEDE); RETURN_META(MRES_IGNORED); }
static void E_MessageEnd() { if (msg_end()) RETURN_META(MRES_SUPERCEDE); RETURN_META(MRES_IGNORED); }
#define WR(name, type, kind) static void E_##name(type v) { if (msg_write(kind, (double)v, nullptr)) RETURN_META(MRES_SUPERCEDE); RETURN_META(MRES_IGNORED); }
WR(WriteByte, int, 1) WR(WriteChar, int, 2) WR(WriteShort, int, 3) WR(WriteLong, int, 4) WR(WriteAngle, float, 5) WR(WriteCoord, float, 6) WR(WriteEntity, int, 8)
static void E_WriteString(const char *s) { if (msg_write(7, 0, s ? s : "")) RETURN_META(MRES_SUPERCEDE); RETURN_META(MRES_IGNORED); }
static void E_EmitSound(edict_t *e, int ch, const char *sample, float vol, float att, int fl, int pitch) {
	if (ev_has(ev_emit)) {
		int idx = ENTINDEX(e);
		EvResult r = ev_fire(ev_emit, 7, [&](lua_State *l) { lua_pushinteger(l, idx); lua_pushinteger(l, ch); lua_pushstring(l, sample ? sample : "");
			lua_pushnumber(l, vol); lua_pushnumber(l, att); lua_pushinteger(l, fl); lua_pushinteger(l, pitch); return 7; });
		if (r.kind == 1) RETURN_META(MRES_SUPERCEDE);
	}
	RETURN_META(MRES_IGNORED);
}

// ------------------------------------------------------------ table exports
static DLL_FUNCTIONS g_dll_pre, g_dll_post;
static NEW_DLL_FUNCTIONS g_new_pre;
static enginefuncs_t g_eng_pre;

extern "C" {
C_DLLEXPORT int GetEntityAPI2(DLL_FUNCTIONS *, int *);
C_DLLEXPORT int GetEntityAPI2_Post(DLL_FUNCTIONS *, int *);
C_DLLEXPORT int GetNewDLLFunctions(NEW_DLL_FUNCTIONS *, int *);
C_DLLEXPORT int GetEngineFunctions(enginefuncs_t *, int *);
C_DLLEXPORT void WINAPI GiveFnptrsToDll(enginefuncs_t *pengfuncsFromEngine, globalvars_t *pGlobals) {
	memcpy(&g_engfuncs, pengfuncsFromEngine, sizeof(enginefuncs_t)); gpGlobals = pGlobals;
}
C_DLLEXPORT int Meta_Query(char *, plugin_info_t **plinfo, mutil_funcs_t *pMetaUtilFuncs) {
	*plinfo = &Plugin_info; gpMetaUtilFuncs = pMetaUtilFuncs; return 1;
}
C_DLLEXPORT int Meta_Attach(PLUG_LOADTIME, META_FUNCTIONS *pFunctionTable, meta_globals_t *pMGlobals, gamedll_funcs_t *pGamedllFuncs) {
	gpMetaGlobals = pMGlobals; gpGamedllFuncs = pGamedllFuncs;
	memset(pFunctionTable, 0, sizeof(META_FUNCTIONS));
	pFunctionTable->pfnGetEntityAPI2 = GetEntityAPI2; pFunctionTable->pfnGetEntityAPI2_Post = GetEntityAPI2_Post;
	pFunctionTable->pfnGetNewDLLFunctions = GetNewDLLFunctions; pFunctionTable->pfnGetEngineFunctions = GetEngineFunctions;
	if (!core_init()) { xerr("failed to create Lua state"); return 0; }
	cache_events(); cmd_register_builtin();
	{ const char *gd = GET_GAME_INFO(PLID, GINFO_REALDLL_FULLPATH); if (gd && *gd && !reapi_init(gd)) xlog("ReGameDLL API not found - reapi:* hooks disabled (ham:* still available)"); }
	g_maxclients = gpGlobals->maxClients;
	core_load_plugins();
	xlog("xmod %s loaded (%zu scripts)", XMOD_VERSION, g_plugins.size());
	return 1;
}
C_DLLEXPORT int Meta_Detach(PLUG_LOADTIME, PL_UNLOAD_REASON) { fire_simple(ev_shutdown); core_shutdown(); return 1; }

C_DLLEXPORT int GetEntityAPI2(DLL_FUNCTIONS *t, int *) {
	memset(&g_dll_pre, 0, sizeof g_dll_pre);
	g_dll_pre.pfnSpawn = Pre_Spawn; g_dll_pre.pfnClientConnect = Pre_ClientConnect; g_dll_pre.pfnClientDisconnect = Pre_ClientDisconnect;
	g_dll_pre.pfnClientUserInfoChanged = Pre_ClientUserInfoChanged; g_dll_pre.pfnClientCommand = Pre_ClientCommand;
	g_dll_pre.pfnServerDeactivate = Pre_ServerDeactivate; g_dll_pre.pfnStartFrame = Pre_StartFrame;
	g_dll_pre.pfnPlayerPreThink = Pre_PlayerPreThink; g_dll_pre.pfnPlayerPostThink = Pre_PlayerPostThink; g_dll_pre.pfnCmdStart = Pre_CmdStart;
	g_dll_pre.pfnTouch = Pre_Touch; g_dll_pre.pfnThink = Pre_Think;
	memcpy(t, &g_dll_pre, sizeof g_dll_pre); return 1;
}
C_DLLEXPORT int GetEntityAPI2_Post(DLL_FUNCTIONS *t, int *) {
	memset(&g_dll_post, 0, sizeof g_dll_post);
	g_dll_post.pfnSpawn = [](edict_t *e) -> int { Post_Spawn(e); RETURN_META_VALUE(MRES_IGNORED, 0); };
	g_dll_post.pfnClientPutInServer = [](edict_t *e) { Post_ClientPutInServer(e); RETURN_META(MRES_IGNORED); };
	g_dll_post.pfnServerActivate = Post_ServerActivate;
	memcpy(t, &g_dll_post, sizeof g_dll_post); return 1;
}
C_DLLEXPORT int GetNewDLLFunctions(NEW_DLL_FUNCTIONS *t, int *) {
	memset(&g_new_pre, 0, sizeof g_new_pre); g_new_pre.pfnCvarValue2 = Pre_CvarValue2;
	memcpy(t, &g_new_pre, sizeof g_new_pre); return 1;
}
C_DLLEXPORT int GetEngineFunctions(enginefuncs_t *t, int *) {
	memset(&g_eng_pre, 0, sizeof g_eng_pre);
	g_eng_pre.pfnMessageBegin = E_MessageBegin; g_eng_pre.pfnMessageEnd = E_MessageEnd; g_eng_pre.pfnWriteByte = E_WriteByte;
	g_eng_pre.pfnWriteChar = E_WriteChar; g_eng_pre.pfnWriteShort = E_WriteShort; g_eng_pre.pfnWriteLong = E_WriteLong;
	g_eng_pre.pfnWriteAngle = E_WriteAngle; g_eng_pre.pfnWriteCoord = E_WriteCoord; g_eng_pre.pfnWriteString = E_WriteString;
	g_eng_pre.pfnWriteEntity = E_WriteEntity; g_eng_pre.pfnEmitSound = E_EmitSound;
	memcpy(t, &g_eng_pre, sizeof g_eng_pre); return 1;
}
}
