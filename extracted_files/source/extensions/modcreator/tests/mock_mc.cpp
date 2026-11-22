// mock_mc - a tiny engine + Metamod that loads the REAL game DLL (ReGameDLL cs.so), xmod and ModCreator, spawns the world
// (game rules), runs frames (think/touch/physics-lite), connects players and drives the Lua test plugin mc_test.lua.
// Build + run: extensions/modcreator/tests/run.sh
#include "../../../include/mm_sdk.h"
#include <dlfcn.h>
#include <entity_state.h>
#include <studio.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

enginefuncs_t g_engfuncs; globalvars_t *gpGlobals;
static const int MAXE = 1200;
static globalvars_t gv; static edict_t ed[MAXE]; static char strbase[16];
static std::vector<std::string> g_argv; static std::string g_args;
static std::map<std::string, void (*)()> svrcmds; static std::map<std::string, cvar_t *> cvars;
static std::map<std::string, int> msgids; static std::map<int, std::string> msgnames; static int next_msg = 64;
static std::map<std::string, int> precached; static int next_pc = 1;
static char g_info[34][256]; static bool g_open = false; static int g_engine_errors = 0, g_msg_count = 0, g_unprecached = 0;
static std::string g_cur_msg; static int g_cur_type = 0; static std::map<int, int> g_msg_by_type;
static void *g_game = nullptr; static DLL_FUNCTIONS g_dll; static NEW_DLL_FUNCTIONS g_newdll; static std::string g_game_path;
static int IdxOf(const edict_t *e) { return e ? (int)(e - ed) : 0; }
static edict_t *PEnt(int i) { return (i >= 0 && i < MAXE) ? &ed[i] : nullptr; }

// ---------------------------------------------------------------- Metamod emulation (pre/post hook tables of the loaded plugins)
struct Plug { const char *name; META_FUNCTIONS mf; DLL_FUNCTIONS pre{}, post{}; enginefuncs_t epre{}, epost{}; bool has_e = false; };
static std::vector<Plug *> g_plugs; static meta_globals_t g_mg;
#define DISPATCH_V(TBL_PRE, TBL_POST, REAL, FN, ...) do { bool sup = false; \
	for (Plug *p : g_plugs) if (p->TBL_PRE.FN) { g_mg.mres = MRES_UNSET; p->TBL_PRE.FN(__VA_ARGS__); if (g_mg.mres == MRES_SUPERCEDE) sup = true; } \
	if (!sup && REAL.FN) REAL.FN(__VA_ARGS__); \
	for (Plug *p : g_plugs) if (p->TBL_POST.FN) { g_mg.mres = MRES_UNSET; p->TBL_POST.FN(__VA_ARGS__); } } while (0)
static enginefuncs_t real;       // the mock engine (what plugins call)
static enginefuncs_t game_eng;   // what the game DLL calls (hooked functions dispatched through the plugins)
#define EWRAP_V(FN, PARAMS, ...) static void w_##FN PARAMS { DISPATCH_V(epre, epost, real, FN, __VA_ARGS__); }
EWRAP_V(pfnMessageBegin, (int a, int b, const float *c, edict_t *d), a, b, c, d)
EWRAP_V(pfnMessageEnd, (), )
EWRAP_V(pfnWriteByte, (int v), v) EWRAP_V(pfnWriteChar, (int v), v) EWRAP_V(pfnWriteShort, (int v), v) EWRAP_V(pfnWriteLong, (int v), v)
EWRAP_V(pfnWriteAngle, (float v), v) EWRAP_V(pfnWriteCoord, (float v), v) EWRAP_V(pfnWriteString, (const char *v), v) EWRAP_V(pfnWriteEntity, (int v), v)
EWRAP_V(pfnSetModel, (edict_t *e, const char *m), e, m)
EWRAP_V(pfnEmitSound, (edict_t *e, int ch, const char *s, float v, float at, int f, int pt), e, ch, s, v, at, f, pt)
EWRAP_V(pfnPlaybackEvent, (int fl, const edict_t *inv, unsigned short ev, float d, float *o, float *a, float f1, float f2, int i1, int i2, int b1, int b2), fl, inv, ev, d, o, a, f1, f2, i1, i2, b1, b2)
// game DLL calls made by the "engine" (dispatched through the plugins)
static void D_Spawn(edict_t *e) { bool sup = false; int r = 0;
	for (Plug *p : g_plugs) if (p->pre.pfnSpawn) { g_mg.mres = MRES_UNSET; p->pre.pfnSpawn(e); if (g_mg.mres == MRES_SUPERCEDE) sup = true; }
	if (!sup) r = g_dll.pfnSpawn(e); (void)r; for (Plug *p : g_plugs) if (p->post.pfnSpawn) p->post.pfnSpawn(e); }
