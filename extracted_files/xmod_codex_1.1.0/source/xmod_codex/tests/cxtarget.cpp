// Test target for Codex: functions with known behaviour in every calling convention Codex supports.
#include <cmath>
#include <cstdio>
#include <cstring>
#define EXP extern "C" __attribute__((visibility("default"), noinline))
static volatile int g_add_calls = 0, g_counter = 0;
EXP int cx_add(int a, int b) { g_add_calls++; return a + b; }
EXP int cx_add_calls() { return g_add_calls; }
EXP __attribute__((stdcall)) int cx_std(int a, int b, int c) { return a * 100 + b * 10 + c; }
EXP __attribute__((fastcall)) int cx_fast(int a, int b, int c) { return a * 100 - b * 10 - c; }
EXP float cx_fmul(float a, float b) { return a * b; }
EXP double cx_dadd(double a, double b) { return a + b; }
EXP long long cx_i64(long long a) { return a * 3; }
EXP const char *cx_greet(const char *name) { static char b[128]; snprintf(b, sizeof b, "hello %s", name ? name : "(null)"); return b; }
EXP float cx_vlen(const float *v) { return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
EXP int cx_crash(int *p) { return *p + 1; }
EXP int cx_divz(int a, int b) { return a / b; }
EXP int cx_uses_string(int x) { const char *volatile s = "CODEX_MAGIC_STRING_123"; return (int)strlen(s) + x; }
EXP int cx_global_inc(int x) { g_counter += x; return g_counter; }                       // PIC: starts with a get_pc_thunk call
EXP int cx_ret5() { return 5; }
// native callers: the way the game calls functions (this is what triggers hooks)
EXP int cx_call_add(int a, int b) { int (*volatile f)(int, int) = cx_add; return f(a, b); }
EXP int cx_call_std_many(int n) { int (__attribute__((stdcall)) *volatile f)(int, int, int) = cx_std; int s = 0; for (int i = 0; i < n; ++i) s += f(1, 2, 3); return s; }
EXP int cx_call_fast(int a, int b, int c) { int (__attribute__((fastcall)) *volatile f)(int, int, int) = cx_fast; return f(a, b, c); }
EXP float cx_call_fmul(float a, float b) { float (*volatile f)(float, float) = cx_fmul; return f(a, b) + 0.0f; }
EXP double cx_call_dadd(double a, double b) { double (*volatile f)(double, double) = cx_dadd; return f(a, b); }
EXP long long cx_call_i64(long long a) { long long (*volatile f)(long long) = cx_i64; return f(a); }
EXP const char *cx_call_greet(const char *n) { const char *(*volatile f)(const char *) = cx_greet; return f(n); }
EXP int cx_call_global_inc(int x) { int (*volatile f)(int) = cx_global_inc; return f(x); }
EXP int cx_call_ret5() { int (*volatile f)() = cx_ret5; return f(); }
// a class with virtual methods (vtable hooks; 'this' is the first stack argument on Linux)
struct Animal {
	int legs = 4;
	virtual int speak(int x);
	virtual float weight(float k);
	virtual ~Animal() {}
};
__attribute__((noinline)) int Animal::speak(int x) { return legs * 10 + x; }
__attribute__((noinline)) float Animal::weight(float k) { return legs * k; }
EXP Animal *cx_new_animal() { return new Animal(); }
EXP int cx_animal_speak(Animal *a, int x) { return a->speak(x); }
EXP float cx_animal_weight(Animal *a, float k) { return a->weight(k); }
// a game-style hard-coded limit: money is clamped to 16000 on two paths, and 16000 is also an early-return check
EXP __attribute__((noinline)) int cx_add_money(int have, int add)
{
	volatile int bonus = 0;
	if (have >= 16000) return 16000;
	int m = have + add + bonus;
	if (m > 16000) m = 16000;
	if (m < 0) m = 0;
	return m;
}
EXP int cx_call_add_money(int h, int a) { int (*volatile f)(int, int) = cx_add_money; return f(h, a); }
// endless recursion: a stack overflow inside a guarded call
EXP __attribute__((noinline)) int cx_recurse(int n)
{
	volatile char pad[256]; pad[0] = (char)n; pad[255] = (char)n;
	return cx_recurse(n + 1) + pad[0] + pad[255];
}
// a function that KEEPS the string pointer it gets (like CREATE_NAMED_ENTITY(MAKE_STRING(name)) does)
static const char *g_kept = nullptr;
EXP __attribute__((noinline)) void cx_keep(const char *s) { g_kept = s; }
EXP __attribute__((noinline)) const char *cx_kept() { return g_kept; }
EXP __attribute__((noinline)) int cx_ident(int x) { return x; }
// call graph (codex.callees / callers / calltree / treemap): direct calls between exported functions go through the PLT
EXP int cx_leaf_a(int x) { return x * 7 + 3; }
EXP int cx_leaf_b(int x) { return x ^ 0x5A5A; }
EXP int cx_tree_root(int x) { const char *volatile s = "CODEX_TREE_STR"; int r = cx_leaf_a(x); if (x > 3) r += cx_leaf_b(x); r += cx_leaf_a(r & 0xFF); return r + (int)strlen(s) * 0x1234; }
EXP int cx_tree_other(int x) { return cx_leaf_b(x) + 1; }
EXP volatile int cx_magic_value = 0x13572468;
EXP int cx_read_magic() { return cx_magic_value; }
