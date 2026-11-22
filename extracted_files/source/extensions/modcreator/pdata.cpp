// pdata.cpp - NAMED private data: mc.pdata.get(ent, "m_iClip") instead of get_pdata_int(ent, 51, 4).
// 859 Counter-Strike members (AMX Mod X gamedata, same layout on vanilla CS and ReGameDLL), right offset for this OS,
// right type (int/float/bool/short/char/entity pointer/EHANDLE/vector/string). Arrays take a 0-based index.
#include "mc.h"
#include <unordered_map>

struct PRow { const char *cls, *mem; int type, count, win, lin, uns; };
#define MC_PDATA_ROW(C, M, T, N, W, L, U) {C, M, T, N, W, L, U},
static const PRow g_rows[] = {
#include "offsets.inc"
	{nullptr, nullptr, 0, 0, 0, 0, 0}};
#undef MC_PDATA_ROW
#define MC_RULES_ROW(C, M, T, N, W, L, U) {C, M, T, N, W, L, U},
static const PRow g_rules_rows[] = {
#include "offsets.inc"
	{nullptr, nullptr, 0, 0, 0, 0, 0}};
#undef MC_RULES_ROW

// bare member names resolve in this class order (the common bases first); "Class.member" is always exact
static const char *g_prio[] = {"CBasePlayer", "CBasePlayerWeapon", "CBasePlayerItem", "CBaseMonster", "CBaseToggle", "CBaseAnimating",
	"CBaseDelay", "CBaseEntity", "CWeaponBox", "CGrenade", "CHostage", "CBaseButton", "CBaseDoor", nullptr};
static int prio(const char *c) { for (int i = 0; g_prio[i]; i++) if (!strcmp(g_prio[i], c)) return i; return 100; }
typedef std::unordered_map<std::string, const PRow *> PMap;
static PMap build(const PRow *rows) {
	PMap m;
	for (const PRow *r = rows; r->cls; ++r) {
		m[std::string(r->cls) + "." + r->mem] = r;
		auto it = m.find(r->mem);
		if (it == m.end() || prio(r->cls) < prio(it->second->cls)) m[r->mem] = r;
	}
	return m;
}
static const PRow *lookup(const PRow *rows, const char *name) {
	static PMap ents, rules; PMap &m = rows == g_rows ? ents : rules;
	if (m.empty()) m = build(rows);
	auto it = m.find(name); return it == m.end() ? nullptr : it->second;
}
// ReGameDLL's CGameRules has two more members (char *m_GameDesc; bool m_bGameOver) than the original mp.dll / cs.so,
// so every CHalfLifeMultiplay member sits 8 bytes later (verified against ReGameDLL's cssdk headers, Windows and Linux).
// Entity classes keep the original layout.
static int rules_shift(const PRow *r) { return (r->cls[1] == 'H' && mc_regamedll()) ? 8 : 0; }   // "CHalfLifeMultiplay"
static bool find_in(const PRow *rows, const char *name, int *off, int *type, int *count, bool *uns) {
	const PRow *r = lookup(rows, name); if (!r) return false;
	*off = (MC_LINUX ? r->lin : r->win) + (rows == g_rules_rows ? rules_shift(r) : 0); *type = r->type; *count = r->count; *uns = r->uns != 0; return true;
}
bool mc_pdata_find(const char *n, int *o, int *t, int *c, bool *u) { return find_in(g_rows, n, o, t, c, u); }
bool mc_rules_find(const char *n, int *o, int *t, int *c, bool *u) { return find_in(g_rules_rows, n, o, t, c, u); }