static void D_Think(edict_t *e) { DISPATCH_V(pre, post, g_dll, pfnThink, e); }
static void D_Touch(edict_t *a, edict_t *b) { DISPATCH_V(pre, post, g_dll, pfnTouch, a, b); }
static void D_StartFrame() { DISPATCH_V(pre, post, g_dll, pfnStartFrame, ); }
static void D_ServerActivate(edict_t *l, int n, int m) { DISPATCH_V(pre, post, g_dll, pfnServerActivate, l, n, m); }
static void D_ServerDeactivate() { DISPATCH_V(pre, post, g_dll, pfnServerDeactivate, ); }
static void D_PutInServer(edict_t *e) { DISPATCH_V(pre, post, g_dll, pfnClientPutInServer, e); }
static void D_ClientCommand(edict_t *e) { DISPATCH_V(pre, post, g_dll, pfnClientCommand, e); }
static void D_PreThink(edict_t *e) { DISPATCH_V(pre, post, g_dll, pfnPlayerPreThink, e); }
static void D_PostThink(edict_t *e) { DISPATCH_V(pre, post, g_dll, pfnPlayerPostThink, e); }
static void D_UpdateClientData(edict_t *e, int sw, clientdata_s *cd) { DISPATCH_V(pre, post, g_dll, pfnUpdateClientData, e, sw, cd); }
static int D_ClientConnect(edict_t *e, const char *n, const char *a, char *rej) { int r = 1; bool sup = false;
	for (Plug *p : g_plugs) if (p->pre.pfnClientConnect) { g_mg.mres = MRES_UNSET; p->pre.pfnClientConnect(e, n, a, rej); if (g_mg.mres == MRES_SUPERCEDE) sup = true; }
	if (!sup) r = g_dll.pfnClientConnect(e, n, a, rej); for (Plug *p : g_plugs) if (p->post.pfnClientConnect) p->post.pfnClientConnect(e, n, a, rej); return r; }

// ---------------------------------------------------------------- a fake studio model ("models/mc_test.mdl") for the animation tests
static std::vector<byte> g_mdl;
static void build_mdl() {
	g_mdl.assign(8192, 0); studiohdr_t *h = (studiohdr_t *)g_mdl.data(); int off = sizeof(studiohdr_t);
	h->id = (('T' << 24) + ('S' << 16) + ('D' << 8) + 'I'); h->version = 10; strcpy(h->name, "mc_test.mdl"); h->eyeposition[2] = 60;
	h->numbones = 3; h->boneindex = off; mstudiobone_t *b = (mstudiobone_t *)(g_mdl.data() + off); strcpy(b[0].name, "Bip01"); strcpy(b[1].name, "Bip01 Head"); strcpy(b[2].name, "Bip01 R Hand"); off += 3 * sizeof(mstudiobone_t);
	h->numbonecontrollers = 1; h->bonecontrollerindex = off; mstudiobonecontroller_t *bc = (mstudiobonecontroller_t *)(g_mdl.data() + off); bc->index = 0; bc->type = STUDIO_YR; bc->start = -90; bc->end = 90; off += sizeof(*bc);
	struct S { const char *n; float fps; int frames; int loop; int act; } seq[] = {{"idle", 10, 11, 1, 1}, {"run", 30, 21, 1, 4}, {"attack1", 30, 16, 0, 30}, {"die", 20, 21, 0, 36}, {"flinch", 30, 10, 0, 26}};
	h->numseq = 5; h->seqindex = off; mstudioseqdesc_t *sd = (mstudioseqdesc_t *)(g_mdl.data() + off);
	for (int i = 0; i < 5; i++) { strcpy(sd[i].label, seq[i].n); sd[i].fps = seq[i].fps; sd[i].numframes = seq[i].frames; sd[i].flags = seq[i].loop ? STUDIO_LOOPING : 0; sd[i].activity = seq[i].act; sd[i].actweight = 1;
		if (i == 2) { sd[i].blendtype[0] = STUDIO_XR; sd[i].blendstart[0] = -45; sd[i].blendend[0] = 45; } }
	off += 5 * sizeof(mstudioseqdesc_t);
	h->numbodyparts = 2; h->bodypartindex = off; mstudiobodyparts_t *bp = (mstudiobodyparts_t *)(g_mdl.data() + off);
	strcpy(bp[0].name, "body"); bp[0].nummodels = 1; bp[0].base = 1; strcpy(bp[1].name, "head"); bp[1].nummodels = 3; bp[1].base = 1; off += 2 * sizeof(mstudiobodyparts_t);
	h->numattachments = 1; h->attachmentindex = off; off += sizeof(mstudioattachment_t); h->length = off;
}

