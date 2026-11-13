// Nexus (server side) - an xmod extension. Talks to the Nexus client library that players install and exposes it to Lua as xmod.nexus.*
//   detection: the server queries the client cvar "nexus_version"; clients that have it get a HELLO and answer with nexus_hello.
//   server -> client: user message "Nexus" (binary ops, see shared/nexus_proto.h), queued per player and paced by a byte budget.
//   client -> server: console commands nexus_hello / nexus_key / nexus_click (validated and rate limited).
// This file has its own Metamod entry points (it needs engine + game DLL hooks), so it does not use xmod_ext_glue.h.
#include "mm_sdk.h"
#include "xmod_ext.h"
#include "../shared/nexus_proto.h"
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

enginefuncs_t g_engfuncs;
globalvars_t *gpGlobals;
meta_globals_t *gpMetaGlobals;
mutil_funcs_t *gpMetaUtilFuncs;
gamedll_funcs_t *gpGamedllFuncs;
plugin_info_t Plugin_info = { META_INTERFACE_VERSION, "nexus", NEXUS_VERSION, __DATE__, "xmod extension", "", "NEXUS", PT_ANYTIME, PT_ANYTIME };
static const XmodApi *X = nullptr;
static XmodExt *g_ext = nullptr;
#ifndef FL_CLIENT
#define FL_CLIENT (1 << 3)
#endif
#ifndef FL_FAKECLIENT
#define FL_FAKECLIENT (1 << 13)
#endif

// ------------------------------------------------------------------------------------------------ per-player state
struct Upd {                          // one element update (only the masked fields are meaningful)
	unsigned mask = 0; int type = 0;
	int xf = 0, yf = 0, xp = 0, yp = 0, wf = 0, hf = 0, wp = 0, hp = 0;
	unsigned char col[4] = {255, 255, 255, 255}, col2[4] = {0, 0, 0, 0};
	int anchor = 0, flags = 0, layer = 0, group = 0, scale = 256, value = 0, ent = 0, wx = 0, wy = 0, wz = 0, maxdist = 0, life = 0, fin = 0, fout = 0;
	std::string text, image;
};
struct Op { int kind; int id; Upd u; std::string raw; };            // kind = NX_OP_*; raw = pre-encoded payload for everything except SET
enum { ST_NONE = 0, ST_PROBE, ST_HELLO, ST_READY, ST_ABSENT };
struct Player {
	int state = ST_NONE, userid = 0, attempts = 0, token = 0, reqid = 0;
	float next = 0, budget = 0, cmd_window = 0; int cmds = 0;
	std::string version; int w = 0, h = 0, caps = 0, proto = 0;
	std::deque<Op> q; unsigned long long sent_bytes = 0, sent_msgs = 0, dropped = 0;
};
static Player g_pl[33];
static int g_msgid = 0, g_reqbase = 1313000000;
static float g_last_time = 0;
static cvar_t cv_rate = {(char *)"nexus_rate", (char *)"5000", 0, 0, nullptr};                   // bytes per second per player
static cvar_t cv_debug = {(char *)"nexus_debug", (char *)"0", 0, 0, nullptr};

static void logf(const char *fmt, ...) { char b[512]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap); if (X) X->log(b); }
static edict_t *ent_of(int id) { return (id >= 1 && id <= gpGlobals->maxClients) ? INDEXENT(id) : nullptr; }
static bool is_human(edict_t *e) { return e && !e->free && e->pvPrivateData && (e->v.flags & FL_CLIENT) && !(e->v.flags & FL_FAKECLIENT) && g_engfuncs.pfnGetPlayerUserId(e) > 0; }

// ------------------------------------------------------------------------------------------------ encoding
static void w8(std::string &b, int v) { b += (char)(v & 0xFF); }
static void w16(std::string &b, int v) { b += (char)(v & 0xFF); b += (char)((v >> 8) & 0xFF); }
static void wstr(std::string &b, const std::string &s) { size_t n = s.size() > 255 ? 255 : s.size(); w8(b, (int)n); b.append(s, 0, n); }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static std::string encode_set(int id, const Upd &u, bool with_text) {
	std::string b; unsigned mask = u.mask; if (!with_text) mask &= ~NX_F_TEXT;
	w8(b, NX_OP_SET); w16(b, id); w8(b, u.type); w16(b, (int)mask);
	if (mask & NX_F_POS) { w16(b, u.xf); w16(b, u.yf); w16(b, u.xp); w16(b, u.yp); }
	if (mask & NX_F_SIZE) { w16(b, u.wf); w16(b, u.hf); w16(b, u.wp); w16(b, u.hp); }
	if (mask & NX_F_COLOR) b.append((const char *)u.col, 4);
	if (mask & NX_F_COLOR2) b.append((const char *)u.col2, 4);
	if (mask & NX_F_STYLE) { w8(b, u.anchor); w8(b, u.flags); w8(b, u.layer); w8(b, u.group); }
	if (mask & NX_F_SCALE) w16(b, u.scale);
	if (mask & NX_F_TEXT) wstr(b, u.text);
	if (mask & NX_F_IMAGE) wstr(b, u.image);
	if (mask & NX_F_VALUE) w16(b, u.value);
	if (mask & NX_F_WORLD) { w16(b, u.ent); w16(b, u.wx); w16(b, u.wy); w16(b, u.wz); w16(b, u.maxdist); }
	if (mask & NX_F_LIFE) { w16(b, u.life); w8(b, u.fin); w8(b, u.fout); }
	return b;
}
static size_t utf8_cut(const std::string &s, size_t pos, size_t max) {      // largest n <= max that does not split a UTF-8 sequence
	size_t n = s.size() - pos < max ? s.size() - pos : max;
	while (n > 0 && pos + n < s.size() && ((unsigned char)s[pos + n] & 0xC0) == 0x80) n--;
	return n ? n : (s.size() - pos < max ? s.size() - pos : max);
}
// turns one queued op into one or more wire blobs (each <= NEXUS_MAX_MSG)
static void encode_op(const Op &op, std::vector<std::string> &out) {
	if (op.kind != NX_OP_SET) { out.push_back(op.raw); return; }
	std::string full = encode_set(op.id, op.u, true);
	if (!(op.u.mask & NX_F_TEXT) || (op.u.text.size() <= 100 && full.size() <= NEXUS_MAX_MSG - 4)) { out.push_back(full); return; }
	out.push_back(encode_set(op.id, op.u, false));                     // long text travels separately, in pieces
	size_t pos = 0; bool first = true;
	do {
		size_t n = utf8_cut(op.u.text, pos, 170);
		std::string b; w8(b, NX_OP_TEXT); w16(b, op.id); w8(b, first ? 0 : 1); wstr(b, op.u.text.substr(pos, n)); out.push_back(b);
		pos += n; first = false;
	} while (pos < op.u.text.size());
}
static void send_blob(edict_t *e, const std::string &b) {
	g_engfuncs.pfnMessageBegin(MSG_ONE, g_msgid, nullptr, e);
	for (unsigned char c : b) g_engfuncs.pfnWriteByte(c);
	g_engfuncs.pfnMessageEnd();
}

