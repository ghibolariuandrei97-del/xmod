// Smoke test: loads xmod_mm_i386.so against a fake engine and drives a few events.
#include "../src/mm_sdk.h"
#include <dlfcn.h>
#include <usercmd.h>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <string>
#include <vector>

enginefuncs_t g_engfuncs; globalvars_t *gpGlobals;
static globalvars_t gv; static edict_t ed[34]; static char strbase[16];
static std::vector<std::string> g_argv; static std::string g_args;
static std::map<std::string, void (*)()> svrcmds; static std::map<std::string, cvar_t *> cvars;
static std::vector<std::string> msgs; static char g_info[34][128];
static std::map<std::string, int> msgids = {{"SayText", 100}, {"ShowMenu", 101}, {"Money", 102}, {"DeathMsg", 103}, {"ScreenFade", 104}};
static void MM_Log(plid_t, const char *f, ...) { char b[1024]; va_list a; va_start(a, f); vsnprintf(b, 1024, f, a); va_end(a); printf("[log] %s\n", b); }
static int MM_MsgId(plid_t, const char *n, int *) { auto i = msgids.find(n); return i == msgids.end() ? 0 : i->second; }
static const char *MM_MsgName(plid_t, int id, int *) { for (auto &p : msgids) if (p.second == id) return p.first.c_str(); return nullptr; }
static mutil_funcs_t mu;
static void *fake_vt[256];
static int fake_takedamage(void *, entvars_t *, entvars_t *, float dmg, int bits) { printf("[orig takedamage] dmg=%.1f bits=%d\n", dmg, bits); return (int)dmg; }
static void fake_spawn(void *) { printf("[orig spawn]\n"); }
static const char *MM_GameInfo(plid_t, ginfo_t) { return ""; }
static edict_t *PEnt(int i) { return &ed[i]; }
static int IdxOf(const edict_t *e) { return (int)(e - ed); }
static void Say(const char *a0, const char *args, std::vector<std::string> v) { g_argv = v; g_args = args; (void)a0; }

