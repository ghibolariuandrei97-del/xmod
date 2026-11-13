// Nexus client core. Receives ops from the server (user message "Nexus"), keeps the element list and draws it after the game's HUD.
// Everything the server can make this code do is listed in shared/nexus_proto.h - there is no way to run commands, change cvars or load code.
#include "nx_sdk.h"
#include "ref_params.h"
#include "nx_client.h"
#include "nx_platform.h"
#include "nx_render.h"
#include "../shared/nexus_proto.h"
#include "../include/nexus_plugin.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <string>
#include <unordered_map>
#include <vector>

namespace nxc {
static cl_enginefunc_t g_eng; static bool g_have_eng = false, g_inited = false;
static cvar_t *cv_enable, *cv_scale, *cv_keys, *cv_speed, *cv_debug;
static int g_w = 1024, g_h = 768; static float g_unit = 1; static bool g_gl = false, g_drawing = false;
static float g_vieworg[3]; static bool g_have_view = false;

struct Anim { int prop; float from[4], to[4]; double t0; float dur; int ease; bool remove; };
struct Layout { bool valid = false; float px = 0, wrap = 0, w = 0, h = 0, lh = 0; bool gl = false; std::vector<std::pair<size_t, size_t>> lines; std::vector<float> lw; };
struct Elem {
	int id = 0, type = 0; float xf = 0, yf = 0, xp = 0, yp = 0, wf = 0, hf = 0, wp = 0, hp = 0;
	float col[4] = {1, 1, 1, 1}, col2[4] = {0, 0, 0, 0}; int anchor = 0, flags = 0, layer = 0, group = 0; float scale = 1, value = 0;
	std::string text, image; int img = -2;                                 // -2 = not looked up yet
	bool world = false; int ent = 0; float w3[3] = {0, 0, 0}, maxdist = 0;
	double born = 0; float life = 0, fin = 0, fout = 0;
	std::vector<Anim> anims; Layout lay;
	float rx = 0, ry = 0, rw = 0, rh = 0; bool shown = false;              // where it was drawn last frame (click tests)
};
static struct Session {
	bool on = false; std::unordered_map<int, Elem> el; unsigned char keys[256]; bool cursor = false; float cx = 0, cy = 0, lock[3];
	int cam_mode = 0; float cam[6]; bool cam_collide = true; double cam_t0 = 0; float cam_dur = 0, cam_from = 0, cam_to = 0;
	int hover = 0; unsigned long long ops = 0, msgs = 0, bad = 0;
} S;
struct Plug { std::string name; void *lib; NexusPlugin p; };
static std::vector<Plug> g_plugins;
static double g_cmd_t = 0; static int g_cmd_n = 0;

static void conf(const char *fmt, ...) {
	if (!g_have_eng || !g_eng.Con_Printf) return;
	char b[512]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap); g_eng.Con_Printf((char *)"%s", b);
}
static bool enabled() { return g_inited && (!cv_enable || cv_enable->value != 0); }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static bool safe_name(const std::string &s) {
	if (s.empty() || s.size() > NEXUS_MAX_NAME || s[0] == '/' || s.find("..") != std::string::npos) return false;
	for (char c : s) if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == '/')) return false;
	return true;
}
static bool ends_with(const std::string &s, const char *e) { size_t n = strlen(e); return s.size() > n && !s.compare(s.size() - n, n, e); }
static bool server_cmd(const std::string &c) {                        // at most 60 commands per second leave this client
	double t = nxp::now(); if (t - g_cmd_t >= 1.0 || t < g_cmd_t) { g_cmd_t = t; g_cmd_n = 0; }
	if (++g_cmd_n > 60 || !g_eng.pfnServerCmd) return false;
	g_eng.pfnServerCmd((char *)c.c_str()); return true;
}
static void notify_session(int ready) { for (auto &p : g_plugins) if (p.p.on_session) p.p.on_session(ready); }
static void session_reset(bool keep_on) {
	bool was = S.on; S.el.clear(); memset(S.keys, 0, sizeof S.keys); S.cursor = false; S.cam_mode = 0; S.cam_from = S.cam_to = 0; S.hover = 0;
	if (!keep_on) { S.on = false; if (was) notify_session(0); }
}