// ------------------------------------------------------------------------------------------------ queue
static void merge(Upd &dst, const Upd &s) {
	dst.type = s.type;
	if (s.mask & NX_F_POS) { dst.xf = s.xf; dst.yf = s.yf; dst.xp = s.xp; dst.yp = s.yp; }
	if (s.mask & NX_F_SIZE) { dst.wf = s.wf; dst.hf = s.hf; dst.wp = s.wp; dst.hp = s.hp; }
	if (s.mask & NX_F_COLOR) memcpy(dst.col, s.col, 4);
	if (s.mask & NX_F_COLOR2) memcpy(dst.col2, s.col2, 4);
	if (s.mask & NX_F_STYLE) { dst.anchor = s.anchor; dst.flags = s.flags; dst.layer = s.layer; dst.group = s.group; }
	if (s.mask & NX_F_SCALE) dst.scale = s.scale;
	if (s.mask & NX_F_TEXT) dst.text = s.text;
	if (s.mask & NX_F_IMAGE) dst.image = s.image;
	if (s.mask & NX_F_VALUE) dst.value = s.value;
	if (s.mask & NX_F_WORLD) { dst.ent = s.ent; dst.wx = s.wx; dst.wy = s.wy; dst.wz = s.wz; dst.maxdist = s.maxdist; }
	if (s.mask & NX_F_LIFE) { dst.life = s.life; dst.fin = s.fin; dst.fout = s.fout; }
	dst.mask |= s.mask;
}
static bool q_room(Player &p) { if (p.q.size() >= 4096) { p.dropped++; return false; } return true; }
static void q_set(Player &p, int id, const Upd &u) {
	for (auto it = p.q.rbegin(); it != p.q.rend(); ++it) {             // coalesce with a pending update of the same element (latest value wins)
		if (it->kind == NX_OP_CLEAR || it->kind == NX_OP_RESET) break;     // never move an update in front of a clear
		if (it->id != id || it->kind != NX_OP_SET && it->kind != NX_OP_DEL && it->kind != NX_OP_ANIM && it->kind != NX_OP_TEXT) continue;
		if (it->kind == NX_OP_SET) { merge(it->u, u); return; }
		break;                                                           // a DEL / ANIM for this id is in between: keep the order
	}
	if (!q_room(p)) return;
	Op op; op.kind = NX_OP_SET; op.id = id; op.u = u; p.q.push_back(op);
}
static void q_raw(Player &p, int kind, int id, const std::string &raw) {
	if (kind == NX_OP_DEL) for (auto it = p.q.begin(); it != p.q.end();) { if (it->id == id && (it->kind == NX_OP_SET || it->kind == NX_OP_ANIM)) it = p.q.erase(it); else ++it; }
	if (kind == NX_OP_KEYS || kind == NX_OP_CURSOR || kind == NX_OP_CAMERA || kind == NX_OP_MUSIC) for (auto it = p.q.begin(); it != p.q.end();) { if (it->kind == kind) it = p.q.erase(it); else ++it; }
	if (kind == NX_OP_RESET) p.q.clear();
	if (!q_room(p)) return;
	Op op; op.kind = kind; op.id = id; op.raw = raw; p.q.push_back(op);
}
static void flush_player(int id, Player &p, float dt) {
	float rate = cv_rate.value < 500 ? 500 : cv_rate.value;
	float burst = rate * 0.25f; if (burst > 1200) burst = 1200; if (burst < 400) burst = 400;   // small bursts: the engine's reliable buffer holds < 4 KB and
	p.budget += rate * dt; if (p.budget > burst) p.budget = burst;                               // overflowing it drops the player ("Reliable channel overflowed")
	edict_t *e = ent_of(id); if (!e) return;
	std::string msg; int sent = 0;
	while (!p.q.empty() && sent < 4) {
		std::vector<std::string> blobs; encode_op(p.q.front(), blobs);
		size_t need = 0; for (auto &b : blobs) need += b.size();
		if (p.budget < (float)need && !(sent == 0 && msg.empty() && p.budget >= burst * 0.95f)) break;   // an op larger than the burst still goes out when the bucket is full
		p.q.pop_front(); p.budget -= (float)need;
		for (auto &b : blobs) {
			if (msg.size() + b.size() > NEXUS_MAX_MSG) { send_blob(e, msg); p.sent_bytes += msg.size(); p.sent_msgs++; msg.clear(); sent++; }
			msg += b;
		}
	}
	if (!msg.empty()) { send_blob(e, msg); p.sent_bytes += msg.size(); p.sent_msgs++; }
}

