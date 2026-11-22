// combat.cpp - bullets, radius damage, damage with hit groups, blood and decals.
#include "mc.h"

static const float HG_MULT[] = {1.0f, 4.0f, 1.0f, 1.25f, 1.0f, 1.0f, 0.75f, 0.75f, 1.0f};   // generic head chest stomach larm rarm lleg rleg shield
static int g_td_idx = -1, g_bc_idx = -1;

static int take_damage(edict_t *victim, edict_t *inflictor, edict_t *attacker, float dmg, int bits) {
	void **vt = mc_vtable(victim); if (!vt) return 0;
	if (g_td_idx < 0) g_td_idx = mc_vidx("takedamage");
	typedef int(MC_CC * TD)(void *, MC_DUMMY entvars_t *, entvars_t *, float, int);
	return reinterpret_cast<TD>(vt[g_td_idx])(victim->pvPrivateData, MC_DUMMY_ARG inflictor ? &inflictor->v : &INDEXENT(0)->v, attacker ? &attacker->v : &INDEXENT(0)->v, dmg, bits);
}
static int blood_color(edict_t *e) {
	void **vt = mc_vtable(e); if (!vt) return -1;
	if (g_bc_idx < 0) g_bc_idx = mc_vidx("bloodcolor");
#if defined(_WIN32) && defined(_MSC_VER)
	return reinterpret_cast<int(__fastcall *)(void *, int)>(vt[g_bc_idx])(e->pvPrivateData, 0);
#else
	return reinterpret_cast<int(MC_CC *)(void *)>(vt[g_bc_idx])(e->pvPrivateData);
#endif
}
static bool is_monster_like(edict_t *e) { return (e->v.flags & (FL_CLIENT | FL_MONSTER | FL_FAKECLIENT)) != 0; }
static void set_hitgroup(edict_t *e, int hg) { if (g_pd.last_hitgroup >= 0 && e->pvPrivateData && is_monster_like(e)) pd<int>(e->pvPrivateData, g_pd.last_hitgroup) = hg; }

static void blood_fx(const float *o, const float *dir, int color, int amount) {
	if (color < 0) return;
	MESSAGE_BEGIN(MSG_PVS, SVC_TEMPENTITY, o, nullptr);
	WRITE_BYTE(101 /*TE_BLOODSTREAM*/); WRITE_COORD(o[0]); WRITE_COORD(o[1]); WRITE_COORD(o[2]);
	WRITE_COORD(dir[0]); WRITE_COORD(dir[1]); WRITE_COORD(dir[2]); WRITE_BYTE(color); WRITE_BYTE(amount > 255 ? 255 : amount);
	MESSAGE_END();
}
static void decal_fx(const float *o, edict_t *hit, int decal) {
	if (decal < 0) return;
	MESSAGE_BEGIN(MSG_PAS, SVC_TEMPENTITY, o, nullptr);
	WRITE_BYTE(109 /*TE_GUNSHOTDECAL*/); WRITE_COORD(o[0]); WRITE_COORD(o[1]); WRITE_COORD(o[2]);
	WRITE_SHORT(hit ? ENTINDEX(hit) : 0); WRITE_BYTE(decal);
	MESSAGE_END();
}
static int shot_decal() { static int d[5] = {-2, -2, -2, -2, -2}; int k = RANDOM_LONG(0, 4); if (d[k] == -2) { char n[16]; snprintf(n, sizeof n, "{shot%d", k + 1); d[k] = DECAL_INDEX(n); } return d[k]; }

