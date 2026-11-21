// Windows test for Codex (runs under Wine): SEH fault guard, MSVC-style thiscall/stdcall/fastcall calls and hooks,
// and the mass trampoline/signature test on real PE binaries (mp.dll, swds.dll).
#include "codex.h"
#include "ext_value.h"
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <set>

static XmodApi api;
static std::map<std::string, std::function<void(const XmodValue *, XmodHookOut *)>> H;
static int pass = 0, failc = 0;
#define OK(c, what) do { if (c) ++pass; else { ++failc; printf("FAIL: %s  (%s)\n", what, cx_last_error().c_str()); } } while (0)

// ------------------------------------------------------------------ targets (MinGW, real Windows conventions)
static volatile int g_calls = 0, g_glob = 0;
extern "C" __attribute__((noinline)) int t_add(int a, int b) { g_calls++; return a + b; }
extern "C" __attribute__((noinline, stdcall)) int t_std(int a, int b, int c) { return a * 100 + b * 10 + c; }
extern "C" __attribute__((noinline, fastcall)) int t_fast(int a, int b, int c) { return a * 100 - b * 10 - c; }
struct Obj { int v = 4; virtual int vm(int x); virtual float vf(float k); };
__attribute__((noinline)) int Obj::vm(int x) { return v * 10 + x; }            // member functions: thiscall on MinGW
__attribute__((noinline)) float Obj::vf(float k) { return v * k; }
extern "C" __attribute__((noinline, thiscall)) int t_this(Obj *self, int x, int y) { return self->v * 100 + x * 10 + y; }
extern "C" __attribute__((noinline)) float t_fmul(float a, float b) { return a * b; }
extern "C" __attribute__((noinline)) double t_dadd(double a, double b) { return a + b; }
extern "C" __attribute__((noinline)) long long t_i64(long long a) { return a * 3; }
extern "C" __attribute__((noinline)) const char *t_greet(const char *n) { static char b[64]; snprintf(b, 64, "hello %s", n); return b; }
extern "C" __attribute__((noinline)) int t_crash(int *p) { return *p + 1; }
extern "C" __attribute__((noinline)) int t_divz(int a, int b) { return a / b; }
extern "C" __attribute__((noinline)) int t_deep(int n, int *p) { if (n > 0) return t_deep(n - 1, p) + 1; return *p; }   // crash deep in a call chain
// native callers (how the game calls things)
extern "C" __attribute__((noinline)) int c_add(int a, int b) { int (*volatile f)(int, int) = t_add; return f(a, b); }
extern "C" __attribute__((noinline)) int c_std_many(int n) { int (__attribute__((stdcall)) *volatile f)(int, int, int) = t_std; int s = 0; for (int i = 0; i < n; ++i) s += f(1, 2, 3); return s; }
extern "C" __attribute__((noinline)) int c_fast(int a, int b, int c) { int (__attribute__((fastcall)) *volatile f)(int, int, int) = t_fast; return f(a, b, c); }
extern "C" __attribute__((noinline)) int c_this_many(Obj *o, int n) { int (__attribute__((thiscall)) *volatile f)(Obj *, int, int) = t_this; int s = 0; for (int i = 0; i < n; ++i) s += f(o, 1, 2); return s; }
extern "C" __attribute__((noinline)) int c_vm(Obj *o, int x) { return o->vm(x); }
extern "C" __attribute__((noinline)) float c_vf(Obj *o, float k) { return o->vf(k); }
extern "C" __attribute__((noinline)) float c_fmul(float a, float b) { float (*volatile f)(float, float) = t_fmul; return f(a, b); }
extern "C" __attribute__((noinline)) const char *c_greet(const char *n) { const char *(*volatile f)(const char *) = t_greet; return f(n); }

// ------------------------------------------------------------------ fake xmod
static int f_has(const char *e) { return H.count(e) ? 1 : 0; }
static int f_fire(const char *e, const XmodValue *a, XmodHookOut *o) { memset(o, 0, sizeof *o); auto it = H.find(e); if (it == H.end()) return 0; it->second(a, o); return 1; }
static int f_main() { return 1; }
static void f_log(const char *m) { printf("  log: %s\n", m); }
static void f_post(xmod_main_fn fn, void *u) { fn(u); }
static void f_emit(const char *, XmodValue *a) { delete a; }