// ------------------------------------------------------------------------------------------------ handshake / lifecycle
static void reset_player(int id, bool notify) {
	Player &p = g_pl[id];
	if (notify && p.state == ST_READY) { XmodValue *a = X->val_array(); X->val_push(a, X->val_int(id)); X->emit_event("nexus_lost", a); }
	p = Player();
}
static XmodValue *info_value(const Player &p) {
	XmodValue *m = X->val_map();
	X->val_set(m, "version", X->val_cstr(p.version.c_str())); X->val_set(m, "proto", X->val_int(p.proto)); X->val_set(m, "w", X->val_int(p.w)); X->val_set(m, "h", X->val_int(p.h));
	X->val_set(m, "gl", X->val_bool(p.caps & NX_CAP_GL)); X->val_set(m, "cursor", X->val_bool(p.caps & NX_CAP_CURSOR)); X->val_set(m, "world", X->val_bool(p.caps & NX_CAP_WORLD));
	X->val_set(m, "images", X->val_bool(p.caps & NX_CAP_IMAGES)); X->val_set(m, "caps", X->val_int(p.caps));
	return m;
}
static void send_hello(int id) {
	Player &p = g_pl[id]; edict_t *e = ent_of(id); if (!e || !g_msgid) return;
	if (!p.token) p.token = 1 + (rand() % 65000);
	std::string b; w8(b, NX_OP_HELLO); w16(b, p.token); send_blob(e, b);
	p.state = ST_HELLO; p.attempts++; p.next = gpGlobals->time + 4.0f;
}
static void probe(int id) {
	Player &p = g_pl[id]; edict_t *e = ent_of(id); if (!e) return;
	if (p.attempts >= 4) { p.state = ST_ABSENT; return; }
	p.attempts++; p.next = gpGlobals->time + 3.0f; p.state = ST_PROBE;
	if (g_engfuncs.pfnQueryClientCvarValue2) { p.reqid = ++g_reqbase; g_engfuncs.pfnQueryClientCvarValue2(e, NEXUS_CVAR, p.reqid); }
	else p.state = ST_ABSENT;                                           // engine without client cvar queries: Nexus cannot be detected safely, stay off
}
static void think() {
	float now = gpGlobals->time, dt = now - g_last_time; if (dt < 0 || dt > 1) dt = 0; g_last_time = now;
	cv_rate.value = g_engfuncs.pfnCVarGetFloat("nexus_rate");
	for (int id = 1; id <= gpGlobals->maxClients && id <= 32; id++) {
		Player &p = g_pl[id]; edict_t *e = ent_of(id);
		if (!is_human(e)) { if (p.state != ST_NONE) reset_player(id, true); continue; }
		int uid = g_engfuncs.pfnGetPlayerUserId(e);
		if (p.userid != uid) { reset_player(id, true); p.userid = uid; p.next = now + 1.5f; }     // a new player took this slot
		if (p.state == ST_NONE && now >= p.next) probe(id);
		else if (p.state == ST_PROBE && now >= p.next) probe(id);
		else if (p.state == ST_HELLO && now >= p.next) { if (p.attempts >= 4) p.state = ST_ABSENT; else send_hello(id); }
		else if (p.state == ST_READY) flush_player(id, p, dt);
	}
}

