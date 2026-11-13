// Test host: a fake HLDS + Metamod that loads xmod and the Nexus extension, plays a scripted session and
//   - checks the server side behaviour (handshake, limits, pacing, events),
//   - writes every "Nexus" message to a stream file that the client tests replay (test/client).
// usage: host <xmod_mm_i386.so> <nexus_mm_i386.so> <stream.bin>      (cwd must contain gd/addons/xmod/...)
#include "mm_sdk.h"
#include <dlfcn.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

enginefuncs_t g_engfuncs; globalvars_t *gpGlobals;
static globalvars_t gv; static edict_t ed[1100]; static char strbase[16];
static std::vector<std::string> g_argv; static std::string g_args;
static std::map<std::string, void (*)()> svrcmds; static std::map<std::string, cvar_t *> cvars; static char g_info[34][128];
static std::map<std::string, int> msgids = {{"SayText", 100}, {"ShowMenu", 101}, {"TextMsg", 102}, {"HudText", 103}};
static int g_nexus_msg = 0, g_cur = -1, g_cur_ent = 0; static std::string g_buf;
struct Msg { int ent; std::string data; float time; };
static std::vector<Msg> g_out; static std::vector<std::pair<int, int>> g_queries;      // (ent, request id)
static FILE *g_stream; static float g_stream_time = 0; static int pass = 0, fail = 0; static size_t g_max_len = 0; static int g_to_others = 0, g_showmenu = 0;
static void ok(bool c, const char *what) { if (c) pass++; else { fail++; printf("HOST FAIL %s\n", what); } }
static void MM_Log(plid_t, const char *f, ...) { char b[2048]; va_list a; va_start(a, f); vsnprintf(b, sizeof b, f, a); va_end(a); printf("[log] %s\n", b); }
static int MM_MsgId(plid_t, const char *n, int *) { auto i = msgids.find(n); return i == msgids.end() ? 0 : i->second; }
static const char *MM_MsgName(plid_t, int id, int *) { for (auto &p : msgids) if (p.second == id) return p.first.c_str(); return nullptr; }
static const char *MM_GameInfo(plid_t, ginfo_t) { return ""; }
static mutil_funcs_t mu; static void *fake_vt[256];
static edict_t *PEnt(int i) { return &ed[i]; }
static int IdxOf(const edict_t *e) { return (int)(e - ed); }
static void stream_time() { int ms = (int)((gv.time - g_stream_time) * 1000 + 0.5f); g_stream_time = gv.time; while (ms > 0) { int n = ms > 60000 ? 60000 : ms; fputc('T', g_stream); fputc(n & 255, g_stream); fputc(n >> 8, g_stream); ms -= n; } }
static void stream_snap(const char *label) { stream_time(); fputc('S', g_stream); fputc((int)strlen(label), g_stream); fputs(label, g_stream); }