// ---------------------------------------------------------------- engine functions
static void link_ent(edict_t *e) { e->v.absmin = e->v.origin + e->v.mins; e->v.absmax = e->v.origin + e->v.maxs; e->v.size = e->v.maxs - e->v.mins; }
static edict_t *E_CreateEntity() {
	for (int i = gv.maxClients + 1; i < MAXE; i++) if (ed[i].free) { int serial = ed[i].serialnumber; memset(&ed[i], 0, sizeof(edict_t)); ed[i].serialnumber = serial + 1; ed[i].v.pContainingEntity = &ed[i]; return &ed[i]; }
	printf("!!! out of edicts\n"); return nullptr;
}
static void E_RemoveEntity(edict_t *e) {
	if (!e || e->free) return;
	if (e->pvPrivateData) { if (g_newdll.pfnOnFreeEntPrivateData) g_newdll.pfnOnFreeEntPrivateData(e); free(e->pvPrivateData); }
	int serial = e->serialnumber; memset(e, 0, sizeof(edict_t)); e->free = 1; e->serialnumber = serial; e->freetime = gv.time;
}
static edict_t *E_CreateNamedEntity(int cls) {
	edict_t *e = E_CreateEntity(); if (!e) return nullptr; e->v.classname = cls;
	typedef void (*Linker)(entvars_t *); Linker f = (Linker)dlsym(g_game, STRING(cls));
	if (!f) { printf("[engine] can't create entity: %s\n", STRING(cls)); E_RemoveEntity(e); return nullptr; }
	f(&e->v); return e;
}
static int pc_index(const char *s) { auto i = precached.find(s); if (i != precached.end()) return i->second; return precached[s] = next_pc++; }
static void E_SetModel(edict_t *e, const char *m) {
	if (!m || !*m) { e->v.model = 0; e->v.modelindex = 0; return; }
	if (m[0] != '*' && !precached.count(m)) { g_unprecached++; printf("!!! ENGINE ERROR: SetModel '%s' NOT PRECACHED (Host_Error on a real server)\n", m); }
	e->v.model = (int)(strdup(m) - strbase); e->v.modelindex = m[0] == '*' ? 1 : pc_index(m);
	if (!strcmp(m, "models/mc_test.mdl")) { e->v.mins = Vector(-16, -16, 0); }
}
static void E_SetSize(edict_t *e, const float *a, const float *b) { e->v.mins = Vector(a[0], a[1], a[2]); e->v.maxs = Vector(b[0], b[1], b[2]); link_ent(e); }
static void E_SetOrigin(edict_t *e, const float *o) { e->v.origin = Vector(o[0], o[1], o[2]); link_ent(e); }
static float yaw_of(float x, float y) { if (x == 0 && y == 0) return 0; float yaw = atan2f(y, x) * 180.f / (float)M_PI; if (yaw < 0) yaw += 360; return yaw; }
static void E_VecToAngles(const float *v, float *a) { float yaw = yaw_of(v[0], v[1]); float fwd = sqrtf(v[0] * v[0] + v[1] * v[1]); float pitch = atan2f(v[2], fwd) * 180.f / (float)M_PI; if (pitch < 0) pitch += 360; a[0] = pitch; a[1] = yaw; a[2] = 0; }
static void E_AngleVectors(const float *ang, float *f, float *r, float *u) {
	float sy = sinf(ang[1] * (float)M_PI / 180), cy = cosf(ang[1] * (float)M_PI / 180), sp = sinf(ang[0] * (float)M_PI / 180), cp = cosf(ang[0] * (float)M_PI / 180), sr = sinf(ang[2] * (float)M_PI / 180), cr = cosf(ang[2] * (float)M_PI / 180);
	if (f) { f[0] = cp * cy; f[1] = cp * sy; f[2] = -sp; }
	if (r) { r[0] = (-1 * sr * sp * cy + -1 * cr * -sy); r[1] = (-1 * sr * sp * sy + -1 * cr * cy); r[2] = -1 * sr * cp; }
	if (u) { u[0] = (cr * sp * cy + -sr * -sy); u[1] = (cr * sp * sy + -sr * cy); u[2] = cr * cp; }
}
static void E_ChangeYaw(edict_t *e) { float cur = e->v.angles.y, ideal = e->v.ideal_yaw, sp = e->v.yaw_speed; float mv = ideal - cur;
	while (mv > 180) mv -= 360; while (mv < -180) mv += 360; if (mv > sp) mv = sp; if (mv < -sp) mv = -sp; e->v.angles.y = fmodf(cur + mv + 360, 360); }
static void E_MoveToOrigin(edict_t *e, const float *g, float dist, int) {
	if (!(e->v.flags & FL_ONGROUND) && !(e->v.flags & (FL_FLY | FL_SWIM))) return;
	float dx = g[0] - e->v.origin.x, dy = g[1] - e->v.origin.y, l = sqrtf(dx * dx + dy * dy); if (l < 0.01f) return;
	e->v.ideal_yaw = yaw_of(dx, dy); E_ChangeYaw(e); if (dist > l) dist = l;
	e->v.origin.x += dx / l * dist; e->v.origin.y += dy / l * dist; link_ent(e);
}
static int E_WalkMove(edict_t *e, float yaw, float dist, int) { e->v.origin.x += cosf(yaw * (float)M_PI / 180) * dist; e->v.origin.y += sinf(yaw * (float)M_PI / 180) * dist; link_ent(e); return 1; }
static int E_DropToFloor(edict_t *e) { e->v.origin.z = 0 - e->v.mins.z; e->v.flags |= FL_ONGROUND; link_ent(e); return 1; }
// traces: world = floor plane z=0; entities = their bounding boxes (SOLID_BBOX / SLIDEBOX / BSP)
static bool ray_box(const Vector &s, const Vector &d, const Vector &mn, const Vector &mx, float &t) {
	float t0 = 0, t1 = 1; for (int k = 0; k < 3; k++) { float o = s[k], v = d[k];
		if (fabsf(v) < 1e-6f) { if (o < mn[k] || o > mx[k]) return false; continue; }
		float a = (mn[k] - o) / v, b = (mx[k] - o) / v; if (a > b) std::swap(a, b); if (a > t0) t0 = a; if (b < t1) t1 = b; if (t0 > t1) return false; }
	t = t0; return true; }
