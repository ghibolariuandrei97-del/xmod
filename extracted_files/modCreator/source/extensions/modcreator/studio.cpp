// studio.cpp - studio models (.mdl): sequences/animation, bone controllers, blending, body groups, attachments, bones.
// Same math as the HLSDK (animation.cpp), so NPCs animate exactly like Half-Life monsters.
#include "mc.h"

static studiohdr_t *hdr_of(edict_t *e) { if (!e || !e->v.modelindex) return nullptr; return reinterpret_cast<studiohdr_t *>(GET_MODEL_PTR(e)); }
static mstudioseqdesc_t *seqs(studiohdr_t *h) { return reinterpret_cast<mstudioseqdesc_t *>(reinterpret_cast<byte *>(h) + h->seqindex); }
#define HDR_OR_ERR(fn) Args a(c); edict_t *e = mc_edict((int)a.i(0)); studiohdr_t *h = hdr_of(e); \
	if (!e) { a.err("mc." fn ": bad entity"); return; } if (!h) { a.ret_nil(); return; }

static int seq_arg(const Args &a, int k, studiohdr_t *h) {     // sequence by index or by name
	if (a.type(k) == XV_STR) { const char *n = a.s(k); mstudioseqdesc_t *s = seqs(h); for (int i = 0; i < h->numseq; i++) if (!strcasecmp(s[i].label, n)) return i; return -1; }
	int i = (int)a.i(k, -1); return (i >= 0 && i < h->numseq) ? i : -1;
}
static float seq_fps(studiohdr_t *h, int i) { return seqs(h)[i].fps; }
static float seq_rate256(studiohdr_t *h, int i) {   // HLSDK m_flFrameRate: 0..256 frame units per second
	mstudioseqdesc_t &s = seqs(h)[i]; return s.numframes > 1 ? 256.0f * s.fps / (s.numframes - 1) : 256.0f; }
static float seq_duration(studiohdr_t *h, int i) { mstudioseqdesc_t &s = seqs(h)[i]; return s.fps > 0 ? (s.numframes > 1 ? (s.numframes - 1) / s.fps : 0.0f) : 0.0f; }

// mc.model_info(ent) -> {name, sequences, bones, attachments, bodyparts, controllers, eye={x,y,z}} or nil (no studio model)
static void f_model_info(XmodCall *c, void *) {
	HDR_OR_ERR("model_info")
	XmodValue *m = g_xmod->val_map();
	g_xmod->val_set(m, "name", g_xmod->val_cstr(h->name)); g_xmod->val_set(m, "sequences", g_xmod->val_int(h->numseq));
	g_xmod->val_set(m, "bones", g_xmod->val_int(h->numbones)); g_xmod->val_set(m, "attachments", g_xmod->val_int(h->numattachments));
	g_xmod->val_set(m, "bodyparts", g_xmod->val_int(h->numbodyparts)); g_xmod->val_set(m, "controllers", g_xmod->val_int(h->numbonecontrollers));
	g_xmod->val_set(m, "eye", mc_vec(&h->eyeposition[0]));
	a.ret(m);
}
// mc.seq_lookup(ent, name) -> index or -1
static void f_seq_lookup(XmodCall *c, void *) { HDR_OR_ERR("seq_lookup") a.ret_int(seq_arg(a, 1, h)); }
// mc.seq_list(ent) -> {"idle", "walk", ...} (index = position - 1)
static void f_seq_list(XmodCall *c, void *) { HDR_OR_ERR("seq_list")
	XmodValue *l = g_xmod->val_array(); mstudioseqdesc_t *s = seqs(h); for (int i = 0; i < h->numseq; i++) g_xmod->val_push(l, g_xmod->val_cstr(s[i].label)); a.ret(l); }
// mc.seq_info(ent, seq) -> {index, name, fps, frames, duration, loop, activity, actweight, motion={x,y,z}, events}
static void f_seq_info(XmodCall *c, void *) { HDR_OR_ERR("seq_info")
	int i = seq_arg(a, 1, h); if (i < 0) { a.ret_nil(); return; }
	mstudioseqdesc_t &s = seqs(h)[i]; XmodValue *m = g_xmod->val_map();
	g_xmod->val_set(m, "index", g_xmod->val_int(i)); g_xmod->val_set(m, "name", g_xmod->val_cstr(s.label)); g_xmod->val_set(m, "fps", g_xmod->val_num(s.fps));
	g_xmod->val_set(m, "frames", g_xmod->val_int(s.numframes)); g_xmod->val_set(m, "duration", g_xmod->val_num(seq_duration(h, i)));
	g_xmod->val_set(m, "loop", g_xmod->val_bool((s.flags & STUDIO_LOOPING) != 0)); g_xmod->val_set(m, "activity", g_xmod->val_int(s.activity));
	g_xmod->val_set(m, "actweight", g_xmod->val_int(s.actweight)); g_xmod->val_set(m, "motion", mc_vec(&s.linearmovement[0]));
	g_xmod->val_set(m, "events", g_xmod->val_int(s.numevents)); a.ret(m);
}
// mc.seq_by_activity(ent, activity) -> random sequence weighted by actweight (HLSDK LookupActivity), -1 none
static void f_seq_by_activity(XmodCall *c, void *) { HDR_OR_ERR("seq_by_activity")
	int act = (int)a.i(1), total = 0, pick = -1; mstudioseqdesc_t *s = seqs(h);
	for (int i = 0; i < h->numseq; i++) if (s[i].activity == act) { total += s[i].actweight; if (!total || RANDOM_LONG(0, total - 1) < s[i].actweight) pick = i; }
	a.ret_int(pick); }
