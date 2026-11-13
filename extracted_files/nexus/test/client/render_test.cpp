// Client core test (native build): the real nexus_client.cpp + renderer run against a fake engine and an off-screen OpenGL (OSMesa).
// Replays the message stream recorded by the server test, renders PNG snapshots and checks behaviour.
// usage: render_test <stream.bin> <asset dir (acts as the game dir)> <out dir>        NOGL=1 -> test the engine-drawing fallback instead
#include "nx_sdk.h"
#include "ref_params.h"
#include "nx_client.h"
#include "nx_platform.h"
#include "../../shared/nexus_proto.h"
#include <GL/gl.h>
#include <GL/osmesa.h>
#include <dlfcn.h>
#include <cmath>
#include <cstdarg>
#include <map>
#include <string>
#include <vector>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

static const int W = 1280, H = 720;
static cl_enginefunc_t E; static triangleapi_t TRI; static event_api_t EV;
static std::vector<std::string> g_srv, g_cl, g_snd, g_con; static std::map<std::string, cvar_t *> g_cvars; static std::map<std::string, void (*)()> g_cmds;
static pfnUserMsgHook g_hook; static std::string g_assets; static double g_clock = 100.0; static float g_angles[3] = {0, 0, 0};
static cl_entity_t g_ents[8]; static struct model_s *g_fake_model = (struct model_s *)0x1000; static int g_fill = 0, g_constr = 0;
static int pass = 0, fail = 0;
static void ok(bool c, const std::string &what) { if (c) pass++; else { fail++; printf("CLIENT FAIL %s\n", what.c_str()); } }
static bool has(const std::vector<std::string> &v, const std::string &s) { for (auto &x : v) if (x == s) return true; return false; }
static bool has_prefix(const std::vector<std::string> &v, const std::string &s) { for (auto &x : v) if (!x.compare(0, s.size(), s)) return true; return false; }
static int elements() { g_con.clear(); g_cmds["nexus_info"](); for (auto &l : g_con) { size_t p = l.find("elements: "); if (p != std::string::npos) return atoi(l.c_str() + p + 10); } return -1; }

static void con_printf(char *fmt, ...) { char b[1024]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a); g_con.push_back(b); }
static int w2s(float *w, float *s) {                                      // camera at (0,0,17) looking along +x, 90 degree horizontal fov
	float f = w[0], r = -w[1], u = w[2] - 17; if (f <= 1) return 1;
	s[0] = r / f; s[1] = u / f * ((float)W / H); return 0;
}
static void trace(float *, float *end, int, int, pmtrace_s *tr) { memset(tr, 0, sizeof *tr); tr->fraction = end[1] < -140 ? 0.5f : 1.0f; }   // a wall hides everything at y < -140
static void fake_createmove(float, usercmd_s *, int) { float a[3]; E.GetViewAngles(a); a[1] -= 10; a[0] += 5; E.SetViewAngles(a); }          // "mouse moved right and down"

static void scene() {                                                    // something game-like behind the overlay
	glViewport(0, 0, W, H); glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, W, H, 0, -1, 1); glMatrixMode(GL_MODELVIEW); glLoadIdentity();
	glDisable(GL_TEXTURE_2D); glDisable(GL_BLEND);
	glBegin(GL_QUADS); glColor3f(0.36f, 0.52f, 0.72f); glVertex2f(0, 0); glVertex2f(W, 0); glColor3f(0.75f, 0.80f, 0.85f); glVertex2f(W, H * 0.5f); glVertex2f(0, H * 0.5f);
	glColor3f(0.48f, 0.42f, 0.32f); glVertex2f(0, H * 0.5f); glVertex2f(W, H * 0.5f); glColor3f(0.30f, 0.26f, 0.20f); glVertex2f(W, H); glVertex2f(0, H);
	glColor3f(0.42f, 0.36f, 0.30f); glVertex2f(820, 250); glVertex2f(1100, 250); glVertex2f(1100, 470); glVertex2f(820, 470); glEnd();
}
static void snapshot(unsigned char *buf, const std::string &path) { glFinish(); stbi_write_png(path.c_str(), W, H, 4, buf, W * 4); }