static void E_TraceLine(const float *a, const float *b, int nomon, edict_t *ign, TraceResult *tr) {
	memset(tr, 0, sizeof *tr); Vector s(a[0], a[1], a[2]), e(b[0], b[1], b[2]), d = e - s; float best = 1.0f; edict_t *hit = &ed[0]; int hg = 0;
	if (e.z < 0 && s.z >= 0) best = s.z / (s.z - e.z);
	for (int i = 1; i < MAXE; i++) { edict_t *x = &ed[i]; if (x->free || x == ign || x->v.solid < SOLID_BBOX) continue; if ((nomon & 1) && (x->v.flags & (FL_MONSTER | FL_CLIENT))) continue;
		if (ign && x->v.owner == ign) continue; if (i <= gv.maxClients && x->v.deadflag != DEAD_NO) continue;
		float t; if (ray_box(s, d, x->v.absmin, x->v.absmax, t) && t < best) { best = t; hit = x; Vector p = s + d * t; hg = (p.z > x->v.absmax.z - 12) ? 1 : 2; } }
	tr->flFraction = best; tr->vecEndPos = s + d * best; tr->pHit = hit; tr->iHitgroup = hg; tr->vecPlaneNormal = Vector(0, 0, 1);
	tr->fInOpen = 1;
}
static edict_t *E_FindEntityByString(edict_t *start, const char *f, const char *v) {
	for (int i = IdxOf(start) + 1; i < MAXE; i++) { edict_t *x = &ed[i]; if (x->free) continue; int sv = 0;
		if (!strcmp(f, "classname")) sv = x->v.classname; else if (!strcmp(f, "targetname")) sv = x->v.targetname; else if (!strcmp(f, "target")) sv = x->v.target; else if (!strcmp(f, "model")) sv = x->v.model; else if (!strcmp(f, "netname")) sv = x->v.netname;
		if (sv && !strcmp(STRING(sv), v)) return x; }
	return &ed[0];
}
static edict_t *E_FindEntityInSphere(edict_t *start, const float *o, float r) {
	for (int i = IdxOf(start) + 1; i < MAXE; i++) { edict_t *x = &ed[i]; if (x->free || !x->v.classname) continue; Vector c = (x->v.absmin + x->v.absmax) * 0.5f; if (x->v.absmin == x->v.absmax) c = x->v.origin;
		Vector dd = c - Vector(o[0], o[1], o[2]); if (dd.Length() <= r) return x; }
	return &ed[0];
}
static void E_Alert(ALERT_TYPE t, char *f, ...) { if (t != at_error && t != at_warning && !getenv("ALERT")) return; char b[1024]; va_list a; va_start(a, f); vsnprintf(b, sizeof b, f, a); va_end(a); printf("[alert %d] %s", (int)t, b); }
static int E_RegUserMsg(const char *n, int) { auto i = msgids.find(n); if (i != msgids.end()) return i->second; int id = next_msg++; msgids[n] = id; msgnames[id] = n; return id; }
static cvar_t *E_CVarGetPointer(const char *n) { auto i = cvars.find(n); if (i != cvars.end()) return i->second;
	cvar_t *c = new cvar_t{strdup(n), strdup("0"), 0, 0, nullptr}; cvars[n] = c; return c; }

static mutil_funcs_t mu;
static void MM_Log(plid_t, const char *f, ...) { char b[1024]; va_list a; va_start(a, f); vsnprintf(b, 1024, f, a); va_end(a); printf("[log] %s\n", b); }
static int MM_MsgId(plid_t, const char *n, int *sz) { if (sz) *sz = -1; auto i = msgids.find(n); return i == msgids.end() ? 0 : i->second; }
static const char *MM_MsgName(plid_t, int id, int *sz) { if (sz) *sz = -1; auto i = msgnames.find(id); return i == msgnames.end() ? nullptr : i->second.c_str(); }
static const char *MM_GameInfo(plid_t, ginfo_t t) { if (t == GINFO_DLL_FULLPATH || t == GINFO_REALDLL_FULLPATH) return g_game_path.c_str(); if (t == GINFO_GAMEDIR) return "gd"; if (t == GINFO_NAME) return "cstrike"; return ""; }

