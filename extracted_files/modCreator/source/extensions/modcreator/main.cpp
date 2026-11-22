// main.cpp - Metamod entry points, the xmod connection and the few engine hooks ModCreator needs.
#include "mc.h"
#include <cstdarg>
#include <cstdio>

enginefuncs_t g_engfuncs;
globalvars_t *gpGlobals;
meta_globals_t *gpMetaGlobals;
mutil_funcs_t *gpMetaUtilFuncs;
gamedll_funcs_t *gpGamedllFuncs;
plugin_info_t Plugin_info = {META_INTERFACE_VERSION, "ModCreator", MC_VERSION, __DATE__, "Andrew & Claude", "", "MODCREATOR", PT_ANYTIME, PT_ANYTIME};

const XmodApi *g_xmod = nullptr;
XmodExt *g_xext = nullptr;

void mc_log(const char *fmt, ...) {
	char b[1024]; va_list ap; va_start(ap, fmt); int n = snprintf(b, sizeof b, "[modcreator] "); vsnprintf(b + n, sizeof b - n, fmt, ap); va_end(ap);
	if (g_xmod) g_xmod->log(b);
}
void Args::err(const char *fmt, ...) const {
	char b[512]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap); g_xmod->call_error(c, b);
}
XmodValue *mc_vec(const float *x) { XmodValue *a = g_xmod->val_array(); for (int k = 0; k < 3; k++) g_xmod->val_push(a, g_xmod->val_num(x[k])); return a; }
bool mc_reg(const char *name, xmod_sync_fn fn) {
	if (g_xmod->register_func(g_xext, MC_NS, name, fn, nullptr)) return true;
	char b[160]; snprintf(b, sizeof b, "[modcreator] cannot register xmod.mc.%s", name); g_xmod->log_error(b); return false;
}

// ---- engine hooks (pre). They only look; they supercede only for custom weapons that asked for it.
static void eh_SetModel(edict_t *e, const char *m) { gpMetaGlobals->mres = MRES_IGNORED; mc_hook_setmodel(e, m); }
static void eh_PlaybackEvent(int flags, const edict_t *invoker, unsigned short ev, float delay, float *o, float *a, float f1, float f2, int i1, int i2, int b1, int b2) {
	(void)delay; (void)o; (void)a; (void)f1; (void)f2; (void)i1; (void)i2; (void)b1; (void)b2;
	gpMetaGlobals->mres = MRES_IGNORED; mc_hook_playbackevent(flags, invoker, ev);
}
static enginefuncs_t g_eng_hooks;
static int GetEngineFunctions(enginefuncs_t *t, int *) {
	memset(&g_eng_hooks, 0, sizeof g_eng_hooks);
	g_eng_hooks.pfnSetModel = eh_SetModel;
	g_eng_hooks.pfnPlaybackEvent = eh_PlaybackEvent;
	memcpy(t, &g_eng_hooks, sizeof g_eng_hooks); return 1;
}
static void dh_ServerActivate(edict_t *, int, int) { gpMetaGlobals->mres = MRES_IGNORED; weapon_on_map_change(); }
static void dh_UpdateClientData(const edict_t *ent, int sendweapons, clientdata_s *cd) { gpMetaGlobals->mres = MRES_IGNORED; mc_hook_ucd(ent, sendweapons, cd); }
static DLL_FUNCTIONS g_dll_post;
static int GetEntityAPI2_Post(DLL_FUNCTIONS *t, int *) {
	memset(&g_dll_post, 0, sizeof g_dll_post); g_dll_post.pfnServerActivate = dh_ServerActivate; g_dll_post.pfnUpdateClientData = dh_UpdateClientData;
	memcpy(t, &g_dll_post, sizeof g_dll_post); return 1;
}

extern "C" {
C_DLLEXPORT void WINAPI GiveFnptrsToDll(enginefuncs_t *eng, globalvars_t *globals) { memcpy(&g_engfuncs, eng, sizeof(enginefuncs_t)); gpGlobals = globals; }
C_DLLEXPORT int Meta_Query(char *, plugin_info_t **pinfo, mutil_funcs_t *mutil) { *pinfo = &Plugin_info; gpMetaUtilFuncs = mutil; return 1; }
C_DLLEXPORT int Meta_Attach(PLUG_LOADTIME, META_FUNCTIONS *t, meta_globals_t *mg, gamedll_funcs_t *gd) {
	gpMetaGlobals = mg; gpGamedllFuncs = gd;
	memset(t, 0, sizeof(META_FUNCTIONS));
	t->pfnGetEngineFunctions = GetEngineFunctions;
	t->pfnGetEntityAPI2_Post = GetEntityAPI2_Post;
	g_xmod = xmod_ext_connect();
	if (!g_xmod) { LOG_ERROR(PLID, "xmod is not loaded (or too old): list xmod BEFORE modcreator in addons/metamod/plugins.ini"); return 0; }
	g_xext = g_xmod->ext_register("modcreator", MC_VERSION);
	if (!g_xext) { LOG_ERROR(PLID, "xmod refused to register modcreator (loaded twice?)"); return 0; }
	mc_pd_init();
	bool ok = reg_ent() && reg_studio() && reg_pdata() && reg_rules() && reg_combat() && reg_weapon();
	if (!ok) { g_xmod->ext_unregister(g_xext); g_xext = nullptr; return 0; }
	mc_log("ModCreator %s ready: xmod.mc.* + require(\"modcreator\")  (%s, game rules: %s)", MC_VERSION, MC_LINUX ? "linux" : "windows", mc_rules_source());
	return 1;
}
C_DLLEXPORT int Meta_Detach(PLUG_LOADTIME, PL_UNLOAD_REASON) {
	if (g_xext) { g_xmod->ext_unregister(g_xext); g_xext = nullptr; }
	return 1;
}
}
