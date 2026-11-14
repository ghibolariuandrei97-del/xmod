// Nexus client.dll / client.so: a transparent proxy in front of the game's own client library.
//   install: rename the game's cl_dlls/client.dll to client_orig.dll and put this file in its place (Linux: client.so -> client_orig.so).
// The engine talks to this library; every call is passed to the original one, and ten of them are also seen by Nexus (see HOOKED below).
// Only the public SDK interface is used: no byte patches, no signature scanning, nothing is read from or written to engine memory.
#include "nx_sdk.h"
#include "nx_client.h"
#include "nx_platform.h"
#include "../shared/nexus_proto.h"
#include <string>

#ifdef _WIN32
#define NX_EXPORT extern "C" __declspec(dllexport)
#define ORIG_NAME "client_orig.dll"
#define ORIG_EXT ".dll"
#define LIB_NAME "client.dll"
#define NX_EXPORT_DATA __declspec(dllexport)
#else
#define NX_EXPORT extern "C" __attribute__((visibility("default")))
#define ORIG_NAME "client_orig.so"
#define ORIG_EXT ".so"
#define LIB_NAME "client.so"
#define NX_EXPORT_DATA __attribute__((visibility("default")))
#endif

enum { N_SLOTS = 43 };                                                    // cldll_func_t of the HL SDK, in this order
static const char *const NAMES[N_SLOTS] = {"Initialize", "HUD_Init", "HUD_VidInit", "HUD_Redraw", "HUD_UpdateClientData", "HUD_Reset", "HUD_PlayerMove", "HUD_PlayerMoveInit", "HUD_PlayerMoveTexture",
	"IN_ActivateMouse", "IN_DeactivateMouse", "IN_MouseEvent", "IN_ClearStates", "IN_Accumulate", "CL_CreateMove", "CL_IsThirdPerson", "CL_CameraOffset", "KB_Find", "CAM_Think", "V_CalcRefdef",
	"HUD_AddEntity", "HUD_CreateEntities", "HUD_DrawNormalTriangles", "HUD_DrawTransparentTriangles", "HUD_StudioEvent", "HUD_PostRunCmd", "HUD_Shutdown", "HUD_TxferLocalOverrides", "HUD_ProcessPlayerState",
	"HUD_TxferPredictionData", "Demo_ReadBuffer", "HUD_ConnectionlessPacket", "HUD_GetHullBounds", "HUD_Frame", "HUD_Key_Event", "HUD_TempEntUpdate", "HUD_GetUserEntity", "HUD_VoiceStatus", "HUD_DirectorMessage",
	"HUD_GetStudioModelInterface", "HUD_ChatInputPosition", "HUD_GetPlayerTeam", "ClientFactory"};
enum { S_Initialize = 0, S_HUD_Init = 1, S_HUD_VidInit = 2, S_HUD_Redraw = 3, S_HUD_UpdateClientData = 4, S_HUD_AddEntity = 20, S_HUD_DrawTransparentTriangles = 23, S_CL_CreateMove = 14, S_CL_IsThirdPerson = 15, S_V_CalcRefdef = 19, S_HUD_Shutdown = 26, S_HUD_Frame = 33, S_HUD_Key_Event = 34 };
typedef char slots_match_the_sdk[sizeof(cldll_func_t) == N_SLOTS * sizeof(void *) ? 1 : -1];

static int nx_zero() { return 0; }                                        // stands in for a function the original library does not have (cdecl: any argument list is fine)
extern "C" { void *nx_slot[N_SLOTS]; }                                    // the original library's functions; never NULL after ensure()
static void *g_orig = nullptr; static bool g_tried = false, g_ok = false;