static void setup_engine() {
	memset(&real, 0, sizeof real);
	real.pfnGetGameDir = [](char *d) { strcpy(d, "gd"); };
	real.pfnServerPrint = [](const char *s) { printf("[srv] %s", s); };
	real.pfnAddServerCommand = [](char *n, void (*f)()) { svrcmds[n] = f; };
	real.pfnCVarGetPointer = E_CVarGetPointer;
	real.pfnCVarRegister = [](cvar_t *c) { auto i = cvars.find(c->name); if (i != cvars.end()) { c->string = i->second->string; c->value = i->second->value; } cvars[c->name] = c; c->value = (float)atof(c->string); };
	real.pfnCVarGetString = [](const char *n) -> const char * { return E_CVarGetPointer(n)->string; };
	real.pfnCVarGetFloat = [](const char *n) -> float { return (float)atof(E_CVarGetPointer(n)->string); };
	real.pfnCVarSetString = [](const char *n, const char *v) { cvar_t *c = E_CVarGetPointer(n); c->string = strdup(v); c->value = (float)atof(v); };
	real.pfnCVarSetFloat = [](const char *n, float v) { char b[32]; snprintf(b, 32, "%g", v); cvar_t *c = E_CVarGetPointer(n); c->string = strdup(b); c->value = v; };
	real.pfnCvar_DirectSet = [](cvar_t *c, char *v) { c->string = strdup(v); c->value = (float)atof(v); };
	real.pfnCmd_Argc = []() { return (int)g_argv.size(); };
	real.pfnCmd_Argv = [](int i) -> const char * { return i < (int)g_argv.size() ? g_argv[i].c_str() : ""; };
	real.pfnCmd_Args = []() -> const char * { return g_args.c_str(); };
	real.pfnPEntityOfEntIndex = PEnt; real.pfnIndexOfEdict = IdxOf;
	real.pfnEntOffsetOfPEntity = [](const edict_t *e) -> int { return (int)((const char *)e - (const char *)ed); };
	real.pfnPEntityOfEntOffset = [](int o) -> edict_t * { return (edict_t *)((char *)ed + o); };
	real.pfnGetVarsOfEnt = [](edict_t *e) -> entvars_t * { return &e->v; };
	real.pfnPvAllocEntPrivateData = [](edict_t *e, int32 sz) -> void * { if (e->pvPrivateData) free(e->pvPrivateData); e->pvPrivateData = calloc(1, sz + 64); return e->pvPrivateData; };
	real.pfnPvEntPrivateData = [](edict_t *e) -> void * { return e ? e->pvPrivateData : nullptr; };
	real.pfnFreeEntPrivateData = [](edict_t *e) { if (e->pvPrivateData) { free(e->pvPrivateData); e->pvPrivateData = nullptr; } };
	real.pfnCreateEntity = E_CreateEntity; real.pfnCreateNamedEntity = E_CreateNamedEntity; real.pfnRemoveEntity = E_RemoveEntity;
	real.pfnSetModel = E_SetModel; real.pfnSetSize = E_SetSize; real.pfnSetOrigin = E_SetOrigin;
	real.pfnModelIndex = [](const char *m) { return pc_index(m); };
	real.pfnPrecacheModel = [](char *m) { return pc_index(m); }; real.pfnPrecacheSound = [](char *m) { return pc_index(m); };
	real.pfnPrecacheGeneric = [](char *m) { return pc_index(m); }; real.pfnPrecacheEvent = [](int, const char *m) -> unsigned short { return (unsigned short)pc_index(m); };
	real.pfnAllocString = [](const char *s) -> int { return (int)(strdup(s) - strbase); };
	real.pfnSzFromIndex = [](int i) -> const char * { return strbase + i; };
	real.pfnFindEntityByString = E_FindEntityByString; real.pfnFindEntityInSphere = E_FindEntityInSphere;
	real.pfnTraceLine = E_TraceLine;
	real.pfnTraceHull = [](const float *a, const float *b, int nm, int, edict_t *ig, TraceResult *tr) { E_TraceLine(a, b, nm, ig, tr); };
	real.pfnPointContents = [](const float *) { return -1; };
	real.pfnVecToYaw = [](const float *v) { return yaw_of(v[0], v[1]); }; real.pfnVecToAngles = E_VecToAngles; real.pfnAngleVectors = E_AngleVectors;
	real.pfnMakeVectors = [](const float *a) { E_AngleVectors(a, gv.v_forward, gv.v_right, gv.v_up); };
	real.pfnMoveToOrigin = E_MoveToOrigin; real.pfnWalkMove = E_WalkMove; real.pfnDropToFloor = E_DropToFloor; real.pfnChangeYaw = E_ChangeYaw;
	real.pfnEntIsOnFloor = [](edict_t *e) { return (e->v.flags & FL_ONGROUND) ? 1 : 0; };
	real.pfnGetModelPtr = [](edict_t *e) -> void * { return e && e->v.model && !strcmp(STRING(e->v.model), "models/mc_test.mdl") ? g_mdl.data() : nullptr; };
	real.pfnGetAttachment = [](const edict_t *e, int n, float *o, float *a) { o[0] = e->v.origin.x + 10; o[1] = e->v.origin.y; o[2] = e->v.origin.z + 50 + n; if (a) a[0] = a[1] = a[2] = 0; };
	real.pfnGetBonePosition = [](const edict_t *e, int n, float *o, float *a) { o[0] = e->v.origin.x; o[1] = e->v.origin.y; o[2] = e->v.origin.z + 20 * (n + 1); if (a) a[0] = a[1] = a[2] = 0; };
	real.pfnRandomLong = [](int32 lo, int32 hi) -> int32 { return hi <= lo ? lo : lo + rand() % (hi - lo + 1); };
	real.pfnRandomFloat = [](float lo, float hi) -> float { return lo + (hi - lo) * (rand() / (float)RAND_MAX); };
	real.pfnTime = []() -> float { return gv.time; };
	real.pfnAlertMessage = E_Alert; real.pfnRegUserMsg = E_RegUserMsg;
	real.pfnDecalIndex = [](const char *) { return 3; };
	real.pfnGetPlayerUserId = [](edict_t *e) { return 100 + IdxOf(e); };
	real.pfnGetPlayerAuthId = [](edict_t *e) -> const char * { static char b[32]; snprintf(b, 32, "STEAM_0:0:%d", 1000 + IdxOf(e)); return b; };
	real.pfnGetInfoKeyBuffer = [](edict_t *e) -> char * { int i = e ? IdxOf(e) : 0; return i >= 0 && i < 34 ? g_info[i] : g_info[0]; };
	real.pfnInfoKeyValue = [](char *buf, char *k) -> char * { static char out[128]; out[0] = 0; std::string b = buf ? buf : "", key = std::string("\\") + k + "\\"; size_t p = b.find(key);
		if (p != std::string::npos) { size_t s = p + key.size(), e = b.find('\\', s); strncpy(out, b.substr(s, e == std::string::npos ? e : e - s).c_str(), 127); } return out; };
	real.pfnSetClientKeyValue = [](int, char *, char *, char *) {};
	real.pfnServerCommand = [](char *c) { if (getenv("SHOWCMD")) printf("[SERVERCMD] %s", c); };
	real.pfnServerExecute = []() {};
	real.pfnIsMapValid = [](char *) { return 1; };
	real.pfnIsDedicatedServer = []() { return 1; };
	real.pfnGetPlayerStats = [](const edict_t *, int *p, int *l) { *p = 5; *l = 0; };
	real.pfnLoadFileForMe = [](char *, int *len) -> byte * { if (len) *len = 0; return nullptr; };
	real.pfnFreeFile = [](void *) {};
	real.pfnMessageBegin = [](int d, int t, const float *, edict_t *e) { if (g_open) { g_engine_errors++; printf("!!! ENGINE ERROR: MESSAGE_BEGIN while a message is open (type %d)\n", t); }
		if ((d == MSG_ONE || d == MSG_ONE_UNRELIABLE) && (!e || IdxOf(e) < 1 || IdxOf(e) > gv.maxClients)) { g_engine_errors++; printf("!!! ENGINE ERROR: MSG_ONE without a client (type %d)\n", t); }
		g_open = true; g_cur_type = t; g_cur_msg = msgnames.count(t) ? msgnames[t] : std::to_string(t); };
	real.pfnMessageEnd = []() { if (!g_open) { g_engine_errors++; printf("!!! ENGINE ERROR: MESSAGE_END without begin\n"); } g_open = false; g_msg_count++; g_msg_by_type[g_cur_type]++;
		if (getenv("SHOWMSG")) printf("[msg] %s\n", g_cur_msg.c_str()); };
	real.pfnClientPrintf = [](edict_t *, PRINT_TYPE, const char *s) { if (getenv("SHOWMSG")) printf("[cprint] %s", s); };
	real.pfnGetCurrentPlayer = []() { return -1; };
	real.pfnGetPhysicsInfoString = [](const edict_t *) -> const char * { return ""; };
	real.pfnGetPhysicsKeyValue = [](const edict_t *, const char *) -> const char * { return ""; };
	real.pfnTraceTexture = [](edict_t *, const float *, const float *) -> const char * { return "default"; };
	real.pfnNameForFunction = [](uint32) -> const char * { return ""; };
	real.pfnGetPlayerWONId = [](edict_t *) -> unsigned int { return 0; };
	real.pfnCanSkipPlayer = [](const edict_t *) { return 0; };
	real.pfnCheckVisibility = [](const edict_t *, unsigned char *) { return 1; };
	{ void **slot = (void **)&real; size_t n = sizeof(real) / sizeof(void *); static void *stub = (void *)+[]() -> int { return 0; }; for (size_t i = 0; i < n; ++i) if (!slot[i]) slot[i] = stub; }
	memcpy(&g_engfuncs, &real, sizeof real); memcpy(&game_eng, &real, sizeof real);
	game_eng.pfnMessageBegin = w_pfnMessageBegin; game_eng.pfnMessageEnd = w_pfnMessageEnd; game_eng.pfnWriteByte = w_pfnWriteByte; game_eng.pfnWriteChar = w_pfnWriteChar;
	game_eng.pfnWriteShort = w_pfnWriteShort; game_eng.pfnWriteLong = w_pfnWriteLong; game_eng.pfnWriteAngle = w_pfnWriteAngle; game_eng.pfnWriteCoord = w_pfnWriteCoord;
	game_eng.pfnWriteString = w_pfnWriteString; game_eng.pfnWriteEntity = w_pfnWriteEntity; game_eng.pfnSetModel = w_pfnSetModel; game_eng.pfnEmitSound = w_pfnEmitSound;
	game_eng.pfnPlaybackEvent = w_pfnPlaybackEvent;
	mu.pfnLogMessage = mu.pfnLogError = mu.pfnLogConsole = mu.pfnLogDeveloper = MM_Log; mu.pfnGetUserMsgID = MM_MsgId; mu.pfnGetUserMsgName = MM_MsgName; mu.pfnGetGameInfo = MM_GameInfo;
}
static bool load_plugin(const char *path, gamedll_funcs_t *gd) {
	void *h = dlopen(path, RTLD_NOW); if (!h) { printf("dlopen %s: %s\n", path, dlerror()); return false; }
	auto Give = (void (*)(enginefuncs_t *, globalvars_t *))dlsym(h, "GiveFnptrsToDll"); auto Query = (int (*)(char *, plugin_info_t **, mutil_funcs_t *))dlsym(h, "Meta_Query");
	auto Attach = (int (*)(PLUG_LOADTIME, META_FUNCTIONS *, meta_globals_t *, gamedll_funcs_t *))dlsym(h, "Meta_Attach");
	Give(&real, &gv); plugin_info_t *pi; Query((char *)"5:13", &pi, &mu);
	Plug *p = new Plug; p->name = pi->name; memset(&p->mf, 0, sizeof p->mf);
	if (!Attach(PT_STARTUP, &p->mf, &g_mg, gd)) { printf("attach of %s FAILED\n", pi->name); return false; }
	int ver = 140; if (p->mf.pfnGetEntityAPI2) p->mf.pfnGetEntityAPI2(&p->pre, &ver); ver = 140; if (p->mf.pfnGetEntityAPI2_Post) p->mf.pfnGetEntityAPI2_Post(&p->post, &ver);
	int ev = 138; if (p->mf.pfnGetEngineFunctions) p->mf.pfnGetEngineFunctions(&p->epre, &ev); ev = 138; if (p->mf.pfnGetEngineFunctions_Post) p->mf.pfnGetEngineFunctions_Post(&p->epost, &ev);
	g_plugs.push_back(p); printf("== loaded plugin %s %s\n", pi->name, pi->version); return true;
}
static void run_frames(int n, float dt) {
	for (int f = 0; f < n; f++) {
		gv.time += dt; gv.frametime = dt; D_StartFrame();
		for (int i = 1; i <= gv.maxClients; i++) if (!ed[i].free && ed[i].pvPrivateData && (ed[i].v.flags & FL_CLIENT)) { D_PreThink(&ed[i]); D_PostThink(&ed[i]);
			clientdata_s cd; memset(&cd, 0, sizeof cd); D_UpdateClientData(&ed[i], 1, &cd); }
		for (int i = 1; i < MAXE; i++) { edict_t *e = &ed[i]; if (e->free || !e->pvPrivateData) continue;
			if (i > gv.maxClients) {
				if (e->v.movetype == MOVETYPE_TOSS || e->v.movetype == MOVETYPE_STEP) { if (!(e->v.flags & FL_ONGROUND) && e->v.origin.z + e->v.mins.z > 0) { e->v.velocity.z -= 800 * dt * (e->v.gravity ? e->v.gravity : 1); }
					e->v.origin = e->v.origin + e->v.velocity * dt; if (e->v.origin.z + e->v.mins.z <= 0) { e->v.origin.z = -e->v.mins.z; e->v.velocity = Vector(0, 0, 0); e->v.flags |= FL_ONGROUND; } link_ent(e); }
				if (e->v.movetype == MOVETYPE_FLY) { e->v.origin = e->v.origin + e->v.velocity * dt; link_ent(e); }
			}
			if (e->v.nextthink > 0 && e->v.nextthink <= gv.time) { e->v.nextthink = 0; D_Think(e); } }
		// touches: triggers vs players
		for (int i = gv.maxClients + 1; i < MAXE; i++) { edict_t *t = &ed[i]; if (t->free || t->v.solid != SOLID_TRIGGER) continue;
			for (int p = 1; p <= gv.maxClients; p++) { edict_t *pl = &ed[p]; if (pl->free || !pl->pvPrivateData || pl->v.deadflag != DEAD_NO || pl->v.health <= 0) continue;
				if (pl->v.absmax.x >= t->v.absmin.x && pl->v.absmin.x <= t->v.absmax.x && pl->v.absmax.y >= t->v.absmin.y && pl->v.absmin.y <= t->v.absmax.y && pl->v.absmax.z >= t->v.absmin.z && pl->v.absmin.z <= t->v.absmax.z) { D_Touch(t, pl); if (t->free) break; } } }
		for (int i = gv.maxClients + 1; i < MAXE; i++) if (!ed[i].free && (ed[i].v.flags & FL_KILLME)) E_RemoveEntity(&ed[i]);
	}
}
static void svr(std::vector<std::string> v) { g_argv = v; g_args = ""; for (size_t i = 1; i < v.size(); i++) g_args += (i > 1 ? " " : "") + v[i];
	auto it = svrcmds.find(v[0]); if (it == svrcmds.end()) { printf("!!! no server command %s\n", v[0].c_str()); return; } it->second(); }