int main(int argc, char **argv) {
	setvbuf(stdout, NULL, _IONBF, 0); if (argc < 4) { printf("usage: render_test stream.bin assets outdir\n"); return 2; }
	g_assets = argv[2]; std::string out = argv[3]; bool nogl = getenv("NOGL") != nullptr;
	std::vector<unsigned char> fb((size_t)W * H * 4); OSMesaContext ctx = nullptr;
	if (!nogl) { ctx = OSMesaCreateContextExt(OSMESA_RGBA, 16, 0, 0, nullptr); if (!ctx || !OSMesaMakeCurrent(ctx, fb.data(), GL_UNSIGNED_BYTE, W, H)) { printf("no OSMesa context\n"); return 2; } OSMesaPixelStore(OSMESA_Y_UP, 0);
		nxp::gl_override = [](const char *n) -> void * { return dlsym(RTLD_DEFAULT, n); }; }
	else nxp::gl_override = [](const char *) -> void * { return nullptr; };
	nxp::clock_override = []() { return g_clock; };
	memset(&E, 0, sizeof E); memset(&TRI, 0, sizeof TRI); memset(&EV, 0, sizeof EV);
	E.Con_Printf = con_printf;
	E.pfnRegisterVariable = [](char *n, char *v, int) -> cvar_t * { cvar_t *c = new cvar_t(); memset(c, 0, sizeof *c); c->name = strdup(n); c->string = strdup(v); c->value = (float)atof(v); g_cvars[n] = c; return c; };
	E.pfnHookUserMsg = [](char *n, pfnUserMsgHook f) -> int { if (!strcmp(n, "Nexus")) g_hook = f; return 1; };
	E.pfnAddCommand = [](char *n, void (*f)()) -> int { g_cmds[n] = f; return 1; };
	E.pfnServerCmd = [](char *c) -> int { g_srv.push_back(c); return 1; };
	E.pfnClientCmd = [](char *c) -> int { g_cl.push_back(c); return 1; };
	E.pfnPlaySoundByName = [](char *n, float v) { char b[128]; snprintf(b, sizeof b, "%s@%.2f", n, v); g_snd.push_back(b); };
	E.pfnGetScreenInfo = [](SCREENINFO *s) -> int { s->iWidth = W; s->iHeight = H; return 1; };
	E.pfnFillRGBA = [](int, int, int, int, int, int, int, int) { g_fill++; };
	E.pfnDrawConsoleString = [](int x, int, char *s) -> int { g_constr++; return x + (int)strlen(s) * 8; };
	E.pfnDrawSetTextColor = [](float, float, float) {};
	E.pfnDrawConsoleStringLen = [](const char *s, int *w, int *h) { *w = (int)strlen(s) * 8; *h = 14; };
	E.GetViewAngles = [](float *a) { memcpy(a, g_angles, sizeof g_angles); }; E.SetViewAngles = [](float *a) { memcpy(g_angles, a, sizeof g_angles); };
	E.GetLocalPlayer = []() -> cl_entity_t * { return &g_ents[1]; };
	E.GetEntityByIndex = [](int i) -> cl_entity_t * { return i >= 0 && i < 8 ? &g_ents[i] : nullptr; };
	E.pfnGetGameDirectory = []() -> const char * { return g_assets.c_str(); };
	E.COM_LoadFile = [](char *p, int, int *len) -> byte * { FILE *f = fopen((g_assets + "/" + p).c_str(), "rb"); if (!f) return nullptr; fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); byte *d = (byte *)malloc(n + 1); if (fread(d, 1, n, f) != (size_t)n) n = 0; fclose(f); *len = (int)n; return d; };
	E.COM_FreeFile = [](void *p) { free(p); };
	TRI.WorldToScreen = w2s; EV.EV_SetTraceHull = [](int) {}; EV.EV_PlayerTrace = trace; E.pTriAPI = &TRI; E.pEventAPI = &EV;
	memset(g_ents, 0, sizeof g_ents);
	auto ent = [](int i, float x, float y, float z, bool visible) { g_ents[i].model = g_fake_model; g_ents[i].origin[0] = x; g_ents[i].origin[1] = y; g_ents[i].origin[2] = z; g_ents[i].curstate.messagenum = visible ? 50 : 10; };
	ent(1, 0, 0, 0, true); ent(2, 250, 40, 0, true); ent(3, 200, 0, 0, false); ent(4, 900, -100, 0, true); ent(5, 300, -150, 0, true);

	// ---- start-up as the proxy drives it
	nxc::on_redraw(0, 0); ok(nxc::on_key(1, 102, "") == 1, "before Initialize every key passes");          // calls before init must be harmless
	nxc::on_initialize(&E); nxc::on_hud_init(); nxc::on_vid_init();
	ok(g_cvars.count("nexus_version") && !strcmp(g_cvars["nexus_version"]->string, NEXUS_VERSION), "client cvar nexus_version registered (the server detects Nexus with it)");
	ok(g_hook != nullptr, "user message Nexus hooked"); ok(g_cmds.count("nexus_info") == 1, "console command nexus_info registered");
	{ unsigned char m[] = {NX_OP_SET, 5, 0, NX_T_RECT, NX_F_SIZE, 0, 0, 0, 0, 0, 10, 0, 10, 0}; g_hook("Nexus", sizeof m, m); ok(elements() == 0, "ops before the handshake are ignored"); }

	// ---- replay
	FILE *f = fopen(argv[1], "rb"); if (!f) { printf("cannot open %s\n", argv[1]); return 2; }
	int c, snaps = 0; std::map<std::string, int> count_at;
	while ((c = fgetc(f)) != EOF) {
		if (c == 'T') { int ms = fgetc(f); ms |= fgetc(f) << 8; g_clock += ms / 1000.0; }
		else if (c == 'M') { int n = fgetc(f); n |= fgetc(f) << 8; std::vector<unsigned char> b(n); if (fread(b.data(), 1, n, f) != (size_t)n) break; g_hook("Nexus", n, b.data()); }
		else if (c == 'S') { int n = fgetc(f); std::string label(n, 0); if (fread(&label[0], 1, n, f) != (size_t)n) break; snaps++;
			float before[3]; memcpy(before, g_angles, sizeof before);
			if (!nogl) scene();
			int mm = 0, tex = 0; GLboolean blend = 0; if (!nogl) { glBindTexture(GL_TEXTURE_2D, 7); glGetIntegerv(GL_MATRIX_MODE, &mm); blend = glIsEnabled(GL_BLEND); }
			nxc::on_frame(g_clock); nxc::on_redraw((float)g_clock, 0);
			if (!nogl) { int mm2 = 0; glGetIntegerv(GL_MATRIX_MODE, &mm2); glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
				ok(mm2 == mm && glIsEnabled(GL_BLEND) == blend && tex == 7 && glGetError() == GL_NO_ERROR, "GL state is restored after drawing (" + label + ")");
				snapshot(fb.data(), out + "/" + label + ".png"); }
			count_at[label] = elements();
			if (label == "hud") { ok(has_prefix(g_srv, "nexus_hello 1 " NEXUS_VERSION " 1280 720 "), "the client answered HELLO with nexus_hello (proto, version, screen, caps, token)");
				ok(count_at[label] == 10, "hud: 10 elements exist (got " + std::to_string(count_at[label]) + ")"); }
			if (label == "menu") { ok(nxc::on_key(1, '2', "slot2") == 0, "menu open: key 2 is swallowed (no weapon switch)"); ok(has(g_srv, "nexus_key 50 1"), "menu open: key 2 is reported to the server");
				ok(nxc::on_key(1, K_ESCAPE, "cancelselect") == 1, "ESC is never swallowed"); ok(nxc::on_key(1, 'w', "+forward") == 1, "unsubscribed keys pass and are not reported"); ok(!has(g_srv, "nexus_key 119 1"), "w was not sent"); }
			if (label == "world") ok(count_at[label] >= 14, "world: labels exist");
			if (label == "anim_end") ok(count_at["anim_end"] == count_at["anim"] - 1, "the element animated with remove=true deleted itself (" + std::to_string(count_at["anim"]) + " -> " + std::to_string(count_at["anim_end"]) + ")");
			if (label == "input") {
				g_srv.clear(); ok(nxc::on_key(1, K_MOUSE1, "+attack") == 0, "cursor mode: mouse1 is swallowed (no shooting)"); ok(has(g_srv, "nexus_click 777 1"), "click in the middle of the screen hits element 777 (sent: " + (g_srv.empty() ? std::string("nothing") : g_srv.back()) + ")");
				usercmd_t cmd; memset(&cmd, 0, sizeof cmd); cmd.buttons = IN_ATTACK; float lock[3]; memcpy(lock, g_angles, sizeof lock);
				for (int i = 0; i < 3; i++) nxc::createmove(fake_createmove, 0.01f, &cmd, 1);
				ok(g_angles[0] == lock[0] && g_angles[1] == lock[1], "cursor mode: the view does not turn"); ok(!(cmd.buttons & IN_ATTACK), "cursor mode: attack is removed from the command");
				if (!nogl) scene(); nxc::on_redraw((float)g_clock, 0); if (!nogl) snapshot(fb.data(), out + "/input_cursor_moved.png");
				g_srv.clear(); nxc::on_key(1, K_MOUSE1, "+attack"); ok(has(g_srv, "nexus_click 0 1"), "after moving the cursor away the click hits nothing (sent: " + (g_srv.empty() ? std::string("nothing") : g_srv.back()) + ")");
				g_srv.clear(); ok(nxc::on_key(1, 'f', "impulse 100") == 0 && has(g_srv, "nexus_key 102 1"), "bound key f: blocked and reported");
				g_cvars["nexus_keys"]->value = 0; g_srv.clear(); ok(nxc::on_key(1, 'f', "impulse 100") == 1 && g_srv.empty(), "nexus_keys 0: the player switched key reporting off"); g_cvars["nexus_keys"]->value = 1;
			}
			if (label == "flood") ok(count_at[label] >= 400, "all 400 flood elements arrived (" + std::to_string(count_at[label]) + " elements)");
			if (label == "misc") {
				ok(nxc::third_person(), "camera: third person is active"); ref_params_t rp; memset(&rp, 0, sizeof rp); rp.vieworg[2] = 17; nxc::post_calcrefdef((ref_params_s *)&rp);
				ok(fabsf(rp.vieworg[0] + 150) < 1 && fabsf(rp.vieworg[2] - 37) < 1, "camera: view moved 150 back and 20 up (" + std::to_string(rp.vieworg[0]) + ", " + std::to_string(rp.vieworg[2]) + ")");
				ok(has(g_cl, "mp3 loop sound/music/theme.mp3\n"), "music: fixed mp3 command"); ok(has(g_snd, "items/gunpickup2.wav@0.50"), "sound played at volume 0.5");
			}
			if (label == "clear") { ok(count_at[label] == 0, "clear: no elements left"); ok(!nxc::third_person(), "camera off"); ok(has(g_cl, "mp3 stop\n"), "music stopped"); }
			(void)before;
		}
	}
	fclose(f); ok(snaps >= 9, "all snapshots were taken");
	if (nogl) ok(g_fill > 20 && g_constr > 20, "without OpenGL the engine's own 2D calls are used (" + std::to_string(g_fill) + " fills, " + std::to_string(g_constr) + " strings)");

	// ---- hostile input: the client must stay inside its rules whatever a server sends
	auto send = [&](std::string b) { g_hook("Nexus", (int)b.size(), (void *)b.data()); };
	auto str = [](const std::string &s) { return std::string(1, (char)s.size()) + s; };
	g_snd.clear(); g_cl.clear();
	send(std::string(1, (char)NX_OP_SOUND) + "\xff" + str("../../evil.wav")); send(std::string(1, (char)NX_OP_SOUND) + "\xff" + str("x.mp3"));
	send(std::string(1, (char)NX_OP_MUSIC) + "\x01" + str("a.mp3;quit")); send(std::string(1, (char)NX_OP_MUSIC) + "\x01" + str("a b\";quit;\".mp3")); send(std::string(1, (char)NX_OP_MUSIC) + "\x01" + str("/etc/x.mp3"));
	ok(g_snd.empty() && g_cl.empty(), "bad sound / music names are refused (no command injection, no path escape)");
	{ std::string k(1, (char)NX_OP_KEYS); k += (char)3; k += (char)K_ESCAPE; k += (char)1; k += '`'; k += (char)1; k += 'x'; k += (char)1; send(k);
	  ok(nxc::on_key(1, K_ESCAPE, "") == 1 && nxc::on_key(1, '`', "toggleconsole") == 1 && nxc::on_key(1, 'x', "") == 0, "ESC and the console key cannot be captured, other keys can"); }
	{ std::string k(1, (char)NX_OP_KEYS); k += (char)0; send(k); }
	send(std::string(1, (char)NX_OP_SET) + std::string("\x09\x00\x03\x80\x00", 5) + str("../../../secret.png")); if (!nogl) nxc::on_redraw(0, 0);
	unsigned seed = 12345; auto rnd = [&]() { seed = seed * 1103515245u + 12345u; return (seed >> 16) & 0xFF; };
	for (int i = 0; i < 30000; i++) { int n = 1 + rnd() % 60; std::string b; for (int j = 0; j < n; j++) b += (char)rnd(); if (i % 3 == 0) b[0] = (char)(2 + rnd() % 12); if (b[0] == NX_OP_MUSIC || b[0] == NX_OP_SOUND) b[0] = NX_OP_SET; send(b); if (i % 500 == 0) { g_clock += 0.3; nxc::on_redraw(0, 0); } }
	ok(elements() <= NEXUS_MAX_ELEMS, "30000 random messages: no crash, element limit holds (" + std::to_string(elements()) + ")");
	ok(g_srv.size() < 400, "random messages do not make the client spam the server");
	nxc::on_vid_init(); ok(elements() == 0 && !nxc::third_person(), "map change (HUD_VidInit) drops the session");
	g_srv.clear(); ok(nxc::on_key(1, 'f', "") == 1 && g_srv.empty(), "no session: nothing is reported");
	g_cvars["nexus_enable"]->value = 0; { unsigned char m[] = {NX_OP_HELLO, 7, 0}; g_hook("Nexus", 3, m); } ok(g_srv.empty(), "nexus_enable 0: HELLO is not answered"); g_cvars["nexus_enable"]->value = 1;

	// ---- client plugin (only when the test put one into <assets>/nexus/)
	g_con.clear(); g_cmds["nexus_info"](); bool plug = false; for (auto &l : g_con) if (l.find("fps_plugin") != std::string::npos) plug = true;
	if (getenv("EXPECT_PLUGIN")) {
		ok(plug, "client plugin from nexus/plugins.ini is loaded");
		{ unsigned char m[] = {NX_OP_HELLO, 9, 0}; g_hook("Nexus", 3, m); } g_srv.clear();
		send(std::string(1, (char)NX_OP_DATA) + str("fps.query") + str("")); ok(has_prefix(g_srv, "nexus_data fps.value "), "plugin answered a data message through api->send (" + (g_srv.empty() ? std::string("nothing") : g_srv.back()) + ")");
		g_clock += 1; nxc::on_frame(g_clock); if (!nogl) { scene(); nxc::on_redraw(0, 0); snapshot(fb.data(), out + "/plugin.png"); }
	} else ok(!plug, "no plugins.ini -> no plugin is loaded");
	nxc::on_shutdown();
	if (ctx) OSMesaDestroyContext(ctx);
	printf("NEXUS CLIENT TEST DONE (%s): %d passed, %d failed\n", nogl ? "engine fallback" : "OpenGL", pass, fail);
	return fail ? 1 : 0;
}
