/* ============================================================================================================
 *  xmod_ext_glue.h - ALL the Metamod boilerplate an xmod extension needs. Include it in exactly ONE .cpp file.
 *
 *    #define XMOD_EXT_NAME    "xmod_myext"        // required
 *    #define XMOD_EXT_VERSION "1.0"               // required
 *    #include "xmod_ext_glue.h"
 *
 *    static bool xext_init(const XmodApi *api, XmodExt *ext) { api->register_func(ext, "myext", "hello", f_hello, NULL); return true; }
 *    static void xext_shutdown(void) { }          // stop your own threads / free resources (called before xmod removes your functions)
 *
 *  Globals available afterwards:  g_xmod (const XmodApi*), g_xext (XmodExt*), and the usual engine globals (g_engfuncs, gpGlobals).
 *  If you also need engine/game hooks, do not use this glue: copy the Metamod entry points from xmod's src/main.cpp instead.
 * ============================================================================================================ */
#ifndef XMOD_EXT_GLUE_H
#define XMOD_EXT_GLUE_H
#ifndef XMOD_EXT_NAME
#error "define XMOD_EXT_NAME before including xmod_ext_glue.h"
#endif
#ifndef XMOD_EXT_VERSION
#error "define XMOD_EXT_VERSION before including xmod_ext_glue.h"
#endif
#include "mm_sdk.h"
#include "xmod_ext.h"

enginefuncs_t g_engfuncs;
globalvars_t *gpGlobals;
meta_globals_t *gpMetaGlobals;
mutil_funcs_t *gpMetaUtilFuncs;
gamedll_funcs_t *gpGamedllFuncs;
plugin_info_t Plugin_info = { META_INTERFACE_VERSION, XMOD_EXT_NAME, XMOD_EXT_VERSION, __DATE__, "xmod extension", "", "XEXT", PT_ANYTIME, PT_ANYTIME };

static const XmodApi *g_xmod = nullptr;
static XmodExt *g_xext = nullptr;
static bool xext_init(const XmodApi *api, XmodExt *ext);   /* you implement */
static void xext_shutdown(void);                            /* you implement */

extern "C" {
C_DLLEXPORT void WINAPI GiveFnptrsToDll(enginefuncs_t *pengfuncsFromEngine, globalvars_t *pGlobals) {
	memcpy(&g_engfuncs, pengfuncsFromEngine, sizeof(enginefuncs_t)); gpGlobals = pGlobals;
}
C_DLLEXPORT int Meta_Query(char *, plugin_info_t **plinfo, mutil_funcs_t *pMetaUtilFuncs) { *plinfo = &Plugin_info; gpMetaUtilFuncs = pMetaUtilFuncs; return 1; }
C_DLLEXPORT int Meta_Attach(PLUG_LOADTIME, META_FUNCTIONS *pFunctionTable, meta_globals_t *pMGlobals, gamedll_funcs_t *pGamedllFuncs) {
	gpMetaGlobals = pMGlobals; gpGamedllFuncs = pGamedllFuncs; memset(pFunctionTable, 0, sizeof(META_FUNCTIONS));
	g_xmod = xmod_ext_connect();
	if (!g_xmod) { LOG_ERROR(PLID, "xmod is not loaded (or too old): list xmod BEFORE %s in addons/metamod/plugins.ini", XMOD_EXT_NAME); return 0; }
	g_xext = g_xmod->ext_register(XMOD_EXT_NAME, XMOD_EXT_VERSION);
	if (!g_xext) { LOG_ERROR(PLID, "xmod refused to register extension '%s' (name already in use?)", XMOD_EXT_NAME); return 0; }
	if (!xext_init(g_xmod, g_xext)) { g_xmod->ext_unregister(g_xext); g_xext = nullptr; return 0; }
	return 1;
}
C_DLLEXPORT int Meta_Detach(PLUG_LOADTIME, PL_UNLOAD_REASON) {
	if (g_xext) { xext_shutdown(); g_xmod->ext_unregister(g_xext); g_xext = nullptr; }
	return 1;
}
}
#endif