static void client_cmd(int id, std::vector<std::string> v) { g_argv = v; g_args = ""; for (size_t i = 1; i < v.size(); i++) g_args += (i > 1 ? " " : "") + v[i]; D_ClientCommand(&ed[id]); }

int main(int argc, char **argv) {
	setvbuf(stdout, NULL, _IONBF, 0); srand(7);
	const char *game = getenv("GAMEDLL"); if (!game) { printf("set GAMEDLL=/path/cs.so\n"); return 1; } g_game_path = game;
	gv.maxClients = 32; gv.maxEntities = MAXE; gv.pStringBase = strbase; gv.time = 1.0f; gv.mapname = (int)(strdup("mc_testmap") - strbase); gv.deathmatch = 1; gpGlobals = &gv;
	for (int i = 0; i < MAXE; i++) { ed[i].free = 1; ed[i].v.pContainingEntity = &ed[i]; }
	ed[0].free = 0;
	build_mdl(); setup_engine();
	cvars["mp_roundtime"] = new cvar_t{(char *)"mp_roundtime", (char *)"2.5", 0, 2.5f, nullptr};
	cvars["mp_freezetime"] = new cvar_t{(char *)"mp_freezetime", (char *)"1", 0, 1, nullptr};
	cvars["sv_gravity"] = new cvar_t{(char *)"sv_gravity", (char *)"800", 0, 800, nullptr};
	cvars["sv_maxspeed"] = new cvar_t{(char *)"sv_maxspeed", (char *)"320", 0, 320, nullptr};
	g_game = dlopen(game, RTLD_NOW); if (!g_game) { printf("game dlopen: %s\n", dlerror()); return 1; }
	((void (*)(enginefuncs_t *, globalvars_t *))dlsym(g_game, "GiveFnptrsToDll"))(&game_eng, &gv);
	int ver = 140; memset(&g_dll, 0, sizeof g_dll); ((int (*)(DLL_FUNCTIONS *, int))dlsym(g_game, "GetEntityAPI"))(&g_dll, 140);
	ver = 1; memset(&g_newdll, 0, sizeof g_newdll); auto gnf = (int (*)(NEW_DLL_FUNCTIONS *, int *))dlsym(g_game, "GetNewDLLFunctions"); if (gnf) gnf(&g_newdll, &ver);
	printf("== game DLL loaded: %s\n", game);
	g_dll.pfnGameInit();
	static gamedll_funcs_t gd = {&g_dll, &g_newdll};
	if (!load_plugin("../addons/xmod/dlls/xmod_mm_i386.so", &gd)) return 1;
	if (!load_plugin(getenv("MCLIB") ? getenv("MCLIB") : "../addons/xmod/dlls/modcreator_mm_i386.so", &gd)) return 1;
	printf("== map start (worldspawn -> precache, game rules)\n");
	ed[0].v.classname = (int)(strdup("worldspawn") - strbase); ((void (*)(entvars_t *))dlsym(g_game, "worldspawn"))(&ed[0].v); D_Spawn(&ed[0]);
	{ edict_t *s = E_CreateNamedEntity((int)(strdup("info_player_start") - strbase)); s->v.origin = Vector(500, 0, 36); D_Spawn(s);
	  edict_t *t = E_CreateNamedEntity((int)(strdup("info_player_deathmatch") - strbase)); t->v.origin = Vector(-500, 0, 36); D_Spawn(t); }
	D_ServerActivate(ed, MAXE, gv.maxClients); run_frames(2, 0.05f);
	printf("== connect two players\n");
	const char *names[] = {"", "Andrew", "Bot Ion"};
	for (int i = 1; i <= 2; i++) { edict_t *e = &ed[i]; e->free = 0; memset(&e->v, 0, sizeof e->v); e->v.pContainingEntity = e; e->v.flags = FL_CLIENT;
		snprintf(g_info[i], 256, "\\name\\%s\\model\\urban\\_vgui_menus\\0\\_pw\\", names[i]); e->v.netname = (int)(strdup(names[i]) - strbase);
		char rej[128] = {0}; D_ClientConnect(e, names[i], "1.2.3.4:27005", rej); D_PutInServer(e); }
	run_frames(5, 0.05f);
	client_cmd(1, {"jointeam", "2"}); client_cmd(1, {"joinclass", "1"});
	client_cmd(2, {"jointeam", "1"}); client_cmd(2, {"joinclass", "1"});
	run_frames(80, 0.05f);
	printf("== player states: 1 hp=%.0f team=%d dead=%d | 2 hp=%.0f team=%d dead=%d\n", ed[1].v.health, ed[1].v.team, ed[1].v.deadflag, ed[2].v.health, ed[2].v.team, ed[2].v.deadflag);
	for (int ph = 1; ph <= 12; ph++) {
		char c[32]; snprintf(c, sizeof c, "mct_phase%d", ph); if (!svrcmds.count(c)) break;
		if (ph == 7) { printf("== client 1: drop\n"); client_cmd(1, {"drop"}); run_frames(1, 0.05f); }
		printf("== %s\n", c); svr({c}); run_frames(ph == 3 ? 120 : 60, 0.05f);
	}
	svr({"mct_report"});
	printf("== engine errors: %d, unprecached models: %d, messages sent: %d\n", g_engine_errors, g_unprecached, g_msg_count);
	D_ServerDeactivate();
	return (g_engine_errors || g_unprecached) ? 2 : 0;
}