static XmodValue *M(std::initializer_list<std::pair<const char *, XmodValue *>> kv) { XmodValue *m = xv::map(); for (auto &p : kv) xv::set(m, p.first, p.second); return m; }
static XmodValue *A(std::initializer_list<const char *> l) { XmodValue *a = xv::array(); for (auto s : l) xv::push(a, xv::cstr(s)); return a; }
static bool define(const char *name, void *addr, const char *conv, std::initializer_list<const char *> args, const char *ret, const char *thist = nullptr)
{
	XmodValue *d = M({{"name", xv::cstr(name)}, {"module", xv::cstr("wintest.exe")}, {"address", xv::integer((int64_t)(uintptr_t)addr)},
	                  {"conv", xv::cstr(conv)}, {"args", A(args)}, {"ret", xv::cstr(ret)}});
	if (thist) xv::set(d, "this", xv::cstr(thist));
	std::string err; bool ok = func_define(d, err); delete d;
	if (!ok) printf("define %s: %s\n", name, err.c_str());
	return ok;
}
static XmodValue *call(const char *name, std::vector<XmodValue *> args, bool hooked = false, std::string *errout = nullptr)
{
	Func *f = func_get(name); XmodValue *r = nullptr; std::string err;
	std::vector<const XmodValue *> av(args.begin(), args.end());
	bool ok = func_call(*f, av.data(), (int)av.size(), hooked, &r, err);
	for (auto *a : args) delete a;
	if (errout) *errout = err;
	return ok ? r : nullptr;
}
static int64_t I(XmodValue *v) { int64_t r = v ? xv::get_int(v, -999999) : -999999; delete v; return r; }
static double D(XmodValue *v) { double r = v ? xv::get_num(v, -1) : -1; delete v; return r; }

// ------------------------------------------------------------------ mass test on a PE file
static void mass(const char *path)
{
	HMODULE h = LoadLibraryExA(path, nullptr, DONT_RESOLVE_DLL_REFERENCES);
	if (!h) { printf("cannot map %s (%lu)\n", path, GetLastError()); return; }
	mod_refresh();
	std::string bn = path; bn = bn.substr(bn.find_last_of("/\\") + 1);
	Module *m = mod_find(bn.c_str());
	std::set<uintptr_t> starts;
	for (auto &s : m->segs) {
		if (!s.x) continue;
		for (uintptr_t p = s.lo; p + 16 < s.hi;) {
			Insn in;
			if (!lde_decode((const uint8_t *)p, in)) { ++p; continue; }
			if (in.is_call && in.rel_size == 4) { uintptr_t t = in.target(p); if (t >= s.lo && t < s.hi) starts.insert(t); }
			p += in.len;
		}
	}
	int ok = 0, refused = 0, wrong = 0, restored = 0, sig_ok = 0, sig_tries = 0, k = 0;
	std::map<std::string, int> why;
	for (uintptr_t fn : starts) {
		uint8_t *t = nullptr; int stolen = 0; std::string err;
		if (!build_trampoline(fn, t, stolen, err)) { ++refused; why[err]++; continue; }
		// verify: copied bytes / relocated branches
		int oo = 0; uint8_t *tp = t; bool good = true;
		while (oo < stolen) {
			Insn a, b; lde_decode((const uint8_t *)fn + oo, a); lde_decode(tp, b);
			if (a.rel_size) { if (!b.rel_size || b.target((uintptr_t)tp) != a.target(fn + oo)) { if (!(a.is_call && !b.rel_size)) good = false; } }
			else if (a.len != b.len || memcmp((void *)(fn + oo), tp, a.len)) good = false;
			oo += a.len; tp += b.len;
		}
		if (!good) { ++wrong; continue; }
		++ok;
		std::vector<uint8_t> before(stolen), bytes(stolen, 0x90);
		memcpy(before.data(), (void *)fn, stolen);
		bytes[0] = 0xE9; int32_t r = (int32_t)((uintptr_t)t - (fn + 5)); memcpy(&bytes[1], &r, 4);
		int id = patch_apply(fn, bytes, &before, "mass");
		if (id && patch_remove(id) && !memcmp(before.data(), (void *)fn, stolen)) ++restored;
		if ((++k % 9) == 0 && sig_tries < 400) {
			++sig_tries; SigMade sm; std::string e2, st; int adj;
			if (sig_make(fn, *m, sm, e2, 128)) { sig_split(sm.text, st, adj); Sig sg; sig_parse(st, sg, e2); std::vector<uintptr_t> hits; if (sig_scan(*m, sg, hits, 4) == 1 && hits[0] + adj == fn) ++sig_ok; }
		}
	}
	printf("%s: %zu functions (call targets) | trampolines OK %d, refused %d, WRONG %d | detour install+restore %d/%d | unique signatures %d/%d\n",
	       bn.c_str(), starts.size(), ok, refused, wrong, restored, ok, sig_ok, sig_tries);
	for (auto &w : why) printf("    refused (%d): %s\n", w.second, w.first.c_str());
}