int main() { setvbuf(stdout, NULL, _IONBF, 0);
	gv.maxClients = 32; gv.maxEntities = 34; gv.pStringBase = strbase; gv.time = 1.0f; gpGlobals = &gv;
	memset(&g_engfuncs, 0, sizeof g_engfuncs);
	g_engfuncs.pfnGetGameDir = [](char *d) { strcpy(d, "gd"); };
	g_engfuncs.pfnServerPrint = [](const char *s) { printf("[srv] %s", s); };
	g_engfuncs.pfnAddServerCommand = [](char *n, void (*f)()) { svrcmds[n] = f; };
	g_engfuncs.pfnCVarGetPointer = [](const char *n) -> cvar_t * { auto i = cvars.find(n); return i == cvars.end() ? nullptr : i->second; };
	g_engfuncs.pfnCVarRegister = [](cvar_t *c) { cvars[c->name] = c; c->value = (float)atof(c->string); };
	g_engfuncs.pfnCVarGetString = [](const char *n) -> const char * { auto i = cvars.find(n); return i == cvars.end() ? "" : i->second->string; };
	g_engfuncs.pfnCVarGetFloat = [](const char *n) -> float { auto i = cvars.find(n); return i == cvars.end() ? 0.f : (float)atof(i->second->string); };
	g_engfuncs.pfnCmd_Argc = []() { return (int)g_argv.size(); };
	g_engfuncs.pfnCmd_Argv = [](int i) -> const char * { return i < (int)g_argv.size() ? g_argv[i].c_str() : ""; };
	g_engfuncs.pfnCmd_Args = []() -> const char * { return g_args.c_str(); };
	g_engfuncs.pfnPEntityOfEntIndex = PEnt; g_engfuncs.pfnIndexOfEdict = IdxOf;
	g_engfuncs.pfnGetPlayerUserId = [](edict_t *e) { return 100 + IdxOf(e); };
	g_engfuncs.pfnGetPlayerAuthId = [](edict_t *e) -> const char * { return IdxOf(e) == 1 ? "STEAM_0:1:1234" : "STEAM_0:0:777"; };
	g_engfuncs.pfnGetInfoKeyBuffer = [](edict_t *e) -> char * { return g_info[IdxOf(e)]; };
	g_engfuncs.pfnInfoKeyValue = [](char *buf, char *k) -> char * { static char out[128]; out[0] = 0; std::string b = buf, key = std::string("\\") + k + "\\"; size_t p = b.find(key);
		if (p != std::string::npos) { size_t s = p + key.size(), e = b.find('\\', s); strncpy(out, b.substr(s, e == std::string::npos ? e : e - s).c_str(), 127); } return out; };
	g_engfuncs.pfnServerCommand = [](char *c) { printf("[SERVERCMD] %s", c); };
	g_engfuncs.pfnServerExecute = []() {};
	g_engfuncs.pfnSetClientKeyValue = [](int i, char *, char *k, char *v) { printf("[setkey] ent %d %s=%s\n", i, k, v); };
	g_engfuncs.pfnCVarSetString = [](const char *n, const char *v) { auto i = cvars.find(n); if (i != cvars.end()) { i->second->string = strdup(v); } printf("[cvarset] %s=%s\n", n, v); };
	g_engfuncs.pfnIsMapValid = [](char *) { return 1; };
	g_engfuncs.pfnGetPlayerStats = [](const edict_t *, int *p, int *l) { *p = 42; *l = 0; };
	g_engfuncs.pfnIsDedicatedServer = []() { return 1; };
	g_engfuncs.pfnMessageBegin = [](int d, int t, const float *, edict_t *e) { char b[64]; snprintf(b, 64, "MSG(dest=%d type=%d ent=%d)", d, t, e ? IdxOf(e) : 0); msgs.push_back(b); };
	g_engfuncs.pfnWriteByte = [](int v) { msgs.back() += " b" + std::to_string(v); };
	g_engfuncs.pfnWriteShort = [](int v) { msgs.back() += " s" + std::to_string(v); };
	g_engfuncs.pfnWriteChar = [](int v) { msgs.back() += " c" + std::to_string(v); };
	g_engfuncs.pfnWriteString = [](const char *s) { msgs.back() += std::string(" \"") + s + "\""; };
	g_engfuncs.pfnMessageEnd = []() { printf("[msg] %s\n", msgs.back().c_str()); };
	g_engfuncs.pfnClientPrintf = [](edict_t *e, PRINT_TYPE t, const char *s) { printf("[print %d->%d] %s", t, IdxOf(e), s); };
	for (int i = 0; i < 34; i++) { ed[i].pvPrivateData = calloc(1, 4096); ed[i].v.pContainingEntity = &ed[i]; *(void ***)ed[i].pvPrivateData = fake_vt; *(entvars_t **)((char *)ed[i].pvPrivateData + 4) = &ed[i].v; }
	fake_vt[12] = (void *)fake_takedamage; fake_vt[0] = (void *)fake_spawn;
	g_engfuncs.pfnCreateNamedEntity = [](int) -> edict_t * { return &ed[33]; };
	g_engfuncs.pfnRemoveEntity = [](edict_t *) {};
	mu.pfnGetGameInfo = MM_GameInfo;
	ed[1].v.health = 100; ed[1].v.deadflag = DEAD_NO;
	mu.pfnLogMessage = mu.pfnLogError = mu.pfnLogConsole = MM_Log; mu.pfnGetUserMsgID = MM_MsgId; mu.pfnGetUserMsgName = MM_MsgName;

	void *h = dlopen("../addons/xmod/dlls/xmod_mm_i386.so", RTLD_NOW); if (!h) { printf("dlopen: %s\n", dlerror()); return 1; }
	auto Give = (void (*)(enginefuncs_t *, globalvars_t *))dlsym(h, "GiveFnptrsToDll");
	auto Query = (int (*)(char *, plugin_info_t **, mutil_funcs_t *))dlsym(h, "Meta_Query");
	auto Attach = (int (*)(PLUG_LOADTIME, META_FUNCTIONS *, meta_globals_t *, gamedll_funcs_t *))dlsym(h, "Meta_Attach");
	auto Detach = (int (*)(PLUG_LOADTIME, PL_UNLOAD_REASON))dlsym(h, "Meta_Detach");
	Give(&g_engfuncs, &gv); plugin_info_t *pi; Query((char *)"5:13", &pi, &mu);
	META_FUNCTIONS mf; meta_globals_t mg; memset(&mg, 0, sizeof mg); gamedll_funcs_t gd = {nullptr, nullptr};
	printf("== attach\n"); if (!Attach(PT_ANYTIME, &mf, &mg, &gd)) { printf("attach failed\n"); return 1; }
	DLL_FUNCTIONS pre, post; mf.pfnGetEntityAPI2(&pre, nullptr); mf.pfnGetEntityAPI2_Post(&post, nullptr);
	printf("== xmod plugins\n"); g_argv = {"xmod", "plugins"}; svrcmds["xmod"]();
	auto cmd = [&](int id, std::vector<std::string> v, const char *args) { g_argv = v; g_args = args; pre.pfnClientCommand(&ed[id]); };
	auto frames = [&](int n, float dt) { for (int i = 0; i < n; i++) { gv.time += dt; pre.pfnStartFrame(); } };
	strcpy(g_info[1], "\\name\\Tester\\model\\urban\\_pw\\"); strcpy(g_info[2], "\\name\\Bob\\model\\urban\\_pw\\");
	printf("== map start\n"); pre.pfnSpawn(&ed[0]); post.pfnServerActivate(nullptr, 0, 32); frames(1, 0.1f);
	printf("== connect players\n"); char rej[128] = {0};
	for (int i = 1; i <= 2; i++) { pre.pfnClientConnect(&ed[i], i == 1 ? "Tester" : "Bob", "1.2.3.4", rej); post.pfnClientPutInServer(&ed[i]); ed[i].v.health = 100; }
	frames(3, 0.5f);
	printf("== xmod_who (admin)\n"); cmd(1, {"xmod_who"}, "");
	printf("== Bob (non admin) tries xmod_kick\n"); cmd(2, {"xmod_kick", "Tester"}, "Tester");
	printf("== admin: slap Bob 10\n"); cmd(1, {"xmod_slap", "Bob", "10"}, "Bob 10"); printf("Bob hp=%.0f\n", ed[2].v.health);
	printf("== admin: ban Bob 5 cheating\n"); cmd(1, {"xmod_ban", "Bob", "5", "cheating"}, "Bob 5 cheating");
	printf("== admin: god + hp via amx_ alias\n"); cmd(1, {"amx_hp", "@all", "250"}, "@all 250"); printf("hp now %.0f %.0f\n", ed[1].v.health, ed[2].v.health);
	printf("== admin: team\n"); cmd(1, {"xmod_team", "Bob", "CT"}, "Bob CT");
	printf("== server console: xmod_cvar + xmod_map\n"); g_argv = {"xmod_cvar", "sv_gravity", "800"}; cvars["sv_gravity"] = new cvar_t{(char *)"sv_gravity", (char *)"1000", 0, 1000, nullptr}; svrcmds["xmod_cvar"]();
	g_argv = {"xmod_map", "de_dust2"}; svrcmds["xmod_map"]();
	printf("== say @ help (Bob) \n"); cmd(2, {"say", "@please help"}, "\"@please help\"");
	printf("== gag Bob then Bob talks\n"); cmd(1, {"xmod_gag", "Bob", "60"}, "Bob 60"); cmd(2, {"say", "hello"}, "\"hello\"");
	printf("== flood: Tester says twice quickly\n"); gv.time += 5; cmd(1, {"say", "a"}, "\"a\""); cmd(1, {"say", "b"}, "\"b\"");
	printf("== /admin menu then Kick -> pick Bob\n"); cmd(1, {"say", "/admin"}, "\"/admin\""); cmd(1, {"menuselect", "1"}, "1"); cmd(1, {"menuselect", "1"}, "1");
	printf("== vote: xmod_vote\n"); cmd(1, {"xmod_vote", "Pizza?", "yes", "no"}, "\"Pizza?\" \"yes\" \"no\""); cmd(1, {"menuselect", "1"}, "1"); cmd(2, {"menuselect", "1"}, "1"); frames(2, 1.0f);
	printf("== say nextmap / timeleft\n"); cmd(1, {"say", "nextmap"}, "\"nextmap\""); cmd(1, {"say", "timeleft"}, "\"timeleft\"");
	printf("== xmod_addadmin via server console\n"); g_argv = {"xmod_addadmin", "STEAM_0:0:777", "ce"}; svrcmds["xmod_addadmin"](); cmd(2, {"xmod_who"}, "");
	printf("== new map: timers persist\n"); pre.pfnServerDeactivate(); gv.time = 0; pre.pfnSpawn(&ed[0]); post.pfnServerActivate(nullptr, 0, 32); frames(4, 1.0f);
	printf("== detach\n"); pre.pfnServerDeactivate(); Detach(PT_ANYTIME, PNL_NULL);
	return 0;
}