struct Plug { void *h; META_FUNCTIONS mf; DLL_FUNCTIONS pre, post; NEW_DLL_FUNCTIONS nw; meta_globals_t mg; int (*detach)(PLUG_LOADTIME, PL_UNLOAD_REASON); };
static DLL_FUNCTIONS gtbl; static gamedll_funcs_t gd = {&gtbl, nullptr};
static bool load(Plug &p, const char *path) {
	memset(&p.mf, 0, sizeof p.mf); memset(&p.pre, 0, sizeof p.pre); memset(&p.post, 0, sizeof p.post); memset(&p.nw, 0, sizeof p.nw); memset(&p.mg, 0, sizeof p.mg);
	p.h = dlopen(path, RTLD_NOW); if (!p.h) { printf("dlopen %s: %s\n", path, dlerror()); return false; }
	auto Give = (void (*)(enginefuncs_t *, globalvars_t *))dlsym(p.h, "GiveFnptrsToDll"); auto Query = (int (*)(char *, plugin_info_t **, mutil_funcs_t *))dlsym(p.h, "Meta_Query");
	auto Attach = (int (*)(PLUG_LOADTIME, META_FUNCTIONS *, meta_globals_t *, gamedll_funcs_t *))dlsym(p.h, "Meta_Attach"); p.detach = (int (*)(PLUG_LOADTIME, PL_UNLOAD_REASON))dlsym(p.h, "Meta_Detach");
	Give(&g_engfuncs, &gv); plugin_info_t *pi; Query((char *)"5:13", &pi, &mu);
	if (!Attach(PT_ANYTIME, &p.mf, &p.mg, &gd)) { printf("attach of %s failed\n", path); return false; }
	if (p.mf.pfnGetEntityAPI2) p.mf.pfnGetEntityAPI2(&p.pre, nullptr); if (p.mf.pfnGetEntityAPI2_Post) p.mf.pfnGetEntityAPI2_Post(&p.post, nullptr); if (p.mf.pfnGetNewDLLFunctions) p.mf.pfnGetNewDLLFunctions(&p.nw, nullptr);
	return true;
}
static Plug X, NX; static int g_token[33]; static bool g_auto_hello = true; static int g_hello_seen = 0;
static void frame(float dt = 0.05f) { gv.time += dt; if (X.pre.pfnStartFrame) X.pre.pfnStartFrame(); if (NX.pre.pfnStartFrame) NX.pre.pfnStartFrame(); }
static void client_cmd(int id, std::vector<std::string> v) {
	g_argv = v; g_args.clear(); for (size_t i = 1; i < v.size(); i++) { if (i > 1) g_args += " "; g_args += v[i]; }
	if (X.pre.pfnClientCommand) X.pre.pfnClientCommand(&ed[id]); if (NX.pre.pfnClientCommand) NX.pre.pfnClientCommand(&ed[id]);
}
// what a real client with / without the Nexus library does with the server's traffic
static void pump() {
	auto q = g_queries; g_queries.clear();
	for (auto &r : q) if (NX.nw.pfnCvarValue2) NX.nw.pfnCvarValue2(&ed[r.first], r.second, "nexus_version", r.first == 1 ? "1.1.0" : "Bad CVAR request");
	for (auto &m : g_out) {
		if (m.data.size() > g_max_len) g_max_len = m.data.size(); if (m.ent != 1) g_to_others++;
		if (m.data.size() == 3 && m.data[0] == 1) { g_hello_seen++; g_token[m.ent] = (unsigned char)m.data[1] | ((unsigned char)m.data[2] << 8);
			if (g_auto_hello) client_cmd(m.ent, {"nexus_hello", "1", "1.1.0", "1280", "720", "127", std::to_string(g_token[m.ent])}); }
	}
	g_out.clear();
}
static void run(float seconds) { for (float t = 0; t < seconds; t += 0.05f) { frame(); pump(); } }
static void step(const char *name, float seconds = 0.5f, bool snap = false) { client_cmd(1, {"nxt", name}); run(seconds); if (snap) stream_snap(name); }