PdOff g_pd;
static int off_of(const char *n) { int o, t, c; bool u; return mc_pdata_find(n, &o, &t, &c, &u) ? o : -1; }
void mc_pd_init() {
	g_pd.active_item = off_of("CBasePlayer.m_pActiveItem"); g_pd.player_items = off_of("CBasePlayer.m_rgpPlayerItems");
	g_pd.ammo = off_of("CBasePlayer.m_rgAmmo"); g_pd.next_attack = off_of("CBaseMonster.m_flNextAttack");
	g_pd.last_hitgroup = off_of("CBaseMonster.m_LastHitGroup"); g_pd.anim_ext = off_of("CBasePlayer.m_szAnimExtention");
	g_pd.wb_items = off_of("CWeaponBox.m_rgpPlayerItems"); g_pd.wb_ammo_names = off_of("CWeaponBox.m_rgiszAmmo"); g_pd.wb_ammo_count = off_of("CWeaponBox.m_rgAmmo");
	g_pd.w_player = off_of("CBasePlayerItem.m_pPlayer"); g_pd.w_next = off_of("CBasePlayerItem.m_pNext"); g_pd.w_id = off_of("CBasePlayerItem.m_iId");
	g_pd.w_next_primary = off_of("CBasePlayerWeapon.m_flNextPrimaryAttack"); g_pd.w_next_secondary = off_of("CBasePlayerWeapon.m_flNextSecondaryAttack");
	g_pd.w_idle = off_of("CBasePlayerWeapon.m_flTimeWeaponIdle"); g_pd.w_ammo_type = off_of("CBasePlayerWeapon.m_iPrimaryAmmoType");
	g_pd.w_clip = off_of("CBasePlayerWeapon.m_iClip"); g_pd.w_in_reload = off_of("CBasePlayerWeapon.m_fInReload");
}

static int type_size(int t) {
	switch (t) { case PT_BOOL: case PT_CHAR: return 1; case PT_SHORT: return 2; case PT_EHANDLE: return 8; case PT_VECTOR: return 12; default: return 4; }
}
XmodValue *mc_read_member(char *base, int type, int count, bool uns, int index) {
	if (type == PT_STRING) return g_xmod->val_cstr(base);     // char[count]
	if (index < 0 || index >= count) return nullptr;
	char *p = base + index * type_size(type);
	switch (type) {
	case PT_INT: return g_xmod->val_int(uns ? (int64_t)*reinterpret_cast<uint32_t *>(p) : *reinterpret_cast<int32_t *>(p));
	case PT_FLOAT: return g_xmod->val_num(*reinterpret_cast<float *>(p));
	case PT_BOOL: return g_xmod->val_bool(*reinterpret_cast<uint8_t *>(p) != 0);
	case PT_SHORT: return g_xmod->val_int(uns ? (int64_t)*reinterpret_cast<uint16_t *>(p) : *reinterpret_cast<int16_t *>(p));
	case PT_CHAR: return g_xmod->val_int(uns ? (int64_t)*reinterpret_cast<uint8_t *>(p) : *reinterpret_cast<int8_t *>(p));
	case PT_CBASE: return g_xmod->val_int(mc_index(mc_edict_of_cbase(*reinterpret_cast<void **>(p))));
	case PT_EHANDLE: { edict_t *e = *reinterpret_cast<edict_t **>(p); int serial = *reinterpret_cast<int *>(p + 4);
		return g_xmod->val_int(e && !e->free && e->serialnumber == serial ? ENTINDEX(e) : 0); }
	case PT_EDICT: { edict_t *e = *reinterpret_cast<edict_t **>(p); return g_xmod->val_int(e && !e->free ? ENTINDEX(e) : 0); }
	case PT_ENTVARS: { entvars_t *v = *reinterpret_cast<entvars_t **>(p); return g_xmod->val_int(v && v->pContainingEntity ? ENTINDEX(v->pContainingEntity) : 0); }
	case PT_VECTOR: return mc_vec(reinterpret_cast<float *>(p));
	case PT_STRINGINT: { int s = *reinterpret_cast<int *>(p); return g_xmod->val_cstr(s ? STRING(s) : ""); }
	}
	return nullptr;
}
bool mc_write_member(char *base, int type, int count, int index, const Args &a, int k, std::string &err) {
	if (type == PT_STRING) { const char *s = a.s(k, ""); size_t n = strlen(s); if (n >= (size_t)count) n = count - 1; memcpy(base, s, n); base[n] = 0; return true; }
	if (index < 0 || index >= count) { err = "index out of range"; return false; }
	char *p = base + index * type_size(type);
	switch (type) {
	case PT_INT: *reinterpret_cast<int32_t *>(p) = (int32_t)a.i(k); return true;
	case PT_FLOAT: *reinterpret_cast<float *>(p) = (float)a.f(k); return true;
	case PT_BOOL: *reinterpret_cast<uint8_t *>(p) = a.b(k) ? 1 : 0; return true;
	case PT_SHORT: *reinterpret_cast<int16_t *>(p) = (int16_t)a.i(k); return true;
	case PT_CHAR: *reinterpret_cast<int8_t *>(p) = (int8_t)a.i(k); return true;
	case PT_CBASE: { int x = (int)a.i(k); edict_t *e = x ? mc_edict(x) : nullptr; if (x && !e) { err = "not a valid entity"; return false; }
		*reinterpret_cast<void **>(p) = e ? e->pvPrivateData : nullptr; return true; }
	case PT_EHANDLE: { int x = (int)a.i(k); edict_t *e = x ? mc_edict(x) : nullptr; if (x && !e) { err = "not a valid entity"; return false; }
		*reinterpret_cast<edict_t **>(p) = e; *reinterpret_cast<int *>(p + 4) = e ? e->serialnumber : 0; return true; }
	case PT_EDICT: { int x = (int)a.i(k); edict_t *e = x ? mc_edict(x) : nullptr; *reinterpret_cast<edict_t **>(p) = e; return true; }
	case PT_ENTVARS: { int x = (int)a.i(k); edict_t *e = x ? mc_edict(x) : nullptr; *reinterpret_cast<entvars_t **>(p) = e ? &e->v : nullptr; return true; }
	case PT_VECTOR: { float *v = reinterpret_cast<float *>(p); v[0] = (float)a.f(k); v[1] = (float)a.f(k + 1); v[2] = (float)a.f(k + 2); return true; }
	case PT_STRINGINT: *reinterpret_cast<int *>(p) = ALLOC_STRING(a.s(k, "")); return true;
	}
	err = "type not writable"; return false;
}

