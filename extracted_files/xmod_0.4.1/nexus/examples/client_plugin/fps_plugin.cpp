// Example Nexus CLIENT plugin: an FPS counter, plus a "ping" data channel to the server.
//   build:   g++ -m32 -shared -fPIC -Iinclude fps_plugin.cpp -o fps_plugin.so        (Windows: i686-w64-mingw32-g++ -shared ... -o fps_plugin.dll -static)
//   install: <game dir>/nexus/plugins/fps_plugin.dll   and a line "fps_plugin" in <game dir>/nexus/plugins.ini
#include "nexus_plugin.h"
#include <cstdio>
#include <cstring>
static const NexusApi *N;
static double g_last = 0, g_acc = 0; static int g_frames = 0, g_fps = 0; static char g_note[64] = "";

static void on_frame() {
	double t = N->time(); if (g_last > 0) g_acc += t - g_last; g_last = t; g_frames++;
	if (g_acc >= 0.5) { g_fps = (int)(g_frames / g_acc + 0.5); g_frames = 0; g_acc = 0; }
}
static void on_draw() {
	int w, h; float unit; N->screen(&w, &h, &unit); char b[96]; snprintf(b, sizeof b, "%d fps %s", g_fps, g_note);
	float px = 14 * unit, tw = N->text_width(b, px);
	N->rect(w - tw - 14 * unit, 6 * unit, tw + 8 * unit, px * 1.35f, 0x00000090);
	N->text(w - tw - 10 * unit, 7 * unit, b, px, g_fps >= 60 ? 0x7CFC7CFF : 0xFFB040FF);
}
static void on_data(const char *channel, const void *data, int len) {            // Lua: xmod.nexus.send(id, "fps.note", "hello")
	if (!strcmp(channel, "fps.note")) { if (len > 63) len = 63; memcpy(g_note, data, len); g_note[len] = 0; }
	if (!strcmp(channel, "fps.query")) { char b[32]; int n = snprintf(b, sizeof b, "%d", g_fps); N->send("fps.value", b, n); }   // -> Lua event nexus_data(id, "fps.value", "143")
}
static void on_session(int ready) { if (!ready) g_note[0] = 0; }

extern "C" NEXUS_PLUGIN_EXPORT int NexusPlugin_Init(const NexusApi *api, NexusPlugin *out) {
	if (!api || api->abi != NEXUS_PLUGIN_ABI) return 0;
	N = api; out->name = "fps_plugin"; out->version = "1.0"; out->on_frame = on_frame; out->on_draw = on_draw; out->on_data = on_data; out->on_session = on_session;
	return NEXUS_PLUGIN_ABI;
}