// ------------------------------------------------------------------------------------------------ message parsing
struct Rd {
	const unsigned char *p, *e; bool ok = true;
	int u8() { if (p >= e) { ok = false; return 0; } return *p++; }
	int u16() { int a = u8(), b = u8(); return a | (b << 8); }
	int i16() { int v = u16(); return v >= 32768 ? v - 65536 : v; }
	std::string str() { int n = u8(); if (!ok || e - p < n) { ok = false; return ""; } std::string s((const char *)p, (size_t)n); p += n; return s; }
	void rgba(float *c) { for (int i = 0; i < 4; i++) c[i] = u8() / 255.0f; }
};
static void anim_value(Elem &e, int prop, float *v, bool set) {       // read (set = false) or write the animated property
	float *a = nullptr, *b = nullptr; int n = 1;
	switch (prop) {
		case NX_A_POS: a = &e.xf; b = &e.yf; n = 2; break;
		case NX_A_PX: a = &e.xp; b = &e.yp; n = 2; break;
		case NX_A_ALPHA: a = &e.col[3]; break;
		case NX_A_SCALE: a = &e.scale; break;
		case NX_A_VALUE: a = &e.value; break;
		case NX_A_COLOR: if (set) memcpy(e.col, v, sizeof e.col); else memcpy(v, e.col, sizeof e.col); return;
		default: return;
	}
	if (set) { *a = v[0]; if (n == 2) *b = v[1]; } else { v[0] = *a; if (n == 2) v[1] = *b; }
	if (set && prop == NX_A_SCALE) e.lay.valid = false;
}
static bool parse_set(Rd &r) {
	int id = r.u16(), type = r.u8(); unsigned mask = (unsigned)r.u16(); if (!r.ok) return false;
	Elem scratch, *e = &scratch; auto it = S.el.find(id);
	if (it != S.el.end()) e = &it->second;
	else if (S.el.size() < NEXUS_MAX_ELEMS && id > 0 && type >= NX_T_TEXT && type <= NX_T_BAR) { e = &S.el[id]; e->id = id; e->born = nxp::now(); }
	e->type = type; e->lay.valid = false;
	if (mask & NX_F_POS) { e->xf = r.i16() / 10000.0f; e->yf = r.i16() / 10000.0f; e->xp = (float)r.i16(); e->yp = (float)r.i16(); for (size_t i = 0; i < e->anims.size();) { if (e->anims[i].prop == NX_A_POS || e->anims[i].prop == NX_A_PX) e->anims.erase(e->anims.begin() + i); else i++; } }
	if (mask & NX_F_SIZE) { e->wf = r.i16() / 10000.0f; e->hf = r.i16() / 10000.0f; e->wp = (float)r.i16(); e->hp = (float)r.i16(); }
	if (mask & NX_F_COLOR) r.rgba(e->col);
	if (mask & NX_F_COLOR2) r.rgba(e->col2);
	if (mask & NX_F_STYLE) { e->anchor = r.u8() % 9; e->flags = r.u8(); int l = r.u8(); e->layer = l >= 128 ? l - 256 : l; e->group = r.u8(); }
	if (mask & NX_F_SCALE) e->scale = clampf(r.u16() / 256.0f, 0.0625f, 64.0f);
	if (mask & NX_F_TEXT) e->text = r.str();
	if (mask & NX_F_IMAGE) { std::string f = r.str(); if (f != e->image) { e->image = f; e->img = -2; } }
	if (mask & NX_F_VALUE) e->value = clampf(r.u16() / 10000.0f, 0, 1);
	if (mask & NX_F_WORLD) { e->world = true; e->ent = r.u16(); for (int i = 0; i < 3; i++) e->w3[i] = (float)r.i16(); e->maxdist = (float)r.u16(); }
	if (mask & NX_F_LIFE) { e->life = r.u16() / 100.0f; e->fin = r.u8() / 20.0f; e->fout = r.u8() / 20.0f; e->born = nxp::now(); }
	return r.ok;
}
static void say_hello(int token) {
	bool was = S.on; session_reset(true); S.on = true;
	SCREENINFO si; memset(&si, 0, sizeof si); si.iSize = sizeof si; if (g_eng.pfnGetScreenInfo) g_eng.pfnGetScreenInfo(&si); if (si.iWidth > 0) { g_w = si.iWidth; g_h = si.iHeight; }
	int caps = NX_CAP_CURSOR | NX_CAP_CAMERA; if (nxr::available()) caps |= NX_CAP_GL | NX_CAP_IMAGES; if (g_eng.pTriAPI && g_eng.pTriAPI->WorldToScreen) caps |= NX_CAP_WORLD; if (!g_plugins.empty()) caps |= NX_CAP_PLUGINS;
	char b[160]; snprintf(b, sizeof b, "nexus_hello %d %s %d %d %d %d", NEXUS_PROTO, NEXUS_VERSION, g_w, g_h, caps, token);
	if (g_eng.pfnServerCmd) g_eng.pfnServerCmd(b);                        // not rate limited: the handshake must get through
	if (!was) notify_session(1);
	if (cv_debug && cv_debug->value) conf("[nexus] server said hello (token %d), answered with caps %d\n", token, caps);
}
int on_message(const char *, int size, void *buf) {
	if (!g_inited || !buf || size <= 0) return 1;
	Rd r{(const unsigned char *)buf, (const unsigned char *)buf + size}; S.msgs++;
	while (r.ok && r.p < r.e) {
		int op = r.u8();
		if (op == NX_OP_HELLO) { int token = r.u16(); if (r.ok && enabled()) say_hello(token); continue; }
		if (!S.on) { S.bad++; break; }                                     // nothing is accepted before the handshake
		S.ops++;
		switch (op) {
			case NX_OP_RESET: session_reset(true); break;
			case NX_OP_SET: parse_set(r); break;
			case NX_OP_DEL: { int id = r.u16(); if (r.ok) S.el.erase(id); break; }
			case NX_OP_CLEAR: { int g = r.u8(); if (!r.ok) break; if (g == 255) S.el.clear(); else for (auto it = S.el.begin(); it != S.el.end();) { if (it->second.group == g) it = S.el.erase(it); else ++it; } break; }
			case NX_OP_ANIM: {
				int id = r.u16(), prop = r.u8(); Anim a; memset(&a, 0, sizeof a); a.prop = prop;
				switch (prop) {
					case NX_A_POS: a.to[0] = r.i16() / 10000.0f; a.to[1] = r.i16() / 10000.0f; break;
					case NX_A_PX: a.to[0] = (float)r.i16(); a.to[1] = (float)r.i16(); break;
					case NX_A_ALPHA: a.to[0] = r.u8() / 255.0f; break;
					case NX_A_SCALE: a.to[0] = clampf(r.u16() / 256.0f, 0.0625f, 64.0f); break;
					case NX_A_VALUE: a.to[0] = clampf(r.u16() / 10000.0f, 0, 1); break;
					case NX_A_COLOR: r.rgba(a.to); break;
					default: r.ok = false;
				}
				a.dur = r.u16() / 100.0f; a.ease = r.u8(); a.remove = (r.u8() & 1) != 0; if (!r.ok) break;
				auto it = S.el.find(id); if (it == S.el.end()) break;
				Elem &e = it->second; anim_value(e, prop, a.from, false); a.t0 = nxp::now();
				for (size_t i = 0; i < e.anims.size();) { if (e.anims[i].prop == prop) e.anims.erase(e.anims.begin() + i); else i++; }
				if (e.anims.size() < 8) e.anims.push_back(a);
				break;
			}
			case NX_OP_TEXT: { int id = r.u16(), fl = r.u8(); std::string s = r.str(); if (!r.ok) break; auto it = S.el.find(id); if (it == S.el.end()) break;
				Elem &e = it->second; if (fl & 1) { if (e.text.size() + s.size() <= NEXUS_MAX_TEXT) e.text += s; } else e.text = s; e.lay.valid = false; break; }
			case NX_OP_KEYS: { int n = r.u8(); unsigned char k[256]; memset(k, 0, sizeof k); int used = 0;
				for (int i = 0; i < n && r.ok; i++) { int key = r.u8(), fl = r.u8(); if (!r.ok) break; if (key == K_ESCAPE || key == '`' || key == '~' || key <= 0 || used >= NEXUS_MAX_KEYS) continue; k[key] = (unsigned char)(1 | ((fl & 1) ? 2 : 0)); used++; }
				if (r.ok) memcpy(S.keys, k, sizeof k); break; }
			case NX_OP_CURSOR: { int on = r.u8(); if (!r.ok) break; bool was = S.cursor; S.cursor = on != 0;
				if (S.cursor && !was) { S.cx = g_w * 0.5f; S.cy = g_h * 0.5f; S.lock[0] = S.lock[1] = S.lock[2] = 0; if (g_eng.GetViewAngles) g_eng.GetViewAngles(S.lock); } break; }
			case NX_OP_SOUND: { int vol = r.u8(); std::string n = r.str(); if (r.ok && enabled() && safe_name(n) && ends_with(n, ".wav") && g_eng.pfnPlaySoundByName) g_eng.pfnPlaySoundByName((char *)n.c_str(), vol / 255.0f); break; }
			case NX_OP_CAMERA: { int mode = r.u8(); float lerp = r.u16() / 100.0f; int fl = r.u8(); float v[6]; for (int i = 0; i < 6; i++) v[i] = (float)r.i16(); if (!r.ok || mode < 0 || mode > 2) break;
				double now = nxp::now(); float cur = S.cam_dur > 0 ? clampf((float)((now - S.cam_t0) / S.cam_dur), 0, 1) : 1; cur = S.cam_from + (S.cam_to - S.cam_from) * cur;
				if (mode) { S.cam_mode = mode; memcpy(S.cam, v, sizeof v); if (mode == 2) for (int i = 3; i < 6; i++) S.cam[i] /= 10.0f; S.cam_collide = (fl & 1) != 0; S.cam_to = 1; } else S.cam_to = 0;
				S.cam_from = cur; S.cam_t0 = now; S.cam_dur = lerp; break; }
			case NX_OP_MUSIC: { int mode = r.u8(); std::string f = r.str(); if (!r.ok || !enabled() || !g_eng.pfnClientCmd) break;
				if (mode == 0) g_eng.pfnClientCmd((char *)"mp3 stop\n");
				else if (safe_name(f) && ends_with(f, ".mp3")) { std::string c = std::string(mode == 2 ? "mp3 loop " : "mp3 play ") + f + "\n"; g_eng.pfnClientCmd((char *)c.c_str()); }   // the name cannot hold spaces, quotes or ';'
				break; }
			case NX_OP_DATA: { std::string ch = r.str(), d = r.str(); if (!r.ok) break; for (auto &p : g_plugins) if (p.p.on_data) p.p.on_data(ch.c_str(), d.data(), (int)d.size()); break; }
			default: r.ok = false;                                           // unknown op: the rest of this message cannot be parsed
		}
	}
	if (!r.ok) { S.bad++; if (cv_debug && cv_debug->value) conf("[nexus] malformed message (%d bytes)\n", size); }
	return 1;
}