// mc.anim(ent, seq_or_name [, framerate=1 [, restart=true]]) -> duration in seconds (at framerate 1), or -1 unknown sequence.
// Sets sequence/frame/animtime/framerate: clients play the animation by themselves; call mc.frame_advance in think to know when it ended.
static void f_anim(XmodCall *c, void *) { HDR_OR_ERR("anim")
	int i = seq_arg(a, 1, h); if (i < 0) { a.ret_int(-1); return; }
	bool restart = a.b(3, true);
	if (restart || e->v.sequence != i) { e->v.frame = 0; }
	e->v.sequence = i; e->v.animtime = gpGlobals->time; e->v.framerate = (float)a.f(2, 1.0);
	a.ret_num(seq_duration(h, i)); }
// mc.frame_advance(ent [, interval]) -> finished (bool). HLSDK StudioFrameAdvance: advances pev->frame (0..255) since animtime.
static void f_frame_advance(XmodCall *c, void *) { HDR_OR_ERR("frame_advance")
	int i = e->v.sequence; if (i < 0 || i >= h->numseq) { a.ret_bool(true); return; }
	float interval = a.has(1) ? (float)a.f(1) : gpGlobals->time - e->v.animtime;
	if (e->v.animtime == 0) interval = 0;
	if (interval > 0.2f && !a.has(1)) interval = 0.2f;
	e->v.animtime = gpGlobals->time;
	e->v.frame += interval * seq_rate256(h, i) * e->v.framerate;
	bool loops = (seqs(h)[i].flags & STUDIO_LOOPING) != 0, finished = false;
	if (e->v.frame < 0.0f || e->v.frame >= 256.0f) {
		if (loops) { e->v.frame -= (int)(e->v.frame / 256.0f) * 256.0f; if (e->v.frame < 0) e->v.frame += 256.0f; }
		else e->v.frame = (e->v.frame < 0.0f) ? 0.0f : 255.0f;
		finished = true;
	}
	a.ret_bool(finished); }
// mc.seq_duration(ent, seq) -> seconds at framerate 1
static void f_seq_duration(XmodCall *c, void *) { HDR_OR_ERR("seq_duration") int i = seq_arg(a, 1, h); a.ret_num(i < 0 ? -1.0 : seq_duration(h, i)); }
static void f_seq_fps(XmodCall *c, void *) { HDR_OR_ERR("seq_fps") int i = seq_arg(a, 1, h); a.ret_num(i < 0 ? -1.0 : seq_fps(h, i)); }

// mc.set_controller(ent, controller 0..3, degrees) -> the angle actually set (HLSDK SetController)
static void f_set_controller(XmodCall *c, void *) { HDR_OR_ERR("set_controller")
	int ctl = (int)a.i(1); float v = (float)a.f(2);
	mstudiobonecontroller_t *bc = reinterpret_cast<mstudiobonecontroller_t *>(reinterpret_cast<byte *>(h) + h->bonecontrollerindex);
	int i = 0; for (; i < h->numbonecontrollers; i++, bc++) if (bc->index == ctl) break;
	if (i >= h->numbonecontrollers || ctl < 0 || ctl > 3) { a.ret_num(v); return; }
	if (bc->type & (STUDIO_XR | STUDIO_YR | STUDIO_ZR)) {
		if (bc->end < bc->start) v = -v;
		if (bc->start + 359.0f >= bc->end) { if (v > ((bc->start + bc->end) / 2.0f) + 180) v -= 360; if (v < ((bc->start + bc->end) / 2.0f) - 180) v += 360; }
		else { if (v > 360) v -= (int)(v / 360.0f) * 360.0f; else if (v < 0) v += (int)((v / -360.0f) + 1) * 360.0f; }
	}
	int s = (int)(255 * (v - bc->start) / (bc->end - bc->start)); if (s < 0) s = 0; if (s > 255) s = 255;
	e->v.controller[ctl] = (byte)s;
	a.ret_num(s * (1.0f / 255.0f) * (bc->end - bc->start) + bc->start); }