static FILE *g_log = nullptr;                                             // cl_dlls/nexus_load.log: what happened while loading (rewritten every start)
static void logl(const std::string &s) { if (!g_log) g_log = fopen((nxp::self_dir() + "nexus_load.log").c_str(), "w"); if (g_log) { fprintf(g_log, "%s\n", s.c_str()); fflush(g_log); } }
static bool ensure() {
	if (g_tried) return g_ok;
	g_tried = true; for (auto &s : nx_slot) s = (void *)nx_zero;
	std::string dir = nxp::self_dir(), path = dir + ORIG_NAME;
	logl("Nexus client " NEXUS_VERSION " (protocol " + std::to_string(NEXUS_PROTO) + ")  folder: " + dir);
	if (!nxp::file_exists(path) && nxp::file_exists(path + ORIG_EXT)) { logl("note: found " ORIG_NAME ORIG_EXT " (double extension from Explorer) - using it"); path += ORIG_EXT; }
	if (!nxp::file_exists(path)) {
		std::string m = "Nexus: the game's original client library was not found.\n\nExpected: " + path + "\nFiles in that folder: " + nxp::list_dir(dir, "client") +
			"\n\nInstall: rename the game's own " LIB_NAME " to " ORIG_NAME " (check that Windows does not hide the extension: the name must not become " ORIG_NAME ORIG_EXT "), then put the Nexus " LIB_NAME " next to it.";
		logl(m); nxp::fatal_box(m.c_str()); return false;
	}
	logl("loading " + path + " ...");
	g_orig = nxp::lib_open(path.c_str());
	if (!g_orig) {
		std::string m = "Nexus: " + path + " exists but could not be loaded.\n" + nxp::lib_error() + "\nFiles in that folder: " + nxp::list_dir(dir, "client") +
			"\n\nIs it really the game's client library (same game and version)? Steam: verify the game files, then rename the fresh " LIB_NAME " again.";
		logl(m); nxp::fatal_box(m.c_str()); return false;
	}
	if (nxp::lib_sym(g_orig, "Nexus_Marker")) { std::string m = ORIG_NAME " is a copy of Nexus, not the game's client library. Restore the original file (Steam: verify the game files) and rename it again."; logl(m); nxp::fatal_box(m.c_str()); g_orig = nullptr; return false; }
	int found = 0; for (int i = 0; i < N_SLOTS; i++) if (void *p = nxp::lib_sym(g_orig, NAMES[i])) { nx_slot[i] = p; found++; }
	// NOTE: never call the original's F here. The engine hands F a table whose slot 2 carries a pointer the client keeps
	// (g_pcldstAddrs, "Hack!" in the HL SDK); calling it with our own buffer gave the real client.dll a NULL there and crashed the game.
	logl("loaded " + path + "  (" + std::to_string(found) + " named exports, F: " + (nxp::lib_sym(g_orig, "F") ? "yes" : "no") + ")");
	return g_ok = true;
}
template <class T> static T slot(int i) { ensure(); return (T)nx_slot[i]; }