// ------------------------------------------------------------------------------------------------ drawing back ends (OpenGL, or the engine's own 2D calls)
static float be_lh(float px) { if (g_gl) return nxr::line_height(px); int w = 0, h = 0; if (g_eng.pfnDrawConsoleStringLen) g_eng.pfnDrawConsoleStringLen("Ay", &w, &h); return h > 0 ? (float)h : 14.0f; }
static float be_tw(const char *s, size_t n, float px) {
	if (g_gl) return nxr::text_width(s, n, px);
	char b[512]; if (n > sizeof b - 1) n = sizeof b - 1; memcpy(b, s, n); b[n] = 0; int w = 0, h = 0; if (g_eng.pfnDrawConsoleStringLen) g_eng.pfnDrawConsoleStringLen(b, &w, &h); return (float)w;
}
static void be_text(float x, float y, const char *s, size_t n, float px, const float *c) {
	if (g_gl) { nxr::text(x, y, s, n, px, c); return; }
	if (c[3] < 0.2f || !g_eng.pfnDrawConsoleString) return;
	char b[512]; if (n > sizeof b - 1) n = sizeof b - 1; memcpy(b, s, n); b[n] = 0;
	if (g_eng.pfnDrawSetTextColor) g_eng.pfnDrawSetTextColor(c[0], c[1], c[2]); g_eng.pfnDrawConsoleString((int)x, (int)y, b);
}
static void be_rect(float x, float y, float w, float h, const float *c) {
	if (g_gl) { nxr::rect(x, y, w, h, c); return; }
	if (w >= 1 && h >= 1 && c[3] > 0 && g_eng.pfnFillRGBA) g_eng.pfnFillRGBA((int)x, (int)y, (int)w, (int)h, (int)(c[0] * 255), (int)(c[1] * 255), (int)(c[2] * 255), (int)(c[3] * 255));
}
static const float PALETTE[10][3] = {{1, 1, 1}, {1, .25f, .25f}, {.3f, 1, .3f}, {1, .9f, .2f}, {.35f, .55f, 1}, {.3f, 1, 1}, {1, .4f, 1}, {1, 1, 1}, {.6f, .6f, .6f}, {1, .6f, .15f}};
static bool is_code(const char *s, size_t n, size_t i) { return s[i] == '^' && i + 1 < n && ((s[i + 1] >= '0' && s[i + 1] <= '9') || s[i + 1] == '^'); }
static float mk_width(const char *s, size_t n, float px, bool markup) {
	if (!markup) return be_tw(s, n, px);
	float w = 0; size_t st = 0;
	for (size_t i = 0; i < n; i++) if (is_code(s, n, i)) { w += be_tw(s + st, i - st, px); if (s[i + 1] == '^') { st = i + 1; i++; } else { st = i + 2; i++; } }
	return w + be_tw(s + st, n - st, px);
}
// draws one line; colour codes switch `cur` (index into PALETTE, -1 = the element's colour). mono = shadow / outline pass.
static void mk_draw(float x, float y, const char *s, size_t n, float px, const float *base, bool markup, int &cur, const float *mono) {
	auto run = [&](size_t a, size_t b) {
		if (b <= a) return; float c[4] = {base[0], base[1], base[2], base[3]};
		if (mono) memcpy(c, mono, sizeof c); else if (cur >= 0) { c[0] = PALETTE[cur][0]; c[1] = PALETTE[cur][1]; c[2] = PALETTE[cur][2]; }
		be_text(x, y, s + a, b - a, px, c); x += be_tw(s + a, b - a, px);
	};
	if (!markup) { run(0, n); return; }
	size_t st = 0;
	for (size_t i = 0; i < n; i++) if (is_code(s, n, i)) { run(st, i); if (s[i + 1] == '^') st = i + 1; else { cur = s[i + 1] == '0' ? -1 : s[i + 1] - '0'; st = i + 2; } i++; }
	run(st, n);
}
static void layout_text(Elem &e, float px, float wrap) {
	Layout &L = e.lay; if (L.valid && L.px == px && L.wrap == wrap && L.gl == g_gl) return;
	L.valid = true; L.px = px; L.wrap = wrap; L.gl = g_gl; L.lines.clear(); L.lw.clear(); L.w = 0; L.lh = be_lh(px);
	const std::string &t = e.text; bool mk = (e.flags & NX_FL_MARKUP) != 0; size_t pos = 0;
	auto add = [&](size_t a, size_t b) { if (L.lines.size() >= 64) return; float w = mk_width(t.data() + a, b - a, px, mk); L.lines.emplace_back(a, b - a); L.lw.push_back(w); if (w > L.w) L.w = w; };
	while (pos <= t.size()) {
		size_t nl = t.find('\n', pos); if (nl == std::string::npos) nl = t.size();
		if (wrap <= 0 || mk_width(t.data() + pos, nl - pos, px, mk) <= wrap) add(pos, nl);
		else {                                                             // greedy word wrap
			size_t ls = pos, i = pos, last_ok = pos;
			while (i < nl) {
				size_t we = t.find(' ', i); if (we == std::string::npos || we > nl) we = nl;
				if (mk_width(t.data() + ls, we - ls, px, mk) > wrap && last_ok > ls) { add(ls, last_ok); ls = last_ok; while (ls < nl && t[ls] == ' ') ls++; i = ls; last_ok = ls; continue; }
				last_ok = we; i = we < nl ? we + 1 : nl;
			}
			if (ls < nl || nl == pos) add(ls, nl);
		}
		pos = nl + 1;
	}
	L.h = L.lh * (float)L.lines.size();
}

