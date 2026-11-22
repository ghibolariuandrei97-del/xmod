// rules.cpp - the game rules object (CHalfLifeMultiplay = CSGameRules()): round state, scores, timers, map flags.
// Found once per call (a new object is created every map):
//   Linux (vanilla CS and ReGameDLL): the exported symbol g_pGameRules
//   Windows + ReGameDLL: IReGameApi::GetGameRules()
//   Windows vanilla: AMX Mod X's signature (CWorld::Precache, "room_type") -> mov eax, [g_pGameRules]
#include "mc.h"
#ifndef _WIN32
#include <dlfcn.h>
#endif

// IReGameApi: virtual ~IReGameApi(); GetMajorVersion; GetMinorVersion; GetFuncs; GetHookchains; GetGameRules ...
// n = index counted without the destructor (GetGameRules = 4)
#ifdef _WIN32
#define MC_VSLOT(n) (n + 1)    // MSVC: one vtable slot for the virtual destructor
#else
#define MC_VSLOT(n) (n + 2)    // Itanium: two (complete + deleting destructor)
#endif
static void **g_rules_pp = nullptr;          // &g_pGameRules
static void *g_regame_api = nullptr;         // IReGameApi*
static const char *g_source = "not found";
static bool g_tried = false;

static const char *basename_of(const char *p) { const char *b = p; for (const char *c = p; *c; ++c) if (*c == '/' || *c == '\\') b = c + 1; return b; }
#ifdef _WIN32
static void **scan_windows(HMODULE h) {
	// \x68\x2A\x2A\x2A\x2A\xFF\x15\x2A\x2A\x2A\x2A\xA1\x2A\x2A\x2A\x2A\x83\xC4\x2A\x85\xC0  (read pointer at +12)
	static const int sig[] = {0x68, -1, -1, -1, -1, 0xFF, 0x15, -1, -1, -1, -1, 0xA1, -1, -1, -1, -1, 0x83, 0xC4, -1, 0x85, 0xC0};
	const int n = sizeof sig / sizeof sig[0];
	byte *base = reinterpret_cast<byte *>(h);
	IMAGE_DOS_HEADER *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base); if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
	IMAGE_NT_HEADERS *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
	IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
	for (int s = 0; s < nt->FileHeader.NumberOfSections; s++, sec++) {
		if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
		byte *p = base + sec->VirtualAddress, *end = p + sec->Misc.VirtualSize - n;
		for (; p < end; ++p) {
			int k = 0; while (k < n && (sig[k] < 0 || p[k] == sig[k])) k++;
			if (k == n) return *reinterpret_cast<void ***>(p + 12);
		}
	}
	return nullptr;
}
#endif
static void resolve() {
	if (g_tried) return; g_tried = true;
	const char *path = gpMetaUtilFuncs && gpMetaUtilFuncs->pfnGetGameInfo ? GET_GAME_INFO(PLID, GINFO_REALDLL_FULLPATH) : nullptr;
	if (!path || !*path) path = gpMetaUtilFuncs && gpMetaUtilFuncs->pfnGetGameInfo ? GET_GAME_INFO(PLID, GINFO_DLL_FULLPATH) : nullptr;
	if (!path || !*path) return;
	typedef void *(*CreateInterface_t)(const char *, int *);
	CreateInterface_t ci = nullptr;
#ifdef _WIN32
	HMODULE h = GetModuleHandleA(basename_of(path)); if (!h) h = GetModuleHandleA(path);
	if (!h) return;
	ci = (CreateInterface_t)GetProcAddress(h, "CreateInterface");
#else
	void *h = dlopen(path, RTLD_NOW | RTLD_NOLOAD); if (!h) return;
	g_rules_pp = reinterpret_cast<void **>(dlsym(h, "g_pGameRules"));
	if (g_rules_pp) { g_source = "g_pGameRules symbol"; }
	ci = (CreateInterface_t)dlsym(h, "CreateInterface");
#endif
	if (ci) { int rc = 0; g_regame_api = ci("VRE_GAMEDLL_API_VERSION001", &rc); }
#ifdef _WIN32
	if (g_regame_api) { g_source = "ReGameDLL API"; return; }
	g_rules_pp = scan_windows(h);
	if (g_rules_pp) g_source = "signature (vanilla mp.dll)";
#else
	if (!g_rules_pp && g_regame_api) g_source = "ReGameDLL API";
	(void)basename_of;
#endif
}
void *mc_rules() {
	resolve();
	if (g_rules_pp) return *g_rules_pp;
	if (g_regame_api) {
		void **vt = *reinterpret_cast<void ***>(g_regame_api);
#if defined(_WIN32) && defined(_MSC_VER)
		return reinterpret_cast<void *(__fastcall *)(void *, int)>(vt[MC_VSLOT(4)])(g_regame_api, 0);
#else
		return reinterpret_cast<void *(MC_CC *)(void *)>(vt[MC_VSLOT(4)])(g_regame_api);
#endif
	}
	return nullptr;
}
const char *mc_rules_source() { resolve(); return g_source; }
bool mc_regamedll() { resolve(); return g_regame_api != nullptr; }