// ------------------------------------------------------------------------------------------------ the ten calls Nexus looks at
static int H_Initialize(cl_enginefunc_t *eng, int version) {
	logl("Initialize ..."); int r = slot<int (*)(cl_enginefunc_t *, int)>(S_Initialize)(eng, version); if (g_ok && r) nxc::on_initialize(eng);
	logl("Initialize(version " + std::to_string(version) + ") -> original returned " + std::to_string(r) + (g_ok ? "" : " (Nexus did not load the original: see above)"));
	return r;
}
static void H_HUD_Init() { logl("HUD_Init ..."); slot<void (*)()>(S_HUD_Init)(); logl("HUD_Init: original done, starting Nexus ..."); if (g_ok) nxc::on_hud_init(); logl("HUD_Init: Nexus ready"); }
static int H_HUD_VidInit() { static int n = 0; bool first = n++ < 3; if (first) logl("HUD_VidInit ..."); int r = slot<int (*)()>(S_HUD_VidInit)(); nxc::on_vid_init(); if (first) logl("HUD_VidInit done"); return r; }
static int H_HUD_Redraw(float t, int intermission) { static bool first = true; if (first) logl("first HUD_Redraw ..."); int r = slot<int (*)(float, int)>(S_HUD_Redraw)(t, intermission); nxc::on_redraw(t, intermission); if (first) { first = false; logl("first HUD_Redraw done - the game is running"); } return r; }
static void H_CL_CreateMove(float ft, usercmd_s *cmd, int active) { nxc::createmove(slot<void (*)(float, usercmd_s *, int)>(S_CL_CreateMove), ft, cmd, active); }
static int H_CL_IsThirdPerson() { return nxc::third_person() ? 1 : slot<int (*)()>(S_CL_IsThirdPerson)(); }
static void H_V_CalcRefdef(ref_params_s *p) { slot<void (*)(ref_params_s *)>(S_V_CalcRefdef)(p); nxc::post_calcrefdef(p); }
static int H_HUD_UpdateClientData(client_data_s *cd, float t) { int r = slot<int (*)(client_data_s *, float)>(S_HUD_UpdateClientData)(cd, t); return nxc::update_client_data(r, cd); }
static int H_HUD_AddEntity(int type, cl_entity_s *ent, const char *model) {
	int r = nx_slot[S_HUD_AddEntity] == (void *)nx_zero ? 1 : slot<int (*)(int, cl_entity_s *, const char *)>(S_HUD_AddEntity)(type, ent, model);   // no handler = draw it
	return nxc::add_entity(r, type, ent);
}
static void H_HUD_DrawTransparentTriangles() { slot<void (*)()>(S_HUD_DrawTransparentTriangles)(); nxc::draw_world(); }
static void H_HUD_Shutdown() { nxc::on_shutdown(); slot<void (*)()>(S_HUD_Shutdown)(); }
static void H_HUD_Frame(double t) { slot<void (*)(double)>(S_HUD_Frame)(t); nxc::on_frame(t); }
static int H_HUD_Key_Event(int down, int key, const char *binding) {
	if (!nxc::on_key(down, key, binding)) return 0;
	if (nx_slot[S_HUD_Key_Event] == (void *)nx_zero) return 1;            // no handler in the game: the engine handles the key
	return slot<int (*)(int, int, const char *)>(S_HUD_Key_Event)(down, key, binding);
}
static const struct { int slot; void *fn; } HOOKED[] = {{S_Initialize, (void *)H_Initialize}, {S_HUD_Init, (void *)H_HUD_Init}, {S_HUD_VidInit, (void *)H_HUD_VidInit}, {S_HUD_Redraw, (void *)H_HUD_Redraw},
	{S_CL_CreateMove, (void *)H_CL_CreateMove}, {S_CL_IsThirdPerson, (void *)H_CL_IsThirdPerson}, {S_V_CalcRefdef, (void *)H_V_CalcRefdef}, {S_HUD_Shutdown, (void *)H_HUD_Shutdown}, {S_HUD_Frame, (void *)H_HUD_Frame}, {S_HUD_Key_Event, (void *)H_HUD_Key_Event},
	{S_HUD_UpdateClientData, (void *)H_HUD_UpdateClientData}, {S_HUD_AddEntity, (void *)H_HUD_AddEntity}, {S_HUD_DrawTransparentTriangles, (void *)H_HUD_DrawTransparentTriangles}};