// mc.set_blending(ent, blender 0..1, value) -> value set (HLSDK SetBlending for the current sequence)
static void f_set_blending(XmodCall *c, void *) { HDR_OR_ERR("set_blending")
	int b = (int)a.i(1); float v = (float)a.f(2); if (b < 0 || b > 1 || e->v.sequence < 0 || e->v.sequence >= h->numseq) { a.ret_num(v); return; }
	mstudioseqdesc_t &s = seqs(h)[e->v.sequence];
	if (s.blendtype[b] == 0) { a.ret_num(v); return; }
	if (s.blendtype[b] & (STUDIO_XR | STUDIO_YR | STUDIO_ZR)) {
		if (s.blendend[b] < s.blendstart[b]) v = -v;
		if (s.blendstart[b] + 359.0f >= s.blendend[b]) { if (v > ((s.blendstart[b] + s.blendend[b]) / 2.0f) + 180) v -= 360; if (v < ((s.blendstart[b] + s.blendend[b]) / 2.0f) - 180) v += 360; }
	}
	int st = (int)(255 * (v - s.blendstart[b]) / (s.blendend[b] - s.blendstart[b])); if (st < 0) st = 0; if (st > 255) st = 255;
	e->v.blending[b] = (byte)st;
	a.ret_num(st * (1.0f / 255.0f) * (s.blendend[b] - s.blendstart[b]) + s.blendstart[b]); }
// mc.set_body(ent, group, value) / mc.get_body(ent, group) - sub-models (heads, weapons in hand ...)
static void f_set_body(XmodCall *c, void *) { HDR_OR_ERR("set_body")
	int g = (int)a.i(1), v = (int)a.i(2); if (g < 0 || g >= h->numbodyparts) { a.ret_bool(false); return; }
	mstudiobodyparts_t *bp = reinterpret_cast<mstudiobodyparts_t *>(reinterpret_cast<byte *>(h) + h->bodypartindex) + g;
	if (v < 0 || v >= bp->nummodels) { a.ret_bool(false); return; }
	int cur = (e->v.body / bp->base) % bp->nummodels; e->v.body = (e->v.body - (cur * bp->base) + (v * bp->base)); a.ret_bool(true); }
static void f_get_body(XmodCall *c, void *) { HDR_OR_ERR("get_body")
	int g = (int)a.i(1); if (g < 0 || g >= h->numbodyparts) { a.ret_int(0); return; }
	mstudiobodyparts_t *bp = reinterpret_cast<mstudiobodyparts_t *>(reinterpret_cast<byte *>(h) + h->bodypartindex) + g;
	a.ret_int(bp->nummodels <= 1 ? 0 : (e->v.body / bp->base) % bp->nummodels); }
// mc.attachment(ent, n) -> {x,y,z} (engine GetAttachment: muzzle, hands ...)
static void f_attachment(XmodCall *c, void *) { HDR_OR_ERR("attachment")
	int n = (int)a.i(1); if (n < 0 || n >= h->numattachments) { a.ret_nil(); return; }
	float o[3], ang[3]; GET_ATTACHMENT(e, n, o, ang); a.ret_vec(o); }
// mc.bone(ent, index_or_name) -> {x,y,z} (engine GetBonePosition)
static void f_bone(XmodCall *c, void *) { HDR_OR_ERR("bone")
	int n = -1; mstudiobone_t *b = reinterpret_cast<mstudiobone_t *>(reinterpret_cast<byte *>(h) + h->boneindex);
	if (a.type(1) == XV_STR) { for (int i = 0; i < h->numbones; i++) if (!strcasecmp(b[i].name, a.s(1))) { n = i; break; } } else n = (int)a.i(1, -1);
	if (n < 0 || n >= h->numbones) { a.ret_nil(); return; }
	float o[3], ang[3]; GET_BONE_POSITION(e, n, o, ang); a.ret_vec(o); }
// mc.bone_list(ent) -> {"Bip01", "Bip01 Pelvis", ...}
static void f_bone_list(XmodCall *c, void *) { HDR_OR_ERR("bone_list")
	XmodValue *l = g_xmod->val_array(); mstudiobone_t *b = reinterpret_cast<mstudiobone_t *>(reinterpret_cast<byte *>(h) + h->boneindex);
	for (int i = 0; i < h->numbones; i++) g_xmod->val_push(l, g_xmod->val_cstr(b[i].name)); a.ret(l); }

bool reg_studio() {
	return mc_reg("model_info", f_model_info) && mc_reg("seq_lookup", f_seq_lookup) && mc_reg("seq_list", f_seq_list) && mc_reg("seq_info", f_seq_info)
		&& mc_reg("seq_by_activity", f_seq_by_activity) && mc_reg("anim", f_anim) && mc_reg("frame_advance", f_frame_advance)
		&& mc_reg("seq_duration", f_seq_duration) && mc_reg("seq_fps", f_seq_fps) && mc_reg("set_controller", f_set_controller)
		&& mc_reg("set_blending", f_set_blending) && mc_reg("set_body", f_set_body) && mc_reg("get_body", f_get_body)
		&& mc_reg("attachment", f_attachment) && mc_reg("bone", f_bone) && mc_reg("bone_list", f_bone_list);
}
