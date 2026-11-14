// Nexus 2D renderer: plain OpenGL 1.1 immediate mode on the engine's own context, with every piece of GL state saved and restored.
#pragma once
#include <string>
namespace nxr {
bool available();                                   // GL entry points found and a context is current
void device_lost();                                 // video mode changed: textures must be uploaded again
bool begin(int w, int h);                           // returns false when GL cannot be used (caller falls back to engine drawing)
void end();
void rect(float x, float y, float w, float h, const float c[4]);
void poly(const float *xy, int n, const float c[4]);
float line_height(float px);
float text_width(const char *s, size_t n, float px);
void text(float x, float y, const char *s, size_t n, float px, const float c[4]);     // one line, UTF-8, no markup
typedef bool (*LoadFn)(const char *path, std::string &bytes);
int  image(const char *path, LoadFn load);          // -1 = cannot be loaded (cached)
bool image_size(int img, int *w, int *h);
void image_draw(int img, float x, float y, float w, float h, const float c[4], bool additive);
unsigned utf8_next(const char *s, size_t n, size_t &i);
// 3D, in world coordinates, inside the game's own world matrices (HUD_DrawTransparentTriangles)
bool begin3d();
void end3d();
void lines3d(const float *pts, int n, bool loop, const float c[4], float width, bool xray, bool additive);   // n points; strip or loop
void segs3d(const float *pts, int nseg, const float c[4], float width, bool xray, bool additive);          // nseg separate segments (2 points each)
void fan3d(const float *pts, int n, const float c[4], bool xray, bool additive);                           // filled convex polygon
}
