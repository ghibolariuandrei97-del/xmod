// main.cpp - Metamod entry points and the xmod connection for Codex.
// (Same calls as xmod_ext_glue.h, written out because codex.h shares g_xmod with every file.)
#include "codex.h"
#include <cstdarg>
#include <cstdio>

enginefuncs_t g_engfuncs;
globalvars_t *gpGlobals;
meta_globals_t *gpMetaGlobals;
mutil_funcs_t *gpMetaUtilFuncs;
gamedll_funcs_t *gpGamedllFuncs;
plugin_info_t Plugin_info = {META_INTERFACE_VERSION, "xmod_codex", CODEX_VERSION, __DATE__, "Andrew & Claude", "", "CODEX", PT_ANYTIME, PT_ANYTIME};

const XmodApi *g_xmod = nullptr;
XmodExt *g_xext = nullptr;
bool g_sync_hooks = false;

void cx_log(const char *fmt, ...)
{
	char b[1200];
	va_list ap; va_start(ap, fmt); int n = snprintf(b, sizeof b, "[codex] "); vsnprintf(b + n, sizeof b - n, fmt, ap); va_end(ap);
	if (g_xmod) g_xmod->log(b);
}
void cx_err(const char *fmt, ...)
{
	char b[1200];
	va_list ap; va_start(ap, fmt); int n = snprintf(b, sizeof b, "[codex] "); vsnprintf(b + n, sizeof b - n, fmt, ap); va_end(ap);
	if (g_xmod) g_xmod->log_error(b);
}

extern "C" {
C_DLLEXPORT void WINAPI GiveFnptrsToDll(enginefuncs_t *eng, globalvars_t *globals)
{
	memcpy(&g_engfuncs, eng, sizeof(enginefuncs_t));
	gpGlobals = globals;
}

C_DLLEXPORT int Meta_Query(char *, plugin_info_t **pinfo, mutil_funcs_t *mutil)
{
	*pinfo = &Plugin_info;
	gpMetaUtilFuncs = mutil;
	return 1;
}

C_DLLEXPORT int Meta_Attach(PLUG_LOADTIME, META_FUNCTIONS *t, meta_globals_t *mg, gamedll_funcs_t *gd)
{
	gpMetaGlobals = mg;
	gpGamedllFuncs = gd;
	memset(t, 0, sizeof(META_FUNCTIONS));
	g_xmod = xmod_ext_connect();
	if (!g_xmod) { LOG_ERROR(PLID, "xmod is not loaded (or too old): list xmod BEFORE xmod_codex in addons/metamod/plugins.ini"); return 0; }
	g_xext = g_xmod->ext_register("xmod_codex", CODEX_VERSION);
	if (!g_xext) { LOG_ERROR(PLID, "xmod refused to register xmod_codex"); return 0; }
	g_sync_hooks = XMOD_EXT_HAS(g_xmod, fire_hook_sync) && XMOD_EXT_HAS(g_xmod, has_handlers);
	guard_install();
	mod_refresh();
	if (!codex_register(g_xmod, g_xext)) { g_xmod->ext_unregister(g_xext); g_xext = nullptr; return 0; }
	cache_load();
	cx_log("Codex %s ready: xmod.codex.* (%s hooks, fault guard on)", CODEX_VERSION, g_sync_hooks ? "full" : "observe-only (update xmod to 0.4.1 for full)");
	return 1;
}

C_DLLEXPORT int Meta_Detach(PLUG_LOADTIME, PL_UNLOAD_REASON)
{
	hook_remove_all();       // detours and vtable slots back to the original bytes
	patch_remove_all();      // every other patch too
	cache_save();
	guard_remove();
	if (g_xext) { g_xmod->ext_unregister(g_xext); g_xext = nullptr; }
	return 1;
}
}
