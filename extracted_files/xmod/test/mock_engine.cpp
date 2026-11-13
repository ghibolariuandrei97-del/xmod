// Smoke test: loads xmod_mm_i386.so against a fake engine and drives a few events.
#include "../include/mm_sdk.h"
#include <dlfcn.h>
#include <usercmd.h>
#include <entity_state.h>
#include <time.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <string>
#include <vector>

enginefuncs_t g_engfuncs; globalvars_t *gpGlobals;
static globalvars_t gv; static edict_t ed[1100]; static char strbase[16];
static std::vector<std::string> g_argv; static std::string g_args;
static std::map<std::string, void (*)()> svrcmds; static std::map<std::string, cvar_t *> cvars;
static std::vector<std::string> msgs; static char g_info[34][128]; static bool g_open = false; static int g_engine_errors = 0;
static std::map<std::string, int> msgids = {{"Blockme", 150}, {"Observe", 151}, {"Plain", 152}, {"SayText", 100}, {"ShowMenu", 101}, {"Money", 102}, {"DeathMsg", 103}, {"ScreenFade", 104}};
static void MM_Log(plid_t, const char *f, ...) { char b[1024]; va_list a; va_start(a, f); vsnprintf(b, 1024, f, a); va_end(a); printf("[log] %s\n", b); }
static int MM_MsgId(plid_t, const char *n, int *) { auto i = msgids.find(n); return i == msgids.end() ? 0 : i->second; }
static const char *MM_MsgName(plid_t, int id, int *) { for (auto &p : msgids) if (p.second == id) return p.first.c_str(); return nullptr; }
static mutil_funcs_t mu;
static void *fake_vt[256], *pristine_vt[256];
static int fake_takedamage(void *, entvars_t *, entvars_t *, float dmg, int bits) { printf("[orig takedamage] dmg=%.1f bits=%d\n", dmg, bits); return (int)dmg; }
static void fake_spawn(void *) { printf("[orig spawn]\n"); }
static const char *MM_GameInfo(plid_t, ginfo_t) { return ""; }
static edict_t *PEnt(int i) { return &ed[i]; }
static int IdxOf(const edict_t *e) { return (int)(e - ed); }
static void Say(const char *a0, const char *args, std::vector<std::string> v) { g_argv = v; g_args = args; (void)a0; }