// ------------------------------------------------------------------------------------------------ world helpers
static bool world_to_screen(const float *w, float *x, float *y) {
	if (!g_eng.pTriAPI || !g_eng.pTriAPI->WorldToScreen) return false;
	float in[3] = {w[0], w[1], w[2]}, sc[3] = {0, 0, 0};
	if (g_eng.pTriAPI->WorldToScreen(in, sc)) return false;               // behind the camera
	*x = (1.0f + sc[0]) * 0.5f * g_w; *y = (1.0f - sc[1]) * 0.5f * g_h; return true;
}
static void eye(float *o) {
	if (g_have_view) { memcpy(o, g_vieworg, sizeof(float) * 3); return; }
	cl_entity_t *lp = g_eng.GetLocalPlayer ? g_eng.GetLocalPlayer() : nullptr; o[0] = o[1] = o[2] = 0; if (lp) { o[0] = lp->origin[0]; o[1] = lp->origin[1]; o[2] = lp->origin[2] + 17; }
}
static bool world_trace_clear(const float *a, const float *b, float *frac) {
	if (!g_eng.pEventAPI || !g_eng.pEventAPI->EV_PlayerTrace || !g_eng.pEventAPI->EV_SetTraceHull) { if (frac) *frac = 1; return true; }
	float s[3] = {a[0], a[1], a[2]}, e[3] = {b[0], b[1], b[2]}; pmtrace_t tr; memset(&tr, 0, sizeof tr); tr.fraction = 1;
	g_eng.pEventAPI->EV_SetTraceHull(2); g_eng.pEventAPI->EV_PlayerTrace(s, e, PM_WORLD_ONLY, -1, &tr);
	if (frac) *frac = tr.fraction; return tr.fraction >= 1.0f;
}
// screen position of a world anchored element; false = do not draw it this frame
static bool world_anchor(Elem &e, float *x, float *y, float *dist) {
	float p[3] = {e.w3[0], e.w3[1], e.w3[2]};
	if (e.ent > 0) {
		cl_entity_t *ent = g_eng.GetEntityByIndex ? g_eng.GetEntityByIndex(e.ent) : nullptr, *lp = g_eng.GetLocalPlayer ? g_eng.GetLocalPlayer() : nullptr;
		if (!ent || !lp || !ent->model) return false;
		if (ent != lp && ent->curstate.messagenum < lp->curstate.messagenum) return false;      // not in the last update: outside the PVS, dead or gone
		for (int i = 0; i < 3; i++) p[i] += ent->origin[i];
	}
	float o[3]; eye(o); float d = sqrtf((p[0] - o[0]) * (p[0] - o[0]) + (p[1] - o[1]) * (p[1] - o[1]) + (p[2] - o[2]) * (p[2] - o[2])); *dist = d;
	if (e.maxdist > 0 && d > e.maxdist) return false;
	if (!world_to_screen(p, x, y)) return false;
	if ((e.flags & NX_FL_OCCLUDE) && !world_trace_clear(o, p, nullptr)) return false;
	return true;
}