// ------------------------------------------------------------------------------------------------ Metamod hooks
static void Hook_StartFrame() { if (X && g_msgid) think(); RETURN_META(MRES_IGNORED); }
static void Hook_ServerActivate_Post(edict_t *, int, int) {
	if (!g_msgid) g_msgid = g_engfuncs.pfnRegUserMsg(NEXUS_MSG_NAME, -1);
	for (int i = 1; i <= 32; i++) reset_player(i, true);               // every client reconnects on a map change and starts clean
	g_last_time = 0;
	RETURN_META(MRES_IGNORED);
}
static void Hook_ClientDisconnect(edict_t *e) { int id = ENTINDEX(e); if (id >= 1 && id <= 32) reset_player(id, true); RETURN_META(MRES_IGNORED); }
static void Hook_CvarValue2(const edict_t *e, int req, const char *name, const char *value) {
	int id = ENTINDEX(const_cast<edict_t *>(e));
	if (id >= 1 && id <= 32 && g_pl[id].state == ST_PROBE && g_pl[id].reqid == req && name && !strcmp(name, NEXUS_CVAR)) {
		Player &p = g_pl[id];
		if (value && value[0] >= '0' && value[0] <= '9') { p.attempts = 0; send_hello(id); }
		else p.state = ST_ABSENT;                                         // "Bad CVAR request": this client has no Nexus
	}
	RETURN_META(MRES_IGNORED);
}
static bool safe_name(const std::string &s);
static bool cmd_allowed(Player &p) {                                  // max 80 Nexus commands per second per player
	float now = gpGlobals->time; if (now - p.cmd_window >= 1.0f || now < p.cmd_window) { p.cmd_window = now; p.cmds = 0; }
	return ++p.cmds <= 80;
}
static void Hook_ClientCommand(edict_t *e) {
	const char *c = CMD_ARGV(0);
	if (!c || strncmp(c, "nexus_", 6)) RETURN_META(MRES_IGNORED);
	int id = ENTINDEX(e); if (id < 1 || id > 32 || !X) RETURN_META(MRES_SUPERCEDE);
	Player &p = g_pl[id]; if (!cmd_allowed(p)) RETURN_META(MRES_SUPERCEDE);
	int argc = CMD_ARGC();
	if (!strcmp(c, "nexus_hello") && argc >= 7) {
		if (p.state != ST_HELLO || atoi(CMD_ARGV(6)) != p.token) RETURN_META(MRES_SUPERCEDE);
		p.proto = atoi(CMD_ARGV(1)); std::string v = CMD_ARGV(2); if (v.size() > 16) v.resize(16);
		for (char &ch : v) if (!(isalnum((unsigned char)ch) || ch == '.' || ch == '-')) ch = '?';
		p.version = v; p.w = clampi(atoi(CMD_ARGV(3)), 0, 16384); p.h = clampi(atoi(CMD_ARGV(4)), 0, 16384); p.caps = atoi(CMD_ARGV(5));
		if (p.proto != NEXUS_PROTO) { logf("nexus: player %d has protocol %d (server has %d) - ignored, the client library must be updated", id, p.proto, NEXUS_PROTO); p.state = ST_ABSENT; RETURN_META(MRES_SUPERCEDE); }
		p.state = ST_READY; p.q.clear(); p.budget = 600;
		std::string r; w8(r, NX_OP_RESET); q_raw(p, NX_OP_RESET, 0, r);
		XmodValue *a = X->val_array(); X->val_push(a, X->val_int(id)); X->val_push(a, info_value(p)); X->emit_event("nexus_ready", a);
		if (cv_debug.value) logf("nexus: player %d ready (client %s, %dx%d, caps %d)", id, p.version.c_str(), p.w, p.h, p.caps);
	} else if (!strcmp(c, "nexus_key") && argc >= 3 && p.state == ST_READY) {
		XmodValue *a = X->val_array(); X->val_push(a, X->val_int(id)); X->val_push(a, X->val_int(clampi(atoi(CMD_ARGV(1)), 0, 255))); X->val_push(a, X->val_bool(atoi(CMD_ARGV(2)) != 0));
		X->emit_event("nexus_key", a);
	} else if (!strcmp(c, "nexus_data") && argc >= 3 && p.state == ST_READY) {
		std::string ch = CMD_ARGV(1), hex = CMD_ARGV(2), data;
		if (safe_name(ch) && ch.size() <= 31 && hex.size() <= NEXUS_MAX_DATA * 2 && hex.size() % 2 == 0) {
			bool ok = true; auto hv = [&](char x) { if (x >= '0' && x <= '9') return x - '0'; if (x >= 'a' && x <= 'f') return x - 'a' + 10; if (x >= 'A' && x <= 'F') return x - 'A' + 10; ok = false; return 0; };
			for (size_t i = 0; i < hex.size(); i += 2) { int a = hv(hex[i]), b = hv(hex[i + 1]); data += (char)(a * 16 + b); }
			if (ok) { XmodValue *a = X->val_array(); X->val_push(a, X->val_int(id)); X->val_push(a, X->val_cstr(ch.c_str())); X->val_push(a, X->val_str(data.data(), data.size())); X->emit_event("nexus_data", a); }
		}
	} else if (!strcmp(c, "nexus_click") && argc >= 3 && p.state == ST_READY) {
		XmodValue *a = X->val_array(); X->val_push(a, X->val_int(id)); X->val_push(a, X->val_int(clampi(atoi(CMD_ARGV(1)), 0, 65535))); X->val_push(a, X->val_int(clampi(atoi(CMD_ARGV(2)), 0, 3)));
		X->emit_event("nexus_click", a);
	}
	RETURN_META(MRES_SUPERCEDE);
}

