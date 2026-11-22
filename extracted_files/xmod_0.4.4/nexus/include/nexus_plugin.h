/* nexus_plugin.h - C ABI for Nexus CLIENT plugins (a .dll / .so loaded by the Nexus client library).
 *
 *   A plugin exports:   int NexusPlugin_Init(const NexusApi *api, NexusPlugin *out);     -> return NEXUS_PLUGIN_ABI
 *   It is loaded only if its name is listed in  <game dir>/nexus/plugins.ini  and the file is  <game dir>/nexus/plugins/<name>.dll (.so).
 *   Servers cannot install or enable plugins: that folder and list are edited by the player only.
 *
 *   Talking to the server:  api->send("channel", data, len)  fires the Lua event  nexus_data(id, channel, data)  on the server;
 *                           Lua  xmod.nexus.send(id, "channel", data)  arrives in  on_data(channel, data, len).   Max 96 bytes each way.
 */
#ifndef NEXUS_PLUGIN_H
#define NEXUS_PLUGIN_H
#ifdef __cplusplus
extern "C" {
#endif
#define NEXUS_PLUGIN_ABI 1
struct cl_enginefuncs_s;

typedef struct NexusApi {
	unsigned abi, size;                                   /* NEXUS_PLUGIN_ABI, sizeof(NexusApi) of the host: check size before using fields added later */
	struct cl_enginefuncs_s *engine;                      /* the engine's client interface (HL SDK cl_enginefunc_t) */
	void   (*log)(const char *text);                      /* console line */
	int    (*session)(void);                              /* 1 while connected to a server that runs Nexus */
	int    (*send)(const char *channel, const void *data, int len);    /* 1 = sent */
	void   (*screen)(int *w, int *h, float *unit);        /* unit = real pixels per virtual pixel (screen height / 768 * nexus_scale) */
	double (*time)(void);                                 /* monotonic seconds */
	/* drawing - only inside on_draw. Coordinates are real pixels, colours 0xRRGGBBAA */
	void   (*rect)(float x, float y, float w, float h, unsigned rgba);
	void   (*text)(float x, float y, const char *utf8, float px, unsigned rgba);
	float  (*text_width)(const char *utf8, float px);
	int    (*image)(const char *path);                    /* png / tga / bmp / jpg below the game dir; -1 = not found */
	void   (*image_draw)(int image, float x, float y, float w, float h, unsigned rgba);
	int    (*world_to_screen)(const float *world, float *x, float *y);   /* 1 = in front of the camera */
} NexusApi;

typedef struct NexusPlugin {                              /* fill what you need, leave the rest NULL */
	const char *name, *version;
	void (*on_frame)(void);                               /* every client frame */
	void (*on_draw)(void);                                /* after the HUD and the server's Nexus elements were drawn */
	int  (*on_key)(int down, int keynum, const char *binding);   /* return 1 to swallow the key */
	void (*on_data)(const char *channel, const void *data, int len);
	void (*on_session)(int ready);                        /* 1 = a Nexus server said hello, 0 = disconnected / map change */
	void (*on_shutdown)(void);
} NexusPlugin;

#ifdef _WIN32
#define NEXUS_PLUGIN_EXPORT __declspec(dllexport)
#else
#define NEXUS_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif
#ifdef __cplusplus
}
#endif
#endif
