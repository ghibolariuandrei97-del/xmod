// Minimal Metamod plugin interface (ABI 5:13) - compatible with Metamod 1.21 / Metamod-R / Metamod-P
#pragma once
#include <extdll.h>
#include <enginecallback.h>
#include <cstdarg>

#ifdef _WIN32
  #undef C_DLLEXPORT
  #define C_DLLEXPORT __declspec(dllexport)
#elif !defined(C_DLLEXPORT)
  #define C_DLLEXPORT __attribute__((visibility("default")))
#endif
#ifndef WINAPI
#define WINAPI
#endif
#ifndef SVC_TEMPENTITY
#define SVC_TEMPENTITY 23
#endif
// ---- helpers normally living in util.h (we avoid pulling in the full game-dll util) ----
extern globalvars_t *gpGlobals;
#undef STRING
#undef MAKE_STRING
#define STRING(off)        ((const char *)(gpGlobals->pStringBase + (unsigned int)(off)))
#define MAKE_STRING(str)   ((unsigned int)((const char *)(str) - gpGlobals->pStringBase))
#define INDEXENT           (*g_engfuncs.pfnPEntityOfEntIndex)
#define ENTINDEX           (*g_engfuncs.pfnIndexOfEdict)
#define SERVER_PRINT       (*g_engfuncs.pfnServerPrint)
#define GET_INFOKEYBUFFER  (*g_engfuncs.pfnGetInfoKeyBuffer)
#define INFOKEY_VALUE      (*g_engfuncs.pfnInfoKeyValue)
#define SET_CLIENT_KEY_VALUE (*g_engfuncs.pfnSetClientKeyValue)
#define REG_SVR_COMMAND    (*g_engfuncs.pfnAddServerCommand)
#define MESSAGE_BEGIN      (*g_engfuncs.pfnMessageBegin)
inline bool FNullEnt(const edict_t *e) { return e == nullptr || (*g_engfuncs.pfnIndexOfEdict)(e) == 0; }

enum PLUG_LOADTIME { PT_NEVER, PT_STARTUP, PT_CHANGELEVEL, PT_ANYTIME, PT_ANYPAUSE };
enum PL_UNLOAD_REASON { PNL_NULL, PNL_INI_DELETED, PNL_FILE_NEWER, PNL_COMMAND, PNL_CMD_FORCED,
	PNL_DELAYED, PNL_PLUGIN, PNL_PLG_FORCED, PNL_RELOAD };

struct plugin_info_t {
	const char *ifvers, *name, *version, *date, *author, *url, *logtag;
	PLUG_LOADTIME loadable, unloadable;
};
typedef plugin_info_t *plid_t;

enum META_RES { MRES_UNSET = 0, MRES_IGNORED, MRES_HANDLED, MRES_OVERRIDE, MRES_SUPERCEDE };

struct meta_globals_t {
	META_RES mres, prev_mres, status;
	void *orig_ret, *override_ret;
};
extern meta_globals_t *gpMetaGlobals;
#define RETURN_META(r)           do { gpMetaGlobals->mres = (r); return; } while (0)
#define RETURN_META_VALUE(r, v)  do { gpMetaGlobals->mres = (r); return (v); } while (0)

typedef int (*GETENTITYAPI_FN)(DLL_FUNCTIONS *, int);
typedef int (*GETENTITYAPI2_FN)(DLL_FUNCTIONS *, int *);
typedef int (*GETNEWDLLFUNCTIONS_FN)(NEW_DLL_FUNCTIONS *, int *);
typedef int (*GET_ENGINE_FUNCTIONS_FN)(enginefuncs_t *, int *);

struct META_FUNCTIONS {
	GETENTITYAPI_FN pfnGetEntityAPI, pfnGetEntityAPI_Post;
	GETENTITYAPI2_FN pfnGetEntityAPI2, pfnGetEntityAPI2_Post;
	GETNEWDLLFUNCTIONS_FN pfnGetNewDLLFunctions, pfnGetNewDLLFunctions_Post;
	GET_ENGINE_FUNCTIONS_FN pfnGetEngineFunctions, pfnGetEngineFunctions_Post;
};
struct gamedll_funcs_t { DLL_FUNCTIONS *dllapi_table; NEW_DLL_FUNCTIONS *newapi_table; };

enum ginfo_t { GINFO_NAME, GINFO_DESC, GINFO_GAMEDIR, GINFO_DLL_FULLPATH, GINFO_DLL_FILENAME, GINFO_REALDLL_FULLPATH };

struct mutil_funcs_t {
	void (*pfnLogConsole)(plid_t, const char *, ...);
	void (*pfnLogMessage)(plid_t, const char *, ...);
	void (*pfnLogError)(plid_t, const char *, ...);
	void (*pfnLogDeveloper)(plid_t, const char *, ...);
	void (*pfnCenterSay)(plid_t, const char *, ...);
	void *pfnCenterSayParms;      // unused placeholders (keep ABI layout)
	void *pfnCenterSayVarargs;
	qboolean (*pfnCallGameEntity)(plid_t, const char *, entvars_t *);
	int (*pfnGetUserMsgID)(plid_t, const char *, int *);
	const char *(*pfnGetUserMsgName)(plid_t, int, int *);
	const char *(*pfnGetPluginPath)(plid_t);
	const char *(*pfnGetGameInfo)(plid_t, ginfo_t);
	int (*pfnLoadPlugin)(plid_t, const char *, PLUG_LOADTIME, void **);
	int (*pfnUnloadPlugin)(plid_t, const char *, PLUG_LOADTIME, PL_UNLOAD_REASON);
	int (*pfnUnloadPluginByHandle)(plid_t, void *, PLUG_LOADTIME, PL_UNLOAD_REASON);
	const char *(*pfnIsQueryingClientCvar)(plid_t, const edict_t *);
	int (*pfnMakeRequestId)(plid_t);
	void (*pfnGetHookTables)(plid_t, enginefuncs_t **, DLL_FUNCTIONS **, NEW_DLL_FUNCTIONS **);
};
extern mutil_funcs_t *gpMetaUtilFuncs;
extern gamedll_funcs_t *gpGamedllFuncs;
extern plugin_info_t Plugin_info;
#define PLID (&Plugin_info)
#define META_INTERFACE_VERSION "5:13"
#define MDLL_ClientKill gpGamedllFuncs->dllapi_table->pfnClientKill
#define MDLL_Spawn      gpGamedllFuncs->dllapi_table->pfnSpawn
#define MDLL_Touch      gpGamedllFuncs->dllapi_table->pfnTouch
#define MDLL_Think      gpGamedllFuncs->dllapi_table->pfnThink
#define MDLL_Use        gpGamedllFuncs->dllapi_table->pfnUse
#define LOG_CONSOLE (*gpMetaUtilFuncs->pfnLogConsole)
#define LOG_MESSAGE (*gpMetaUtilFuncs->pfnLogMessage)
#define LOG_ERROR   (*gpMetaUtilFuncs->pfnLogError)
#define GET_USER_MSG_ID   (*gpMetaUtilFuncs->pfnGetUserMsgID)
#define GET_USER_MSG_NAME (*gpMetaUtilFuncs->pfnGetUserMsgName)
#define GET_GAME_INFO     (*gpMetaUtilFuncs->pfnGetGameInfo)