// ------------------------------------------------------------------------------------------------ Lua-facing functions (all sync: they only queue bytes)
static bool get_num(const XmodValue *m, const char *k, double &out) { const XmodValue *v = X->val_get(m, k); int t = X->val_type(v); if (t != XV_INT && t != XV_NUM) return false; out = X->val_get_num(v, 0); return true; }
static bool get_flag(const XmodValue *m, const char *k, bool &present) { const XmodValue *v = X->val_get(m, k); present = X->val_type(v) == XV_BOOL; return present && X->val_get_bool(v, 0); }
static bool get_color(const XmodValue *m, const char *k, unsigned char *c) {
	const XmodValue *v = X->val_get(m, k); if (X->val_type(v) != XV_ARRAY || X->val_len(v) < 3) return false;
	for (int i = 0; i < 4; i++) c[i] = (unsigned char)clampi((int)X->val_get_num(X->val_at(v, i), i == 3 ? 255 : 0), 0, 255);
	return true;
}
static bool get_vec(const XmodValue *m, const char *k, int *o) {
	const XmodValue *v = X->val_get(m, k); if (X->val_type(v) != XV_ARRAY || X->val_len(v) < 3) return false;
	for (int i = 0; i < 3; i++) o[i] = clampi((int)X->val_get_num(X->val_at(v, i), 0), -32767, 32767);
	return true;
}
static int frac(double f) { return clampi((int)(f * 10000.0 + (f < 0 ? -0.5 : 0.5)), -32767, 32767); }
static bool safe_name(const std::string &s) {
	if (s.empty() || s.size() > NEXUS_MAX_NAME || s[0] == '/' || s.find("..") != std::string::npos) return false;
	for (char c : s) if (!(isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.' || c == '/')) return false;
	return true;
}
static const char *const ANCHORS[] = {"topleft", "top", "topright", "left", "center", "right", "bottomleft", "bottom", "bottomright"};
static bool parse_props(const XmodValue *m, Upd &u, std::string &err) {
	if (X->val_type(m) != XV_MAP) { err = "props must be a table"; return false; }
	const char *t = X->val_get_str(X->val_get(m, "type"), nullptr);
	if (!t) { err = "props.type is required (\"text\", \"rect\", \"image\" or \"bar\")"; return false; }
	u.type = !strcmp(t, "text") ? NX_T_TEXT : !strcmp(t, "rect") ? NX_T_RECT : !strcmp(t, "image") ? NX_T_IMAGE : !strcmp(t, "bar") ? NX_T_BAR : 0;
	if (!u.type) { err = "unknown element type"; return false; }
	double a, b; bool ha, hb;
	ha = get_num(m, "x", a); hb = get_num(m, "y", b); double c, d; bool hc = get_num(m, "px", c), hd = get_num(m, "py", d);
	if (ha || hb || hc || hd) { u.mask |= NX_F_POS; u.xf = ha ? frac(a) : 0; u.yf = hb ? frac(b) : 0; u.xp = hc ? clampi((int)c, -32767, 32767) : 0; u.yp = hd ? clampi((int)d, -32767, 32767) : 0; }
	ha = get_num(m, "w", a); hb = get_num(m, "h", b); hc = get_num(m, "pw", c); hd = get_num(m, "ph", d);
	if (ha || hb || hc || hd) { u.mask |= NX_F_SIZE; u.wf = ha ? frac(a) : 0; u.hf = hb ? frac(b) : 0; u.wp = hc ? clampi((int)c, -32767, 32767) : 0; u.hp = hd ? clampi((int)d, -32767, 32767) : 0; }
	if (get_color(m, "color", u.col)) u.mask |= NX_F_COLOR;
	if (get_color(m, "color2", u.col2)) u.mask |= NX_F_COLOR2;
	bool style = false, pr; u.flags = 0;
	const XmodValue *av = X->val_get(m, "anchor");
	if (X->val_type(av) == XV_STR) { style = true; const char *s = X->val_get_str(av, nullptr); u.anchor = -1; for (int i = 0; i < 9; i++) if (!strcmp(s, ANCHORS[i])) u.anchor = i; if (u.anchor < 0) { err = "unknown anchor"; return false; } }
	else if (X->val_type(av) == XV_INT) { style = true; u.anchor = clampi((int)X->val_get_int(av, 0), 0, 8); }
	if (get_num(m, "layer", a)) { style = true; u.layer = clampi((int)a, -127, 127); }
	if (get_num(m, "group", a)) { style = true; u.group = clampi((int)a, 0, 254); }
	static const struct { const char *k; int f; } FL[] = {{"shadow", NX_FL_SHADOW}, {"outline", NX_FL_OUTLINE}, {"click", NX_FL_CLICK}, {"occlude", NX_FL_OCCLUDE}, {"additive", NX_FL_ADDITIVE}, {"hidden", NX_FL_HIDDEN}, {"distscale", NX_FL_DISTSCALE}, {"markup", NX_FL_MARKUP}};
	for (auto &f : FL) { if (get_flag(m, f.k, pr)) u.flags |= f.f; if (pr) style = true; }
	if (style) u.mask |= NX_F_STYLE;
	if (get_num(m, "scale", a)) { u.mask |= NX_F_SCALE; u.scale = clampi((int)(a * 256.0), 16, 65535); }
	size_t n; const char *s = X->val_get_str(X->val_get(m, "text"), &n);
	if (s) { u.mask |= NX_F_TEXT; u.text.assign(s, n > NEXUS_MAX_TEXT ? NEXUS_MAX_TEXT : n); }
	s = X->val_get_str(X->val_get(m, "image"), &n);
	if (s) { u.image.assign(s, n); if (!safe_name(u.image)) { err = "bad image name (relative path, [a-zA-Z0-9_./-], max 63 chars)"; return false; } u.mask |= NX_F_IMAGE; }
	if (get_num(m, "value", a)) { u.mask |= NX_F_VALUE; u.value = clampi((int)(a * 10000.0), 0, 10000); }
	int v3[3]; bool he = get_num(m, "ent", a);
	if (he || get_vec(m, "pos3", v3) || X->val_get(m, "offset")) {
		u.mask |= NX_F_WORLD; u.ent = he ? clampi((int)a, 0, 4095) : 0;
		if (get_vec(m, u.ent ? "offset" : "pos3", v3)) { u.wx = v3[0]; u.wy = v3[1]; u.wz = v3[2]; }
		if (get_num(m, "maxdist", a)) u.maxdist = clampi((int)a, 0, 65535);
	}
	ha = get_num(m, "life", a); hb = get_num(m, "fadein", b); hc = get_num(m, "fadeout", c);
	if (ha || hb || hc) { u.mask |= NX_F_LIFE; u.life = ha ? clampi((int)(a * 100.0), 0, 65535) : 0; u.fin = hb ? clampi((int)(b * 20.0), 0, 255) : 0; u.fout = hc ? clampi((int)(c * 20.0), 0, 255) : 0; }
	return true;
}
// calls fn for the target player (id) or every ready player (id == 0); returns how many were addressed
template <class F> static int each(int id, F fn) {
	int n = 0;
	if (id == 0) { for (int i = 1; i <= 32; i++) if (g_pl[i].state == ST_READY) { fn(g_pl[i]); n++; } }
	else if (id >= 1 && id <= 32 && g_pl[id].state == ST_READY) { fn(g_pl[id]); n = 1; }
	return n;
}
static int arg_int(XmodCall *c, int i, int d = 0) { return (int)X->val_get_num(X->call_arg(c, i), d); }
static void ret_int(XmodCall *c, int v) { X->call_return(c, X->val_int(v)); }

static void f_has(XmodCall *c, void *) { int id = arg_int(c, 0); X->call_return(c, X->val_bool(id >= 1 && id <= 32 && g_pl[id].state == ST_READY)); }
static void f_info(XmodCall *c, void *) { int id = arg_int(c, 0); if (id >= 1 && id <= 32 && g_pl[id].state == ST_READY) X->call_return(c, info_value(g_pl[id])); }
static void f_players(XmodCall *c, void *) { XmodValue *a = X->val_array(); for (int i = 1; i <= 32; i++) if (g_pl[i].state == ST_READY) X->val_push(a, X->val_int(i)); X->call_return(c, a); }
static void f_set(XmodCall *c, void *) {          // set(player|0, elem_id, props) -> number of players addressed
	int id = arg_int(c, 0), eid = arg_int(c, 1); Upd u; std::string err;
	if (eid < 1 || eid > 65535) { X->call_error(c, "element id must be 1..65535"); return; }
	if (!parse_props(X->call_arg(c, 2), u, err)) { X->call_error(c, err.c_str()); return; }
	ret_int(c, each(id, [&](Player &p) { q_set(p, eid, u); }));
}
static void f_del(XmodCall *c, void *) { int id = arg_int(c, 0), eid = arg_int(c, 1); std::string b; w8(b, NX_OP_DEL); w16(b, eid); ret_int(c, each(id, [&](Player &p) { q_raw(p, NX_OP_DEL, eid, b); })); }
static void f_clear(XmodCall *c, void *) {        // clear(player|0 [, group])
	int id = arg_int(c, 0), grp = X->call_argc(c) >= 2 ? clampi(arg_int(c, 1), 0, 254) : 255; std::string b; w8(b, NX_OP_CLEAR); w8(b, grp);
	ret_int(c, each(id, [&](Player &p) {
		for (auto it = p.q.begin(); it != p.q.end();) { if (grp == 255 && (it->kind == NX_OP_SET || it->kind == NX_OP_ANIM || it->kind == NX_OP_DEL || it->kind == NX_OP_TEXT)) it = p.q.erase(it); else ++it; }
		q_raw(p, NX_OP_CLEAR, 0, b); }));
}
static void f_anim(XmodCall *c, void *) {         // anim(player|0, elem_id, {x=,y= | px=,py= | alpha= | scale= | value= | color=, time=, ease=, remove=})
	int id = arg_int(c, 0), eid = arg_int(c, 1); const XmodValue *m = X->call_arg(c, 2);
	if (X->val_type(m) != XV_MAP) { X->call_error(c, "usage: xmod.nexus.anim(player, elem_id, {alpha=0, time=1})"); return; }
	double t = 0.5, a, b2; get_num(m, "time", t); int dur = clampi((int)(t * 100.0), 1, 65535);
	const char *es = X->val_get_str(X->val_get(m, "ease"), nullptr); int ease = !es ? 1 : !strcmp(es, "linear") ? 0 : !strcmp(es, "inout") ? 2 : 1;
	bool pr; int fl = get_flag(m, "remove", pr) ? 1 : 0; int count = 0, total = 0; unsigned char col[4];
	auto push = [&](int prop, const std::string &target, bool last) {
		std::string b; w8(b, NX_OP_ANIM); w16(b, eid); w8(b, prop); b += target; w16(b, dur); w8(b, ease); w8(b, last ? fl : 0);
		total = each(id, [&](Player &p) { q_raw(p, NX_OP_ANIM, eid, b); }); count++;
	};
	std::vector<std::pair<int, std::string>> ops; std::string tg;
	bool hx = get_num(m, "x", a), hy = get_num(m, "y", b2);
	if (hx && hy) { tg.clear(); w16(tg, frac(a)); w16(tg, frac(b2)); ops.emplace_back(NX_A_POS, tg); }
	hx = get_num(m, "px", a); hy = get_num(m, "py", b2);
	if (hx && hy) { tg.clear(); w16(tg, clampi((int)a, -32767, 32767)); w16(tg, clampi((int)b2, -32767, 32767)); ops.emplace_back(NX_A_PX, tg); }
	if (get_num(m, "alpha", a)) { tg.clear(); w8(tg, clampi((int)a, 0, 255)); ops.emplace_back(NX_A_ALPHA, tg); }
	if (get_num(m, "scale", a)) { tg.clear(); w16(tg, clampi((int)(a * 256.0), 16, 65535)); ops.emplace_back(NX_A_SCALE, tg); }
	if (get_num(m, "value", a)) { tg.clear(); w16(tg, clampi((int)(a * 10000.0), 0, 10000)); ops.emplace_back(NX_A_VALUE, tg); }
	if (get_color(m, "color", col)) { tg.assign((const char *)col, 4); ops.emplace_back(NX_A_COLOR, tg); }
	if (ops.empty()) { X->call_error(c, "anim needs at least one target: x+y, px+py, alpha, scale, value or color"); return; }
	for (size_t i = 0; i < ops.size(); i++) push(ops[i].first, ops[i].second, i + 1 == ops.size());
	ret_int(c, total);
}
static void f_keys(XmodCall *c, void *) {         // keys(player|0, { {key=<keynum>, block=true}, ... })   replaces the list; {} unsubscribes
	int id = arg_int(c, 0); const XmodValue *l = X->call_arg(c, 1); std::string b; w8(b, NX_OP_KEYS);
	size_t n = X->val_type(l) == XV_ARRAY ? X->val_len(l) : 0; if (n > NEXUS_MAX_KEYS) n = NEXUS_MAX_KEYS; w8(b, (int)n);
	for (size_t i = 0; i < n; i++) { const XmodValue *k = X->val_at(l, i); bool pr; int key = X->val_type(k) == XV_MAP ? (int)X->val_get_num(X->val_get(k, "key"), 0) : (int)X->val_get_num(k, 0);
		w8(b, clampi(key, 0, 255)); w8(b, (X->val_type(k) == XV_MAP && get_flag(k, "block", pr)) ? 1 : 0); }
	ret_int(c, each(id, [&](Player &p) { q_raw(p, NX_OP_KEYS, 0, b); }));
}
static void f_cursor(XmodCall *c, void *) { int id = arg_int(c, 0); std::string b; w8(b, NX_OP_CURSOR); w8(b, X->val_get_bool(X->call_arg(c, 1), 0) ? 1 : 0); ret_int(c, each(id, [&](Player &p) { q_raw(p, NX_OP_CURSOR, 0, b); })); }
static void f_sound(XmodCall *c, void *) {        // sound(player|0, "path/file.wav" [, volume 0..1])
	int id = arg_int(c, 0); size_t n; const char *s = X->val_get_str(X->call_arg(c, 1), &n); std::string name = s ? std::string(s, n) : "";
	if (!safe_name(name) || name.size() < 5 || name.compare(name.size() - 4, 4, ".wav")) { X->call_error(c, "sound must be a relative .wav path below sound/ ([a-zA-Z0-9_./-])"); return; }
	std::string b; w8(b, NX_OP_SOUND); w8(b, clampi((int)(X->val_get_num(X->call_arg(c, 2), 1.0) * 255.0), 0, 255)); wstr(b, name);
	ret_int(c, each(id, [&](Player &p) { q_raw(p, NX_OP_SOUND, 0, b); }));
}
static void f_camera(XmodCall *c, void *) {       // camera(player|0, {mode="third"|"fixed"|"off", dist=, height=, side=, collide=, pos={x,y,z}, angles={pitch,yaw,roll}, time=})
	int id = arg_int(c, 0); const XmodValue *m = X->call_arg(c, 1); int mode = 0, v[6] = {0, 0, 0, 0, 0, 0}, fl = 1; double t = 0.4, a; bool pr;
	if (X->val_type(m) == XV_MAP) {
		const char *ms = X->val_get_str(X->val_get(m, "mode"), nullptr);
		mode = !ms ? 0 : !strcmp(ms, "third") ? 1 : !strcmp(ms, "fixed") ? 2 : !strcmp(ms, "off") ? 0 : -1;
		if (mode < 0) { X->call_error(c, "camera mode must be \"third\", \"fixed\" or \"off\""); return; }
		get_num(m, "time", t); if (!get_flag(m, "collide", pr) && pr) fl = 0;
		if (mode == 1) { v[0] = 120; if (get_num(m, "dist", a)) v[0] = clampi((int)a, 16, 2000); if (get_num(m, "height", a)) v[1] = clampi((int)a, -500, 2000); if (get_num(m, "side", a)) v[2] = clampi((int)a, -500, 500); }
		if (mode == 2) { int p3[3];
			if (!get_vec(m, "pos", p3)) { X->call_error(c, "fixed camera needs pos = {x, y, z}"); return; }
			v[0] = p3[0]; v[1] = p3[1]; v[2] = p3[2];
			const XmodValue *an = X->val_get(m, "angles"); if (X->val_type(an) == XV_ARRAY) for (int i = 0; i < 3; i++) v[3 + i] = clampi((int)(X->val_get_num(X->val_at(an, i), 0) * 10.0), -32767, 32767); }
	}
	std::string b; w8(b, NX_OP_CAMERA); w8(b, mode); w16(b, clampi((int)(t * 100.0), 0, 65535)); w8(b, fl); for (int i = 0; i < 6; i++) w16(b, v[i]);
	ret_int(c, each(id, [&](Player &p) { q_raw(p, NX_OP_CAMERA, 0, b); }));
}
static void f_music(XmodCall *c, void *) {        // music(player|0, "sound/music/file.mp3" [, loop])   music(player|0) stops
	int id = arg_int(c, 0); size_t n; const char *s = X->val_get_str(X->call_arg(c, 1), &n); std::string name = s ? std::string(s, n) : ""; int mode = 0;
	if (!name.empty()) {
		if (!safe_name(name) || name.size() < 5 || name.compare(name.size() - 4, 4, ".mp3")) { X->call_error(c, "music must be a relative .mp3 path ([a-zA-Z0-9_./-]), e.g. \"sound/music/theme.mp3\""); return; }
		mode = X->val_get_bool(X->call_arg(c, 2), 0) ? 2 : 1;
	}
	std::string b; w8(b, NX_OP_MUSIC); w8(b, mode); wstr(b, name);
	ret_int(c, each(id, [&](Player &p) { q_raw(p, NX_OP_MUSIC, 0, b); }));
}
static void f_send(XmodCall *c, void *) {         // send(player|0, channel, data)  -> client plugins listening on that channel
	int id = arg_int(c, 0); size_t n, dn; const char *s = X->val_get_str(X->call_arg(c, 1), &n), *d = X->val_get_str(X->call_arg(c, 2), &dn);
	std::string ch = s ? std::string(s, n) : "";
	if (!safe_name(ch) || ch.size() > 31 || !d) { X->call_error(c, "usage: xmod.nexus.send(player, channel, data)  (channel: [a-zA-Z0-9_./-], max 31 chars)"); return; }
	if (dn > NEXUS_MAX_DATA) { X->call_error(c, "data is too long (max 96 bytes per message)"); return; }
	std::string b; w8(b, NX_OP_DATA); wstr(b, ch); wstr(b, std::string(d, dn));
	ret_int(c, each(id, [&](Player &p) { q_raw(p, NX_OP_DATA, 0, b); }));
}
static void f_reset(XmodCall *c, void *) { int id = arg_int(c, 0); std::string b; w8(b, NX_OP_RESET); ret_int(c, each(id, [&](Player &p) { q_raw(p, NX_OP_RESET, 0, b); })); }
static void svr_nexus() {                          // server console: nexus
	static const char *const ST[] = {"waiting", "probing", "hello", "READY", "no nexus"}; char buf[256];
	snprintf(buf, sizeof buf, "nexus %s (protocol %d, message id %d, rate %d B/s)\n", NEXUS_VERSION, NEXUS_PROTO, g_msgid, (int)cv_rate.value); g_engfuncs.pfnServerPrint(buf);
	for (int i = 1; i <= gpGlobals->maxClients && i <= 32; i++) { Player &p = g_pl[i]; if (p.state == ST_NONE && !p.userid) continue;
		snprintf(buf, sizeof buf, "  #%-2d %-9s client %-8s %dx%d caps %d  queued %zu  sent %llu B\n", i, ST[p.state], p.version.c_str(), p.w, p.h, p.caps, p.q.size(), p.sent_bytes); g_engfuncs.pfnServerPrint(buf); }
}
static void f_stats(XmodCall *c, void *) {
	XmodValue *m = X->val_map(); int ready = 0, absent = 0; unsigned long long bytes = 0, msgs = 0, dropped = 0; size_t queued = 0;
	for (int i = 1; i <= 32; i++) { Player &p = g_pl[i]; if (p.state == ST_READY) ready++; if (p.state == ST_ABSENT) absent++; bytes += p.sent_bytes; msgs += p.sent_msgs; dropped += p.dropped; queued += p.q.size(); }
	X->val_set(m, "ready", X->val_int(ready)); X->val_set(m, "without_nexus", X->val_int(absent)); X->val_set(m, "sent_bytes", X->val_int((int64_t)bytes)); X->val_set(m, "sent_messages", X->val_int((int64_t)msgs));
	X->val_set(m, "queued_ops", X->val_int((int64_t)queued)); X->val_set(m, "dropped_ops", X->val_int((int64_t)dropped)); X->val_set(m, "version", X->val_cstr(NEXUS_VERSION)); X->val_set(m, "proto", X->val_int(NEXUS_PROTO));
	X->call_return(c, m);
}

// ------------------------------------------------------------------------------------------------ Metamod entry points
static DLL_FUNCTIONS g_pre, g_post; static NEW_DLL_FUNCTIONS g_new;
extern "C" {
C_DLLEXPORT int GetEntityAPI2(DLL_FUNCTIONS *t, int *) {
	memset(&g_pre, 0, sizeof g_pre); g_pre.pfnStartFrame = Hook_StartFrame; g_pre.pfnClientCommand = Hook_ClientCommand; g_pre.pfnClientDisconnect = Hook_ClientDisconnect;
	memcpy(t, &g_pre, sizeof g_pre); return 1;
}
C_DLLEXPORT int GetEntityAPI2_Post(DLL_FUNCTIONS *t, int *) { memset(&g_post, 0, sizeof g_post); g_post.pfnServerActivate = Hook_ServerActivate_Post; memcpy(t, &g_post, sizeof g_post); return 1; }
C_DLLEXPORT int GetNewDLLFunctions(NEW_DLL_FUNCTIONS *t, int *) { memset(&g_new, 0, sizeof g_new); g_new.pfnCvarValue2 = Hook_CvarValue2; memcpy(t, &g_new, sizeof g_new); return 1; }
C_DLLEXPORT void WINAPI GiveFnptrsToDll(enginefuncs_t *pengfuncsFromEngine, globalvars_t *pGlobals) { memcpy(&g_engfuncs, pengfuncsFromEngine, sizeof(enginefuncs_t)); gpGlobals = pGlobals; }
C_DLLEXPORT int Meta_Query(char *, plugin_info_t **plinfo, mutil_funcs_t *pMetaUtilFuncs) { *plinfo = &Plugin_info; gpMetaUtilFuncs = pMetaUtilFuncs; return 1; }
C_DLLEXPORT int Meta_Attach(PLUG_LOADTIME, META_FUNCTIONS *ft, meta_globals_t *pMGlobals, gamedll_funcs_t *pGamedllFuncs) {
	gpMetaGlobals = pMGlobals; gpGamedllFuncs = pGamedllFuncs; memset(ft, 0, sizeof(META_FUNCTIONS));
	ft->pfnGetEntityAPI2 = GetEntityAPI2; ft->pfnGetEntityAPI2_Post = GetEntityAPI2_Post; ft->pfnGetNewDLLFunctions = GetNewDLLFunctions;
	X = xmod_ext_connect();
	if (!X) { LOG_ERROR(PLID, "xmod is not loaded (or too old): list xmod BEFORE nexus in addons/metamod/plugins.ini"); return 0; }
	g_ext = X->ext_register("nexus", NEXUS_VERSION);
	if (!g_ext) { LOG_ERROR(PLID, "xmod refused to register the nexus extension"); return 0; }
	bool ok = X->register_func(g_ext, "nexus", "has", f_has, nullptr) && X->register_func(g_ext, "nexus", "info", f_info, nullptr) && X->register_func(g_ext, "nexus", "players", f_players, nullptr)
		&& X->register_func(g_ext, "nexus", "set", f_set, nullptr) && X->register_func(g_ext, "nexus", "del", f_del, nullptr) && X->register_func(g_ext, "nexus", "clear", f_clear, nullptr)
		&& X->register_func(g_ext, "nexus", "anim", f_anim, nullptr) && X->register_func(g_ext, "nexus", "keys", f_keys, nullptr) && X->register_func(g_ext, "nexus", "cursor", f_cursor, nullptr)
		&& X->register_func(g_ext, "nexus", "sound", f_sound, nullptr) && X->register_func(g_ext, "nexus", "stats", f_stats, nullptr)
		&& X->register_func(g_ext, "nexus", "camera", f_camera, nullptr) && X->register_func(g_ext, "nexus", "music", f_music, nullptr)
		&& X->register_func(g_ext, "nexus", "send", f_send, nullptr) && X->register_func(g_ext, "nexus", "reset", f_reset, nullptr);
	if (!ok) { X->ext_unregister(g_ext); g_ext = nullptr; return 0; }
	if (!g_engfuncs.pfnCVarGetPointer("nexus_rate")) g_engfuncs.pfnCVarRegister(&cv_rate);
	if (!g_engfuncs.pfnCVarGetPointer("nexus_debug")) g_engfuncs.pfnCVarRegister(&cv_debug);
	static bool cmd_added = false; if (!cmd_added) { cmd_added = true; g_engfuncs.pfnAddServerCommand((char *)"nexus", svr_nexus); }
	g_msgid = g_engfuncs.pfnRegUserMsg(NEXUS_MSG_NAME, -1);            // clients learn about it when they connect
	logf("nexus %s ready (protocol %d, message id %d)", NEXUS_VERSION, NEXUS_PROTO, g_msgid);
	return 1;
}
C_DLLEXPORT int Meta_Detach(PLUG_LOADTIME, PL_UNLOAD_REASON) {
	if (g_ext) { for (int i = 1; i <= 32; i++) { Player &p = g_pl[i]; if (p.state == ST_READY) { edict_t *e = ent_of(i); std::string b; w8(b, NX_OP_RESET); if (e) send_blob(e, b); } p = Player(); }
		X->ext_unregister(g_ext); g_ext = nullptr; }
	return 1;
}
}