// mc.pdata_get(ent, member [, index]) -> value (entity pointers come back as entity indexes, vectors as {x,y,z})
static void f_pdata_get(XmodCall *c, void *) {
	Args a(c); edict_t *e = mc_edict((int)a.i(0)); const char *n = a.s(1); int o, t, cnt; bool u;
	if (!e || !e->pvPrivateData || !n) { a.err("usage: xmod.mc.pdata_get(ent, member [, index]) - entity %d has no private data", (int)a.i(0)); return; }
	if (!mc_pdata_find(n, &o, &t, &cnt, &u)) { a.err("mc.pdata_get: unknown member '%s'", n); return; }
	XmodValue *v = mc_read_member(reinterpret_cast<char *>(e->pvPrivateData) + o, t, cnt, u, (int)a.i(2, 0));
	if (!v) { a.err("mc.pdata_get(%s): index out of range 0..%d", n, cnt - 1); return; }
	a.ret(v);
}
// mc.pdata_set(ent, member, value [, index])   (vectors: pdata_set(ent, member, x, y, z) ; arrays: index after the value)
static void f_pdata_set(XmodCall *c, void *) {
	Args a(c); edict_t *e = mc_edict((int)a.i(0)); const char *n = a.s(1); int o, t, cnt; bool u;
	if (!e || !e->pvPrivateData || !n) { a.err("usage: xmod.mc.pdata_set(ent, member, value [, index])"); return; }
	if (!mc_pdata_find(n, &o, &t, &cnt, &u)) { a.err("mc.pdata_set: unknown member '%s'", n); return; }
	std::string err; if (!mc_write_member(reinterpret_cast<char *>(e->pvPrivateData) + o, t, cnt, t == PT_VECTOR ? 0 : (int)a.i(3, 0), a, 2, err)) { a.err("mc.pdata_set(%s): %s", n, err.c_str()); return; }
	a.ret_nil();
}
static const char *g_tnames[] = {"int", "float", "bool", "short", "char", "entity", "ehandle", "edict", "entvars", "vector", "string", "string_t"};
// mc.pdata_info(member) -> {class, member, type, count, offset (bytes, this OS), offset4 (AMXX-style 4-byte units), linux_diff4}
static void f_pdata_info(XmodCall *c, void *) {
	Args a(c); const char *n = a.s(0); bool rules = a.b(1); const PRow *r = n ? lookup(rules ? g_rules_rows : g_rows, n) : nullptr;
	if (!r) { a.ret_nil(); return; }
	XmodValue *m = g_xmod->val_map(); int sh = rules ? rules_shift(r) : 0; int off = (MC_LINUX ? r->lin : r->win) + sh;
	g_xmod->val_set(m, "class", g_xmod->val_cstr(r->cls)); g_xmod->val_set(m, "member", g_xmod->val_cstr(r->mem));
	g_xmod->val_set(m, "type", g_xmod->val_cstr(g_tnames[r->type])); g_xmod->val_set(m, "count", g_xmod->val_int(r->count));
	g_xmod->val_set(m, "offset", g_xmod->val_int(off)); g_xmod->val_set(m, "windows", g_xmod->val_int(r->win + sh)); g_xmod->val_set(m, "linux", g_xmod->val_int(r->lin + sh));
	if (!rules && r->win % 4 == 0 && r->lin % 4 == 0) { g_xmod->val_set(m, "offset4", g_xmod->val_int(r->win / 4)); g_xmod->val_set(m, "linux_diff4", g_xmod->val_int((r->lin - r->win) / 4)); }
	a.ret(m);
}
// mc.pdata_list([class_or_prefix]) -> {"CBasePlayer.m_iTeam", ...}
static void f_pdata_list(XmodCall *c, void *) {
	Args a(c); const char *f = a.s(0, ""); bool rules = a.b(1); XmodValue *l = g_xmod->val_array();
	for (const PRow *r = rules ? g_rules_rows : g_rows; r->cls; ++r) {
		std::string full = std::string(r->cls) + "." + r->mem;
		if (!*f || full.compare(0, strlen(f), f) == 0 || strstr(r->mem, f)) g_xmod->val_push(l, g_xmod->val_cstr(full.c_str()));
	}
	a.ret(l);
}
// mc.raw_get(ent, byte_offset, type) / mc.raw_set(ent, byte_offset, type, value) - for members that are not in the table.
// type: "int" "float" "bool" "short" "char" "entity" "ehandle" "vector" "string_t"
static int type_of(const char *s) { if (!s) return -1; for (int i = 0; i < 12; i++) if (!strcmp(g_tnames[i], s)) return i; return -1; }
static void f_raw_get(XmodCall *c, void *) {
	Args a(c); edict_t *e = mc_edict((int)a.i(0)); int off = (int)a.i(1), t = type_of(a.s(2, "int"));
	if (!e || !e->pvPrivateData || t < 0 || t == PT_STRING || off < 0 || off > 0x4000) { a.err("usage: xmod.mc.raw_get(ent, byte_offset, type)"); return; }
	a.ret(mc_read_member(reinterpret_cast<char *>(e->pvPrivateData) + off, t, 1, false, 0));
}
static void f_raw_set(XmodCall *c, void *) {
	Args a(c); edict_t *e = mc_edict((int)a.i(0)); int off = (int)a.i(1), t = type_of(a.s(2, "int"));
	if (!e || !e->pvPrivateData || t < 0 || t == PT_STRING || off < 0 || off > 0x4000) { a.err("usage: xmod.mc.raw_set(ent, byte_offset, type, value)"); return; }
	std::string err; if (!mc_write_member(reinterpret_cast<char *>(e->pvPrivateData) + off, t, 1, 0, a, 3, err)) { a.err("mc.raw_set: %s", err.c_str()); return; }
	a.ret_nil();
}
bool reg_pdata() {
	return mc_reg("pdata_get", f_pdata_get) && mc_reg("pdata_set", f_pdata_set) && mc_reg("pdata_info", f_pdata_info) && mc_reg("pdata_list", f_pdata_list)
		&& mc_reg("raw_get", f_raw_get) && mc_reg("raw_set", f_raw_set);
}
