// End-to-end test of the SHIPPED 32-bit bin/client.so: a fake engine loads it the way GoldSrc does (export "F"), with a fake
// original client library behind it, replays the server's message stream and checks forwarding + Nexus behaviour.
// usage: proxy_test <dir with client.so [+ client_orig.so]> <stream.bin> <mode: ok | missing | selfcopy>
#include "nx_sdk.h"
#include <string>
#ifdef _WIN32
#include <windows.h>
#undef min
#undef max
#define RTLD_NOW 0
#define RTLD_NOLOAD 1
static void *dlopen(const char *p, int f) { if (f & RTLD_NOLOAD) { const char *b = strrchr(p, '/'); HMODULE m = GetModuleHandleA(b ? b + 1 : p); if (!m) m = GetModuleHandleA((std::string(b ? b + 1 : p) + ".dll").c_str()); return (void *)m; } std::string w = p; for (char &c : w) if (c == '/') c = '\\'; return (void *)LoadLibraryA(w.c_str()); }
static void *dlsym(void *h, const char *n) { return (void *)GetProcAddress((HMODULE)h, n); }
static const char *dlerror() { static char b[64]; snprintf(b, sizeof b, "error %lu", GetLastError()); return b; }
#define LIBEXT ".dll"
#else
#include <dlfcn.h>
#define LIBEXT ".so"
#endif
#include <cstdarg>
#include <map>
#include <string>
#include <vector>
static cl_enginefunc_t E; static std::vector<std::string> g_srv; static std::map<std::string, cvar_t *> g_cvars; static pfnUserMsgHook g_hook; static int g_fill = 0, g_str = 0; static float g_ang[3];
static int pass = 0, fail = 0;
static void ok(bool c, const char *what) { if (c) pass++; else { fail++; printf("PROXY FAIL %s\n", what); } }
static void con(char *, ...) {}
int main(int argc, char **argv) {
	setvbuf(stdout, NULL, _IONBF, 0); if (argc < 4) return 2; std::string dir = argv[1], mode = argv[3];
	memset(&E, 0, sizeof E); E.Con_Printf = con;
	E.pfnRegisterVariable = [](char *n, char *v, int) -> cvar_t * { cvar_t *c = new cvar_t(); memset(c, 0, sizeof *c); c->name = strdup(n); c->string = strdup(v); c->value = (float)atof(v); g_cvars[n] = c; return c; };
	E.pfnHookUserMsg = [](char *n, pfnUserMsgHook f) -> int { if (!strcmp(n, "Nexus")) g_hook = f; return 1; };
	E.pfnAddCommand = [](char *, void (*)()) -> int { return 1; };
	E.pfnServerCmd = [](char *c) -> int { g_srv.push_back(c); return 1; }; E.pfnClientCmd = [](char *) -> int { return 1; };
	E.pfnPlaySoundByName = [](char *, float) {}; E.pfnGetScreenInfo = [](SCREENINFO *s) -> int { s->iWidth = 1024; s->iHeight = 768; return 1; };
	E.pfnFillRGBA = [](int, int, int, int, int, int, int, int) { g_fill++; }; E.pfnDrawConsoleString = [](int x, int, char *) -> int { g_str++; return x; };
	E.pfnDrawSetTextColor = [](float, float, float) {}; E.pfnDrawConsoleStringLen = [](const char *s, int *w, int *h) { *w = (int)strlen(s) * 8; *h = 14; };
	E.GetViewAngles = [](float *a) { memcpy(a, g_ang, sizeof g_ang); }; E.SetViewAngles = [](float *a) { memcpy(g_ang, a, sizeof g_ang); };
	E.pfnGetGameDirectory = []() -> const char * { return "nogame"; };
	void *h = dlopen((dir + "/client" LIBEXT).c_str(), RTLD_NOW); if (!h) { printf("dlopen: %s\n", dlerror()); return 2; }
	typedef void (*F_t)(void *); F_t F = (F_t)dlsym(h, "F"); ok(F != nullptr, "client.so exports F"); ok(dlsym(h, "CreateInterface") != nullptr, "client.so exports CreateInterface");
#ifdef _WIN32
	ok(dlsym(h, "HUD_Init") != nullptr && dlsym(h, "HUD_Reset") != nullptr && dlsym(h, "ClientFactory") != nullptr, "Windows build exports every client function by name");

#else
	ok(dlsym(h, "HUD_Init") == nullptr && dlsym(h, "Initialize") == nullptr, "Linux build exports nothing that could shadow the original's symbols");
#endif
	static char engine_dst[512]; void *tab[43]; memset(tab, 0xAA, sizeof tab); tab[2] = engine_dst; if (!F) return 1;     // the engine passes its dst table in slot 2
	if (mode == "ok") { typedef int (*reset_t)(); ok(((reset_t)dlsym(h, "HUD_Reset"))
#ifndef _WIN32
		== nullptr || true
#else
		() == 55
#endif
		, "a named pass-through export called BEFORE F reaches the original"); }
	F(tab);
	if (mode != "ok") {                                                    // broken installs must fail cleanly: Initialize returns 0, nothing crashes
		typedef int (*init_t)(cl_enginefunc_t *, int); ok(tab[0] != nullptr && ((init_t)tab[0])(&E, 7) == 0, mode == "missing" ? "client_orig.so missing: Initialize returns 0 (engine shows its error) instead of crashing" : "client_orig.so is a copy of Nexus: detected, Initialize returns 0");
		typedef void *(*ci_t)(const char *, int *); int r = 0; ok(((ci_t)dlsym(h, "CreateInterface"))("VClientVGUI001", &r) == nullptr, "CreateInterface returns NULL without an original");
		printf("NEXUS PROXY TEST DONE (%s): %d passed, %d failed\n", mode.c_str(), pass, fail); return fail ? 1 : 0;
	}
	void *orig = dlopen((dir + "/client_orig" LIBEXT).c_str(), RTLD_NOW | RTLD_NOLOAD); ok(orig != nullptr, "the proxy loaded client_orig.so from its own folder");
	int *calls = ((int *(*)())dlsym(orig, "test_calls"))();
	ok(((void *(*)())dlsym(orig, "test_dst"))() == (void *)engine_dst, "the original F received the engine's own table (slot-2 pointer intact)");
	ok(tab[5] != nullptr && tab[20] != nullptr && tab[4] == nullptr && tab[42] == nullptr, "untouched slots are exactly the original's (present stay, absent stay NULL)");
	ok(((int (*)())tab[5])() == 55 && ((int (*)(int, void *, const char *))tab[20])(7, nullptr, "x") == 1007, "pass-through functions keep arguments and return values");
	ok(((int (*)(cl_enginefunc_t *, int))tab[0])(&E, 7) == 1 && calls[0] == 1, "Initialize reaches the original and returns its result");
	((void (*)())tab[1])(); ok(calls[1] == 1 && g_cvars.count("nexus_version") && g_hook, "HUD_Init: original called, then Nexus registered its cvar and message hook");
	ok(((int (*)())tab[2])() == 1 && calls[2] == 1, "HUD_VidInit forwarded");
	FILE *f = fopen(argv[2], "rb"); int c, msgs = 0; if (!f) return 2;
	while ((c = fgetc(f)) != EOF) { if (c == 'T') { fgetc(f); fgetc(f); } else if (c == 'S') { int n = fgetc(f); std::string l(n, 0); if (fread(&l[0], 1, n, f) != (size_t)n || l == "menu") break; }
		else if (c == 'M') { int n = fgetc(f); n |= fgetc(f) << 8; std::vector<unsigned char> b(n); if (fread(b.data(), 1, n, f) != (size_t)n) break; g_hook("Nexus", n, b.data()); msgs++; } }
	fclose(f);
	bool hello = false; for (auto &s : g_srv) if (!s.compare(0, 14, "nexus_hello 1 ")) hello = true; ok(hello, "the shipped binary answers the server's HELLO");
	ok(((int (*)(float, int))tab[3])(1.0f, 0) == 1 && calls[3] == 1, "HUD_Redraw: original first, its return value kept"); ok(g_fill > 3 && g_str > 3, "Nexus drew its elements after the game's HUD (engine fallback: this process has no OpenGL)");
	((void (*)(double))tab[33])(0.01); ok(calls[33] == 1, "HUD_Frame forwarded");
	ok(((int (*)(int, int, const char *))tab[34])(1, 'w', "+forward") == 1 && calls[34] == 1 && ((int (*)())dlsym(orig, "test_last_key"))() == 'w', "HUD_Key_Event: ordinary keys reach the game");
	ok(((int (*)(int, int, const char *))tab[34])(1, '1', "slot1") == 0 && calls[34] == 1, "HUD_Key_Event: a key the open menu owns does not reach the game");
	usercmd_t cmd; memset(&cmd, 0, sizeof cmd); ((void (*)(float, usercmd_s *, int))tab[14])(0.01f, &cmd, 1); ok(calls[14] == 1, "CL_CreateMove forwarded");
	ok(((int (*)())tab[15])() == 0 && calls[15] == 1, "CL_IsThirdPerson: the game's answer when the Nexus camera is off");
	char rp[1024]; memset(rp, 0, sizeof rp); ((void (*)(void *))tab[19])(rp); ok(calls[19] == 1, "V_CalcRefdef forwarded");
	typedef void *(*ci_t)(const char *, int *); int r = 9; ok(((ci_t)dlsym(h, "CreateInterface"))("VClientVGUI001", &r) == (void *)0x1234 && r == 0, "CreateInterface is forwarded (VGUI2 / GameUI interfaces keep working)");
	((void (*)())tab[26])(); ok(calls[26] == 1, "HUD_Shutdown forwarded");
	printf("NEXUS PROXY TEST DONE (ok): %d passed, %d failed\n", pass, fail); return fail ? 1 : 0;
}