// ------------------------------------------------------------------------------------------------ exports
extern "C" { NX_EXPORT_DATA int Nexus_Marker = NEXUS_PROTO; }
// Modern engines ask for "F" and let it fill the whole function table.
NX_EXPORT void F(void *pv) {
	if (!pv) return; void **tab = (void **)pv;
	if (!ensure()) { for (int i = 0; i < N_SLOTS; i++) tab[i] = nullptr; tab[S_Initialize] = (void *)nx_zero; return; }     // Initialize returns 0: the engine stops with its own error
	typedef void (*f_t)(void *); f_t of = (f_t)nxp::lib_sym(g_orig, "F");
	if (of) { logl("calling the original F ..."); of(pv); int n = 0; for (int i = 0; i < N_SLOTS; i++) if (tab[i]) { nx_slot[i] = tab[i]; n++; } logl("engine called F: original filled " + std::to_string(n) + " of 43 slots"); }                                   // the original's table is the truth
	else { for (int i = 0; i < N_SLOTS; i++) tab[i] = nx_slot[i] == (void *)nx_zero ? nullptr : nx_slot[i]; logl("engine called F: the original has no F, using its named exports"); }
	for (auto &h : HOOKED) tab[h.slot] = h.fn;
}
NX_EXPORT void *CreateInterface(const char *name, int *ret) {
	typedef void *(*ci_t)(const char *, int *); ci_t ci = ensure() ? (ci_t)nxp::lib_sym(g_orig, "CreateInterface") : nullptr;
	if (ci) return ci(name, ret);
	if (ret) *ret = 1; return nullptr;
}
#ifdef _WIN32
// Engines (and tools) that look the functions up by name get the same behaviour. Not exported on Linux: there the names could shadow
// the original library's own symbols; "F" is what the Linux engine uses.
extern "C" {
__declspec(dllexport) int Initialize(cl_enginefunc_t *e, int v) { return H_Initialize(e, v); }
__declspec(dllexport) void HUD_Init() { H_HUD_Init(); }
__declspec(dllexport) int HUD_VidInit() { return H_HUD_VidInit(); }
__declspec(dllexport) int HUD_Redraw(float t, int i) { return H_HUD_Redraw(t, i); }
__declspec(dllexport) void CL_CreateMove(float f, usercmd_s *c, int a) { H_CL_CreateMove(f, c, a); }
__declspec(dllexport) int CL_IsThirdPerson() { return H_CL_IsThirdPerson(); }
__declspec(dllexport) void V_CalcRefdef(ref_params_s *p) { H_V_CalcRefdef(p); }
__declspec(dllexport) void HUD_Shutdown() { H_HUD_Shutdown(); }
__declspec(dllexport) void HUD_Frame(double t) { H_HUD_Frame(t); }
__declspec(dllexport) int HUD_Key_Event(int d, int k, const char *b) { return H_HUD_Key_Event(d, k, b); }
__declspec(dllexport) int HUD_UpdateClientData(client_data_s *cd, float t) { return H_HUD_UpdateClientData(cd, t); }
__declspec(dllexport) int HUD_AddEntity(int type, cl_entity_s *e, const char *m) { return H_HUD_AddEntity(type, e, m); }
__declspec(dllexport) void HUD_DrawTransparentTriangles() { H_HUD_DrawTransparentTriangles(); }
}
// Everything else jumps straight into the original function: arguments and return value pass through untouched.
// The thunk first makes sure the original is loaded (an engine that resolves functions by name may call any of them first);
// call/ret leaves the stack exactly as the caller built it, so the jump hands over the untouched arguments and return address.
extern "C" void nx_ensure_c() { ensure(); }
#define THUNK(name, idx) extern "C" __declspec(dllexport) __attribute__((naked)) void name() { __asm__("call _nx_ensure_c\n\tjmp *_nx_slot+4*" #idx); }
THUNK(HUD_Reset, 5) THUNK(HUD_PlayerMove, 6) THUNK(HUD_PlayerMoveInit, 7) THUNK(HUD_PlayerMoveTexture, 8) THUNK(IN_ActivateMouse, 9) THUNK(IN_DeactivateMouse, 10)
THUNK(IN_MouseEvent, 11) THUNK(IN_ClearStates, 12) THUNK(IN_Accumulate, 13) THUNK(CL_CameraOffset, 16) THUNK(KB_Find, 17) THUNK(CAM_Think, 18) THUNK(HUD_CreateEntities, 21)
THUNK(HUD_DrawNormalTriangles, 22) THUNK(HUD_StudioEvent, 24) THUNK(HUD_PostRunCmd, 25) THUNK(HUD_TxferLocalOverrides, 27) THUNK(HUD_ProcessPlayerState, 28)
THUNK(HUD_TxferPredictionData, 29) THUNK(Demo_ReadBuffer, 30) THUNK(HUD_ConnectionlessPacket, 31) THUNK(HUD_GetHullBounds, 32) THUNK(HUD_TempEntUpdate, 35) THUNK(HUD_GetUserEntity, 36) THUNK(HUD_VoiceStatus, 37)
THUNK(HUD_DirectorMessage, 38) THUNK(HUD_GetStudioModelInterface, 39) THUNK(HUD_ChatInputPosition, 40) THUNK(HUD_GetPlayerTeam, 41) THUNK(ClientFactory, 42)
// the table must be usable even if a thunk is called before Initialize / F
static struct SlotInit { SlotInit() { if (!g_tried) for (auto &s : nx_slot) if (!s) s = (void *)nx_zero; } } g_slot_init;
#endif