int main() { setvbuf(stdout, NULL, _IONBF, 0);
	gv.maxClients = 32; gv.mapname = (int)(strdup("de_dust2") - strbase); gv.maxEntities = 1100; gv.pStringBase = strbase; gv.time = 1.0f; gpGlobals = &gv;
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
	g_engfuncs.pfnMessageBegin = [](int d, int t, const float *, edict_t *e) { if (g_open) { g_engine_errors++; printf("!!! ENGINE ERROR: MESSAGE_BEGIN while a message is still open (crash on a real server)\n"); } g_open = true; char b[64]; snprintf(b, 64, "MSG(dest=%d type=%d ent=%d)", d, t, e ? IdxOf(e) : 0); msgs.push_back(b); };
	g_engfuncs.pfnWriteByte = [](int v) { msgs.back() += " b" + std::to_string(v); };
	g_engfuncs.pfnWriteShort = [](int v) { msgs.back() += " s" + std::to_string(v); };
	g_engfuncs.pfnWriteChar = [](int v) { msgs.back() += " c" + std::to_string(v); };
	g_engfuncs.pfnWriteString = [](const char *s) { msgs.back() += std::string(" \"") + s + "\""; };
	g_engfuncs.pfnMessageEnd = []() { if (!g_open) { g_engine_errors++; printf("!!! ENGINE ERROR: MESSAGE_END without begin\n"); } g_open = false; printf("[msg] %s\n", msgs.back().c_str()); };
	g_engfuncs.pfnClientPrintf = [](edict_t *e, PRINT_TYPE t, const char *s) { printf("[print %d->%d] %s", t, IdxOf(e), s); };
	for (int i = 0; i < 1100; i++) { ed[i].pvPrivateData = calloc(1, 4096); ed[i].v.pContainingEntity = &ed[i]; *(void ***)ed[i].pvPrivateData = fake_vt; *(entvars_t **)((char *)ed[i].pvPrivateData + 4) = &ed[i].v; }
	for (int i = 0; i < 1100; i++) { ed[i].pvPrivateData = ed[i].pvPrivateData; } fake_vt[12] = (void *)fake_takedamage; fake_vt[0] = (void *)fake_spawn;
	g_engfuncs.pfnCreateNamedEntity = [](int cls) -> edict_t * { ed[1000].v.classname = cls; ed[1000].free = 0; return &ed[1000]; };
	g_engfuncs.pfnRemoveEntity = [](edict_t *e) { e->v.flags |= FL_KILLME; };
	g_engfuncs.pfnSetOrigin = [](edict_t *e, const float *o) { e->v.origin = Vector(o[0], o[1], o[2]); };
	g_engfuncs.pfnAllocString = [](const char *s) -> int { return (int)(strdup(s) - strbase); };
	g_engfuncs.pfnFindEntityByString = [](edict_t *start, const char *f, const char *v) -> edict_t * {
		for (int i = IdxOf(start) + 1; i < 1100; i++) { if (ed[i].free) continue; int sv = !strcmp(f, "classname") ? ed[i].v.classname : ed[i].v.targetname;
			if (sv && !strcmp(STRING(sv), v)) return &ed[i]; } return &ed[0]; };
	g_engfuncs.pfnFindEntityInSphere = [](edict_t *start, const float *o, float r) -> edict_t * {
		for (int i = IdxOf(start) + 1; i < 1100; i++) { if (ed[i].free) continue; Vector d = ed[i].v.origin - Vector(o[0], o[1], o[2]); if (d.x * d.x + d.y * d.y + d.z * d.z <= r * r && ed[i].v.classname) return &ed[i]; } return &ed[0]; };
	g_engfuncs.pfnTraceLine = [](const float *a, const float *b, int, edict_t *, TraceResult *tr) { memset(tr, 0, sizeof *tr); tr->flFraction = 1.0f; tr->vecEndPos = Vector(b[0], b[1], b[2]); tr->vecPlaneNormal = Vector(0, 0, 1); tr->pHit = &ed[0]; };
	g_engfuncs.pfnPointContents = [](const float *) { return -1; };
	mu.pfnGetGameInfo = MM_GameInfo;
	ed[1].v.health = 100; ed[1].v.deadflag = DEAD_NO;
	mu.pfnLogMessage = mu.pfnLogError = mu.pfnLogConsole = MM_Log; mu.pfnGetUserMsgID = MM_MsgId; mu.pfnGetUserMsgName = MM_MsgName;

	memcpy(pristine_vt, fake_vt, sizeof pristine_vt);
	void *h = dlopen("../addons/xmod/dlls/xmod_mm_i386.so", RTLD_NOW); if (!h) { printf("dlopen: %s\n", dlerror()); return 1; }
	auto Give = (void (*)(enginefuncs_t *, globalvars_t *))dlsym(h, "GiveFnptrsToDll");
	auto Query = (int (*)(char *, plugin_info_t **, mutil_funcs_t *))dlsym(h, "Meta_Query");
	auto Attach = (int (*)(PLUG_LOADTIME, META_FUNCTIONS *, meta_globals_t *, gamedll_funcs_t *))dlsym(h, "Meta_Attach");
	auto Detach = (int (*)(PLUG_LOADTIME, PL_UNLOAD_REASON))dlsym(h, "Meta_Detach");
	Give(&g_engfuncs, &gv); plugin_info_t *pi; Query((char *)"5:13", &pi, &mu);
	META_FUNCTIONS mf; meta_globals_t mg; memset(&mg, 0, sizeof mg); static DLL_FUNCTIONS gtbl; memset(&gtbl, 0, sizeof gtbl); gtbl.pfnClientKill = [](edict_t *e) { printf("[game] ClientKill(%d): game sends messages now (open=%d)\n", IdxOf(e), (int)g_open); if (g_open) { g_engine_errors++; printf("!!! ENGINE ERROR: game sent a message while one is open\n"); } };
	gamedll_funcs_t gd = {&gtbl, nullptr};
	printf("== attach\n"); if (!Attach(PT_ANYTIME, &mf, &mg, &gd)) { printf("attach failed\n"); return 1; }
	// an xmod extension is just another Metamod plugin, loaded AFTER xmod (extensions come from env EXTLIBS, space separated)
	std::vector<void *> ext_handles; std::vector<int (*)(PLUG_LOADTIME, PL_UNLOAD_REASON)> ext_detach;
	if (getenv("EXTLIBS")) { std::string libs = getenv("EXTLIBS"); size_t p = 0;
		while (p < libs.size()) { size_t sp = libs.find(' ', p); if (sp == std::string::npos) sp = libs.size(); std::string path = libs.substr(p, sp - p); p = sp + 1; if (path.empty()) continue;
			void *eh = dlopen(path.c_str(), RTLD_NOW); if (!eh) { printf("ext dlopen %s: %s\n", path.c_str(), dlerror()); return 1; }
			auto eGive = (void (*)(enginefuncs_t *, globalvars_t *))dlsym(eh, "GiveFnptrsToDll"); auto eQuery = (int (*)(char *, plugin_info_t **, mutil_funcs_t *))dlsym(eh, "Meta_Query");
			auto eAttach = (int (*)(PLUG_LOADTIME, META_FUNCTIONS *, meta_globals_t *, gamedll_funcs_t *))dlsym(eh, "Meta_Attach");
			eGive(&g_engfuncs, &gv); plugin_info_t *epi; eQuery((char *)"5:13", &epi, &mu); static META_FUNCTIONS emf; static meta_globals_t emg; memset(&emg, 0, sizeof emg); static gamedll_funcs_t egd = {&gtbl, nullptr};
			printf("== loading extension %s\n", epi->name); if (!eAttach(PT_ANYTIME, &emf, &emg, &egd)) { printf("extension attach FAILED\n"); return 1; }
			ext_handles.push_back(eh); ext_detach.push_back((int (*)(PLUG_LOADTIME, PL_UNLOAD_REASON))dlsym(eh, "Meta_Detach")); } }
	DLL_FUNCTIONS pre, post; mf.pfnGetEntityAPI2(&pre, nullptr); mf.pfnGetEntityAPI2_Post(&post, nullptr);
	printf("== xmod plugins\n"); g_argv = {"xmod", "plugins"}; svrcmds["xmod"]();
	auto cmd = [&](int id, std::vector<std::string> v, const char *args) { g_argv = v; g_args = args; pre.pfnClientCommand(&ed[id]); };
	auto frames = [&](int n, float dt) { for (int i = 0; i < n; i++) { gv.time += dt; pre.pfnStartFrame(); } };
	strcpy(g_info[1], "\\name\\Tester\\model\\urban\\_pw\\"); strcpy(g_info[2], "\\name\\Bob\\model\\urban\\_pw\\");
	printf("== map start\n"); pre.pfnSpawn(&ed[0]); post.pfnServerActivate(nullptr, 0, 32); frames(1, 0.1f);
	printf("== connect players\n"); char rej[128] = {0};
	for (int i = 1; i <= 2; i++) { pre.pfnClientConnect(&ed[i], i == 1 ? "Tester" : "Bob", "1.2.3.4", rej); post.pfnClientPutInServer(&ed[i]); ed[i].v.health = 100; }
	frames(3, 0.5f);
	if (getenv("QTEST")) {
		for (int i = 0; i < 3000; i++) { gv.time += 0.01f; pre.pfnStartFrame(); usleep(3000); }
		for (size_t i = ext_detach.size(); i > 0; i--) ext_detach[i - 1](PT_ANYTIME, PNL_NULL);
		Detach(PT_ANYTIME, PNL_NULL); return 0;
	}
	if (getenv("EXTTEST")) {
		auto svr = [&](std::vector<std::string> v) { g_argv = v; g_args = ""; svrcmds[v[0]](); };
		auto run = [&](int n) { for (int i = 0; i < n; i++) { gv.time += 0.01f; pre.pfnStartFrame(); usleep(3000); } };
		printf("== running frames (workers + callbacks)\n"); run(500);
		printf("== job started, then xmod reload: its callback must be dropped\n"); svr({"t_slow"}); svr({"xmod", "reload"}); run(400); svr({"t_check_add"});
		printf("== extension unload\n"); for (size_t i = ext_detach.size(); i > 0; i--) ext_detach[i - 1](PT_ANYTIME, PNL_NULL); run(5); svr({"t_check_unloaded"});
		Detach(PT_ANYTIME, PNL_NULL); return 0;
	}
	if (getenv("MSGTEST")) {
		auto svr = [&](std::vector<std::string> v) { g_argv = v; g_args = ""; svrcmds[v[0]](); };
		enginefuncs_t epre, epost; int ver = 0; mf.pfnGetEngineFunctions(&epre, &ver); mf.pfnGetEngineFunctions_Post(&epost, &ver);
		// emulate Metamod: call pre hook; if not superseded call the real engine; call post hook after End
		auto begin = [&](int dest, int type) { mg.mres = MRES_UNSET; epre.pfnMessageBegin(dest, type, nullptr, nullptr); if (mg.mres != MRES_SUPERCEDE) g_engfuncs.pfnMessageBegin(dest, type, nullptr, nullptr); };
		auto wbyte = [&](int v) { mg.mres = MRES_UNSET; epre.pfnWriteByte(v); if (mg.mres != MRES_SUPERCEDE) g_engfuncs.pfnWriteByte(v); };
		auto wstr = [&](const char *v) { mg.mres = MRES_UNSET; epre.pfnWriteString(v); if (mg.mres != MRES_SUPERCEDE) g_engfuncs.pfnWriteString(v); };
		auto end = [&]() { mg.mres = MRES_UNSET; epre.pfnMessageEnd(); if (mg.mres != MRES_SUPERCEDE) g_engfuncs.pfnMessageEnd(); epost.pfnMessageEnd(); };
		printf("== 1. game sends DeathMsg; handler sends HUD/chat/fade and kills a player INSIDE the handler\n");
		begin(MSG_ALL, 103); wbyte(1); wbyte(2); wbyte(1); wstr("ak47"); end();
		printf("== 2. blocked message (handler returns true): must NOT reach the engine, and must not leave it open\n");
		begin(MSG_ALL, 150); wbyte(7); end(); printf("(engine open flag after blocked message: %d)\n", (int)g_open);
		printf("== 3. observed message passes through unchanged\n");
		begin(MSG_ALL, 151); wbyte(4); wbyte(5); end();
		printf("== 4. unhooked message passes straight through\n");
		begin(MSG_ALL, 152); wbyte(9); end();
		printf("== 5. Lua runs while a pass-through message is OPEN: sends must be deferred, then flushed after it ends\n");
		begin(MSG_ALL, 152); wbyte(1); svr({"t_chat"}); printf("(chat queued, nothing sent yet; open=%d)\n", (int)g_open); wbyte(2); end();
		printf("== 6. script error between begin() and finish(): nothing may reach the engine\n");
		svr({"t_builder_err"}); printf("(open=%d)\n", (int)g_open); svr({"t_builder_ok"});
		printf("== 7. ReGameDLL-style bot: CreateFakeClient (engine post hook) then first think\n");
		edict_t *bot = &ed[5]; mg.orig_ret = &bot; bot->v.flags = FL_FAKECLIENT | FL_CLIENT; strcpy(g_info[5], "\\name\\Bot Ana\\model\\urban\\_pw\\");
		mg.mres = MRES_UNSET; epost.pfnCreateFakeClient("Bot Ana");
		{ static void *bot_vt[256]; memcpy(bot_vt, pristine_vt, sizeof bot_vt); *(void ***)bot->pvPrivateData = bot_vt;   // bots (CCSBot) have their own vtable
		  pre.pfnPlayerPreThink(bot); pre.pfnPlayerPreThink(bot);
		  typedef int (*TD)(void *, entvars_t *, entvars_t *, float, int);
		  printf("-- ham takedamage on the BOT's own vtable (hook must fire):\n"); ((TD)bot_vt[12])(bot->pvPrivateData, &ed[1].v, &ed[1].v, 25.f, 0);
		  printf("-- ham takedamage on a human (hook must fire):\n"); ((TD)fake_vt[12])(ed[1].pvPrivateData, &ed[2].v, &ed[2].v, 25.f, 0); }
		svr({"xmod_who"});
		pre.pfnClientDisconnect(bot);
		printf("== 8. client edict nobody registered (e.g. xmod loaded mid-game) is picked up lazily\n");
		edict_t *late = &ed[6]; late->v.flags = FL_CLIENT; strcpy(g_info[6], "\\name\\Latecomer\\_pw\\"); pre.pfnPlayerPreThink(late);
		printf("== engine errors: %d\n", g_engine_errors);
		Detach(PT_ANYTIME, PNL_NULL); return g_engine_errors ? 2 : 0;
	}
	if (getenv("DBG")) {
		auto svr = [&](std::vector<std::string> v) { g_argv = v; std::string a; for (size_t i = 1; i < v.size(); i++) a += (i > 1 ? " " : "") + v[i]; g_args = a; svrcmds[v[0]](); };
		auto run_frames = [&](int n, float dt) { for (int i = 0; i < n; i++) { for (int k = 0; k < 1100; k++) if (ed[k].v.flags & FL_KILLME) { ed[k].free = 1; } gv.time += dt; pre.pfnStartFrame(); } };
		printf("== xmod_dbg help / status (server console)\n"); svr({"xmod_dbg", "help"}); svr({"xmod_dbg", "status"});
		printf("== chat / commands / messages for the log\n"); cmd(2, {"say", "hello world"}, "\"hello world\""); cmd(1, {"weapon_ak47"}, "");
		{ pre.pfnSpawn(&ed[0]); }
		printf("== self test as client 1 (with watchdog test)\n"); cmd(1, {"xmod_dbg", "test", "timeout"}, "test timeout"); run_frames(60, 0.05f);
		cmd(1, {"menuselect", "1"}, "1");
		printf("== micro benchmark\n"); svr({"xmod_dbg", "bench"}); run_frames(60, 0.06f);
		printf("== AddToFullPack timing through the real metamod-style hook table\n");
		if (!pre.pfnAddToFullPack) printf("pfnAddToFullPack NOT installed\n");
		else {
			entity_state_s st; memset(&st, 0, sizeof st); int retv = 1; mg.orig_ret = &retv; unsigned char pset[16];
			for (int pass = 0; pass < 2; pass++) { svr({"xmod_dbg", "stats", pass == 0 ? "on" : "off"}); printf("--- per-event timing (xmod_dbg stats): %s\n", pass == 0 ? "ON" : "OFF");
			for (int mode = 0; mode <= 4; mode++) {
				svr({"xmod_dbg", "mode", std::to_string(mode)}); if (mode == 4) run_frames(25, 0.05f); svr({"xmod_dbg", "stats", "reset"});
				const int N = 400000; struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0); long hidden = 0;
				for (int i = 0; i < N; i++) { int e = 1 + (i % 900), host = 1 + (i & 1); int r = pre.pfnAddToFullPack(&st, e, &ed[e], &ed[host], 0, e <= 32, pset);
					if (mg.mres == MRES_SUPERCEDE) { hidden++; continue; } post.pfnAddToFullPack(&st, e, &ed[e], &ed[host], 0, e <= 32, pset); (void)r; }
				clock_gettime(CLOCK_MONOTONIC, &t1); double ns = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / N;
				printf("mode %d: %.0f ns per entity-visibility check (pre+post), hidden=%ld\n", mode, ns, hidden);
				if (pass == 0) svr({"xmod_dbg", "report"});
			} }
		}
		printf("== done\n"); pre.pfnServerDeactivate(); Detach(PT_ANYTIME, PNL_NULL); return 0;
	}
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