// mc.fire_bullets(attacker, inflictor, sx,sy,sz, dx,dy,dz, spread, distance, damage [, shots=1 [, bits=DMG_BULLET|DMG_NEVERGIB [, fx=true]]])
//   -> {{ent, x, y, z, hitgroup, damage}, ...} one row per shot. spread = tangent of the cone half-angle (0.02 = rifle, 0.1 = shotgun).
// Player/NPC hits get the CS hit group multipliers (head x4, stomach x1.25, legs x0.75) and m_LastHitGroup (armor/helmet + headshot DeathMsg work).
static void f_fire_bullets(XmodCall *c, void *) {
	Args a(c); edict_t *att = mc_edict((int)a.i(0)), *inf = mc_edict((int)a.i(1));
	if (!att) { a.err("mc.fire_bullets: bad attacker"); return; }
	if (!inf) inf = att;
	float src[3] = {(float)a.f(2), (float)a.f(3), (float)a.f(4)}, dir[3] = {(float)a.f(5), (float)a.f(6), (float)a.f(7)};
	float spread = (float)a.f(8), dist = (float)a.f(9, 8192), dmg = (float)a.f(10, 10); int shots = (int)a.i(11, 1); int bits = (int)a.i(12, (1 << 1) | (1 << 12)); bool fx = a.b(13, true);
	if (shots < 1) shots = 1; if (shots > 64) shots = 64;
	float len = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]); if (len < 1e-6f) { a.err("mc.fire_bullets: zero direction"); return; }
	for (float &x : dir) x /= len;
	float ang[3], fw[3], rt[3], up[3]; VEC_TO_ANGLES(dir, ang); ang[0] = -ang[0]; g_engfuncs.pfnAngleVectors(ang, fw, rt, up);
	XmodValue *rows = g_xmod->val_array();
	for (int s = 0; s < shots; s++) {
		float x, y, z; do { x = RANDOM_FLOAT(-0.5f, 0.5f) + RANDOM_FLOAT(-0.5f, 0.5f); y = RANDOM_FLOAT(-0.5f, 0.5f) + RANDOM_FLOAT(-0.5f, 0.5f); z = x * x + y * y; } while (z > 1);
		float d[3], end[3]; for (int k = 0; k < 3; k++) { d[k] = dir[k] + x * spread * rt[k] + y * spread * up[k]; end[k] = src[k] + d[k] * dist; }
		TraceResult tr; TRACE_LINE(src, end, 0 /*dont_ignore_monsters*/, att, &tr);
		XmodValue *row = g_xmod->val_array(); edict_t *hit = tr.pHit; int hidx = hit && !hit->free ? ENTINDEX(hit) : 0; float done = 0;
		if (tr.flFraction < 1.0f && hit && hidx > 0 && hit->v.takedamage != 0) {
			int hg = tr.iHitgroup; if (hg < 0 || hg > 8) hg = 0;
			float d2 = dmg * (is_monster_like(hit) ? HG_MULT[hg] : 1.0f);
			set_hitgroup(hit, hg);
			if (fx && is_monster_like(hit)) { float bd[3] = {-d[0], -d[1], -d[2]}; blood_fx(tr.vecEndPos, bd, blood_color(hit), (int)(d2 * 2)); }
			take_damage(hit, inf, att, d2, bits); done = d2;
		} else if (tr.flFraction < 1.0f && fx && (!hit || hidx == 0 || hit->v.solid == SOLID_BSP)) decal_fx(tr.vecEndPos, hit, shot_decal());
		g_xmod->val_push(row, g_xmod->val_int(hidx)); for (int k = 0; k < 3; k++) g_xmod->val_push(row, g_xmod->val_num(tr.vecEndPos[k]));
		g_xmod->val_push(row, g_xmod->val_int(tr.iHitgroup)); g_xmod->val_push(row, g_xmod->val_num(done));
		g_xmod->val_push(rows, row);
	}
	a.ret(rows);
}
// mc.radius_damage(x,y,z, radius, damage, inflictor, attacker [, bits=DMG_BLAST [, falloff=true [, ignore_ent=0 [, los=true]]]])
//   -> {{ent, damage}, ...}. Damage falls off linearly with distance; walls block it when los is true.
static void f_radius_damage(XmodCall *c, void *) {
	Args a(c); float o[3] = {(float)a.f(0), (float)a.f(1), (float)a.f(2)}, rad = (float)a.f(3), dmg = (float)a.f(4);
	edict_t *inf = mc_edict((int)a.i(5)), *att = mc_edict((int)a.i(6)); int bits = (int)a.i(7, 1 << 6); bool falloff = a.b(8, true); int ign = (int)a.i(9, 0); bool los = a.b(10, true);
	if (rad <= 0) { a.err("mc.radius_damage: radius must be > 0"); return; }
	if (!att) att = INDEXENT(0); if (!inf) inf = att;
	std::vector<edict_t *> list; edict_t *e = g_engfuncs.pfnFindEntityInSphere(INDEXENT(0), o, rad);
	while (e && !FNullEnt(e)) { if (!e->free && e->v.takedamage != 0 && ENTINDEX(e) != ign) list.push_back(e); e = g_engfuncs.pfnFindEntityInSphere(e, o, rad); if (list.size() > 512) break; }
	XmodValue *rows = g_xmod->val_array();
	for (edict_t *t : list) {
		if (t->free || t->v.takedamage == 0) continue;
		float ctr[3] = {(t->v.absmin.x + t->v.absmax.x) * 0.5f, (t->v.absmin.y + t->v.absmax.y) * 0.5f, (t->v.absmin.z + t->v.absmax.z) * 0.5f};
		if (t->v.solid != SOLID_BSP && t->v.absmin.x == 0 && t->v.absmax.x == 0) { ctr[0] = t->v.origin.x; ctr[1] = t->v.origin.y; ctr[2] = t->v.origin.z; }
		if (los) { TraceResult tr; TRACE_LINE(o, ctr, 1 /*ignore_monsters*/, inf, &tr); if (tr.flFraction < 1.0f && tr.pHit != t) continue; }
		float dx = ctr[0] - o[0], dy = ctr[1] - o[1], dz = ctr[2] - o[2], dist = sqrtf(dx * dx + dy * dy + dz * dz);
		float d = falloff ? dmg * (1.0f - dist / rad) : dmg; if (d <= 0) continue;
		set_hitgroup(t, 0); take_damage(t, inf, att, d, bits);
		XmodValue *row = g_xmod->val_array(); g_xmod->val_push(row, g_xmod->val_int(ENTINDEX(t))); g_xmod->val_push(row, g_xmod->val_num(d)); g_xmod->val_push(rows, row);
	}
	a.ret(rows);
}
// mc.damage(victim, inflictor, attacker, damage [, bits=DMG_GENERIC [, hitgroup]]) -> result of TakeDamage (ham hooks fire)
static void f_damage(XmodCall *c, void *) {
	Args a(c); edict_t *v = mc_edict((int)a.i(0)), *inf = mc_edict((int)a.i(1)), *att = mc_edict((int)a.i(2));
	if (!v || !v->pvPrivateData) { a.err("mc.damage: bad victim"); return; }
	if (a.has(5)) set_hitgroup(v, (int)a.i(5));
	a.ret_int(take_damage(v, inf ? inf : att, att, (float)a.f(3), (int)a.i(4, 0)));
}
// mc.blood(x,y,z, dirx,diry,dirz [, color=247 [, amount=50]])   247 = red, 195 = yellow (alien)
static void f_blood(XmodCall *c, void *) { Args a(c); float o[3] = {(float)a.f(0), (float)a.f(1), (float)a.f(2)}, d[3] = {(float)a.f(3), (float)a.f(4), (float)a.f(5)};
	blood_fx(o, d, (int)a.i(6, 247), (int)a.i(7, 50)); a.ret_nil(); }
// mc.decal(x,y,z [, name="{shot1" [, ent=0]]) - gunshot decal + ricochet on a wall/brush entity
static void f_decal(XmodCall *c, void *) { Args a(c); float o[3] = {(float)a.f(0), (float)a.f(1), (float)a.f(2)};
	int d = a.has(3) ? DECAL_INDEX(a.s(3, "{shot1")) : shot_decal(); decal_fx(o, mc_edict((int)a.i(4, 0)), d); a.ret_int(d); }
// mc.blood_color(ent) -> -1 none (DONT_BLEED), 247 red, 195 yellow
static void f_blood_color(XmodCall *c, void *) { Args a(c); edict_t *e = mc_edict((int)a.i(0)); if (!e) { a.err("mc.blood_color: bad entity"); return; } a.ret_int(blood_color(e)); }

bool reg_combat() {
	return mc_reg("fire_bullets", f_fire_bullets) && mc_reg("radius_damage", f_radius_damage) && mc_reg("damage", f_damage)
		&& mc_reg("blood", f_blood) && mc_reg("decal", f_decal) && mc_reg("blood_color", f_blood_color);
}
