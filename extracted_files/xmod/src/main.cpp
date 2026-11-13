// xmod - Metamod glue: exports, hook tables, lifecycle events
#include "xmod.h"
#include "hookutil.h"
#include <usercmd.h>
#include <entity_state.h>
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
	*ev_pre, *ev_post, *ev_cmdstart, *ev_touch, *ev_think, *ev_spawn, *ev_emit, *ev_atfp, *ev_atfp_post;
entity_state_t *g_fp_state = nullptr;               // valid only while an add_to_full_pack:post handler runs (xmod.fullpack.*)
unsigned char g_fp_hide[XMOD_MAXPLAYERS + 1][2048]; unsigned char g_fp_hide_all[2048]; bool g_fp_native = false;   // native hide table: no Lua call per entity
bool g_attach_done = false, g_atfp_pre_installed = false, g_atfp_post_installed = false;

static void cache_events() {
	ev_precache = ev_get("precache"); ev_init = ev_get("init"); ev_config = ev_get("config"); ev_map_end = ev_get("map_end");
	ev_shutdown = ev_get("shutdown"); ev_frame = ev_get("frame"); ev_connect = ev_get("client_connect"); ev_putin = ev_get("client_putinserver");
	ev_disc = ev_get("client_disconnect"); ev_info = ev_get("client_infochanged"); ev_pre = ev_get("prethink"); ev_post = ev_get("postthink");
	ev_cmdstart = ev_get("cmd_start"); ev_touch = ev_get("touch"); ev_think = ev_get("think"); ev_spawn = ev_get("spawn"); ev_emit = ev_get("emit_sound"); ev_atfp = ev_get("add_to_full_pack"); ev_atfp_post = ev_get("add_to_full_pack:post");
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
// ---- client registration. Humans come through ClientConnect/ClientPutInServer. ReGameDLL/CS bots do NOT: the game calls its own
// ClientPutInServer internally (invisible to Metamod), so bots are registered from the engine's CreateFakeClient (like AMXX does),
// 'putinserver' is fired the first time the bot thinks, and any client edict we somehow missed is registered lazily in PlayerPreThink.
static void register_client(edict_t *e, const char *name, const char *ip, bool bot, bool fire_connect) {
	int id = ENTINDEX(e); if (id < 1 || id > XMOD_MAXPLAYERS) return;
	PlayerState &p = g_players[id]; if (p.connected) return;
	p = PlayerState(); p.connected = true; p.bot = bot; p.name = name ? name : ""; p.ip = ip ? ip : ""; p.userid = GETPLAYERUSERID(e);
	clear_player_pdata(id);
	if (fire_connect && ev_has(ev_connect)) ev_fire(ev_connect, 3, [&](lua_State *l) { lua_pushinteger(l, id); lua_pushstring(l, p.name.c_str()); lua_pushstring(l, p.ip.c_str()); return 3; });
}
static void fire_putin(edict_t *e, int id) {
	PlayerState &p = g_players[id]; if (p.putin) return;
	p.connected = true; p.putin = true; p.bot = (e->v.flags & FL_FAKECLIENT) != 0 || p.bot;
	if (p.bot) ham_hook_special(e);
	p.userid = GETPLAYERUSERID(e); const char *a = GETPLAYERAUTHID(e); p.authid = a ? a : "";
	char *ib = GET_INFOKEYBUFFER(e); p.name = INFOKEY_VALUE(ib, "name");
	if (ev_has(ev_putin)) ev_fire(ev_putin, 1, [&](lua_State *l) { lua_pushinteger(l, id); return 1; });
}
static void Post_ClientPutInServer(edict_t *pEntity) {
	int id = ENTINDEX(pEntity); if (id < 1 || id > XMOD_MAXPLAYERS) return;
	if (!g_players[id].connected) register_client(pEntity, STRING(pEntity->v.netname), "", (pEntity->v.flags & FL_FAKECLIENT) != 0, true);
	fire_putin(pEntity, id);
}
static edict_t *Post_CreateFakeClient(const char *netname) {
	edict_t *e = *reinterpret_cast<edict_t **>(gpMetaGlobals->orig_ret);
	if (e) register_client(e, netname, "127.0.0.1", true, true);
	RETURN_META_VALUE(MRES_IGNORED, nullptr);
}
static void Post_MessageEnd() { msg_end_post(); RETURN_META(MRES_IGNORED); }
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
	memset(g_fp_hide, 0, sizeof g_fp_hide); memset(g_fp_hide_all, 0, sizeof g_fp_hide_all);   // entity indices change with the map
	g_map_active = false; g_world_spawned = false;
	for (int i = 0; i <= XMOD_MAXPLAYERS; i++) g_players[i] = PlayerState();
	RETURN_META(MRES_IGNORED);
}
static void Pre_StartFrame() {
	g_time = gpGlobals->time;
	msg_frame_reset();
	{ static float next_poll = 0; if (g_time >= next_poll || g_time + 5.0f < next_poll) { next_poll = g_time + 1.0f; core_set_timeout((int)CVAR_GET_FLOAT("xmod_script_timeout")); ext_set_limits((int)CVAR_GET_FLOAT("xmod_async_threads"), (int)CVAR_GET_FLOAT("xmod_async_budget_ms"), (int)CVAR_GET_FLOAT("xmod_async_queue_max")); } }
	ext_pump();
	if (g_need_config) {          // frame 1: exec our cfg (async), frame 2: fire 'config' so cvars from it are already applied
		if (!g_cfg_exec) { g_cfg_exec = true; SERVER_COMMAND(const_cast<char *>("exec addons/xmod/configs/xmod.cfg\n")); }
		else { g_cfg_exec = false; g_need_config = false; fire_simple(ev_config); }
	}
	timers_run(); cvar_watch_run();
	if (g_stats_on) stats_frame();
	if (ev_has(ev_frame)) ev_fire_nums(ev_frame, nullptr, 0, 0);
	RETURN_META(MRES_IGNORED);
}
static void Pre_PlayerPreThink(edict_t *e) {
	{ int id = ENTINDEX(e);
	  if (id >= 1 && id <= XMOD_MAXPLAYERS && !g_players[id].putin && (e->v.flags & (FL_CLIENT | FL_FAKECLIENT))) {
		if (!g_players[id].connected) register_client(e, INFOKEY_VALUE(GET_INFOKEYBUFFER(e), "name"), (e->v.flags & FL_FAKECLIENT) ? "127.0.0.1" : "", (e->v.flags & FL_FAKECLIENT) != 0, true);
		fire_putin(e, id);
	  } }
	if (ev_has(ev_pre)) { int id = ENTINDEX(e); if (is_valid_player(id)) { double v[1] = {(double)id}; ev_fire_nums(ev_pre, v, 1, 1); } }
	RETURN_META(MRES_IGNORED);
}
static void Pre_PlayerPostThink(edict_t *e) {
	if (ev_has(ev_post)) { int id = ENTINDEX(e); if (is_valid_player(id)) { double v[1] = {(double)id}; ev_fire_nums(ev_post, v, 1, 1); } }
	RETURN_META(MRES_IGNORED);
}
static void Pre_CmdStart(const edict_t *player, const struct usercmd_s *cmd, unsigned int) {
	if (ev_has(ev_cmdstart)) {
		int id = ENTINDEX(const_cast<edict_t *>(player));
		if (is_valid_player(id)) { double v[6] = {(double)id, (double)cmd->buttons, (double)cmd->impulse, cmd->forwardmove, cmd->sidemove, cmd->upmove}; ev_fire_nums(ev_cmdstart, v, 6, 7); }
	}
	RETURN_META(MRES_IGNORED);
}
static std::unordered_map<int, Event *> g_touch_cls, g_think_cls;     // classname string_t -> per-class event (no allocation on the hot path)
static Event *class_event(std::unordered_map<int, Event *> &cache, const char *prefix, edict_t *e) {
	int key = e->v.classname; auto it = cache.find(key); if (it != cache.end()) return it->second;
	Event *ev = ev_get((std::string(prefix) + STRING(key)).c_str()); cache[key] = ev; return ev;
}
static void Pre_Touch(edict_t *a, edict_t *b) {
	if (!ev_has(ev_touch) && !g_any_class_hook) RETURN_META(MRES_IGNORED);
	double v[2] = {(double)ENTINDEX(a), (double)ENTINDEX(b)};
	if (ev_has(ev_touch) && ev_fire_nums(ev_touch, v, 2, 3).kind == 1) RETURN_META(MRES_SUPERCEDE);
	if (g_any_class_hook && a) { Event *ce = class_event(g_touch_cls, "touch:", a); if (ev_has(ce) && ev_fire_nums(ce, v, 2, 3).kind == 1) RETURN_META(MRES_SUPERCEDE); }
	RETURN_META(MRES_IGNORED);
}
static void Pre_Think(edict_t *e) {
	if (!ev_has(ev_think) && !g_any_class_hook) RETURN_META(MRES_IGNORED);
	double v[1] = {(double)ENTINDEX(e)};
	if (ev_has(ev_think) && ev_fire_nums(ev_think, v, 1, 1).kind == 1) RETURN_META(MRES_SUPERCEDE);
	if (g_any_class_hook && e) { Event *ce = class_event(g_think_cls, "think:", e); if (ev_has(ce) && ev_fire_nums(ce, v, 1, 1).kind == 1) RETURN_META(MRES_SUPERCEDE); }
	RETURN_META(MRES_IGNORED);
}
// AddToFullPack: called for every entity, for every client, every frame (can be millions of calls/s) - only installed if a script registered it.
static int Pre_AddToFullPack(struct entity_state_s *, int e, edict_t *, edict_t *host, int, int player, unsigned char *) {
	int hi = ENTINDEX(host);
	if (g_fp_native && e >= 0 && e < 2048 && (g_fp_hide_all[e] || (hi >= 0 && hi <= XMOD_MAXPLAYERS && g_fp_hide[hi][e]))) RETURN_META_VALUE(MRES_SUPERCEDE, 0);
	if (!ev_has(ev_atfp)) RETURN_META_VALUE(MRES_IGNORED, 0);
	double v[3] = {(double)e, (double)hi, player ? 1.0 : 0.0};
	if (ev_fire_nums(ev_atfp, v, 3, 7).kind == 1) RETURN_META_VALUE(MRES_SUPERCEDE, 0);
	RETURN_META_VALUE(MRES_IGNORED, 0);
}
static int Post_AddToFullPack(struct entity_state_s *state, int e, edict_t *, edict_t *host, int, int player, unsigned char *) {
	int ret = *(int *)gpMetaGlobals->orig_ret;
	if (!ret) RETURN_META_VALUE(MRES_IGNORED, 0);                 // entity was culled for this client: nothing to do (no Lua call)
	double v[4] = {(double)e, (double)ENTINDEX(host), player ? 1.0 : 0.0, (double)ret};
	g_fp_state = state; EvResult r = ev_fire_nums(ev_atfp_post, v, 4, 15); g_fp_state = nullptr;
	if (r.kind == 1) RETURN_META_VALUE(MRES_OVERRIDE, 0);
	RETURN_META_VALUE(MRES_IGNORED, 1);
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
static enginefuncs_t g_eng_pre, g_eng_post;

extern "C" {
C_DLLEXPORT int GetEntityAPI2(DLL_FUNCTIONS *, int *);
C_DLLEXPORT int GetEntityAPI2_Post(DLL_FUNCTIONS *, int *);
C_DLLEXPORT int GetNewDLLFunctions(NEW_DLL_FUNCTIONS *, int *);
C_DLLEXPORT int GetEngineFunctions(enginefuncs_t *, int *);
C_DLLEXPORT int GetEngineFunctions_Post(enginefuncs_t *, int *);
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
	pFunctionTable->pfnGetNewDLLFunctions = GetNewDLLFunctions; pFunctionTable->pfnGetEngineFunctions = GetEngineFunctions; pFunctionTable->pfnGetEngineFunctions_Post = GetEngineFunctions_Post;
	if (!core_init()) { xerr("failed to create Lua state"); return 0; }
	cache_events(); cmd_register_builtin();
	for (auto d : { std::make_pair("xmod_async_threads", "4"), std::make_pair("xmod_async_budget_ms", "2"), std::make_pair("xmod_async_queue_max", "4096") })
		if (!CVAR_GET_POINTER(d.first)) { cvar_t *cv = new cvar_t; cv->name = strdup(d.first); cv->string = strdup(d.second); cv->flags = FCVAR_SERVER; cv->value = 0; cv->next = nullptr; CVAR_REGISTER(cv); }
	if (!CVAR_GET_POINTER("xmod_script_timeout")) { cvar_t *cv = new cvar_t; cv->name = strdup("xmod_script_timeout"); cv->string = strdup("100"); cv->flags = FCVAR_SERVER; cv->value = 0; cv->next = nullptr; CVAR_REGISTER(cv); }
	core_set_timeout((int)CVAR_GET_FLOAT("xmod_script_timeout"));
	{ const char *gd = GET_GAME_INFO(PLID, GINFO_REALDLL_FULLPATH); if (gd && *gd && !reapi_init(gd)) xlog("ReGameDLL API not found - reapi:* hooks disabled (ham:* still available)"); }
	g_maxclients = gpGlobals->maxClients;
	core_load_plugins();
	g_attach_done = true;
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
	if (ev_has(ev_atfp) || g_fp_native) { g_dll_pre.pfnAddToFullPack = Pre_AddToFullPack; g_atfp_pre_installed = true; }
	memcpy(t, &g_dll_pre, sizeof g_dll_pre); return 1;
}
C_DLLEXPORT int GetEntityAPI2_Post(DLL_FUNCTIONS *t, int *) {
	memset(&g_dll_post, 0, sizeof g_dll_post);
	g_dll_post.pfnSpawn = [](edict_t *e) -> int { Post_Spawn(e); RETURN_META_VALUE(MRES_IGNORED, 0); };
	g_dll_post.pfnClientPutInServer = [](edict_t *e) { Post_ClientPutInServer(e); RETURN_META(MRES_IGNORED); };
	g_dll_post.pfnServerActivate = Post_ServerActivate;
	if (ev_has(ev_atfp_post)) { g_dll_post.pfnAddToFullPack = Post_AddToFullPack; g_atfp_post_installed = true; }
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
C_DLLEXPORT int GetEngineFunctions_Post(enginefuncs_t *t, int *) {
	memset(&g_eng_post, 0, sizeof g_eng_post);
	g_eng_post.pfnCreateFakeClient = Post_CreateFakeClient; g_eng_post.pfnMessageEnd = Post_MessageEnd;
	memcpy(t, &g_eng_post, sizeof g_eng_post); return 1;
}
}