// ------------------------------------------------------------------------------------------------ frame
static float ease(float t, int mode) { t = clampf(t, 0, 1); return mode == 0 ? t : mode == 2 ? t * t * (3 - 2 * t) : 1 - (1 - t) * (1 - t); }
static void tick(double now) {
	for (auto it = S.el.begin(); it != S.el.end();) {
		Elem &e = it->second; bool kill = e.life > 0 && now - e.born >= e.life;
		for (size_t i = 0; i < e.anims.size() && !kill;) {
			Anim &a = e.anims[i]; float t = a.dur > 0 ? (float)((now - a.t0) / a.dur) : 1, k = ease(t, a.ease), v[4];
			for (int j = 0; j < 4; j++) v[j] = a.from[j] + (a.to[j] - a.from[j]) * k;
			anim_value(e, a.prop, v, true);
			if (t >= 1) { if (a.remove) kill = true; e.anims.erase(e.anims.begin() + i); } else i++;
		}
		if (kill) it = S.el.erase(it); else ++it;
	}
}
static void draw_elem(Elem &e, double now) {
	e.shown = false; if (e.flags & NX_FL_HIDDEN) return;
	float fade = 1, age = (float)(now - e.born);
	if (e.fin > 0 && age < e.fin) fade = age / e.fin;
	if (e.life > 0 && e.fout > 0 && e.life - age < e.fout) fade *= clampf((e.life - age) / e.fout, 0, 1);
	int ax = e.anchor % 3, ay = e.anchor / 3; float bx = ax * 0.5f * g_w, by = ay * 0.5f * g_h, k = 1;
	if (e.world) {
		float d = 0; if (!world_anchor(e, &bx, &by, &d)) return;
		if (e.flags & NX_FL_DISTSCALE) k = clampf(320.0f / (d > 1 ? d : 1), 0.35f, 1.6f);
		if (e.maxdist > 0 && d > e.maxdist * 0.85f) fade *= clampf((e.maxdist - d) / (e.maxdist * 0.15f), 0, 1);
	}
	float u = g_unit * k, x = bx + e.xf * g_w + e.xp * u, y = by + e.yf * g_h + e.yp * u, w = e.wf * g_w + e.wp * u, h = e.hf * g_h + e.hp * u;
	float c[4] = {e.col[0], e.col[1], e.col[2], e.col[3] * fade}, c2[4] = {e.col2[0], e.col2[1], e.col2[2], e.col2[3] * fade};
	if (c[3] <= 0.003f && c2[3] <= 0.003f) return;
	bool hover = S.cursor && S.hover == e.id && (e.flags & NX_FL_CLICK); const float hl[4] = {1, 1, 1, 0.14f * fade};
	if (e.type == NX_T_TEXT) {
		float px = 16.0f * e.scale * u; if (px < 4) px = 4;
		layout_text(e, px, w > 0 ? w : 0); Layout &L = e.lay; float tw = w > 0 ? w : L.w, th = L.h, pad = c2[3] > 0 ? 5 * u : 0;
		float left = x - ax * 0.5f * tw, top = y - ay * 0.5f * th;
		e.rx = left - pad; e.ry = top - pad; e.rw = tw + pad * 2; e.rh = th + pad * 2; e.shown = true;
		if (c2[3] > 0) be_rect(e.rx, e.ry, e.rw, e.rh, c2);
		if (hover) be_rect(e.rx, e.ry, e.rw, e.rh, hl);
		bool mk = (e.flags & NX_FL_MARKUP) != 0; const float black[4] = {0, 0, 0, c[3] * 0.85f}; float o = px / 14.0f; if (o < 1) o = 1;
		for (int pass = 0; pass < 3; pass++) {                             // 0 = outline, 1 = shadow, 2 = text
			if ((pass == 0 && !(e.flags & NX_FL_OUTLINE)) || (pass == 1 && !(e.flags & NX_FL_SHADOW)) || (pass < 2 && !g_gl && c[3] < 0.2f)) continue;
			int cur = -1;
			for (size_t i = 0; i < L.lines.size(); i++) {
				const char *s = e.text.data() + L.lines[i].first; size_t n = L.lines[i].second; float lx = left + ax * 0.5f * (tw - L.lw[i]), ly = top + L.lh * (float)i; int cc = cur;
				if (pass == 0) { static const float D[8][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-.7f, -.7f}, {.7f, -.7f}, {-.7f, .7f}, {.7f, .7f}}; for (auto &d : D) { cc = cur; mk_draw(lx + d[0] * o, ly + d[1] * o, s, n, px, c, mk, cc, black); } }
				else if (pass == 1) mk_draw(lx + o, ly + o, s, n, px, c, mk, cc, black);
				else mk_draw(lx, ly, s, n, px, c, mk, cc, nullptr);
				cur = cc;
			}
		}
		return;
	}
	if (e.type == NX_T_IMAGE) {
		if (!g_gl) return;
		if (e.img == -2) e.img = safe_name(e.image) ? nxr::image(e.image.c_str(), [](const char *p, std::string &out) { int len = 0; byte *d = g_eng.COM_LoadFile ? g_eng.COM_LoadFile((char *)p, 5, &len) : nullptr; if (!d) return false; out.assign((const char *)d, (size_t)(len > 0 ? len : 0)); g_eng.COM_FreeFile(d); return len > 0; }) : -1;
		int iw = 0, ih = 0; if (!nxr::image_size(e.img, &iw, &ih)) return;
		if (w <= 0 && h <= 0) { w = iw * u * e.scale; h = ih * u * e.scale; } else if (h <= 0) h = w * ih / iw; else if (w <= 0) w = h * iw / ih;
		float left = x - ax * 0.5f * w, top = y - ay * 0.5f * h; e.rx = left; e.ry = top; e.rw = w; e.rh = h; e.shown = true;
		nxr::image_draw(e.img, left, top, w, h, c, (e.flags & NX_FL_ADDITIVE) != 0); if (hover) be_rect(left, top, w, h, hl);
		return;
	}
	if (w <= 0 || h <= 0) return;
	float left = x - ax * 0.5f * w, top = y - ay * 0.5f * h; e.rx = left; e.ry = top; e.rw = w; e.rh = h; e.shown = true;
	if (e.type == NX_T_RECT) {
		be_rect(left, top, w, h, c);
		if ((e.flags & NX_FL_OUTLINE) && c2[3] > 0) { float b = u < 1 ? 1 : floorf(u + 0.5f); be_rect(left, top, w, b, c2); be_rect(left, top + h - b, w, b, c2); be_rect(left, top + b, b, h - 2 * b, c2); be_rect(left + w - b, top + b, b, h - 2 * b, c2); }
	} else if (e.type == NX_T_BAR) {
		be_rect(left, top, w, h, c2);
		if (h > w) be_rect(left, top + h * (1 - e.value), w, h * e.value, c); else be_rect(left, top, w * e.value, h, c);
	}
	if (hover) be_rect(left, top, w, h, hl);
}
static int hit_test() {
	int best = 0, bl = -1000; 
	for (auto &kv : S.el) { Elem &e = kv.second; if (!e.shown || !(e.flags & NX_FL_CLICK)) continue;
		if (S.cx >= e.rx && S.cx < e.rx + e.rw && S.cy >= e.ry && S.cy < e.ry + e.rh && (e.layer > bl || (e.layer == bl && e.id > best))) { best = e.id; bl = e.layer; } }
	return best;
}
void on_redraw(float, int) {
	if (!g_inited) return;
	SCREENINFO si; memset(&si, 0, sizeof si); si.iSize = sizeof si; if (g_eng.pfnGetScreenInfo) g_eng.pfnGetScreenInfo(&si);
	if (si.iWidth > 0 && si.iHeight > 0) { g_w = si.iWidth; g_h = si.iHeight; }
	g_unit = g_h / 768.0f * clampf(cv_scale ? cv_scale->value : 1, 0.5f, 3.0f);
	double now = nxp::now(); tick(now);
	bool any_plugin = false; for (auto &p : g_plugins) if (p.p.on_draw) any_plugin = true;
	if (!enabled() || ((!S.on || (S.el.empty() && !S.cursor)) && !any_plugin)) return;
	g_gl = nxr::begin(g_w, g_h); g_drawing = true;
	if (S.on) {
		std::vector<Elem *> order; order.reserve(S.el.size()); for (auto &kv : S.el) order.push_back(&kv.second);
		std::sort(order.begin(), order.end(), [](const Elem *a, const Elem *b) { return a->layer != b->layer ? a->layer < b->layer : a->id < b->id; });
		for (Elem *e : order) draw_elem(*e, now);
		S.hover = S.cursor ? hit_test() : 0;
	}
	for (auto &p : g_plugins) if (p.p.on_draw) p.p.on_draw();
	if (S.on && S.cursor) {
		float s = 1.1f * g_unit, x = S.cx, y = S.cy; const float white[4] = {1, 1, 1, 1}, black[4] = {0, 0, 0, 0.9f};
		if (g_gl) {
			auto arrow = [&](float ox, float oy, const float *c) {
				const float head[6] = {x + ox, y + oy, x + ox, y + oy + 16 * s, x + ox + 11.5f * s, y + oy + 11.5f * s};
				const float tail[8] = {x + ox + 3.5f * s, y + oy + 12.5f * s, x + ox + 6.5f * s, y + oy + 11.5f * s, x + ox + 10 * s, y + oy + 19 * s, x + ox + 7 * s, y + oy + 20.5f * s};
				nxr::poly(head, 3, c); nxr::poly(tail, 4, c);
			};
			for (int dx = -1; dx <= 1; dx++) for (int dy = -1; dy <= 1; dy++) if (dx || dy) arrow(dx * 1.5f, dy * 1.5f, black);
			arrow(0, 0, white);
		} else { be_rect(x - 1, y - 1, 8, 8, black); be_rect(x, y, 6, 6, white); }
	}
	g_drawing = false; if (g_gl) nxr::end();
}
void on_frame(double) { for (auto &p : g_plugins) if (p.p.on_frame) p.p.on_frame(); }

