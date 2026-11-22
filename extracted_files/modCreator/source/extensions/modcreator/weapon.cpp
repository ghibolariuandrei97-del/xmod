// weapon.cpp - weapon helpers for custom weapons (the C half of MC.weapons in lib/modcreator.lua):
// inventory walking, clip / backpack ammo, view-model animations, next attack times, the 3rd-person animation
// extension, custom world models for dropped weapons (weaponbox) and blocking a weapon's client event
// (so a custom weapon can play its own fire sound/animation).
#include "mc.h"
#include <unordered_map>
#include <entity_state.h>

struct CustomW { std::string wmodel; bool block_events = false; };
static std::unordered_map<int, CustomW> g_custom;     // pev->impulse of the weapon entity -> settings

static void *player_cb(const Args &a, int k, edict_t **out = nullptr) {
	int id = (int)a.i(k); if (id < 1 || id > gpGlobals->maxClients) return nullptr;
	edict_t *e = mc_edict(id); if (out) *out = e; return e ? e->pvPrivateData : nullptr;
}
static void *active_item(void *pcb) { return g_pd.active_item >= 0 ? pd<void *>(pcb, g_pd.active_item) : nullptr; }

// mc.weapon_active(id) -> weapon entity in the player's hands (0 none)
static void f_weapon_active(XmodCall *c, void *) { Args a(c); void *p = player_cb(a, 0); if (!p) { a.err("mc.weapon_active: bad player"); return; }
	a.ret_int(mc_index(mc_edict_of_cbase(active_item(p)))); }
// mc.weapon_owner(wpn) -> player id (0 none)
static void f_weapon_owner(XmodCall *c, void *) { Args a(c); edict_t *w = mc_edict((int)a.i(0)); if (!w || !w->pvPrivateData) { a.err("mc.weapon_owner: bad weapon"); return; }
	a.ret_int(mc_index(mc_edict_of_cbase(pd<void *>(w->pvPrivateData, g_pd.w_player)))); }
// mc.weapon_list(id) -> {wpn, ...} every item the player carries (6 slots, linked lists)
static void f_weapon_list(XmodCall *c, void *) {
	Args a(c); void *p = player_cb(a, 0); if (!p) { a.err("mc.weapon_list: bad player"); return; }
	XmodValue *l = g_xmod->val_array();
	for (int s = 0; s < 6; s++) { void *it = pd<void *>(p, g_pd.player_items + s * 4); int guard = 0;
		while (it && guard++ < 32) { edict_t *e = mc_edict_of_cbase(it); if (e) g_xmod->val_push(l, g_xmod->val_int(ENTINDEX(e))); it = pd<void *>(it, g_pd.w_next); } }
	a.ret(l);
}
// mc.weapon_find(id, classname_or_impulse) -> weapon entity or 0
static void f_weapon_find(XmodCall *c, void *) {
	Args a(c); void *p = player_cb(a, 0); if (!p) { a.err("mc.weapon_find: bad player"); return; }
	bool by_name = a.type(1) == XV_STR; const char *n = a.s(1, ""); int imp = (int)a.i(1);
	for (int s = 0; s < 6; s++) { void *it = pd<void *>(p, g_pd.player_items + s * 4); int guard = 0;
		while (it && guard++ < 32) { edict_t *e = mc_edict_of_cbase(it);
			if (e && (by_name ? !strcmp(STRING(e->v.classname), n) : e->v.impulse == imp)) { a.ret_int(ENTINDEX(e)); return; }
			it = pd<void *>(it, g_pd.w_next); } }
	a.ret_int(0);
}
// mc.weapon_info(wpn) -> {id (CSW_*), classname, owner, clip, ammo_type, bpammo, in_reload, next_primary, next_secondary, idle, impulse}
static void f_weapon_info(XmodCall *c, void *) {
	Args a(c); edict_t *w = mc_edict((int)a.i(0)); if (!w || !w->pvPrivateData) { a.err("mc.weapon_info: bad weapon"); return; }
	void *cb = w->pvPrivateData; XmodValue *m = g_xmod->val_map(); void *owner = pd<void *>(cb, g_pd.w_player); int at = pd<int>(cb, g_pd.w_ammo_type);
	g_xmod->val_set(m, "id", g_xmod->val_int(pd<int>(cb, g_pd.w_id))); g_xmod->val_set(m, "classname", g_xmod->val_cstr(STRING(w->v.classname)));
	g_xmod->val_set(m, "owner", g_xmod->val_int(mc_index(mc_edict_of_cbase(owner)))); g_xmod->val_set(m, "clip", g_xmod->val_int(pd<int>(cb, g_pd.w_clip)));
	g_xmod->val_set(m, "ammo_type", g_xmod->val_int(at));
	g_xmod->val_set(m, "bpammo", g_xmod->val_int(owner && at > 0 && at < 32 ? pd<int>(owner, g_pd.ammo + at * 4) : 0));
	g_xmod->val_set(m, "in_reload", g_xmod->val_bool(pd<int>(cb, g_pd.w_in_reload) != 0));
	g_xmod->val_set(m, "next_primary", g_xmod->val_num(pd<float>(cb, g_pd.w_next_primary))); g_xmod->val_set(m, "next_secondary", g_xmod->val_num(pd<float>(cb, g_pd.w_next_secondary)));
	g_xmod->val_set(m, "idle", g_xmod->val_num(pd<float>(cb, g_pd.w_idle))); g_xmod->val_set(m, "impulse", g_xmod->val_int(w->v.impulse));
	a.ret(m);
}
// mc.weapon_clip(wpn [, new]) -> clip
static void f_weapon_clip(XmodCall *c, void *) { Args a(c); edict_t *w = mc_edict((int)a.i(0)); if (!w || !w->pvPrivateData) { a.err("mc.weapon_clip: bad weapon"); return; }
	if (a.has(1)) pd<int>(w->pvPrivateData, g_pd.w_clip) = (int)a.i(1); a.ret_int(pd<int>(w->pvPrivateData, g_pd.w_clip)); }
