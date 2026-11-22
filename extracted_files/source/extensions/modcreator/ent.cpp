// ent.cpp - entities: virtual calls (like ExecuteHamB), KeyValue dispatch, engine movement, sight checks, extra entvars fields.
#include "mc.h"
#include <tuple>
#include <utility>
#include <unordered_map>

struct HamCbase;

edict_t *mc_edict(int i) {
	if (i < 0 || !gpGlobals || i >= gpGlobals->maxEntities) return nullptr;
	edict_t *e = INDEXENT(i);
	if (!e || e->free) return nullptr;
	if (i > 0 && i <= gpGlobals->maxClients) return e->pvPrivateData ? e : nullptr;   // player slot without a player
	return e;
}
void *mc_cbase(edict_t *e) { return e ? e->pvPrivateData : nullptr; }
edict_t *mc_edict_of_cbase(void *cb) {
	if (!cb) return nullptr;
	entvars_t *pev = *reinterpret_cast<entvars_t **>(reinterpret_cast<char *>(cb) + 4);
	return pev ? pev->pContainingEntity : nullptr;
}
void **mc_vtable(edict_t *e) { return e && e->pvPrivateData ? *reinterpret_cast<void ***>(e->pvPrivateData) : nullptr; }

// ---------------------------------------------------------------------------------------------------- virtual calls
// mc.ham(name, ent, ...) calls the entity's virtual function through its vtable, exactly like AMXX ExecuteHamB:
// xmod's ham:* hooks DO fire (they live in the vtable). Signatures come from xmod's own ham table.
template <class T> struct Conv;
template <> struct Conv<int> { static int get(const Args &a, int i, bool &) { return (int)a.i(i); } };
template <> struct Conv<float> { static float get(const Args &a, int i, bool &) { return (float)a.f(i); } };
template <> struct Conv<const char *> { static const char *get(const Args &a, int i, bool &) { return a.s(i, ""); } };
template <> struct Conv<entvars_t *> { static entvars_t *get(const Args &a, int i, bool &bad) {
	int x = (int)a.i(i); if (x == 0 && !a.has(i)) return nullptr; edict_t *e = mc_edict(x); if (!e) { bad = x != 0; return x == 0 ? &INDEXENT(0)->v : nullptr; } return &e->v; } };
template <> struct Conv<HamCbase *> { static HamCbase *get(const Args &a, int i, bool &bad) {
	int x = (int)a.i(i); edict_t *e = mc_edict(x); if (!e || !e->pvPrivateData) { bad = a.has(i) && x != 0; return nullptr; } return reinterpret_cast<HamCbase *>(e->pvPrivateData); } };
template <class R> struct Ret;
template <> struct Ret<void> {};
template <> struct Ret<int> { static XmodValue *mk(int v) { return g_xmod->val_int(v); } };
template <> struct Ret<float> { static XmodValue *mk(float v) { return g_xmod->val_num(v); } };
template <> struct Ret<const char *> { static XmodValue *mk(const char *v) { return g_xmod->val_cstr(v ? v : ""); } };
template <> struct Ret<HamCbase *> { static XmodValue *mk(HamCbase *v) { return g_xmod->val_int(mc_index(mc_edict_of_cbase(v))); } };

template <class R, class... A> struct VCall {
	using Fn = R(MC_CC *)(void *, MC_DUMMY A...);
	template <size_t... I> static void run(const Args &a, void *self, void *fn, std::index_sequence<I...>) {
		bool bad = false;
		std::tuple<A...> t{Conv<A>::get(a, 2 + (int)I, bad)...};
		if (bad) { a.err("mc.ham: an entity argument is not a valid entity"); return; }
		if constexpr (std::is_void<R>::value) { reinterpret_cast<Fn>(fn)(self, MC_DUMMY_ARG std::get<I>(t)...); a.ret_nil(); }
		else { R r = reinterpret_cast<Fn>(fn)(self, MC_DUMMY_ARG std::get<I>(t)...); a.ret(Ret<R>::mk(r)); }
	}
	static void call(const Args &a, void *self, void *fn) { run(a, self, fn, std::index_sequence_for<A...>{}); }
};
struct HamDef { const char *name; int idx; void (*call)(const Args &, void *, void *); };
#ifdef _WIN32
#define HAM_IDX(W, L) W
#else
#define HAM_IDX(W, L) L
#endif
#define HAM(NAME, W, L, R, ...) { #NAME, HAM_IDX(W, L), &VCall<R, ##__VA_ARGS__>::call },
static const HamDef g_hams[] = {
#include "../../src/ham_list.inc"
	{ nullptr, 0, nullptr } };