// ------------------------------------------------------------------------------------------------ input
int on_key(int down, int keynum, const char *binding) {
	if (!g_inited) return 1;
	for (auto &p : g_plugins) if (p.p.on_key && p.p.on_key(down, keynum, binding)) return 0;
	if (!S.on || !enabled() || keynum < 0 || keynum > 255) return 1;
	if (S.cursor && keynum >= K_MOUSE1 && keynum <= K_MOUSE1 + 2) {
		if (down) { char b[64]; snprintf(b, sizeof b, "nexus_click %d %d", hit_test(), keynum - K_MOUSE1 + 1); server_cmd(b); }
		return 0;
	}
	if (cv_keys && cv_keys->value == 0) return 1;                       // the player switched key reporting off
	unsigned char k = S.keys[keynum]; if (!(k & 1)) return 1;
	char b[64]; snprintf(b, sizeof b, "nexus_key %d %d", keynum, down ? 1 : 0); server_cmd(b);
	return (k & 2) ? 0 : 1;
}
void createmove(void (*orig)(float, usercmd_s *, int), float frametime, usercmd_s *cmd, int active) {
	if (!g_inited || !S.on || !S.cursor || !active || !enabled() || !g_eng.GetViewAngles || !g_eng.SetViewAngles) { if (orig) orig(frametime, cmd, active); return; }
	float neutral[3] = {0, S.lock[1], 0}, after[3] = {0, 0, 0};          // let the game turn a neutral view: the turn it makes IS the mouse movement
	g_eng.SetViewAngles(neutral); if (orig) orig(frametime, cmd, active); g_eng.GetViewAngles(after);
	float dyaw = after[1] - neutral[1], dpitch = after[0] - neutral[0]; while (dyaw > 180) dyaw -= 360; while (dyaw < -180) dyaw += 360;
	float k = g_w / 90.0f * clampf(cv_speed ? cv_speed->value : 1, 0.1f, 10.0f);
	S.cx = clampf(S.cx - dyaw * k, 0, (float)g_w - 1); S.cy = clampf(S.cy + dpitch * k, 0, (float)g_h - 1);
	g_eng.SetViewAngles(S.lock);
	if (cmd) { cmd->viewangles[0] = S.lock[0]; cmd->viewangles[1] = S.lock[1]; cmd->viewangles[2] = S.lock[2]; cmd->buttons &= ~(IN_ATTACK | IN_ATTACK2); }
}