// mc.bpammo(id, ammo_type_or_wpn [, new]) -> backpack ammo. Second arg: ammo type 1..31, or a weapon entity (> 31) whose ammo type is used
static void f_bpammo(XmodCall *c, void *) {
	Args a(c); void *p = player_cb(a, 0); if (!p) { a.err("mc.bpammo: bad player"); return; }
	int t = (int)a.i(1); if (t > 31) { edict_t *w = mc_edict(t); t = w && w->pvPrivateData ? pd<int>(w->pvPrivateData, g_pd.w_ammo_type) : -1; }
	if (t < 1 || t > 31) { a.ret_int(0); return; }
	if (a.has(2)) pd<int>(p, g_pd.ammo + t * 4) = (int)a.i(2); a.ret_int(pd<int>(p, g_pd.ammo + t * 4)); }
// mc.weapon_times(wpn [, next_primary, next_secondary, idle])  values are RELATIVE seconds (CS weapons use relative timers); returns current
static void f_weapon_times(XmodCall *c, void *) {
	Args a(c); edict_t *w = mc_edict((int)a.i(0)); if (!w || !w->pvPrivateData) { a.err("mc.weapon_times: bad weapon"); return; } void *cb = w->pvPrivateData;
	if (a.has(1)) pd<float>(cb, g_pd.w_next_primary) = (float)a.f(1);
	if (a.has(2)) pd<float>(cb, g_pd.w_next_secondary) = (float)a.f(2);
	if (a.has(3)) pd<float>(cb, g_pd.w_idle) = (float)a.f(3);
	XmodValue *r = g_xmod->val_array(); g_xmod->val_push(r, g_xmod->val_num(pd<float>(cb, g_pd.w_next_primary)));
	g_xmod->val_push(r, g_xmod->val_num(pd<float>(cb, g_pd.w_next_secondary))); g_xmod->val_push(r, g_xmod->val_num(pd<float>(cb, g_pd.w_idle))); a.ret(r); }
// mc.next_attack(id [, seconds]) -> player m_flNextAttack (blocks every weapon while > 0)
static void f_next_attack(XmodCall *c, void *) { Args a(c); void *p = player_cb(a, 0); if (!p) { a.err("mc.next_attack: bad player"); return; }
	if (a.has(1)) pd<float>(p, g_pd.next_attack) = (float)a.f(1); a.ret_num(pd<float>(p, g_pd.next_attack)); }
// mc.weapon_anim(id, anim [, body=0]) - plays a view-model sequence for that player only (like SendWeaponAnim)
static void f_weapon_anim(XmodCall *c, void *) { Args a(c); edict_t *e = nullptr; if (!player_cb(a, 0, &e)) { a.err("mc.weapon_anim: bad player"); return; }
	int anim = (int)a.i(1); e->v.weaponanim = anim;
	if (!(e->v.flags & FL_FAKECLIENT)) { MESSAGE_BEGIN(MSG_ONE, 35 /*SVC_WEAPONANIM*/, nullptr, e); WRITE_BYTE(anim); WRITE_BYTE((int)a.i(2, 0)); MESSAGE_END(); }
	a.ret_nil(); }