#undef HAM
static const HamDef *ham_find(const char *n) {
	static std::unordered_map<std::string, const HamDef *> m;
	if (m.empty()) for (const HamDef *d = g_hams; d->name; ++d) m[d->name] = d;
	auto it = m.find(n); return it == m.end() ? nullptr : it->second;
}
int mc_vidx(const char *n) { const HamDef *d = ham_find(n); return d ? d->idx : -1; }
const char *mc_ham_names() {
	static std::string s; if (s.empty()) for (const HamDef *d = g_hams; d->name; ++d) { if (!s.empty()) s += ","; s += d->name; } return s.c_str();
}
static void f_ham(XmodCall *c, void *) {
	Args a(c); const char *n = a.s(0);
	if (!n) { a.err("usage: xmod.mc.ham(name, ent, ...)"); return; }
	const HamDef *d = ham_find(n); if (!d) { a.err("mc.ham: unknown function '%s' (see xmod.mc.ham_list())", n); return; }
	edict_t *e = mc_edict((int)a.i(1)); void **vt = mc_vtable(e);
	if (!vt) { a.err("mc.ham(%s): entity %d is not valid / has no private data", n, (int)a.i(1)); return; }
	d->call(a, e->pvPrivateData, vt[d->idx]);
}
static void f_ham_list(XmodCall *c, void *) { Args(c).ret_str(mc_ham_names()); }

// ---------------------------------------------------------------------------------------------------- KeyValue
// mc.keyvalue(ent, key, value [, classname]) -> handled (bool). Like DispatchKeyValue: map-entity keys BEFORE spawn.
static void f_keyvalue(XmodCall *c, void *) {
	Args a(c); edict_t *e = mc_edict((int)a.i(0)); const char *k = a.s(1), *v = a.s(2);
	if (!e || !k || !v) { a.err("usage: xmod.mc.keyvalue(ent, key, value [, classname])"); return; }
	KeyValueData kvd; std::string cls = a.s(3, e->v.classname ? STRING(e->v.classname) : "");
	kvd.szClassName = const_cast<char *>(cls.c_str()); kvd.szKeyName = const_cast<char *>(k); kvd.szValue = const_cast<char *>(v); kvd.fHandled = 0;
	if (!strcmp(k, "classname")) { e->v.classname = ALLOC_STRING(v); a.ret_bool(true); return; }
	gpGamedllFuncs->dllapi_table->pfnKeyValue(e, &kvd);
	a.ret_bool(kvd.fHandled != 0);
}

// ---------------------------------------------------------------------------------------------------- movement (engine)
// mc.walk_move(ent, yaw, dist [, mode=0 WALKMOVE_NORMAL|1 WORLDONLY|2 CHECKONLY]) -> moved
static void f_walk_move(XmodCall *c, void *) { Args a(c); edict_t *e = mc_edict((int)a.i(0)); if (!e) { a.err("mc.walk_move: bad entity"); return; }
	a.ret_bool(WALK_MOVE(e, (float)a.f(1), (float)a.f(2), (int)a.i(3, 0)) != 0); }
// mc.move_to_origin(ent, x, y, z, dist [, type=0 MOVE_NORMAL|1 MOVE_STRAFE]) - one step of HL monster navigation (MOVETYPE_STEP/FLY)
static void f_move_to_origin(XmodCall *c, void *) { Args a(c); edict_t *e = mc_edict((int)a.i(0)); if (!e) { a.err("mc.move_to_origin: bad entity"); return; }
	float g[3] = {(float)a.f(1), (float)a.f(2), (float)a.f(3)}; MOVE_TO_ORIGIN(e, g, (float)a.f(4), (int)a.i(5, 0)); a.ret_nil(); }
// mc.drop_to_floor(ent) -> 1 on floor, 0 in air, -1 stuck in solid
static void f_drop_to_floor(XmodCall *c, void *) { Args a(c); edict_t *e = mc_edict((int)a.i(0)); if (!e) { a.err("mc.drop_to_floor: bad entity"); return; } a.ret_int(DROP_TO_FLOOR(e)); }
static void f_on_floor(XmodCall *c, void *) { Args a(c); edict_t *e = mc_edict((int)a.i(0)); if (!e) { a.err("mc.on_floor: bad entity"); return; } a.ret_bool(g_engfuncs.pfnEntIsOnFloor(e) != 0); }
// mc.change_yaw(ent) - turn towards ideal_yaw by yaw_speed (engine ChangeYaw), returns the new yaw
static void f_change_yaw(XmodCall *c, void *) { Args a(c); edict_t *e = mc_edict((int)a.i(0)); if (!e) { a.err("mc.change_yaw: bad entity"); return; } g_engfuncs.pfnChangeYaw(e); a.ret_num(e->v.angles.y); }
static void f_vec_to_yaw(XmodCall *c, void *) { Args a(c); float v[3] = {(float)a.f(0), (float)a.f(1), (float)a.f(2)}; a.ret_num(VEC_TO_YAW(v)); }
static void f_vec_to_angles(XmodCall *c, void *) { Args a(c); float v[3] = {(float)a.f(0), (float)a.f(1), (float)a.f(2)}, o[3]; VEC_TO_ANGLES(v, o); a.ret_vec(o); }
// mc.angle_vectors(pitch, yaw, roll) -> {forward}, {right}, {up} as one flat array of 9 numbers
static void f_angle_vectors(XmodCall *c, void *) { Args a(c); float ang[3] = {(float)a.f(0), (float)a.f(1), (float)a.f(2)}, f[3], r[3], u[3];
	g_engfuncs.pfnAngleVectors(ang, f, r, u); XmodValue *o = g_xmod->val_array(); for (float *p : {f, r, u}) for (int k = 0; k < 3; k++) g_xmod->val_push(o, g_xmod->val_num(p[k])); a.ret(o); }