#define RULES_OR_ERR(fn) Args a(c); char *gr = reinterpret_cast<char *>(mc_rules()); if (!gr) { a.err("mc." fn ": game rules not available (%s) - is this Counter-Strike and is a map running?", g_source); return; }
// mc.rules_available() -> bool, source
static void f_rules_available(XmodCall *c, void *) { Args a(c); XmodValue *r = g_xmod->val_array(); g_xmod->val_push(r, g_xmod->val_bool(mc_rules() != nullptr));
	g_xmod->val_push(r, g_xmod->val_cstr(g_source)); g_xmod->val_push(r, g_xmod->val_bool(mc_regamedll())); a.ret(r); }
// mc.rules_get(member [, index]) -> value      e.g. rules_get("m_iNumCTWins"), rules_get("m_bFreezePeriod")
static void f_rules_get(XmodCall *c, void *) {
	RULES_OR_ERR("rules_get") const char *n = a.s(0); int o, t, cnt; bool u;
	if (!n || !mc_rules_find(n, &o, &t, &cnt, &u)) { a.err("mc.rules_get: unknown game rules member '%s' (see xmod.mc.pdata_list('', true))", n ? n : "?"); return; }
	XmodValue *v = mc_read_member(gr + o, t, cnt, u, (int)a.i(1, 0)); if (!v) { a.err("mc.rules_get(%s): bad index", n); return; } a.ret(v);
}
// mc.rules_set(member, value [, index])
static void f_rules_set(XmodCall *c, void *) {
	RULES_OR_ERR("rules_set") const char *n = a.s(0); int o, t, cnt; bool u;
	if (!n || !mc_rules_find(n, &o, &t, &cnt, &u)) { a.err("mc.rules_set: unknown game rules member '%s'", n ? n : "?"); return; }
	std::string err; if (!mc_write_member(gr + o, t, cnt, (int)a.i(2, 0), a, 1, err)) { a.err("mc.rules_set(%s): %s", n, err.c_str()); return; } a.ret_nil();
}
static int R(const char *n) { int o, t, c; bool u; return mc_rules_find(n, &o, &t, &c, &u) ? o : -1; }
// mc.round_terminate(delay, win_status) - CHalfLifeMultiplay::TerminateRound: the next round starts in `delay` seconds.
// win_status: 1 CT, 2 T, 3 draw, 0 none.  (messages, sounds and scores are done by lib/modcreator.lua: MC.round.finish)
static void f_round_terminate(XmodCall *c, void *) {
	RULES_OR_ERR("round_terminate")
	pd<int>(gr, R("m_iRoundWinStatus")) = (int)a.i(1, 3);
	pd<float>(gr, R("m_fTeamCount")) = gpGlobals->time + (float)a.f(0, 5.0);
	pd<bool>(gr, R("m_bRoundTerminating")) = true;
	a.ret_bool(true);
}
// mc.round_state() -> {terminating, freeze, win_status, restart_at, round_start, round_time, time_left, ct_wins, t_wins, rounds_played, game_started}
static void f_round_state(XmodCall *c, void *) {
	RULES_OR_ERR("round_state") XmodValue *m = g_xmod->val_map();
	float start = pd<float>(gr, R("m_fRoundCount")); int secs = pd<int>(gr, R("m_iRoundTimeSecs"));
	g_xmod->val_set(m, "terminating", g_xmod->val_bool(pd<bool>(gr, R("m_bRoundTerminating"))));
	g_xmod->val_set(m, "freeze", g_xmod->val_bool(pd<int>(gr, R("m_bFreezePeriod")) != 0));
	g_xmod->val_set(m, "win_status", g_xmod->val_int(pd<int>(gr, R("m_iRoundWinStatus"))));
	g_xmod->val_set(m, "restart_at", g_xmod->val_num(pd<float>(gr, R("m_fTeamCount"))));
	g_xmod->val_set(m, "round_start", g_xmod->val_num(start));
	g_xmod->val_set(m, "round_time", g_xmod->val_int(secs));
	g_xmod->val_set(m, "time_left", g_xmod->val_num(secs - gpGlobals->time + start));
	g_xmod->val_set(m, "ct_wins", g_xmod->val_int(pd<short>(gr, R("m_iNumCTWins"))));
	g_xmod->val_set(m, "t_wins", g_xmod->val_int(pd<short>(gr, R("m_iNumTerroristWins"))));
	g_xmod->val_set(m, "rounds_played", g_xmod->val_int(pd<int>(gr, R("m_iTotalRoundsPlayed"))));
	g_xmod->val_set(m, "game_started", g_xmod->val_bool(pd<bool>(gr, R("m_bFirstConnected"))));
	a.ret(m);
}
bool reg_rules() {
	return mc_reg("rules_available", f_rules_available) && mc_reg("rules_get", f_rules_get) && mc_reg("rules_set", f_rules_set)
		&& mc_reg("round_terminate", f_round_terminate) && mc_reg("round_state", f_round_state);
}