// ------------------------------------------------------------------------------------------------ camera
static float cam_blend() { float t = S.cam_dur > 0 ? clampf((float)((nxp::now() - S.cam_t0) / S.cam_dur), 0, 1) : 1; t = t * t * (3 - 2 * t); return S.cam_from + (S.cam_to - S.cam_from) * t; }
bool third_person() { return g_inited && S.on && S.cam_mode && enabled() && cam_blend() > 0.001f; }
static void angle_vectors(const float *a, float *f, float *r, float *u) {
	float p = a[0] * 0.01745329252f, y = a[1] * 0.01745329252f, ro = a[2] * 0.01745329252f, sp = sinf(p), cp = cosf(p), sy = sinf(y), cy = cosf(y), sr = sinf(ro), cr = cosf(ro);
	f[0] = cp * cy; f[1] = cp * sy; f[2] = -sp;
	r[0] = -sr * sp * cy + cr * sy; r[1] = -sr * sp * sy - cr * cy; r[2] = -sr * cp;
	u[0] = cr * sp * cy + sr * sy; u[1] = cr * sp * sy - sr * cy; u[2] = cr * cp;
}
void post_calcrefdef(ref_params_s *pp) {
	if (!pp) return; ref_params_t *p = (ref_params_t *)pp;
	if (g_inited && S.on && S.cam_mode && enabled()) {
		float b = cam_blend();
		if (b <= 0.001f && S.cam_to == 0) S.cam_mode = 0;
		else if (S.cam_mode == 1) {
			float f[3], r[3], u[3], tgt[3]; angle_vectors(p->viewangles, f, r, u);
			for (int i = 0; i < 3; i++) tgt[i] = p->vieworg[i] - f[i] * S.cam[0] * b + r[i] * S.cam[2] * b; tgt[2] += S.cam[1] * b;
			if (S.cam_collide) { float fr = 1; if (!world_trace_clear(p->vieworg, tgt, &fr)) { fr = fr - 0.06f; if (fr < 0) fr = 0; for (int i = 0; i < 3; i++) tgt[i] = p->vieworg[i] + (tgt[i] - p->vieworg[i]) * fr; } }
			for (int i = 0; i < 3; i++) p->vieworg[i] = tgt[i];
		} else if (S.cam_mode == 2) {
			for (int i = 0; i < 3; i++) { p->vieworg[i] += (S.cam[i] - p->vieworg[i]) * b; float d = S.cam[3 + i] - p->viewangles[i]; while (d > 180) d -= 360; while (d < -180) d += 360; p->viewangles[i] += d * b; }
		}
	}
	memcpy(g_vieworg, p->vieworg, sizeof g_vieworg); g_have_view = true;
}