// mc.face(ent, x, y, z [, instant=false]) - sets ideal_yaw towards a point (and angles.y if instant)
static void f_face(XmodCall *c, void *) { Args a(c); edict_t *e = mc_edict((int)a.i(0)); if (!e) { a.err("mc.face: bad entity"); return; }
	float d[3] = {(float)a.f(1) - e->v.origin.x, (float)a.f(2) - e->v.origin.y, (float)a.f(3) - e->v.origin.z}; float y = VEC_TO_YAW(d);
	e->v.ideal_yaw = y; if (a.b(4)) e->v.angles.y = y; a.ret_num(y); }

static void eyes(edict_t *e, float *o) { o[0] = e->v.origin.x + e->v.view_ofs.x; o[1] = e->v.origin.y + e->v.view_ofs.y; o[2] = e->v.origin.z + e->v.view_ofs.z; }
// mc.visible(ent, target) - line of sight eyes->eyes (ignores glass/monsters like FVisible: ignore_monsters, ignore_glass)
static void f_visible(XmodCall *c, void *) { Args a(c); edict_t *e = mc_edict((int)a.i(0)), *t = mc_edict((int)a.i(1)); if (!e || !t) { a.err("mc.visible: bad entity"); return; }
	if ((t->v.flags & FL_NOTARGET)) { a.ret_bool(false); return; }
	float s[3], d[3]; eyes(e, s); eyes(t, d); TraceResult tr; TRACE_LINE(s, d, 1 | 0x100, e, &tr);
	a.ret_bool(tr.flFraction >= 1.0f || tr.pHit == t); }
// mc.visible_point(ent, x, y, z)
static void f_visible_point(XmodCall *c, void *) { Args a(c); edict_t *e = mc_edict((int)a.i(0)); if (!e) { a.err("mc.visible_point: bad entity"); return; }
	float s[3], d[3] = {(float)a.f(1), (float)a.f(2), (float)a.f(3)}; eyes(e, s); TraceResult tr; TRACE_LINE(s, d, 1 | 0x100, e, &tr); a.ret_bool(tr.flFraction >= 1.0f); }
// mc.in_view_cone(ent, x, y, z [, fov_degrees=90])
static void f_in_view_cone(XmodCall *c, void *) { Args a(c); edict_t *e = mc_edict((int)a.i(0)); if (!e) { a.err("mc.in_view_cone: bad entity"); return; }
	float dx = (float)a.f(1) - e->v.origin.x, dy = (float)a.f(2) - e->v.origin.y, len = sqrtf(dx * dx + dy * dy);
	if (len < 0.001f) { a.ret_bool(true); return; }
	float yaw = e->v.angles.y * (float)M_PI / 180.f, dot = (dx * cosf(yaw) + dy * sinf(yaw)) / len;
	a.ret_bool(dot >= cosf((float)a.f(4, 90.0) * 0.5f * (float)M_PI / 180.f)); }
// mc.illum(ent) -> light level 0..255 at the entity (engine GetEntityIllum)
static void f_illum(XmodCall *c, void *) { Args a(c); edict_t *e = mc_edict((int)a.i(0)); if (!e) { a.err("mc.illum: bad entity"); return; } a.ret_int(GETENTITYILLUM(e)); }

