// Nexus client core: everything between the proxy (client.dll exports) and the renderer.
#pragma once
struct cl_enginefuncs_s; struct usercmd_s; struct ref_params_s; struct client_data_s; struct cl_entity_s;
namespace nxc {
void on_initialize(cl_enginefuncs_s *eng);
void on_hud_init();
void on_vid_init();
void on_redraw(float time, int intermission);
void on_frame(double time);
int  on_key(int down, int keynum, const char *binding);              // 0 = swallow the key (do not pass it to the game)
void createmove(void (*orig)(float, usercmd_s *, int), float frametime, usercmd_s *cmd, int active);
void post_calcrefdef(ref_params_s *params);
bool third_person();
void on_shutdown();
int  on_message(const char *name, int size, void *buf);
int  update_client_data(int orig_result, client_data_s *cd);         // FOV override
int  add_entity(int orig_result, int type, cl_entity_s *ent);        // per-entity hide / render overrides (0 = do not draw)
void draw_world();                                                   // 3D shapes (HUD_DrawTransparentTriangles)              // the "Nexus" user message (public for the tests)
}
