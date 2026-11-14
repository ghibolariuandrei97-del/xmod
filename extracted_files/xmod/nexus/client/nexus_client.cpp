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
static cvar_t *cv_enable, *cv_scale, *cv_keys, *cv_speed, *cv_debug, *cv_urls;
// a link the server asked to open: shown in Nexus' OWN prompt, answered only with the player's keys (Y / N), never by the server
static struct UrlAsk { bool on = false; int req = 0; std::string url, host, label; double t0 = 0; } g_url; static double g_url_last = -100;
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
// ---- Nexus 2: effects and control (all reset with the session)
struct EntOv { int flags = 0, mode = 0, amt = 255, fx = 0; unsigned char col[3] = {255, 255, 255}; float scale = 0; };
struct Shape { int kind = 0, flags = 0, ent = 0; float col[4]; float a[3], b[3]; double born = 0; float life = 0, width = 2; };
struct Light { int ent = 0; float pos[3], radius = 0; unsigned char col[3]; double born = 0; float life = 0; };
struct Saved { cl_entity_t *e; int mode, amt, fx; color24 col; float scale; };
static struct Fx {
	bool fov_on = false, fov_release = false; float fov_from = 0, fov_to = 0, fov_cur = 0, fov_dur = 0; double fov_t0 = 0;
	float shake_amp = 0, shake_freq = 0, shake_dur = 0; double shake_t0 = 0;
	float sway_amp = 0, sway_speed = 1; float punch[2] = {0, 0}; double punch_t0 = 0;
	int block = 0, force = 0, invert = 0; float sens = 1;
	std::unordered_map<int, EntOv> ent; std::unordered_map<int, Shape> shapes; std::unordered_map<int, Light> lights;
} FX;
static std::vector<Saved> g_saved;                                   // render fields changed for this frame; put back after the world is drawn
static void restore_entities() { for (auto &s : g_saved) { s.e->curstate.rendermode = s.mode; s.e->curstate.renderamt = s.amt; s.e->curstate.renderfx = s.fx; s.e->curstate.rendercolor = s.col; s.e->curstate.scale = s.scale; } g_saved.clear(); }
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
static void url_reply(int req, int result) { char b[64]; snprintf(b, sizeof b, "nexus_url %d %d", req, result); server_cmd(b); }
static void url_finish(int result) { if (!g_url.on) return; g_url.on = false; g_url_last = nxp::now(); url_reply(g_url.req, result); }
// https://host[/path...] only; host = letters, digits, '.', '-'; no user@, no port, no quotes / spaces / backslashes / control characters
static bool url_ok(const std::string &u, std::string &host) {
	if (u.size() < 12 || u.size() > 200 || u.compare(0, 8, "https://")) return false;
	for (unsigned char c : u) if (c <= 32 || c >= 127 || c == '"' || c == '\'' || c == '\\' || c == '<' || c == '>' || c == '`' || c == '^' || c == '{' || c == '}' || c == '|') return false;
	size_t e = u.find_first_of("/?#", 8); host = u.substr(8, e == std::string::npos ? std::string::npos : e - 8);
	if (host.empty() || host.size() > 100 || host[0] == '.' || host[0] == '-' || host.find('.') == std::string::npos) return false;
	for (char &c : host) { if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a'); if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-')) return false; }
	return true;
}
static void notify_session(int ready) { for (auto &p : g_plugins) if (p.p.on_session) p.p.on_session(ready); }
static void session_reset(bool keep_on) {
	if (g_url.on) { g_url.on = false; }                                // the server that asked is gone: nothing is opened
	bool was = S.on; S.el.clear(); memset(S.keys, 0, sizeof S.keys); S.cursor = false; S.cam_mode = 0; S.cam_from = S.cam_to = 0; S.hover = 0;
	restore_entities(); FX = Fx();
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
	int caps = NX_CAP_CURSOR | NX_CAP_CAMERA; if (nxr::available()) caps |= NX_CAP_GL | NX_CAP_IMAGES; if (g_eng.pTriAPI && g_eng.pTriAPI->WorldToScreen) caps |= NX_CAP_WORLD; if (!g_plugins.empty()) caps |= NX_CAP_PLUGINS; if (!cv_urls || cv_urls->value != 0) caps |= NX_CAP_URL; caps |= NX_CAP_FX;
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
			case NX_OP_URL: {
				int req = r.u16(); std::string url = r.str(), label = r.str(); if (!r.ok) break; std::string host;
				if (!enabled() || (cv_urls && cv_urls->value == 0)) { url_reply(req, 2); break; }
				if (g_url.on || nxp::now() - g_url_last < 5.0) { url_reply(req, 3); break; }
				if (!url_ok(url, host)) { url_reply(req, 4); break; }
				std::string lab; for (unsigned char ch : label) if (ch >= 32 && lab.size() < 80) lab += (char)ch;     // printable, short
				g_url.on = true; g_url.req = req; g_url.url = url; g_url.host = host; g_url.label = lab; g_url.t0 = nxp::now();
				break;
			}
			case NX_OP_VIEW: {
				int what = r.u8(); double now = nxp::now();
				if (what == 1) { int fov = r.u8(); float t = r.u16() / 100.0f; if (!r.ok) break;
					FX.fov_from = FX.fov_on ? FX.fov_cur : 0; FX.fov_to = (float)fov; FX.fov_t0 = now; FX.fov_dur = t; FX.fov_release = fov == 0; FX.fov_on = true; }
				else if (what == 2) { FX.shake_amp = r.u16() / 100.0f; FX.shake_freq = r.u16() / 10.0f; FX.shake_dur = r.u16() / 100.0f; FX.shake_t0 = now; }
				else if (what == 3) { FX.sway_amp = clampf(r.u16() / 100.0f, 0, 30); FX.sway_speed = r.u16() / 100.0f; }
				else if (what == 4) { float p = r.i16() / 100.0f, y = r.i16() / 100.0f; if (!r.ok) break;
					float k = expf(-(float)(now - FX.punch_t0) * 6.0f); FX.punch[0] = clampf(FX.punch[0] * k + p, -45, 45); FX.punch[1] = clampf(FX.punch[1] * k + y, -45, 45); FX.punch_t0 = now; }
				else r.ok = false;
				if (FX.shake_amp > 30) FX.shake_amp = 30;
				break;
			}
			case NX_OP_INPUT: { int b = r.u16(), f = r.u16(), se = r.u16(), fl = r.u8(); if (!r.ok) break; FX.block = b; FX.force = f; FX.sens = clampf(se / 100.0f, 0, 10); FX.invert = fl & 3; break; }
			case NX_OP_ENTFX: {
				int ent = r.u16(); EntOv o; o.flags = r.u8(); o.mode = r.u8(); o.amt = r.u8(); o.col[0] = (unsigned char)r.u8(); o.col[1] = (unsigned char)r.u8(); o.col[2] = (unsigned char)r.u8(); o.fx = r.u8(); o.scale = r.u16() / 100.0f;
				if (!r.ok) break;
				if (!o.flags) { if (ent == 0) FX.ent.clear(); else FX.ent.erase(ent); }
				else if (ent > 0 && (FX.ent.size() < 512 || FX.ent.count(ent))) FX.ent[ent] = o;
				break;
			}
			case NX_OP_SHAPE: {
				int id = r.u16(); Shape sh; sh.kind = r.u8(); sh.flags = r.u8(); r.rgba(sh.col); sh.ent = r.u16();
				for (int i = 0; i < 3; i++) sh.a[i] = (float)r.i16(); for (int i = 0; i < 3; i++) sh.b[i] = (float)r.i16(); sh.life = r.u16() / 100.0f; sh.width = (float)r.u8(); sh.born = nxp::now();
				if (!r.ok) break;
				if (!sh.kind) FX.shapes.erase(id); else if (sh.kind <= NX_SH_CYLINDER && (FX.shapes.size() < 256 || FX.shapes.count(id))) FX.shapes[id] = sh;
				break;
			}
			case NX_OP_LIGHT: {
				int id = r.u16(); Light li; li.radius = (float)r.u16(); li.col[0] = (unsigned char)r.u8(); li.col[1] = (unsigned char)r.u8(); li.col[2] = (unsigned char)r.u8(); li.ent = r.u16();
				for (int i = 0; i < 3; i++) li.pos[i] = (float)r.i16(); li.life = r.u16() / 100.0f; li.born = nxp::now();
				if (!r.ok) break;
				if (li.radius <= 0) FX.lights.erase(id); else if (FX.lights.size() < 32 || FX.lights.count(id)) FX.lights[id] = li;
				break;
			}
			case NX_OP_EFFECT: {
				int kind = r.u8(); float a[3], b[3]; for (float &v : a) v = (float)r.i16(); for (float &v : b) v = (float)r.i16();
				int cr = r.u8(), cg = r.u8(), cb = r.u8(), size = r.u8(), count = r.u16(); float life = r.u16() / 100.0f; if (!r.ok || !enabled()) break;
				efx_api_s *fx = g_eng.pEfxAPI; if (!fx) break;
				switch (kind) {
					case NX_FX_EXPLOSION: if (fx->R_ParticleExplosion) fx->R_ParticleExplosion(a); break;
					case NX_FX_SPARKS: if (fx->R_SparkShower) fx->R_SparkShower(a); break;
					case NX_FX_TELEPORT: if (fx->R_TeleportSplash) fx->R_TeleportSplash(a); break;
					case NX_FX_LAVA: if (fx->R_LavaSplash) fx->R_LavaSplash(a); break;
					case NX_FX_BLOB: if (fx->R_BlobExplosion) fx->R_BlobExplosion(a); break;
					case NX_FX_BURST: if (fx->R_ParticleBurst) fx->R_ParticleBurst(a, size, cr, life); break;
					case NX_FX_IMPLOSION: if (fx->R_Implosion) fx->R_Implosion(a, (float)size * 4, count > 512 ? 512 : count, life); break;
					case NX_FX_BEAM: if (fx->R_ParticleLine) fx->R_ParticleLine(a, b, (unsigned char)cr, (unsigned char)cg, (unsigned char)cb, life); break;
				}
				break;
			}
			case NX_OP_SOUND3D: {
				int ent = r.u16(); float pos[3]; for (float &v : pos) v = (float)r.i16(); int vol = r.u8(), attn = r.u8(), pitch = r.u8(); std::string n = r.str();
				if (!r.ok || !enabled() || !safe_name(n) || !ends_with(n, ".wav") || !g_eng.pEventAPI || !g_eng.pEventAPI->EV_PlaySound) break;
				if (ent > 0) { cl_entity_t *e = g_eng.GetEntityByIndex ? g_eng.GetEntityByIndex(ent) : nullptr; if (e) for (int i = 0; i < 3; i++) pos[i] = e->origin[i]; }
				g_eng.pEventAPI->EV_PlaySound(ent, pos, 0, n.c_str(), vol / 255.0f, attn / 64.0f, 0, pitch);
				break;
			}
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
	restore_entities();
	if (!g_inited) return;
	SCREENINFO si; memset(&si, 0, sizeof si); si.iSize = sizeof si; if (g_eng.pfnGetScreenInfo) g_eng.pfnGetScreenInfo(&si);
	if (si.iWidth > 0 && si.iHeight > 0) { g_w = si.iWidth; g_h = si.iHeight; }
	g_unit = g_h / 768.0f * clampf(cv_scale ? cv_scale->value : 1, 0.5f, 3.0f);
	double now = nxp::now(); tick(now);
	bool any_plugin = false; for (auto &p : g_plugins) if (p.p.on_draw) any_plugin = true;
	if (g_url.on && now - g_url.t0 > 20.0) url_finish(0);             // no answer in 20 s = no
	if (!enabled() || ((!S.on || (S.el.empty() && !S.cursor && !g_url.on)) && !any_plugin)) return;
	g_gl = nxr::begin(g_w, g_h); g_drawing = true;
	if (S.on) {
		std::vector<Elem *> order; order.reserve(S.el.size()); for (auto &kv : S.el) order.push_back(&kv.second);
		std::sort(order.begin(), order.end(), [](const Elem *a, const Elem *b) { return a->layer != b->layer ? a->layer < b->layer : a->id < b->id; });
		for (Elem *e : order) draw_elem(*e, now);
		S.hover = S.cursor ? hit_test() : 0;
	}
	for (auto &p : g_plugins) if (p.p.on_draw) p.p.on_draw();
	if (g_url.on) {                                                       // Nexus' own prompt: servers cannot change its look or answer it
		float u = g_unit, w = 560 * u, h = 196 * u, x = (g_w - w) / 2, y = g_h * 0.30f, pad = 18 * u; char b[300];
		const float bg[4] = {0.05f, 0.06f, 0.09f, 0.96f}, edge[4] = {0.35f, 0.75f, 1, 1}, white[4] = {1, 1, 1, 1}, gray[4] = {0.7f, 0.7f, 0.75f, 1}, yellow[4] = {1, 0.85f, 0.25f, 1}, blue[4] = {0.45f, 0.8f, 1, 1};
		be_rect(x - 2, y - 2, w + 4, h + 4, edge); be_rect(x, y, w, h, bg);
		be_text(x + pad, y + pad, "The server wants to open a link in your browser", 47, 16 * u, gray);
		be_text(x + pad, y + pad + 28 * u, g_url.host.c_str(), g_url.host.size(), 26 * u, blue);
		std::string shown = g_url.url.size() > 70 ? g_url.url.substr(0, 67) + "..." : g_url.url; be_text(x + pad, y + pad + 66 * u, shown.c_str(), shown.size(), 13 * u, gray);
		if (!g_url.label.empty()) { snprintf(b, sizeof b, "\"%s\"", g_url.label.c_str()); be_text(x + pad, y + pad + 90 * u, b, strlen(b), 15 * u, white); }
		int left = (int)ceil(20.0 - (now - g_url.t0)); snprintf(b, sizeof b, "[Y] open     [N] no     (%d)", left < 0 ? 0 : left);
		be_text(x + pad, y + h - pad - 22 * u, b, strlen(b), 19 * u, yellow);
	}
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
void on_frame(double) {
	for (auto &p : g_plugins) if (p.p.on_frame) p.p.on_frame();
	if (!g_inited || !S.on || FX.lights.empty()) return;
	double now = nxp::now(); efx_api_s *fx = g_eng.pEfxAPI; float ct = g_eng.GetClientTime ? g_eng.GetClientTime() : 0;
	for (auto it = FX.lights.begin(); it != FX.lights.end();) {
		Light &li = it->second; if (li.life > 0 && now - li.born >= li.life) { it = FX.lights.erase(it); continue; }
		float o[3] = {li.pos[0], li.pos[1], li.pos[2]}; bool show = enabled();
		if (li.ent > 0) { cl_entity_t *e = g_eng.GetEntityByIndex ? g_eng.GetEntityByIndex(li.ent) : nullptr, *lp = g_eng.GetLocalPlayer ? g_eng.GetLocalPlayer() : nullptr;
			if (!e || !lp || !e->model || (e != lp && e->curstate.messagenum < lp->curstate.messagenum)) show = false; else for (int i = 0; i < 3; i++) o[i] += e->origin[i]; }
		if (show && fx && fx->CL_AllocDlight) {
			dlight_t *dl = fx->CL_AllocDlight(0x4E580000 + it->first);       // the same key every frame = the same light, moved / refreshed
			if (dl) { for (int i = 0; i < 3; i++) dl->origin[i] = o[i]; dl->radius = li.radius; dl->color.r = li.col[0]; dl->color.g = li.col[1]; dl->color.b = li.col[2]; dl->die = ct + 0.1f; dl->decay = 0; dl->dark = 0; }
		}
		++it;
	}
}

// ------------------------------------------------------------------------------------------------ input
int on_key(int down, int keynum, const char *binding) {
	if (!g_inited) return 1;
	if (g_url.on) {                                                       // the link prompt owns Y / N (and ESC = no)
		if (keynum == 'y' || keynum == 'n') {
			if (down) { if (keynum == 'y') { std::string u = g_url.url; bool okk = nxp::open_url(u); url_finish(okk ? 1 : 0); if (cv_debug && cv_debug->value) conf("[nexus] opened %s\n", u.c_str()); } else url_finish(0); }
			return 0;
		}
		if (keynum == K_ESCAPE && down) url_finish(0);
	}
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
static void input_control(void (*orig)(float, usercmd_s *, int), float frametime, usercmd_s *cmd, int active) {
	float before[3] = {0, 0, 0}, after[3] = {0, 0, 0}; bool look = (FX.block & NX_IN_LOOK) || FX.sens != 1 || FX.invert;
	if (look && g_eng.GetViewAngles) g_eng.GetViewAngles(before);
	if (orig) orig(frametime, cmd, active);
	if (!cmd || !active) return;
	if (look && g_eng.GetViewAngles && g_eng.SetViewAngles) {
		g_eng.GetViewAngles(after); float d[2] = {after[0] - before[0], after[1] - before[1]}; while (d[1] > 180) d[1] -= 360; while (d[1] < -180) d[1] += 360;
		float k = (FX.block & NX_IN_LOOK) ? 0 : FX.sens; float n[3] = {before[0] + d[0] * k * ((FX.invert & 2) ? -1 : 1), before[1] + d[1] * k * ((FX.invert & 1) ? -1 : 1), after[2]};
		n[0] = clampf(n[0], -89, 89); g_eng.SetViewAngles(n); for (int i = 0; i < 3; i++) cmd->viewangles[i] = n[i];
	}
	int b = cmd->buttons;
	if (FX.block & NX_IN_ATTACK) b &= ~IN_ATTACK;   if (FX.block & NX_IN_ATTACK2) b &= ~IN_ATTACK2; if (FX.block & NX_IN_JUMP) b &= ~IN_JUMP;
	if (FX.block & NX_IN_DUCK) b &= ~IN_DUCK;       if (FX.block & NX_IN_USE) b &= ~IN_USE;         if (FX.block & NX_IN_RELOAD) b &= ~IN_RELOAD;
	if (FX.block & NX_IN_MOVE) { b &= ~(IN_FORWARD | IN_BACK); cmd->forwardmove = 0; cmd->upmove = 0; }
	if (FX.block & NX_IN_STRAFE) { b &= ~(IN_MOVELEFT | IN_MOVERIGHT); cmd->sidemove = 0; }
	if (FX.force & NX_IN_DUCK) b |= IN_DUCK; if (FX.force & NX_IN_JUMP) b |= IN_JUMP; if (FX.force & NX_IN_ATTACK) b |= IN_ATTACK; if (FX.force & NX_IN_ATTACK2) b |= IN_ATTACK2; if (FX.force & NX_IN_USE) b |= IN_USE;
	if (FX.force & NX_IN_WALK) { cmd->forwardmove *= 0.52f; cmd->sidemove *= 0.52f; }
	cmd->buttons = b;
}
void createmove(void (*orig)(float, usercmd_s *, int), float frametime, usercmd_s *cmd, int active) {
	bool control = g_inited && S.on && enabled() && (FX.block || FX.force || FX.sens != 1 || FX.invert);
	if ((!g_inited || !S.on || !S.cursor || !active || !enabled() || !g_eng.GetViewAngles || !g_eng.SetViewAngles)) { if (control) input_control(orig, frametime, cmd, active); else if (orig) orig(frametime, cmd, active); return; }
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
	if (g_inited && S.on && enabled()) {                                // shake / sway / punch: the picture moves, the aim does not
		double now = nxp::now(); float add[3] = {0, 0, 0};
		if (FX.shake_amp > 0 && now - FX.shake_t0 < FX.shake_dur) {
			float t = (float)(now - FX.shake_t0), env = 1 - t / (FX.shake_dur > 0 ? FX.shake_dur : 1), w = 6.2831853f * (FX.shake_freq > 0 ? FX.shake_freq : 10);
			add[0] += FX.shake_amp * env * sinf(w * t + 1.3f); add[1] += FX.shake_amp * env * sinf(w * 1.13f * t); add[2] += FX.shake_amp * 0.5f * env * sinf(w * 0.71f * t + 0.4f);
		}
		if (FX.sway_amp > 0) { float t = (float)now * FX.sway_speed; add[0] += FX.sway_amp * sinf(t * 1.3f); add[1] += FX.sway_amp * 0.7f * sinf(t * 0.9f + 1); add[2] += FX.sway_amp * 0.5f * sinf(t * 0.7f); }
		if (FX.punch[0] || FX.punch[1]) { float k = expf(-(float)(now - FX.punch_t0) * 6.0f); if (k < 0.002f) FX.punch[0] = FX.punch[1] = 0; add[0] += FX.punch[0] * k; add[1] += FX.punch[1] * k; }
		for (int i = 0; i < 3; i++) p->viewangles[i] += add[i];
	}
	memcpy(g_vieworg, p->vieworg, sizeof g_vieworg); g_have_view = true;
}

// ------------------------------------------------------------------------------------------------ FOV, entity overrides, 3D shapes
int update_client_data(int orig_result, client_data_s *cdp) {
	client_data_t *cd = (client_data_t *)cdp;
	if (!cd || !g_inited || !S.on || !enabled() || !FX.fov_on) return orig_result;
	float game = cd->fov > 1 ? cd->fov : 90, from = FX.fov_from > 0 ? FX.fov_from : game, to = FX.fov_release ? game : FX.fov_to;
	float t = FX.fov_dur > 0 ? clampf((float)((nxp::now() - FX.fov_t0) / FX.fov_dur), 0, 1) : 1; t = t * t * (3 - 2 * t);
	float v = from + (to - from) * t;
	if (FX.fov_release && t >= 1) { FX.fov_on = false; return orig_result; }
	cd->fov = clampf(v, 10, 150); FX.fov_cur = cd->fov; return 1;
}
int add_entity(int orig_result, int, cl_entity_s *entp) {
	cl_entity_t *e = (cl_entity_t *)entp;
	if (!orig_result || !e || !g_inited || !S.on || FX.ent.empty() || !enabled()) return orig_result;
	auto it = FX.ent.find(e->index); if (it == FX.ent.end()) return orig_result;
	const EntOv &o = it->second; if (o.flags & 1) return 0;
	if (o.flags & 2) {
		bool saved = false; for (auto &s : g_saved) if (s.e == e) saved = true;
		if (!saved && g_saved.size() < 1024) g_saved.push_back(Saved{e, e->curstate.rendermode, e->curstate.renderamt, e->curstate.renderfx, e->curstate.rendercolor, e->curstate.scale});
		e->curstate.rendermode = o.mode; e->curstate.renderamt = o.amt; e->curstate.renderfx = o.fx;
		e->curstate.rendercolor.r = o.col[0]; e->curstate.rendercolor.g = o.col[1]; e->curstate.rendercolor.b = o.col[2]; if (o.scale > 0) e->curstate.scale = o.scale;
	}
	return orig_result;
}
void draw_world() {
	if (!g_inited || !S.on || FX.shapes.empty() || !enabled()) return;
	double now = nxp::now(); bool begun = false;
	for (auto it = FX.shapes.begin(); it != FX.shapes.end();) {
		Shape &s = it->second; if (s.life > 0 && now - s.born >= s.life) { it = FX.shapes.erase(it); continue; }
		float base[3] = {0, 0, 0}; bool show = true;
		if (s.ent > 0) { cl_entity_t *e = g_eng.GetEntityByIndex ? g_eng.GetEntityByIndex(s.ent) : nullptr, *lp = g_eng.GetLocalPlayer ? g_eng.GetLocalPlayer() : nullptr;
			if (!e || !lp || !e->model || (e != lp && e->curstate.messagenum < lp->curstate.messagenum)) show = false; else for (int i = 0; i < 3; i++) base[i] = e->origin[i]; }
		if (!show) { ++it; continue; }
		if (!begun) { if (!nxr::begin3d()) return; begun = true; }
		float c[4] = {s.col[0], s.col[1], s.col[2], s.col[3]};
		if (s.flags & NX_SHF_PULSE) c[3] *= 0.55f + 0.45f * sinf((float)now * 4.0f);
		if (s.life > 0 && s.life - (now - s.born) < 0.5) c[3] *= (float)((s.life - (now - s.born)) / 0.5);
		bool xr = (s.flags & NX_SHF_XRAY) != 0, add = (s.flags & NX_SHF_ADDITIVE) != 0, fill = (s.flags & NX_SHF_FILL) != 0;
		float A[3] = {base[0] + s.a[0], base[1] + s.a[1], base[2] + s.a[2]}, B[3] = {base[0] + s.b[0], base[1] + s.b[1], base[2] + s.b[2]};
		float fc[4] = {c[0], c[1], c[2], c[3] * 0.35f};
		if (s.kind == NX_SH_LINE) { float p[6] = {A[0], A[1], A[2], B[0], B[1], B[2]}; nxr::segs3d(p, 1, c, s.width, xr, add); }
		else if (s.kind == NX_SH_BOX) {
			float x0 = A[0], y0 = A[1], z0 = A[2], x1 = B[0], y1 = B[1], z1 = B[2];
			float v[8][3] = {{x0, y0, z0}, {x1, y0, z0}, {x1, y1, z0}, {x0, y1, z0}, {x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}};
			static const int E[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6}, {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
			float p[72]; for (int i = 0; i < 12; i++) for (int k = 0; k < 2; k++) for (int j = 0; j < 3; j++) p[i * 6 + k * 3 + j] = v[E[i][k]][j];
			if (fill) { static const int F[6][4] = {{0, 1, 2, 3}, {4, 5, 6, 7}, {0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}};
				for (auto &f : F) { float q[12]; for (int k = 0; k < 4; k++) for (int j = 0; j < 3; j++) q[k * 3 + j] = v[f[k]][j]; nxr::fan3d(q, 4, fc, xr, add); } }
			nxr::segs3d(p, 12, c, s.width, xr, add);
		} else if (s.kind == NX_SH_QUAD) {
			float q[12] = {A[0], A[1], A[2], B[0], A[1], A[2], B[0], B[1], A[2], A[0], B[1], A[2]};
			if (fill) nxr::fan3d(q, 4, fc, xr, add); nxr::lines3d(q, 4, true, c, s.width, xr, add);
		} else if (s.kind == NX_SH_CIRCLE || s.kind == NX_SH_CYLINDER) {
			int n = (int)s.b[2]; if (n < 6) n = 6; if (n > 96) n = 96; float r = s.b[0], h = s.kind == NX_SH_CYLINDER ? s.b[1] : 0;
			float ring[97 * 3], top[97 * 3];
			for (int i = 0; i < n; i++) { float a = 6.2831853f * i / n; ring[i * 3] = A[0] + r * cosf(a); ring[i * 3 + 1] = A[1] + r * sinf(a); ring[i * 3 + 2] = A[2]; top[i * 3] = ring[i * 3]; top[i * 3 + 1] = ring[i * 3 + 1]; top[i * 3 + 2] = A[2] + h; }
			if (fill && !h) nxr::fan3d(ring, n, fc, xr, add);
			nxr::lines3d(ring, n, true, c, s.width, xr, add);
			if (h) { nxr::lines3d(top, n, true, c, s.width, xr, add); float seg[16 * 6]; int k = 0;
				for (int i = 0; i < n && k < 16; i += (n / 8 > 0 ? n / 8 : 1), k++) { for (int j = 0; j < 3; j++) { seg[k * 6 + j] = ring[i * 3 + j]; seg[k * 6 + 3 + j] = top[i * 3 + j]; } }
				nxr::segs3d(seg, k, c, s.width, xr, add);
				if (fill) for (int i = 0; i < n; i++) { int j2 = (i + 1) % n; float q[12] = {ring[i * 3], ring[i * 3 + 1], ring[i * 3 + 2], ring[j2 * 3], ring[j2 * 3 + 1], ring[j2 * 3 + 2], top[j2 * 3], top[j2 * 3 + 1], top[j2 * 3 + 2], top[i * 3], top[i * 3 + 1], top[i * 3 + 2]}; nxr::fan3d(q, 4, fc, xr, add); } }
		}
		++it;
	}
	if (begun) nxr::end3d();
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
		cv_urls = g_eng.pfnRegisterVariable((char *)"nexus_urls", (char *)"1", FCVAR_ARCHIVE);     // 0 = servers may never ask to open links
	}
	if (g_eng.pfnHookUserMsg) g_eng.pfnHookUserMsg((char *)NEXUS_MSG_NAME, on_message);
	if (g_eng.pfnAddCommand) g_eng.pfnAddCommand((char *)"nexus_info", cmd_info);
	g_inited = true; load_plugins();
	conf("[nexus] client %s ready (%d plugin%s)\n", NEXUS_VERSION, (int)g_plugins.size(), g_plugins.size() == 1 ? "" : "s");
}
void on_vid_init() { g_saved.clear(); session_reset(false); nxr::device_lost(); g_have_view = false; }      // new map / new connection: the server greets us again
void on_shutdown() { session_reset(false); for (auto &p : g_plugins) if (p.p.on_shutdown) p.p.on_shutdown(); g_plugins.clear(); g_inited = false; }
}
