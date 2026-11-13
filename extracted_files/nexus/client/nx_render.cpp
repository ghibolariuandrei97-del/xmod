#include "nx_render.h"
#include "nx_platform.h"
#include "font_data.h"
#include <cstring>
#include <unordered_map>
#include <vector>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_ONLY_PNG
#define STBI_ONLY_TGA
#define STBI_ONLY_BMP
#define STBI_ONLY_JPEG
#include "third_party/stb_image.h"

#ifdef _WIN32
#define GLA __stdcall
#else
#define GLA
#endif
namespace nxr {
static struct {
	void (GLA *Begin)(unsigned); void (GLA *End)(void); void (GLA *Vertex2f)(float, float); void (GLA *TexCoord2f)(float, float); void (GLA *Color4f)(float, float, float, float);
	void (GLA *Enable)(unsigned); void (GLA *Disable)(unsigned); void (GLA *BlendFunc)(unsigned, unsigned); void (GLA *BindTexture)(unsigned, unsigned);
	void (GLA *TexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *); void (GLA *TexParameteri)(unsigned, unsigned, int); void (GLA *PixelStorei)(unsigned, int);
	void (GLA *MatrixMode)(unsigned); void (GLA *PushMatrix)(void); void (GLA *PopMatrix)(void); void (GLA *LoadIdentity)(void); void (GLA *Ortho)(double, double, double, double, double, double);
	void (GLA *PushAttrib)(unsigned); void (GLA *PopAttrib)(void); void (GLA *PushClientAttrib)(unsigned); void (GLA *PopClientAttrib)(void); void (GLA *TexEnvi)(unsigned, unsigned, int);
	void (GLA *GetIntegerv)(unsigned, int *); void (GLA *UseProgram)(unsigned); void (GLA *ActiveTexture)(unsigned);
} gl;
static bool g_loaded = false, g_load_tried = false, g_ext_tried = false, g_font_up = false; static int g_prev_prog = 0;
static const unsigned TEX_BASE = 0x3E4E5800u;       // GoldSrc numbers its textures itself (1, 2, 3 ...), so names from glGenTextures could collide: use a private range
static std::vector<unsigned char> g_font_px;
static std::unordered_map<unsigned, const NxGlyph *> g_glyphs;
struct Image { std::string path; int w = 0, h = 0, tw = 0, th = 0; std::vector<unsigned char> px; bool ok = false, up = false; };
static std::vector<Image> g_img;

static bool load() {
	if (g_load_tried) return g_loaded;
	g_load_tried = true;
#define L(f) if (!(*(void **)&gl.f = nxp::gl_sym("gl" #f))) return false;
	L(Begin) L(End) L(Vertex2f) L(TexCoord2f) L(Color4f) L(Enable) L(Disable) L(BlendFunc) L(BindTexture) L(TexImage2D) L(TexParameteri) L(PixelStorei) L(MatrixMode) L(PushMatrix) L(PopMatrix)
	L(LoadIdentity) L(Ortho) L(PushAttrib) L(PopAttrib) L(PushClientAttrib) L(PopClientAttrib) L(TexEnvi) L(GetIntegerv)
#undef L
	for (const NxGlyph &g : nxf_glyphs) g_glyphs[g.cp] = &g;
	g_font_px.resize((size_t)NXF_W * NXF_H); size_t o = 0;
	for (unsigned i = 0; i + 1 < nxf_rle_len && o < g_font_px.size(); i += 2) { size_t n = nxf_rle[i]; if (o + n > g_font_px.size()) n = g_font_px.size() - o; memset(&g_font_px[o], nxf_rle[i + 1], n); o += n; }
	return g_loaded = true;
}
bool available() { return load() && nxp::gl_context(); }
void device_lost() { g_font_up = false; for (auto &i : g_img) i.up = false; }

static void upload(unsigned tex, int w, int h, unsigned fmt, const unsigned char *px, bool mips) {
	int comps = fmt == 0x1906 ? 1 : 4;                                      // GL_ALPHA : GL_RGBA
	gl.BindTexture(0x0DE1, tex);
	gl.TexParameteri(0x0DE1, 0x2801, mips ? 0x2703 : 0x2601); gl.TexParameteri(0x0DE1, 0x2800, 0x2601);    // min: LINEAR_MIPMAP_LINEAR / LINEAR, mag: LINEAR
	gl.TexParameteri(0x0DE1, 0x2802, 0x2900); gl.TexParameteri(0x0DE1, 0x2803, 0x2900);                    // GL_CLAMP
	gl.TexImage2D(0x0DE1, 0, comps == 1 ? 0x1906 : 0x1908, w, h, 0, fmt, 0x1401, px);
	if (!mips) return;
	std::vector<unsigned char> cur(px, px + (size_t)w * h * comps), nxt; int lvl = 0;
	while (w > 1 || h > 1) {                                                // box filter, one level at a time
		int nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1; nxt.assign((size_t)nw * nh * comps, 0);
		for (int y = 0; y < nh; y++) for (int x = 0; x < nw; x++) for (int c = 0; c < comps; c++) {
			int x0 = x * 2 < w ? x * 2 : w - 1, x1 = x * 2 + 1 < w ? x * 2 + 1 : w - 1, y0 = y * 2 < h ? y * 2 : h - 1, y1 = y * 2 + 1 < h ? y * 2 + 1 : h - 1;
			nxt[((size_t)y * nw + x) * comps + c] = (unsigned char)((cur[((size_t)y0 * w + x0) * comps + c] + cur[((size_t)y0 * w + x1) * comps + c] + cur[((size_t)y1 * w + x0) * comps + c] + cur[((size_t)y1 * w + x1) * comps + c] + 2) / 4);
		}
		w = nw; h = nh; cur.swap(nxt); gl.TexImage2D(0x0DE1, ++lvl, comps == 1 ? 0x1906 : 0x1908, w, h, 0, fmt, 0x1401, cur.data());
	}
}
bool begin(int w, int h) {
	if (!available() || w < 1 || h < 1) return false;
	if (!g_ext_tried) { g_ext_tried = true; *(void **)&gl.UseProgram = nxp::gl_ext_sym("glUseProgram"); *(void **)&gl.ActiveTexture = nxp::gl_ext_sym("glActiveTexture"); if (!gl.ActiveTexture) *(void **)&gl.ActiveTexture = nxp::gl_ext_sym("glActiveTextureARB"); }
	gl.PushAttrib(0x000FFFFF); gl.PushClientAttrib(0x00000001);              // everything the fixed pipeline has + pixel store
	g_prev_prog = 0; if (gl.UseProgram) { gl.GetIntegerv(0x8B8D, &g_prev_prog); if (g_prev_prog) gl.UseProgram(0); }
	if (gl.ActiveTexture) gl.ActiveTexture(0x84C0);
	gl.MatrixMode(0x1701); gl.PushMatrix(); gl.LoadIdentity(); gl.Ortho(0, w, h, 0, -1, 1);
	gl.MatrixMode(0x1700); gl.PushMatrix(); gl.LoadIdentity();
	for (unsigned cap : {0x0B71u, 0x0B44u, 0x0BC0u, 0x0B50u, 0x0C11u, 0x0B60u, 0x0B90u, 0x0DE1u}) gl.Disable(cap);   // depth, cull, alpha test, lighting, scissor, fog, stencil, texture
	gl.Enable(0x0BE2); gl.BlendFunc(0x0302, 0x0303); gl.TexEnvi(0x2300, 0x2200, 0x2100);
	gl.PixelStorei(0x0CF5, 1); gl.PixelStorei(0x0CF2, 0); gl.PixelStorei(0x0CF3, 0); gl.PixelStorei(0x0CF4, 0);
	if (!g_font_up) { upload(TEX_BASE, NXF_W, NXF_H, 0x1906, g_font_px.data(), true); g_font_up = true; }
	return true;
}
void end() {
	gl.MatrixMode(0x1701); gl.PopMatrix(); gl.MatrixMode(0x1700); gl.PopMatrix();
	gl.PopClientAttrib(); gl.PopAttrib();
	if (g_prev_prog && gl.UseProgram) gl.UseProgram((unsigned)g_prev_prog);
}
void rect(float x, float y, float w, float h, const float c[4]) {
	if (w <= 0 || h <= 0 || c[3] <= 0) return;
	gl.Disable(0x0DE1); gl.Color4f(c[0], c[1], c[2], c[3]);
	gl.Begin(7); gl.Vertex2f(x, y); gl.Vertex2f(x + w, y); gl.Vertex2f(x + w, y + h); gl.Vertex2f(x, y + h); gl.End();
}
void poly(const float *xy, int n, const float c[4]) {
	gl.Disable(0x0DE1); gl.Color4f(c[0], c[1], c[2], c[3]);
	gl.Begin(6); for (int i = 0; i < n; i++) gl.Vertex2f(xy[i * 2], xy[i * 2 + 1]); gl.End();          // triangle fan
}
unsigned utf8_next(const char *s, size_t n, size_t &i) {
	unsigned c = (unsigned char)s[i++]; int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
	if (!more) return c < 0x80 ? c : '?';
	c &= (0x3F >> more);
	while (more-- && i < n && ((unsigned char)s[i] & 0xC0) == 0x80) c = (c << 6) | ((unsigned char)s[i++] & 0x3F);
	return c;
}
static const NxGlyph *glyph(unsigned cp) { auto it = g_glyphs.find(cp); if (it != g_glyphs.end()) return it->second; it = g_glyphs.find('?'); return it != g_glyphs.end() ? it->second : nullptr; }
float line_height(float px) { return px * NXF_LINE / (float)NXF_SIZE; }
float text_width(const char *s, size_t n, float px) {
	load(); float k = px / (float)NXF_SIZE, w = 0;
	for (size_t i = 0; i < n;) { const NxGlyph *g = glyph(utf8_next(s, n, i)); if (g) w += g->adv * k; }
	return w;
}
void text(float x, float y, const char *s, size_t n, float px, const float c[4]) {
	if (c[3] <= 0 || !n) return;
	float k = px / (float)NXF_SIZE;
	gl.Enable(0x0DE1); gl.BindTexture(0x0DE1, TEX_BASE); gl.Color4f(c[0], c[1], c[2], c[3]);
	gl.Begin(7);
	for (size_t i = 0; i < n;) {
		const NxGlyph *g = glyph(utf8_next(s, n, i)); if (!g) continue;
		if (g->w && g->h) {
			float x0 = x + g->xo * k, y0 = y + g->yo * k, x1 = x0 + g->w * k, y1 = y0 + g->h * k;
			float u0 = g->x / (float)NXF_W, v0 = g->y / (float)NXF_H, u1 = (g->x + g->w) / (float)NXF_W, v1 = (g->y + g->h) / (float)NXF_H;
			gl.TexCoord2f(u0, v0); gl.Vertex2f(x0, y0); gl.TexCoord2f(u1, v0); gl.Vertex2f(x1, y0); gl.TexCoord2f(u1, v1); gl.Vertex2f(x1, y1); gl.TexCoord2f(u0, v1); gl.Vertex2f(x0, y1);
		}
		x += g->adv * k;
	}
	gl.End();
}
int image(const char *path, LoadFn load_fn) {
	for (size_t i = 0; i < g_img.size(); i++) if (g_img[i].path == path) return g_img[i].ok ? (int)i : -1;
	if (g_img.size() >= 256) return -1;
	Image im; im.path = path; std::string bytes;
	if (load_fn && load_fn(path, bytes) && !bytes.empty()) {
		int w = 0, h = 0, comp = 0; unsigned char *px = stbi_load_from_memory((const unsigned char *)bytes.data(), (int)bytes.size(), &w, &h, &comp, 4);
		if (px && w > 0 && h > 0 && w <= 2048 && h <= 2048) {
			im.w = w; im.h = h; im.tw = 1; im.th = 1; while (im.tw < w) im.tw *= 2; while (im.th < h) im.th *= 2;      // power-of-two canvas: works on every GL
			im.px.assign((size_t)im.tw * im.th * 4, 0);
			for (int y = 0; y < h; y++) memcpy(&im.px[(size_t)y * im.tw * 4], px + (size_t)y * w * 4, (size_t)w * 4);
			im.ok = true;
		}
		if (px) stbi_image_free(px);
	}
	g_img.push_back(std::move(im)); return g_img.back().ok ? (int)g_img.size() - 1 : -1;
}
bool image_size(int img, int *w, int *h) { if (img < 0 || img >= (int)g_img.size() || !g_img[img].ok) return false; *w = g_img[img].w; *h = g_img[img].h; return true; }
void image_draw(int img, float x, float y, float w, float h, const float c[4], bool additive) {
	if (img < 0 || img >= (int)g_img.size() || !g_img[img].ok || c[3] <= 0) return;
	Image &im = g_img[img]; unsigned tex = TEX_BASE + 16 + (unsigned)img;
	if (!im.up) { upload(tex, im.tw, im.th, 0x1908, im.px.data(), false); im.up = true; }
	gl.Enable(0x0DE1); gl.BindTexture(0x0DE1, tex); gl.Color4f(c[0], c[1], c[2], c[3]);
	if (additive) gl.BlendFunc(0x0302, 1);
	float u = im.w / (float)im.tw, v = im.h / (float)im.th;
	gl.Begin(7); gl.TexCoord2f(0, 0); gl.Vertex2f(x, y); gl.TexCoord2f(u, 0); gl.Vertex2f(x + w, y); gl.TexCoord2f(u, v); gl.Vertex2f(x + w, y + h); gl.TexCoord2f(0, v); gl.Vertex2f(x, y + h); gl.End();
	if (additive) gl.BlendFunc(0x0302, 0x0303);
}
}