// ---------------------------------------------------------------------------------------------------- extra entvars fields
// mc.ev(ent, field [, value...]) - entvars fields xmod.ent.get/set does not have: ideal_yaw yaw_speed pitch_speed idealpitch
// weaponanim controller0..3 blending0..1 dmg_take dmg_save light_level air_finished pain_finished radsuit_finished
// teleport_time gamestate impacttime starttime flFallVelocity bInDuck flTimeStepSound flSwimTime flDuckTime
// iStepLeft pushmsec watertype? -> use xmod.ent for the others. Returns the (new) value.
struct EvF { const char *n; int t; size_t off; };   // t: 0 float, 1 int, 2 byte
#define EF(n) {#n, 0, offsetof(entvars_t, n)}
#define EI(n) {#n, 1, offsetof(entvars_t, n)}
static const EvF g_ev[] = {
	EF(ideal_yaw), EF(yaw_speed), EF(pitch_speed), EF(idealpitch), EI(weaponanim), EF(dmg_take), EF(dmg_save), EI(light_level),
	EF(air_finished), EF(pain_finished), EF(radsuit_finished), EF(teleport_time), EI(gamestate), EF(impacttime), EF(starttime),
	EF(flFallVelocity), EI(bInDuck), EI(flTimeStepSound), EI(flSwimTime), EI(flDuckTime), EI(iStepLeft), EI(pushmsec), EF(armortype), EI(fixangle), EF(maxspeed), EI(deadflag),
	{"controller0", 2, offsetof(entvars_t, controller) + 0}, {"controller1", 2, offsetof(entvars_t, controller) + 1},
	{"controller2", 2, offsetof(entvars_t, controller) + 2}, {"controller3", 2, offsetof(entvars_t, controller) + 3},
	{"blending0", 2, offsetof(entvars_t, blending) + 0}, {"blending1", 2, offsetof(entvars_t, blending) + 1},
	{nullptr, 0, 0}};
static void f_ev(XmodCall *c, void *) {
	Args a(c); edict_t *e = mc_edict((int)a.i(0)); const char *n = a.s(1);
	if (!e || !n) { a.err("usage: xmod.mc.ev(ent, field [, value])"); return; }
	for (const EvF *f = g_ev; f->n; ++f) if (!strcmp(f->n, n)) {
		char *p = reinterpret_cast<char *>(&e->v) + f->off;
		if (a.has(2)) { if (f->t == 0) *reinterpret_cast<float *>(p) = (float)a.f(2); else if (f->t == 1) *reinterpret_cast<int *>(p) = (int)a.i(2); else *reinterpret_cast<byte *>(p) = (byte)a.i(2); }
		if (f->t == 0) a.ret_num(*reinterpret_cast<float *>(p)); else if (f->t == 1) a.ret_int(*reinterpret_cast<int *>(p)); else a.ret_int(*reinterpret_cast<byte *>(p));
		return;
	}
	a.err("mc.ev: unknown field '%s'", n);
}
// mc.alloc_string(s) -> string_t (int) for raw pdata writes; mc.string(i) -> text
static void f_alloc_string(XmodCall *c, void *) { Args a(c); a.ret_int((int)ALLOC_STRING(a.s(0, ""))); }
static void f_string(XmodCall *c, void *) { Args a(c); a.ret_str(STRING((int)a.i(0))); }
// mc.entity_count() -> used, max
static void f_entity_count(XmodCall *c, void *) { Args a(c); int n = 0; for (int i = 0; i < gpGlobals->maxEntities; i++) { edict_t *e = INDEXENT(i); if (e && !e->free) n++; }
	XmodValue *r = g_xmod->val_array(); g_xmod->val_push(r, g_xmod->val_int(n)); g_xmod->val_push(r, g_xmod->val_int(gpGlobals->maxEntities)); a.ret(r); }
// mc.create(classname) -> ent: the game DLL's own entity (CREATE_NAMED_ENTITY), private data allocated, NOT spawned yet
static void f_create(XmodCall *c, void *) { Args a(c); const char *cls = a.s(0); if (!cls) { a.err("usage: xmod.mc.create(classname)"); return; }
	edict_t *e = CREATE_NAMED_ENTITY(ALLOC_STRING(cls)); a.ret_int(e && !FNullEnt(e) ? ENTINDEX(e) : 0); }

bool reg_ent() {
	return mc_reg("ham", f_ham) && mc_reg("ham_list", f_ham_list) && mc_reg("keyvalue", f_keyvalue) && mc_reg("walk_move", f_walk_move)
		&& mc_reg("move_to_origin", f_move_to_origin) && mc_reg("drop_to_floor", f_drop_to_floor) && mc_reg("on_floor", f_on_floor)
		&& mc_reg("change_yaw", f_change_yaw) && mc_reg("vec_to_yaw", f_vec_to_yaw) && mc_reg("vec_to_angles", f_vec_to_angles)
		&& mc_reg("angle_vectors", f_angle_vectors) && mc_reg("face", f_face) && mc_reg("visible", f_visible) && mc_reg("visible_point", f_visible_point)
		&& mc_reg("in_view_cone", f_in_view_cone) && mc_reg("illum", f_illum) && mc_reg("ev", f_ev) && mc_reg("alloc_string", f_alloc_string)
		&& mc_reg("string", f_string) && mc_reg("entity_count", f_entity_count) && mc_reg("create", f_create);
}