int main(int argc, char **argv) {
	setvbuf(stdout, NULL, _IONBF, 0); if (argc < 4) { printf("usage: host xmod.so nexus.so stream.bin\n"); return 2; }
	g_stream = fopen(argv[3], "wb"); if (!g_stream) return 2;
	gv.maxClients = 32; gv.mapname = (int)(strdup("de_dust2") - strbase); gv.maxEntities = 1100; gv.pStringBase = strbase; gv.time = 1.0f; gpGlobals = &gv; g_stream_time = gv.time;
	memset(&g_engfuncs, 0, sizeof g_engfuncs);
	g_engfuncs.pfnGetGameDir = [](char *d) { strcpy(d, "gd"); };
	g_engfuncs.pfnServerPrint = [](const char *s) { printf("[srv] %s", s); };
	g_engfuncs.pfnAddServerCommand = [](char *n, void (*f)()) { svrcmds[n] = f; };
	g_engfuncs.pfnCVarGetPointer = [](const char *n) -> cvar_t * { auto i = cvars.find(n); return i == cvars.end() ? nullptr : i->second; };
	g_engfuncs.pfnCVarRegister = [](cvar_t *c) { cvars[c->name] = c; c->value = (float)atof(c->string); };
	g_engfuncs.pfnCVarGetString = [](const char *n) -> const char * { auto i = cvars.find(n); return i == cvars.end() ? "" : i->second->string; };
	g_engfuncs.pfnCVarGetFloat = [](const char *n) -> float { auto i = cvars.find(n); return i == cvars.end() ? 0.f : (float)atof(i->second->string); };
	g_engfuncs.pfnCVarSetString = [](const char *n, const char *v) { auto i = cvars.find(n); if (i != cvars.end()) i->second->string = strdup(v); };
	g_engfuncs.pfnCmd_Argc = []() { return (int)g_argv.size(); };
	g_engfuncs.pfnCmd_Argv = [](int i) -> const char * { return i < (int)g_argv.size() ? g_argv[i].c_str() : ""; };
	g_engfuncs.pfnCmd_Args = []() -> const char * { return g_args.c_str(); };
	g_engfuncs.pfnPEntityOfEntIndex = PEnt; g_engfuncs.pfnIndexOfEdict = IdxOf;
	g_engfuncs.pfnGetPlayerUserId = [](edict_t *e) { return 100 + IdxOf(e); };
	g_engfuncs.pfnGetPlayerAuthId = [](edict_t *e) -> const char * { return IdxOf(e) == 1 ? "STEAM_0:1:1234" : "STEAM_0:0:777"; };
	g_engfuncs.pfnGetInfoKeyBuffer = [](edict_t *e) -> char * { return g_info[IdxOf(e)]; };
	g_engfuncs.pfnInfoKeyValue = [](char *buf, char *k) -> char * { static char out[128]; out[0] = 0; std::string b = buf, key = std::string("\\") + k + "\\"; size_t p = b.find(key);
		if (p != std::string::npos) { size_t s = p + key.size(), e = b.find('\\', s); strncpy(out, b.substr(s, e == std::string::npos ? e : e - s).c_str(), 127); } return out; };
	g_engfuncs.pfnServerCommand = [](char *) {}; g_engfuncs.pfnServerExecute = []() {};
	g_engfuncs.pfnSetClientKeyValue = [](int, char *, char *, char *) {};
	g_engfuncs.pfnIsMapValid = [](char *) { return 1; }; g_engfuncs.pfnIsDedicatedServer = []() { return 1; };
	g_engfuncs.pfnGetPlayerStats = [](const edict_t *, int *p, int *l) { *p = 42; *l = 0; };
	g_engfuncs.pfnRegUserMsg = [](const char *n, int) -> int { auto i = msgids.find(n); if (i != msgids.end()) return i->second; int id = 180 + (int)msgids.size(); msgids[n] = id; if (!strcmp(n, "Nexus")) g_nexus_msg = id; return id; };
	g_engfuncs.pfnQueryClientCvarValue2 = [](const edict_t *e, const char *, int req) { g_queries.emplace_back(IdxOf(e), req); };
	g_engfuncs.pfnMessageBegin = [](int, int t, const float *, edict_t *e) { g_cur = t; g_cur_ent = e ? IdxOf(e) : 0; g_buf.clear(); if (t == 101) g_showmenu++; };
	g_engfuncs.pfnWriteByte = [](int v) { g_buf += (char)v; }; g_engfuncs.pfnWriteChar = [](int v) { g_buf += (char)v; };
	g_engfuncs.pfnWriteShort = [](int v) { g_buf += (char)v; g_buf += (char)(v >> 8); }; g_engfuncs.pfnWriteLong = [](int) {}; g_engfuncs.pfnWriteAngle = [](float) {}; g_engfuncs.pfnWriteCoord = [](float) {};
	g_engfuncs.pfnWriteString = [](const char *s) { g_buf += s; g_buf += '\0'; }; g_engfuncs.pfnWriteEntity = [](int) {};
	g_engfuncs.pfnMessageEnd = []() {
		if (g_cur == g_nexus_msg && g_nexus_msg) { g_out.push_back({g_cur_ent, g_buf, gv.time});
			if (g_cur_ent == 1) { stream_time(); fputc('M', g_stream); fputc((int)(g_buf.size() & 255), g_stream); fputc((int)(g_buf.size() >> 8), g_stream); fwrite(g_buf.data(), 1, g_buf.size(), g_stream); } }
		g_cur = -1; };
	g_engfuncs.pfnClientPrintf = [](edict_t *, PRINT_TYPE, const char *) {};
	for (int i = 0; i < 1100; i++) { ed[i].pvPrivateData = calloc(1, 4096); ed[i].v.pContainingEntity = &ed[i]; *(void ***)ed[i].pvPrivateData = fake_vt; *(entvars_t **)((char *)ed[i].pvPrivateData + 4) = &ed[i].v; }
	g_engfuncs.pfnAllocString = [](const char *s) -> int { return (int)(strdup(s) - strbase); };
	g_engfuncs.pfnFindEntityByString = [](edict_t *, const char *, const char *) -> edict_t * { return &ed[0]; };
	g_engfuncs.pfnFindEntityInSphere = [](edict_t *, const float *, float) -> edict_t * { return &ed[0]; };
	g_engfuncs.pfnTraceLine = [](const float *, const float *b, int, edict_t *, TraceResult *tr) { memset(tr, 0, sizeof *tr); tr->flFraction = 1.0f; tr->vecEndPos = Vector(b[0], b[1], b[2]); tr->pHit = &ed[0]; };
	g_engfuncs.pfnPointContents = [](const float *) { return -1; };
	g_engfuncs.pfnRemoveEntity = [](edict_t *e) { e->v.flags |= FL_KILLME; }; g_engfuncs.pfnSetOrigin = [](edict_t *e, const float *o) { e->v.origin = Vector(o[0], o[1], o[2]); };
	mu.pfnGetGameInfo = MM_GameInfo; mu.pfnLogMessage = mu.pfnLogError = mu.pfnLogConsole = MM_Log; mu.pfnGetUserMsgID = MM_MsgId; mu.pfnGetUserMsgName = MM_MsgName;
	memset(&gtbl, 0, sizeof gtbl);

	printf("== load xmod, then nexus\n"); if (!load(X, argv[1]) || !load(NX, argv[2])) return 1;
	ok(g_nexus_msg != 0, "the extension registered the Nexus user message"); ok(svrcmds.count("nexus") == 1, "server command nexus exists");
	strcpy(g_info[1], "\\name\\Tester\\model\\urban"); strcpy(g_info[2], "\\name\\Bob\\model\\urban"); strcpy(g_info[3], "\\name\\Bot\\model\\urban");
	auto map_start = [&]() { if (X.pre.pfnSpawn) X.pre.pfnSpawn(&ed[0]); if (X.post.pfnServerActivate) X.post.pfnServerActivate(nullptr, 0, 32); if (NX.post.pfnServerActivate) NX.post.pfnServerActivate(nullptr, 0, 32); frame(0.1f); };
	auto connect = [&](int i, bool bot) { char rej[128] = {0}; if (X.pre.pfnClientConnect) X.pre.pfnClientConnect(&ed[i], g_info[i] + 6, "1.2.3.4", rej); ed[i].v.flags = FL_CLIENT | (bot ? FL_FAKECLIENT : 0); ed[i].v.health = 100; if (X.post.pfnClientPutInServer) X.post.pfnClientPutInServer(&ed[i]); };
	map_start(); connect(1, false); connect(2, false); connect(3, true);

	printf("== handshake\n");
	g_auto_hello = false; run(3.0f);
	ok(g_hello_seen >= 1, "player 1 (has the nexus_version cvar) got a HELLO");
	client_cmd(1, {"nexus_hello", "1", "1.1.0", "1280", "720", "127", std::to_string(g_token[1] + 1)});      // wrong token
	client_cmd(2, {"nexus_hello", "1", "1.1.0", "1280", "720", "127", "1"});                                    // a player that was never greeted
	client_cmd(1, {"nexus_key", "102", "1"});                                                                    // not ready yet
	run(0.2f);
	g_auto_hello = true; client_cmd(1, {"nexus_hello", "1", "1.1.0", "1280", "720", "127", std::to_string(g_token[1])}); run(0.5f);
	if (const char *ex = getenv("EXTRA_CMDS")) {                 // ad-hoc: "cmd a b|cmd c" run as player 1 (used to debug user plugins)
		std::string all = ex; size_t p = 0;
		while (p <= all.size()) { size_t e = all.find('|', p); if (e == std::string::npos) e = all.size(); std::string c = all.substr(p, e - p); p = e + 1;
			std::vector<std::string> v; size_t q = 0; while (q < c.size()) { size_t sp = c.find(' ', q); if (sp == std::string::npos) sp = c.size(); if (sp > q) v.push_back(c.substr(q, sp - q)); q = sp + 1; }
			if (!v.empty()) { printf("== player 1: %s\n", c.c_str()); client_cmd(1, v); } run(1.5f); static int k = 0; stream_snap(("extra" + std::to_string(++k)).c_str()); }
		g_argv = {"xmod", "plugins"}; if (svrcmds.count("xmod")) svrcmds["xmod"]();
		printf("EXTRA DONE\n"); return 0;
	}
	step("state");
	printf("== features\n");
	step("hud", 3.0f, true);
	step("menu", 1.5f, true);
	client_cmd(1, {"nexus_key", "51", "1"}); client_cmd(1, {"nexus_key", "50", "1"}); run(0.3f);        // 3 = disabled item, 2 = armor
	step("menu_check", 1.0f); client_cmd(1, {"nexus_key", "48", "1"}); run(0.5f); step("menu_check2", 0.5f);
	step("world", 1.5f, true);
	step("anim", 0.5f, true); run(1.5f); stream_snap("anim_end");
	step("input", 1.0f, true);
	client_cmd(1, {"nexus_key", "102", "1"}); client_cmd(1, {"nexus_key", "102", "0"}); client_cmd(1, {"nexus_click", "777", "1"}); client_cmd(1, {"nexus_click", "778", "1"});
	client_cmd(1, {"nexus_data", "test.chan", "6869007468657265"}); client_cmd(1, {"nexus_data", "test.chan", "zz"}); client_cmd(1, {"nexus_data", "bad chan!", "00"}); client_cmd(2, {"nexus_data", "test.chan", "00"});
	run(0.3f); step("input_check", 1.0f);
	printf("== pacing\n");
	{ size_t before = ftell(g_stream); float t0 = gv.time; client_cmd(1, {"nxt", "flood"}); int frames = 0; unsigned long long peak = 0;
	  for (int i = 0; i < 400; i++) { size_t b = ftell(g_stream); frame(); pump(); size_t d = ftell(g_stream) - b; if (d > peak) peak = d; if (d) frames = i + 1; }
	  size_t total = ftell(g_stream) - before; float secs = frames * 0.05f; (void)t0; printf("   flood: %zu bytes in %.1f s (%.0f B/s), largest single frame %llu B\n", total, secs, total / secs, peak);
	  ok(total > 4000, "the flood was delivered"); ok(total / secs <= 5000 * 1.25f, "average rate stays near nexus_rate (5000 B/s)"); ok(peak <= 1400, "no frame sends more than the burst limit (reliable channel safety)"); }
	stream_snap("flood"); step("flood_check", 0.5f);
	step("misc", 1.0f, true);
	step("url", 1.0f, true); client_cmd(1, {"nexus_url", "1", "1"}); run(0.2f); step("url_check", 0.3f);
	printf("== command flood\n");
	for (int i = 0; i < 300; i++) client_cmd(1, {"nexus_key", "102", "1"});
	run(0.2f); step("cmdflood_check", 1.2f);
	step("clear", 1.0f, true);
	g_argv = {"nexus"}; svrcmds["nexus"]();
	ok(g_max_len <= 190, "no Nexus message is longer than 190 bytes"); ok(g_to_others == 0, "nothing was ever sent to players without the Nexus client");
	printf("   longest message %zu bytes\n", g_max_len);
	printf("== disconnect, reconnect, map change\n");
	if (X.pre.pfnClientDisconnect) X.pre.pfnClientDisconnect(&ed[1]); if (NX.pre.pfnClientDisconnect) NX.pre.pfnClientDisconnect(&ed[1]); ed[1].v.flags = 0; run(0.5f);
	client_cmd(2, {"nxt", "lost"}); run(0.2f);
	map_start(); connect(1, false); run(4.0f); client_cmd(1, {"nxt", "remap"}); run(0.3f);
	// classic fallback: player 2 has no Nexus -> N.menu must use the ShowMenu message
	client_cmd(2, {"nxt", "fallback"}); run(0.3f); ok(g_showmenu > 0, "the classic menu was sent as a ShowMenu message");
	client_cmd(2, {"nxt", "done"}); run(0.2f);
	stream_time(); fflush(g_stream);
	if (NX.detach) NX.detach(PT_ANYTIME, PNL_COMMAND); if (X.detach) X.detach(PT_ANYTIME, PNL_COMMAND);
	fclose(g_stream);
	printf("NEXUS HOST TEST DONE: %d passed, %d failed\n", pass, fail);
	return fail ? 1 : 0;
}