// mc.anim_ext(id [, "rifle"]) -> player 3rd-person animation set (m_szAnimExtention: knife onehanded dualpistols carbine rifle mp5 shotgun m249 grenade c4 ak47 shieldgun ...)
static void f_anim_ext(XmodCall *c, void *) { Args a(c); void *p = player_cb(a, 0); if (!p || g_pd.anim_ext < 0) { a.err("mc.anim_ext: bad player"); return; }
	char *s = reinterpret_cast<char *>(p) + g_pd.anim_ext;
	if (a.has(1)) { const char *n = a.s(1, ""); size_t l = strlen(n); if (l > 31) l = 31; memcpy(s, n, l); s[l] = 0; } a.ret_str(s); }

// mc.custom_weapon(impulse, {wmodel="models/w_x.mdl", block_events=true}) - native side of a custom weapon (impulse = its pev->impulse key)
static void f_custom_weapon(XmodCall *c, void *) {
	Args a(c); int imp = (int)a.i(0); const XmodValue *o = a.v(1);
	if (imp == 0) { a.err("usage: xmod.mc.custom_weapon(impulse, {wmodel=, block_events=})"); return; }
	if (!o || g_xmod->val_type(o) == XV_NIL) { g_custom.erase(imp); a.ret_bool(true); return; }
	if (g_xmod->val_type(o) != XV_MAP) { a.err("mc.custom_weapon: 2nd argument must be a table"); return; }
	CustomW &w = g_custom[imp];
	if (const XmodValue *x = g_xmod->val_get(o, "wmodel")) { const char *s = g_xmod->val_get_str(x, nullptr); w.wmodel = s ? s : ""; }
	if (const XmodValue *x = g_xmod->val_get(o, "block_events")) w.block_events = g_xmod->val_get_bool(x, 0) != 0;
	a.ret_bool(true);
}
void weapon_on_map_change() {}

void mc_hook_setmodel(edict_t *e, const char *m) {
	if (g_custom.empty() || !e || !e->pvPrivateData || !m || strncmp(m, "models/w_", 9) != 0 || g_pd.wb_items < 0) return;
	if (strcmp(STRING(e->v.classname), "weaponbox") != 0) return;
	for (int s = 0; s < 6; s++) {
		void *it = pd<void *>(e->pvPrivateData, g_pd.wb_items + s * 4); if (!it) continue;
		edict_t *w = mc_edict_of_cbase(it); if (!w) continue;
		auto f = g_custom.find(w->v.impulse);
		if (f != g_custom.end() && !f->second.wmodel.empty()) { g_engfuncs.pfnSetModel(e, f->second.wmodel.c_str()); gpMetaGlobals->mres = MRES_SUPERCEDE; return; }
	}
}
void mc_hook_playbackevent(int, const edict_t *inv, unsigned short) {
	if (g_custom.empty() || !inv || !inv->pvPrivateData || !(inv->v.flags & FL_CLIENT) || g_pd.active_item < 0) return;
	void *w = active_item(inv->pvPrivateData); edict_t *we = mc_edict_of_cbase(w); if (!we) return;
	auto f = g_custom.find(we->v.impulse);
	if (f != g_custom.end() && f->second.block_events) gpMetaGlobals->mres = MRES_SUPERCEDE;
}

// UpdateClientData post: a custom weapon with block_events turns off client-side weapon prediction for its holder,
// so the client does not play the base weapon's own shot sound/animation (the classic cd->m_flNextAttack trick)
void mc_hook_ucd(const edict_t *ent, int, clientdata_s *cd) {
	if (g_custom.empty() || !ent || !cd || !ent->pvPrivateData || g_pd.active_item < 0) return;
	edict_t *we = mc_edict_of_cbase(active_item(ent->pvPrivateData)); if (!we) return;
	auto f = g_custom.find(we->v.impulse);
	if (f != g_custom.end() && f->second.block_events) cd->m_flNextAttack = gpGlobals->time + 0.001f;
}
bool reg_weapon() {
	return mc_reg("weapon_active", f_weapon_active) && mc_reg("weapon_owner", f_weapon_owner) && mc_reg("weapon_list", f_weapon_list)
		&& mc_reg("weapon_find", f_weapon_find) && mc_reg("weapon_info", f_weapon_info) && mc_reg("weapon_clip", f_weapon_clip)
		&& mc_reg("bpammo", f_bpammo) && mc_reg("weapon_times", f_weapon_times) && mc_reg("next_attack", f_next_attack)
		&& mc_reg("weapon_anim", f_weapon_anim) && mc_reg("anim_ext", f_anim_ext) && mc_reg("custom_weapon", f_custom_weapon);
}