int main(int argc, char **argv)
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	memset(&api, 0, sizeof api);
	api.abi_version = XMOD_EXT_ABI; api.size = sizeof api;
	xmod_fill_value_api(&api);
	api.has_handlers = f_has; api.fire_hook_sync = f_fire; api.is_main_thread = f_main; api.log = f_log; api.log_error = f_log;
	api.post_main = f_post; api.emit_event = f_emit;
	g_xmod = &api; g_xext = (XmodExt *)1; g_sync_hooks = true;
	guard_install();
	mod_refresh();

	OK(define("add", (void *)t_add, "cdecl", {"int", "int"}, "int"), "define add");
	OK(define("std", (void *)t_std, "stdcall", {"int", "int", "int"}, "int"), "define stdcall");
	OK(define("fast", (void *)t_fast, "fastcall", {"int", "int", "int"}, "int"), "define fastcall");
	OK(define("this3", (void *)t_this, "thiscall", {"int", "int"}, "int", "ptr"), "define thiscall");
	OK(define("fmul", (void *)t_fmul, "cdecl", {"float", "float"}, "float"), "define float");
	OK(define("dadd", (void *)t_dadd, "cdecl", {"double", "double"}, "double"), "define double");
	OK(define("i64", (void *)t_i64, "cdecl", {"i64"}, "i64"), "define i64");
	OK(define("greet", (void *)t_greet, "cdecl", {"string"}, "string"), "define string");
	OK(define("crash", (void *)t_crash, "cdecl", {"ptr"}, "int"), "define crash");
	OK(define("divz", (void *)t_divz, "cdecl", {"int", "int"}, "int"), "define divz");
	OK(define("deep", (void *)t_deep, "cdecl", {"int", "ptr"}, "int"), "define deep");
	for (auto &x : std::vector<std::pair<const char *, void *>>{{"c_add", (void *)c_add}, {"c_std_many", (void *)c_std_many}, {"c_fast", (void *)c_fast},
	                                                             {"c_this_many", (void *)c_this_many}, {"c_vm", (void *)c_vm}, {"c_vf", (void *)c_vf}, {"c_fmul", (void *)c_fmul}, {"c_greet", (void *)c_greet}}) {
		const char *n = x.first;
		if (!strcmp(n, "c_add")) define(n, x.second, "cdecl", {"int", "int"}, "int");
		else if (!strcmp(n, "c_std_many")) define(n, x.second, "cdecl", {"int"}, "int");
		else if (!strcmp(n, "c_fast")) define(n, x.second, "cdecl", {"int", "int", "int"}, "int");
		else if (!strcmp(n, "c_this_many")) define(n, x.second, "cdecl", {"ptr", "int"}, "int");
		else if (!strcmp(n, "c_vm")) define(n, x.second, "cdecl", {"ptr", "int"}, "int");
		else if (!strcmp(n, "c_vf")) define(n, x.second, "cdecl", {"ptr", "float"}, "float");
		else if (!strcmp(n, "c_fmul")) define(n, x.second, "cdecl", {"float", "float"}, "float");
		else if (!strcmp(n, "c_greet")) define(n, x.second, "cdecl", {"string"}, "string");
	}
	XmodValue *vd = M({{"name", xv::cstr("vm")}, {"conv", xv::cstr("thiscall")}, {"this", xv::cstr("ptr")}, {"vindex", xv::integer(0)}, {"args", A({"int"})}, {"ret", xv::cstr("int")}});
	std::string e; OK(func_define(vd, e), "define virtual"); delete vd;
	vd = M({{"name", xv::cstr("vf")}, {"conv", xv::cstr("thiscall")}, {"this", xv::cstr("ptr")}, {"vindex", xv::integer(1)}, {"args", A({"float"})}, {"ret", xv::cstr("float")}});
	OK(func_define(vd, e), "define virtual float"); delete vd;

	Obj *obj = new Obj();
	auto P = [&](void *p) { return xv::integer((int64_t)(uintptr_t)p); };
	// plain calls
	OK(I(call("add", {xv::integer(2), xv::integer(3)})) == 5, "cdecl call");
	OK(I(call("std", {xv::integer(1), xv::integer(2), xv::integer(3)})) == 123, "stdcall call");
	OK(I(call("fast", {xv::integer(9), xv::integer(3), xv::integer(2)})) == 868, "fastcall call");
	OK(I(call("this3", {P(obj), xv::integer(1), xv::integer(2)})) == 412, "MSVC thiscall (this in ecx, callee pops)");
	OK(fabs(D(call("fmul", {xv::number(1.5), xv::number(4)})) - 6) < 1e-6, "float return");
	OK(fabs(D(call("dadd", {xv::number(1.25), xv::number(2.5)})) - 3.75) < 1e-9, "double");
	OK(I(call("i64", {xv::integer(4000000000LL)})) == 12000000000LL, "int64");
	{ XmodValue *r = call("greet", {xv::cstr("andrew")}); OK(r && std::string(xv::get_str(r, nullptr)) == "hello andrew", "string"); delete r; }
	OK(I(call("vm", {P(obj), xv::integer(2)})) == 42, "virtual call");
	OK(fabs(D(call("vf", {P(obj), xv::number(2.5)})) - 10) < 1e-6, "virtual float");

	// SEH fault guard
	std::string err;
	OK(!call("crash", {xv::integer(0)}, false, &err) && err.find("access violation") != std::string::npos, "NULL deref caught by SEH");
	printf("  -> %s\n", err.c_str());
	func_get("crash")->faulted = false;
	OK(!call("divz", {xv::integer(1), xv::integer(0)}, false, &err) && err.find("divide") != std::string::npos, "divide by zero caught");
	printf("  -> %s\n", err.c_str());
	OK(!call("deep", {xv::integer(500), xv::integer(0)}, false, &err), "crash 500 frames deep caught");
	OK(I(call("add", {xv::integer(20), xv::integer(22)})) == 42, "fine after three caught crashes");
	OK(fabs(D(call("fmul", {xv::number(2), xv::number(3)})) - 6) < 1e-6, "x87 state intact after crashes");
	{   // a real exception the called code handles itself must NOT be eaten by the guard (it runs first)
		int x = 0; OK(mem_read((uintptr_t)&x, &x, 4) && !mem_read(16, &x, 4) && !mem_read(0x7FFF0000, &x, 4), "safe reads");
	}

	// hooks
	Func *fadd = func_get("add");
	OK(hook_add(*fadd, nullptr, e), "hook add (detour)");
	H["codex:add"] = [](const XmodValue *a, XmodHookOut *o) { o->set_mask = 1; o->set_val[0] = xv::get_int(xv::at(a, 0), 0) * 10; };
	OK(I(call("c_add", {xv::integer(2), xv::integer(3)})) == 23, "pre-hook set_arg through a native caller");
	OK(I(call("add", {xv::integer(2), xv::integer(3)})) == 5, "codex.call bypasses the hook");
	H["codex:add:post"] = [](const XmodValue *a, XmodHookOut *o) { if (xv::get_int(xv::at(a, 2), 0) == 23) { o->code = 1; o->has_num = 1; o->num = 99; } };
	OK(I(call("c_add", {xv::integer(2), xv::integer(3)})) == 99, "post-hook overrides the return");
	int before = g_calls;
	H["codex:add"] = [](const XmodValue *, XmodHookOut *o) { o->code = 1; o->has_num = 1; o->num = 7; };
	H.erase("codex:add:post");
	OK(I(call("c_add", {xv::integer(1), xv::integer(1)})) == 7 && g_calls == before, "supercede skips the original");

	OK(hook_add(*func_get("std"), nullptr, e), "hook stdcall");
	H["codex:std"] = [](const XmodValue *, XmodHookOut *o) { o->set_mask = 4; o->set_val[2] = 9; };
	OK(I(call("c_std_many", {xv::integer(1000)})) == 129000, "stdcall hook x1000, callee-pop stack balanced");
	OK(hook_add(*func_get("fast"), nullptr, e), "hook fastcall");
	H["codex:fast"] = [](const XmodValue *a, XmodHookOut *o) { OK(xv::get_int(xv::at(a, 0), 0) == 9 && xv::get_int(xv::at(a, 1), 0) == 3, "fastcall args seen"); o->set_mask = 2; o->set_val[1] = 0; };
	OK(I(call("c_fast", {xv::integer(9), xv::integer(3), xv::integer(2)})) == 898, "fastcall hook (ecx/edx)");
	OK(hook_add(*func_get("this3"), nullptr, e), "hook MSVC thiscall");
	H["codex:this3"] = [&](const XmodValue *a, XmodHookOut *o) { if (xv::get_int(xv::at(a, 0), 0) != (int64_t)(uintptr_t)obj) printf("FAIL this\n"); o->set_mask = 1; o->set_val[0] = 5; };
	OK(I(call("c_this_many", {P(obj), xv::integer(1000)})) == 1000 * 452, "thiscall hook x1000: this in ecx, callee pops, stack balanced");
	OK(hook_add(*func_get("fmul"), nullptr, e), "hook float");
	H["codex:fmul:post"] = [](const XmodValue *a, XmodHookOut *o) { o->code = 1; o->has_num = 1; o->num = xv::get_num(xv::at(a, 2), 0) + 0.5; };
	OK(fabs(D(call("c_fmul", {xv::number(2), xv::number(3)})) - 6.5) < 1e-6, "float return through st(0) after a post-hook");
	OK(hook_add(*func_get("greet"), nullptr, e), "hook string");
	H["codex:greet"] = [](const XmodValue *, XmodHookOut *) { std::string er; XmodValue *v = xv::cstr("world"); frame_set_arg(frame_top(), 1, v, er); delete v; };
	{ XmodValue *r = call("c_greet", {xv::cstr("x")}); OK(r && std::string(xv::get_str(r, nullptr)) == "hello world", "string arg replaced in pre-hook"); delete r; }
	XmodValue *ho = M({{"object", P(obj)}});
	OK(hook_add(*func_get("vm"), ho, e), "vtable hook (thiscall virtual)");
	OK(hook_add(*func_get("vf"), ho, e), "vtable hook (virtual float)");
	delete ho;
	H["codex:vm"] = [](const XmodValue *a, XmodHookOut *o) { o->set_mask = 1; o->set_val[0] = xv::get_int(xv::at(a, 1), 0) + 100; };
	H["codex:vf:post"] = [](const XmodValue *a, XmodHookOut *o) { o->code = 1; o->has_num = 1; o->num = xv::get_num(xv::at(a, 2), 0) * 2; };
	OK(I(call("c_vm", {P(obj), xv::integer(1)})) == 141, "virtual call through the vtable hook");
	OK(fabs(D(call("c_vf", {P(obj), xv::number(1)})) - 8) < 1e-6, "virtual float through the vtable hook");
	OK(I(call("vm", {P(obj), xv::integer(1)})) == 41, "codex.call bypasses the vtable hook");

	// signatures on this exe (PE relocations decide the wildcards)
	Module *me = mod_of((uintptr_t)&t_add);
	SigMade sm; std::string se;
	OK(sig_make((uintptr_t)&t_dadd, *me, sm, se, 128), "sig_make on PE");
	printf("  t_dadd sig: %s\n", sm.text.c_str());

	// unhook everything and check behaviour is original again
	hook_remove_all();
	OK(I(call("c_add", {xv::integer(2), xv::integer(3)})) == 5 && I(call("c_this_many", {P(obj), xv::integer(10)})) == 4120, "all hooks removed");
	printf("WINTEST: %d passed, %d failed\n", pass, failc);

	for (int i = 1; i < argc; ++i) mass(argv[i]);
	return failc != 0;
}