// ------------------------------------------------------------------------------------------------ client plugins
static void unpack(unsigned rgba, float *c) { c[0] = ((rgba >> 24) & 255) / 255.0f; c[1] = ((rgba >> 16) & 255) / 255.0f; c[2] = ((rgba >> 8) & 255) / 255.0f; c[3] = (rgba & 255) / 255.0f; }
static void api_log(const char *t) { conf("%s\n", t ? t : ""); }
static int api_session() { return S.on ? 1 : 0; }
static int api_send(const char *ch, const void *data, int len) {
	if (!S.on || !ch || len < 0 || len > NEXUS_MAX_DATA || (len && !data)) return 0; std::string c = ch; if (!safe_name(c) || c.size() > 31) return 0;
	static const char H[] = "0123456789abcdef"; std::string cmd = "nexus_data " + c + " "; if (!len) cmd += "00";       // an empty argument would be dropped by the tokenizer
	for (int i = 0; i < len; i++) { unsigned char b = ((const unsigned char *)data)[i]; cmd += H[b >> 4]; cmd += H[b & 15]; }
	return server_cmd(cmd) ? 1 : 0;
}
static void api_screen(int *w, int *h, float *unit) { if (w) *w = g_w; if (h) *h = g_h; if (unit) *unit = g_unit; }
static double api_time() { return nxp::now(); }
static void api_rect(float x, float y, float w, float h, unsigned rgba) { if (!g_drawing) return; float c[4]; unpack(rgba, c); be_rect(x, y, w, h, c); }
static void api_text(float x, float y, const char *s, float px, unsigned rgba) { if (!g_drawing || !s) return; float c[4]; unpack(rgba, c); be_text(x, y, s, strlen(s), px, c); }
static float api_text_width(const char *s, float px) { return s ? be_tw(s, strlen(s), px) : 0; }
static int api_image(const char *path) { if (!path || !safe_name(path) || !nxr::available()) return -1; return nxr::image(path, [](const char *p, std::string &out) { int len = 0; byte *d = g_eng.COM_LoadFile ? g_eng.COM_LoadFile((char *)p, 5, &len) : nullptr; if (!d) return false; out.assign((const char *)d, (size_t)(len > 0 ? len : 0)); g_eng.COM_FreeFile(d); return len > 0; }); }
static void api_image_draw(int img, float x, float y, float w, float h, unsigned rgba) { if (!g_drawing || !g_gl) return; float c[4]; unpack(rgba, c); nxr::image_draw(img, x, y, w, h, c, false); }
static int api_w2s(const float *w, float *x, float *y) { float a = 0, b = 0; if (!w || !world_to_screen(w, &a, &b)) return 0; if (x) *x = a; if (y) *y = b; return 1; }
static NexusApi g_api;
static void load_plugins() {
	const char *gd = g_eng.pfnGetGameDirectory ? g_eng.pfnGetGameDirectory() : nullptr; if (!gd || !*gd) return;
	std::string base = std::string(gd) + "/nexus/"; FILE *f = fopen((base + "plugins.ini").c_str(), "r"); if (!f) return;     // plain file access on purpose: never the engine's search paths (downloads)
	g_api = NexusApi{NEXUS_PLUGIN_ABI, (unsigned)sizeof(NexusApi), (cl_enginefuncs_s *)&g_eng, api_log, api_session, api_send, api_screen, api_time, api_rect, api_text, api_text_width, api_image, api_image_draw, api_w2s};
	char line[256];
	while (fgets(line, sizeof line, f) && g_plugins.size() < 32) {
		std::string n = line; size_t c = n.find_first_of(";#\r\n"); if (c != std::string::npos) n.resize(c); while (!n.empty() && (n.back() == ' ' || n.back() == '\t')) n.pop_back(); while (!n.empty() && (n[0] == ' ' || n[0] == '\t')) n.erase(0, 1);
		if (n.empty()) continue;
		bool ok = n.size() <= 48; for (char ch : n) if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-')) ok = false;
		if (!ok) { conf("[nexus] plugins.ini: bad plugin name \"%s\" (letters, digits, _ and - only)\n", n.c_str()); continue; }
#ifdef _WIN32
		std::string path = base + "plugins/" + n + ".dll";
#else
		std::string path = base + "plugins/" + n + ".so";
#endif
		void *lib = nxp::lib_open(path.c_str()); if (!lib) { conf("[nexus] plugin %s: cannot load %s\n", n.c_str(), path.c_str()); continue; }
		typedef int (*init_t)(const NexusApi *, NexusPlugin *); init_t init = (init_t)nxp::lib_sym(lib, "NexusPlugin_Init");
		Plug pl; pl.name = n; pl.lib = lib; memset(&pl.p, 0, sizeof pl.p);
		if (!init || init(&g_api, &pl.p) != NEXUS_PLUGIN_ABI) { conf("[nexus] plugin %s: no NexusPlugin_Init or wrong ABI (host has %d)\n", n.c_str(), NEXUS_PLUGIN_ABI); continue; }
		g_plugins.push_back(pl); conf("[nexus] plugin %s %s loaded\n", pl.p.name ? pl.p.name : n.c_str(), pl.p.version ? pl.p.version : "");
	}
	fclose(f);
}

// ------------------------------------------------------------------------------------------------ lifecycle
static void cmd_info() {
	conf("Nexus client %s (protocol %d)\n  session: %s   elements: %d   cursor: %s   camera: %d\n  renderer: %s   screen %dx%d, unit %.2f\n  messages %llu, ops %llu, rejected %llu   plugins: %d\n",
		NEXUS_VERSION, NEXUS_PROTO, S.on ? "connected to a Nexus server" : "none", (int)S.el.size(), S.cursor ? "on" : "off", S.cam_mode, nxr::available() ? "OpenGL" : "engine fallback (no OpenGL)", g_w, g_h, g_unit, S.msgs, S.ops, S.bad, (int)g_plugins.size());
	for (auto &p : g_plugins) conf("    %s %s\n", p.p.name ? p.p.name : p.name.c_str(), p.p.version ? p.p.version : "");
}
void on_initialize(cl_enginefuncs_s *eng) { if (!eng) return; memcpy(&g_eng, eng, sizeof g_eng); g_have_eng = true; }
void on_hud_init() {
	if (!g_have_eng || g_inited) return;
	if (g_eng.pfnRegisterVariable) {
		g_eng.pfnRegisterVariable((char *)NEXUS_CVAR, (char *)NEXUS_VERSION, 0);
		cv_enable = g_eng.pfnRegisterVariable((char *)"nexus_enable", (char *)"1", FCVAR_ARCHIVE);
		cv_scale = g_eng.pfnRegisterVariable((char *)"nexus_scale", (char *)"1", FCVAR_ARCHIVE);
		cv_keys = g_eng.pfnRegisterVariable((char *)"nexus_keys", (char *)"1", FCVAR_ARCHIVE);
		cv_speed = g_eng.pfnRegisterVariable((char *)"nexus_cursor_speed", (char *)"1", FCVAR_ARCHIVE);
		cv_debug = g_eng.pfnRegisterVariable((char *)"nexus_cl_debug", (char *)"0", 0);
	}
	if (g_eng.pfnHookUserMsg) g_eng.pfnHookUserMsg((char *)NEXUS_MSG_NAME, on_message);
	if (g_eng.pfnAddCommand) g_eng.pfnAddCommand((char *)"nexus_info", cmd_info);
	g_inited = true; load_plugins();
	conf("[nexus] client %s ready (%d plugin%s)\n", NEXUS_VERSION, (int)g_plugins.size(), g_plugins.size() == 1 ? "" : "s");
}
void on_vid_init() { session_reset(false); nxr::device_lost(); g_have_view = false; }      // new map / new connection: the server greets us again
void on_shutdown() { session_reset(false); for (auto &p : g_plugins) if (p.p.on_shutdown) p.p.on_shutdown(); g_plugins.clear(); g_inited = false; }
}
