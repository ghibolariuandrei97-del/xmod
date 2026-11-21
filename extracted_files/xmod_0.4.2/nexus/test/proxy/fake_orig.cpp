// Stands in for the game's own client library in the proxy test: counts calls, has "F" like the real one.
#include <cstring>
#ifdef _WIN32
#define EXP __declspec(dllexport)
#else
#define EXP __attribute__((visibility("default")))
#endif
extern "C" {
int calls[43]; int last_key = 0; void *engine_seen = nullptr;
static int f_init(void *eng, int ver) { calls[0]++; engine_seen = eng; return ver == 7 ? 1 : 0; }
static void f_hudinit() { calls[1]++; }
static int f_vidinit() { calls[2]++; return 1; }
static int f_redraw(float, int) { calls[3]++; return 1; }
static void f_createmove(float, void *, int) { calls[14]++; }
static int f_third() { calls[15]++; return 0; }
static void f_calcref(void *) { calls[19]++; }
static void f_shutdown() { calls[26]++; }
static void f_frame(double) { calls[33]++; }
static int f_key(int, int key, const char *) { calls[34]++; last_key = key; return 1; }
static int f_reset() { calls[5]++; return 55; }
static int f_addent(int type, void *, const char *) { calls[20]++; return type + 1000; }
void *g_dst = nullptr;                                  // like g_pcldstAddrs in the HL SDK client
EXP void F(void *pv) {
	void **t = (void **)pv; g_dst = t[2]; if (!g_dst) *(volatile int *)0 = 1;   // the real client keeps this pointer; NULL = crash later in game
	memset(t, 0, 43 * sizeof(void *));
	t[0] = (void *)f_init; t[1] = (void *)f_hudinit; t[2] = (void *)f_vidinit; t[3] = (void *)f_redraw; t[5] = (void *)f_reset; t[14] = (void *)f_createmove; t[15] = (void *)f_third; t[19] = (void *)f_calcref;
	t[20] = (void *)f_addent; t[26] = (void *)f_shutdown; t[33] = (void *)f_frame; t[34] = (void *)f_key;
}
EXP void *CreateInterface(const char *name, int *ret) { if (ret) *ret = 0; return !strcmp(name, "VClientVGUI001") ? (void *)0x1234 : nullptr; }
EXP int *test_calls() { return calls; }
EXP void *test_dst() { return g_dst; }
EXP int HUD_Reset() { return f_reset(); }                    // the real client exports its functions by name as well
EXP int test_last_key() { return last_key; }
}
