// api.cpp - the Lua functions xmod.codex.* (all synchronous, main thread).
// Conventions: lookups return nil on failure and codex.last_error() tells why; misuse (bad arguments) raises a Lua error.
#include "codex.h"
#include <cstdarg>
#include <cstdio>
#include <set>
#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cmath>

#define X g_xmod
struct NameOfs { const char *name; size_t ofs; };
#include "fnames.inc"

// ---------------------------------------------------------------------------------------------------------------
static std::string s_last_error;
void cx_set_error(const char *fmt, ...)
{
	char b[1024];
	va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
	s_last_error = b;
}
const std::string &cx_last_error() { return s_last_error; }

static inline const XmodValue *arg(XmodCall *c, int i) { return X->call_arg(c, i); }
static inline int argc(XmodCall *c) { return X->call_argc(c); }
static inline void ret(XmodCall *c, XmodValue *v) { X->call_return(c, v); }
static inline void ret_nil(XmodCall *c) { ret(c, X->val_nil()); }
static inline void ret_int(XmodCall *c, int64_t v) { ret(c, X->val_int(v)); }
static inline void ret_bool(XmodCall *c, bool b) { ret(c, X->val_bool(b)); }
static void fail(XmodCall *c, const std::string &why) { s_last_error = why; ret_nil(c); }
static std::string sarg(XmodCall *c, int i, const char *d = "")
{
	size_t n = 0; const char *s = X->val_get_str(arg(c, i), &n);
	return s ? std::string(s, n) : std::string(d);
}
static bool iarg(XmodCall *c, int i, int64_t &out)
{
	const XmodValue *v = arg(c, i);
	int t = X->val_type(v);
	if (t == XV_INT) { out = X->val_get_int(v, 0); return true; }
	if (t == XV_NUM) { out = (int64_t)X->val_get_num(v, 0); return true; }
	if (t == XV_STR) { const char *s = X->val_get_str(v, nullptr); char *e; long long n = strtoll(s, &e, 0); if (e != s && !*e) { out = n; return true; } }
	return false;
}
static uintptr_t addr_arg(XmodCall *c, int i, const char *what = "address")
{
	int64_t a;
	if (!iarg(c, i, a) || a <= 0) { X->call_error(c, (std::string("codex: ") + what + " expected (a number)").c_str()); return 0; }
	return (uintptr_t)a;
}
// entity index or raw pointer -> object pointer (CBaseEntity* / private data)
static uintptr_t obj_arg(XmodCall *c, int i)
{
	int64_t a;
	if (!iarg(c, i, a)) { X->call_error(c, "codex: entity index or pointer expected"); return 0; }
	if (a > 0x10000) return (uintptr_t)a;
	uintptr_t p = ent_to_cbase((int)a);
	if (!p) { X->call_error(c, ("codex: entity " + std::to_string(a) + " has no private data").c_str()); return 0; }
	return p;
}
static Module *module_arg(XmodCall *c, int i)
{
	std::string n = sarg(c, i, "game");
	Module *m = mod_find(n.c_str());
	if (!m) s_last_error = "module '" + n + "' is not loaded";
	return m;
}
static XmodValue *module_val(const Module &m)
{
	XmodValue *t = X->val_map();
	X->val_set(t, "name", X->val_cstr(m.name.c_str()));
	X->val_set(t, "path", X->val_cstr(m.path.c_str()));
	X->val_set(t, "base", X->val_int(m.base));
	X->val_set(t, "size", X->val_int(m.size));
	X->val_set(t, "build", X->val_cstr(m.fingerprint.c_str()));
	return t;
}
static Func *func_arg(XmodCall *c, int i)
{
	std::string n = sarg(c, i);
	Func *f = func_get(n);
	if (!f) X->call_error(c, ("codex: no function named '" + n + "' (define it with codex.func{...})").c_str());
	return f;
}

// ---------------------------------------------------------------------------------------------------------------
// modules, search
static void f_module(XmodCall *c, void *) { Module *m = module_arg(c, 0); if (m) ret(c, module_val(*m)); else ret_nil(c); }
static void f_modules(XmodCall *c, void *)
{
	mod_refresh();
	XmodValue *a = X->val_array();
	for (auto &m : mod_list()) if (m.loaded) X->val_push(a, module_val(m));
	ret(c, a);
}
static void f_module_of(XmodCall *c, void *)
{
	uintptr_t a = addr_arg(c, 0); if (!a) return;
	Module *m = mod_of(a);
	if (!m) { fail(c, "no module contains this address"); return; }
	XmodValue *t = module_val(*m);
	X->val_set(t, "offset", X->val_int(a - m->base));
	ret(c, t);
}
static void f_find(XmodCall *c, void *)
{
	Module *m = module_arg(c, 0);
	if (!m) { ret_nil(c); return; }
	Sig s; std::string err, st; int sadj = 0;
	sig_split(sarg(c, 1), st, sadj);
	if (!sig_parse(st, s, err)) { X->call_error(c, ("codex.find: " + err).c_str()); return; }
	int64_t index = 0, adjust = sadj;
	const XmodValue *o = arg(c, 2);
	if (X->val_type(o) == XV_MAP) {
		const XmodValue *v;
		if ((v = X->val_get(o, "index"))) index = X->val_get_int(v, 0);
		if ((v = X->val_get(o, "adjust"))) adjust += X->val_get_int(v, 0);
	}
	std::vector<uintptr_t> hits;
	size_t n = sig_scan(*m, s, hits, 1024, true);
	if (n == 0) { fail(c, "signature not found in " + m->name); return; }
	if (n > 1 && index <= 0) { fail(c, "signature matches " + std::to_string(n) + " places in " + m->name + " (make it longer or pass {index = n})"); return; }
	size_t k = index > 0 ? (size_t)index - 1 : 0;
	if (k >= hits.size()) { fail(c, "index out of range (" + std::to_string(n) + " matches)"); return; }
	ret_int(c, (int64_t)(hits[k] + adjust));
}
static void f_find_all(XmodCall *c, void *)
{
	Module *m = module_arg(c, 0);
	if (!m) { ret_nil(c); return; }
	Sig s; std::string err, st; int sadj = 0;
	sig_split(sarg(c, 1), st, sadj);
	if (!sig_parse(st, s, err)) { X->call_error(c, ("codex.find_all: " + err).c_str()); return; }
	int64_t max = 64; iarg(c, 2, max);
	std::vector<uintptr_t> hits;
	sig_scan(*m, s, hits, (size_t)std::max<int64_t>(1, std::min<int64_t>(max, 100000)), true);
	XmodValue *a = X->val_array();
	for (auto h : hits) X->val_push(a, X->val_int((int64_t)(h + sadj)));
	ret(c, a);
}
static void f_symbol(XmodCall *c, void *)
{
	Module *m = module_arg(c, 0);
	if (!m) { ret_nil(c); return; }
	std::string err;
	uintptr_t a = sym_find(*m, sarg(c, 1), err);
	if (!a) fail(c, err); else ret_int(c, a);
}
static void f_xref(XmodCall *c, void *)
{
	Module *m = module_arg(c, 0);
	if (!m) { ret_nil(c); return; }
	int64_t max = 16; iarg(c, 2, max);
	auto refs = xref_string(*m, sarg(c, 1), (size_t)std::max<int64_t>(1, max));
	XmodValue *a = X->val_array();
	for (auto &r : refs) {
		XmodValue *t = X->val_map();
		X->val_set(t, "ref", X->val_int(r.ref));
		X->val_set(t, "func", X->val_int(r.func));
		X->val_push(a, t);
	}
	ret(c, a);
}
static void f_func_start(XmodCall *c, void *)
{
	uintptr_t a = addr_arg(c, 0); if (!a) return;
	Module *m = mod_of(a);
	bool exact = false;
	uintptr_t s = m ? func_start(*m, a, &exact) : 0;
	if (!s) fail(c, "function start not found"); else ret_int(c, s);
}
static void f_sig_make(XmodCall *c, void *)
{
	uintptr_t a;
	Module *m = nullptr;
	if (X->val_type(arg(c, 0)) == XV_STR && !X->val_get_str(arg(c, 0), nullptr)[0]) { X->call_error(c, "codex.sig_make: address or function name"); return; }
	int64_t v;
	if (iarg(c, 0, v)) a = (uintptr_t)v;
	else {                                                    // a codex function name
		Func *f = func_get(sarg(c, 0));
		if (!f || !f->resolved || !f->addr) { fail(c, "unknown or unresolved function '" + sarg(c, 0) + "'"); return; }
		a = f->addr;
	}
	m = mod_of(a);
	if (!m) { fail(c, "address is not inside a loaded module"); return; }
	int64_t maxlen = 160; iarg(c, 1, maxlen);
	SigMade sm; std::string err;
	bool ok = sig_make(a, *m, sm, err, (int)std::max<int64_t>(8, std::min<int64_t>(maxlen, 1024)));
	XmodValue *t = X->val_map();
	X->val_set(t, "sig", X->val_cstr(sm.text.c_str()));
	X->val_set(t, "len", X->val_int(sm.len));
	X->val_set(t, "matches", X->val_int(sm.matches));
	X->val_set(t, "unique", X->val_bool(ok));
	X->val_set(t, "module", X->val_cstr(m->name.c_str()));
	X->val_set(t, "offset", X->val_int(a - m->base));
	X->val_set(t, "adjust", X->val_int(sm.adjust));
	if (!ok) { X->val_set(t, "error", X->val_cstr(err.c_str())); s_last_error = err; }
	ret(c, t);
}
static void f_insn(XmodCall *c, void *)
{
	uintptr_t a = addr_arg(c, 0); if (!a) return;
	int64_t n = 8; iarg(c, 1, n);
	XmodValue *arr = X->val_array();
	for (int i = 0; i < std::min<int64_t>(n, 256); ++i) {
		uint8_t b[16];
		if (!mem_read(a, b, 16)) break;
		Insn in;
		if (!lde_decode(b, in)) break;
		XmodValue *t = X->val_map();
		X->val_set(t, "addr", X->val_int(a));
		X->val_set(t, "len", X->val_int(in.len));
		X->val_set(t, "hex", X->val_cstr(hex_bytes(b, in.len).c_str()));
		if (in.rel_size) X->val_set(t, "target", X->val_int(in.target(a)));
		if (in.is_ret) X->val_set(t, "ret", X->val_bool(true));
		if (in.imm_size) { X->val_set(t, "imm", X->val_int(in.imm)); X->val_set(t, "imm_size", X->val_int(in.imm_size)); }
		if (in.disp_size) { X->val_set(t, "disp", X->val_int(in.disp)); if (in.disp_abs) X->val_set(t, "disp_abs", X->val_bool(true)); }
		X->val_push(arr, t);
		a += in.len;
	}
	ret(c, arr);
}

// ---------------------------------------------------------------------------------------------------------------
static bool int_of(const XmodValue *v, int64_t &out)
{
	int t = X->val_type(v);
	if (t == XV_INT || t == XV_NUM || t == XV_BOOL) { out = X->val_get_int(v, 0); if (t == XV_NUM) out = (int64_t)X->val_get_num(v, 0); return true; }
	return false;
}
static std::string hex_addr(uintptr_t a) { char b[16]; snprintf(b, sizeof b, "%08x", (unsigned)a); return b; }
// constants baked into code ("hard-coded limits"): find every instruction of a function that uses a value, change it
static bool read_orig(uintptr_t a, uint8_t *b, size_t n) { return mem_read_orig(a, b, n); }   // as the game shipped them
struct ConstSite { uintptr_t insn, field; int size; bool is_disp; int32_t value; int delta; std::string hex; };
// recursive descent from the function start: every branch inside [start, start + limit) is followed, so code after an
// early return is seen too; calls are not entered (only this function's own body)
static std::vector<ConstSite> const_sites(uintptr_t start, int64_t value, uint32_t limit, bool want_disp, std::string &err, bool near_ok = false)
{
	std::vector<ConstSite> out;
	std::vector<uintptr_t> work{start};
	std::set<uintptr_t> seen;
	size_t decoded = 0;
	while (!work.empty() && decoded < 20000) {
		uintptr_t a = work.back(); work.pop_back();
		while (a >= start && a < start + limit && !seen.count(a)) {
			seen.insert(a);
			uint8_t b[16];
			if (!read_orig(a, b, 16)) break;
			Insn in;
			if (!lde_decode(b, in)) break;
			++decoded;
			auto take = [&](int off, int size, int32_t v, bool disp) {
				if (!size) return;
				int64_t sv = v;                                        // imm8/imm16 compare sign-extended
				if (size == 4 && value > INT32_MAX && (uint32_t)v == (uint64_t)value) sv = value;
				int64_t d = sv - value;
				if (d == 0 || (near_ok && (d == 1 || d == -1)))           // near: "x >= 16000" is often compiled as "cmp x, 15999"
					out.push_back({a, a + off, size, disp, v, (int)d, hex_bytes(b, in.len)});
			};
			take(in.imm_off, in.imm_size, in.imm, false);
			if (want_disp && !in.disp_abs) take(in.disp_off, in.disp_size, in.disp, true);
			if (in.rel_size) {
				uintptr_t t = in.target(a);
				if (!in.is_call && t >= start && t < start + limit) work.push_back(t);
			}
			if (in.is_ret || in.is_jmp || (in.opcode == 0xFF && (in.reg == 4 || in.reg == 5)) || in.opcode == 0xCC) break;
			a += in.len;
		}
	}
	if (decoded >= 20000) err = "function too large, stopped after 20000 instructions";
	std::sort(out.begin(), out.end(), [](const ConstSite &x, const ConstSite &y) { return x.field < y.field; });
	return out;
}
static bool target_addr(XmodCall *c, int i, uintptr_t &a)
{
	int64_t v;
	if (iarg(c, i, v)) { a = (uintptr_t)v; return a != 0; }
	Func *f = func_get(sarg(c, i));
	if (!f || !f->resolved || !f->addr) { s_last_error = "unknown or unresolved function '" + sarg(c, i) + "'"; return false; }
	a = f->addr;
	return true;
}
// codex.consts(func|addr, value [, {size = bytes, disp = true}]) -> { {addr = field, insn = .., size = 1|2|4, hex = .., disp = bool}, ... }
static void f_consts(XmodCall *c, void *)
{
	uintptr_t a; if (!target_addr(c, 0, a)) { ret_nil(c); return; }
	int64_t value; if (!iarg(c, 1, value)) { X->call_error(c, "codex.consts(func, value [, opts])"); return; }
	Module *m = mod_of(a);
	if (!m || !mem_executable(a)) { fail(c, "address is not code of a loaded module"); return; }
	uint32_t limit = 8192;
	if (const Module::Sym *s = sym_containing(*m, a)) if (s->addr == a && s->size) limit = s->size;
	bool want_disp = false, near_ok = false;
	const XmodValue *o = arg(c, 2);
	if (X->val_type(o) == XV_MAP) {
		int64_t l; if (int_of(X->val_get(o, "size"), l) && l > 0) limit = (uint32_t)std::min<int64_t>(l, 1 << 20);
		const XmodValue *d = X->val_get(o, "disp"); want_disp = d && X->val_get_bool(d, 0);
		const XmodValue *n = X->val_get(o, "near"); near_ok = n && X->val_get_bool(n, 0);
	}
	std::string err;
	auto sites = const_sites(a, value, limit, want_disp, err, near_ok);
	if (!err.empty()) s_last_error = err;
	XmodValue *arr = X->val_array();
	for (auto &s : sites) {
		XmodValue *t = X->val_map();
		X->val_set(t, "addr", X->val_int(s.field));
		X->val_set(t, "insn", X->val_int(s.insn));
		X->val_set(t, "size", X->val_int(s.size));
		X->val_set(t, "hex", X->val_cstr(s.hex.c_str()));
		X->val_set(t, "value", X->val_int(s.value));
		if (s.delta) X->val_set(t, "delta", X->val_int(s.delta));
		if (s.is_disp) X->val_set(t, "disp", X->val_bool(true));
		X->val_push(arr, t);
	}
	ret(c, arr);
}
// codex.set_const(site | {sites} | func, [old_value,] new_value) -> array of patch ids (all or nothing)
//   the new value must fit the field: a 1-byte immediate (e.g. "cmp eax, 10") cannot become 1000 - that is refused
static void f_set_const(XmodCall *c, void *)
{
	struct F { uintptr_t first; int second; int64_t delta; };
	std::vector<F> fields;                                     // field address, size, value offset (near_ok matches)
	std::string err;
	int64_t nv0;
	const XmodValue *s = arg(c, 0);
	auto add_site = [&](const XmodValue *t) {
		int64_t ad = 0, sz = 0, dl = 0;
		if (!int_of(X->val_get(t, "addr"), ad) || !int_of(X->val_get(t, "size"), sz) || (sz != 1 && sz != 2 && sz != 4)) return false;
		int_of(X->val_get(t, "delta"), dl);
		fields.push_back({(uintptr_t)ad, (int)sz, dl}); return true;
	};
	int vi = 1;
	if (X->val_type(s) == XV_MAP && X->val_get(s, "addr")) { if (!add_site(s)) { X->call_error(c, "codex.set_const: bad site"); return; } }
	else if (X->val_type(s) == XV_ARRAY) { for (size_t i = 0; i < X->val_len(s); ++i) if (!add_site(X->val_at(s, i))) { X->call_error(c, "codex.set_const: bad site list"); return; } }
	else {                                                     // set_const(func, old, new): every use of old in func
		uintptr_t a; int64_t ov;
		if (!target_addr(c, 0, a) || !iarg(c, 1, ov)) { fail(c, s_last_error.empty() ? "codex.set_const(func, old, new)" : s_last_error); return; }
		Module *m = mod_of(a);
		uint32_t limit = 8192;
		if (m) if (const Module::Sym *sy = sym_containing(*m, a)) if (sy->addr == a && sy->size) limit = sy->size;
		for (auto &cs : const_sites(a, ov, limit, false, err, false)) fields.push_back({cs.field, cs.size, cs.delta});
		vi = 2;
	}
	if (!iarg(c, vi, nv0)) { X->call_error(c, "codex.set_const: new value expected"); return; }
	if (fields.empty()) { fail(c, "no constant site to change"); return; }
	for (auto &f : fields) {
		int64_t nv = nv0 + f.delta;
		int64_t lo = f.second == 1 ? -128 : f.second == 2 ? -32768 : INT32_MIN, hi = f.second == 1 ? 127 : f.second == 2 ? 32767 : UINT32_MAX;
		if (nv < lo || nv > hi) { fail(c, "value " + std::to_string(nv) + " does not fit the " + std::to_string(f.second) + "-byte field at " + hex_addr(f.first) +
		                                  " (the instruction only has room for " + std::to_string(lo) + ".." + std::to_string(hi) + ") - patch the instruction itself"); return; }
	}
	std::vector<int> ids;
	for (auto &f : fields) {
		std::vector<uint8_t> b(f.second);
		uint32_t v = (uint32_t)(nv0 + f.delta);
		memcpy(b.data(), &v, f.second);
		int id = patch_apply(f.first, b, nullptr, "codex.set_const");
		if (!id) { std::string e = cx_last_error(); for (int x : ids) patch_remove(x); fail(c, e); return; }
		ids.push_back(id);
	}
	XmodValue *arr = X->val_array();
	for (int x : ids) X->val_push(arr, X->val_int(x));
	ret(c, arr);
}

// ---------------------------------------------------------------------------------------------------------------
// memory
enum MT { M_I8, M_U8, M_I16, M_U16, M_I32, M_U32, M_I64, M_F32, M_F64, M_PTR, M_STR, M_VEC, M_BYTES, M_BAD };
static MT mtype(const std::string &t, int &size)
{
	static const struct { const char *n; MT t; int s; } T[] = {
		{"i8", M_I8, 1}, {"char", M_I8, 1}, {"u8", M_U8, 1}, {"byte", M_U8, 1}, {"bool", M_U8, 1}, {"i16", M_I16, 2}, {"short", M_I16, 2}, {"u16", M_U16, 2},
		{"i32", M_I32, 4}, {"int", M_I32, 4}, {"u32", M_U32, 4}, {"uint", M_U32, 4}, {"i64", M_I64, 8}, {"f32", M_F32, 4}, {"float", M_F32, 4},
		{"f64", M_F64, 8}, {"double", M_F64, 8}, {"ptr", M_PTR, 4}, {"str", M_STR, 0}, {"string", M_STR, 0}, {"vec", M_VEC, 12}, {"vector", M_VEC, 12},
		{"bytes", M_BYTES, 1},
	};
	for (auto &e : T) if (t == e.n) { size = e.s; return e.t; }
	size = 0;
	return M_BAD;
}
static XmodValue *mread_one(MT t, uintptr_t a, bool &ok)
{
	uint8_t b[16] = {0};
	ok = true;
	switch (t) {
	case M_STR: { std::string s; if (!mem_cstr(a, s)) { ok = false; return nullptr; } return X->val_str(s.data(), s.size()); }
	case M_VEC: {
		float v[3]; if (!mem_read(a, v, 12)) { ok = false; return nullptr; }
		XmodValue *m = X->val_map(); X->val_set(m, "x", X->val_num(v[0])); X->val_set(m, "y", X->val_num(v[1])); X->val_set(m, "z", X->val_num(v[2])); return m;
	}
	default: break;
	}
	int sz; mtype("", sz);
	static const int SZ[] = {1, 1, 2, 2, 4, 4, 8, 4, 8, 4};
	if (t > M_PTR || !mem_read(a, b, SZ[t])) { ok = false; return nullptr; }
	switch (t) {
	case M_I8: return X->val_int((int8_t)b[0]);
	case M_U8: return X->val_int(b[0]);
	case M_I16: { int16_t v; memcpy(&v, b, 2); return X->val_int(v); }
	case M_U16: { uint16_t v; memcpy(&v, b, 2); return X->val_int(v); }
	case M_I32: { int32_t v; memcpy(&v, b, 4); return X->val_int(v); }
	case M_U32: case M_PTR: { uint32_t v; memcpy(&v, b, 4); return X->val_int(v); }
	case M_I64: { int64_t v; memcpy(&v, b, 8); return X->val_int(v); }
	case M_F32: { float v; memcpy(&v, b, 4); return X->val_num(v); }
	case M_F64: { double v; memcpy(&v, b, 8); return X->val_num(v); }
	default: ok = false; return nullptr;
	}
}
static bool hex_parse(const std::string &s, std::vector<uint8_t> &out)
{
	Sig sg; std::string err;
	if (!sig_parse(s, sg, err)) return false;
	for (auto m : sg.m) if (!m) return false;                 // no wildcards in data
	out = sg.b;
	return true;
}
static bool mwrite_one(MT t, uintptr_t a, const XmodValue *v, std::string &err, bool code)
{
	uint8_t b[16]; size_t n = 0;
	auto num = [&](void) -> double { return X->val_type(v) == XV_BOOL ? X->val_get_bool(v, 0) : X->val_get_num(v, 0); };
	auto in = [&](void) -> int64_t { return X->val_type(v) == XV_BOOL ? X->val_get_bool(v, 0) : X->val_type(v) == XV_NUM ? (int64_t)X->val_get_num(v, 0) : X->val_get_int(v, 0); };
	std::vector<uint8_t> big;
	switch (t) {
	case M_I8: case M_U8: { uint8_t q = (uint8_t)in(); memcpy(b, &q, 1); n = 1; break; }
	case M_I16: case M_U16: { uint16_t q = (uint16_t)in(); memcpy(b, &q, 2); n = 2; break; }
	case M_I32: case M_U32: case M_PTR: { uint32_t q = (uint32_t)in(); memcpy(b, &q, 4); n = 4; break; }
	case M_I64: { int64_t q = in(); memcpy(b, &q, 8); n = 8; break; }
	case M_F32: { float q = (float)num(); memcpy(b, &q, 4); n = 4; break; }
	case M_F64: { double q = num(); memcpy(b, &q, 8); n = 8; break; }
	case M_VEC: {
		float f[3] = {0, 0, 0};
		if (X->val_type(v) != XV_MAP) { err = "vector table expected"; return false; }
		f[0] = (float)X->val_get_num(X->val_get(v, "x"), 0); f[1] = (float)X->val_get_num(X->val_get(v, "y"), 0); f[2] = (float)X->val_get_num(X->val_get(v, "z"), 0);
		memcpy(b, f, 12); n = 12; break;
	}
	case M_STR: { size_t l = 0; const char *s = X->val_get_str(v, &l); if (!s) { err = "string expected"; return false; } big.assign(s, s + l); big.push_back(0); break; }
	case M_BYTES: { const char *s = X->val_get_str(v, nullptr); if (!s || !hex_parse(s, big)) { err = "hex bytes expected, e.g. \"90 90 EB\""; return false; } break; }
	default: err = "bad type"; return false;
	}
	const uint8_t *src = big.empty() ? b : big.data();
	size_t len = big.empty() ? n : big.size();
	if (code) return patch_apply(a, std::vector<uint8_t>(src, src + len), nullptr, "codex.write") != 0 || (err = cx_last_error(), false);
	if (!mem_write(a, src, len)) { err = "address is not writable memory (code? use codex.patch)"; return false; }
	return true;
}
static void f_read(XmodCall *c, void *)
{
	uintptr_t a = addr_arg(c, 0); if (!a) return;
	int sz; MT t = mtype(sarg(c, 1, "i32"), sz);
	if (t == M_BAD) { X->call_error(c, "codex.read: type must be i8 u8 i16 u16 i32 u32 i64 f32 f64 ptr str vec bytes"); return; }
	int64_t n = 0; bool has_n = iarg(c, 2, n);
	if (t == M_BYTES) {
		if (!has_n || n <= 0 || n > 65536) n = 16;
		std::vector<uint8_t> buf((size_t)n);
		if (!mem_read(a, buf.data(), buf.size())) { fail(c, "memory not readable"); return; }
		ret(c, X->val_cstr(hex_bytes(buf.data(), buf.size()).c_str()));
		return;
	}
	bool ok;
	if (!has_n) { XmodValue *v = mread_one(t, a, ok); if (!ok) fail(c, "memory not readable"); else ret(c, v); return; }
	XmodValue *arr = X->val_array();
	for (int64_t i = 0; i < std::min<int64_t>(n, 4096); ++i) {
		XmodValue *v = mread_one(t, a + (uintptr_t)(i * (sz ? sz : 4)), ok);
		if (!ok) break;
		X->val_push(arr, v);
	}
	ret(c, arr);
}
static void f_write(XmodCall *c, void *)
{
	uintptr_t a = addr_arg(c, 0); if (!a) return;
	int sz; MT t = mtype(sarg(c, 1, "i32"), sz);
	if (t == M_BAD) { X->call_error(c, "codex.write: bad type"); return; }
	std::string err;
	if (!mwrite_one(t, a, arg(c, 2), err, false)) { fail(c, err); return; }
	ret_bool(c, true);
}
static void f_readable(XmodCall *c, void *) { int64_t a = 0, n = 1; iarg(c, 0, a); iarg(c, 1, n); ret_bool(c, a > 0 && mem_readable((uintptr_t)a, (size_t)std::max<int64_t>(1, n))); }
static void f_writable(XmodCall *c, void *) { int64_t a = 0, n = 1; iarg(c, 0, a); iarg(c, 1, n); ret_bool(c, a > 0 && mem_writable((uintptr_t)a, (size_t)std::max<int64_t>(1, n))); }

static void f_patch(XmodCall *c, void *)
{
	uintptr_t a = addr_arg(c, 0); if (!a) return;
	std::vector<uint8_t> bytes, expect;
	if (!hex_parse(sarg(c, 1), bytes)) { X->call_error(c, "codex.patch: bytes as hex text, e.g. \"90 90 EB 05\""); return; }
	if (argc(c) >= 3 && X->val_type(arg(c, 2)) == XV_STR && !hex_parse(sarg(c, 2), expect)) { X->call_error(c, "codex.patch: expected bytes must be hex"); return; }
	if (!mem_executable(a) && !mem_readable(a, bytes.size())) { fail(c, "address is not mapped"); return; }
	int id = patch_apply(a, bytes, expect.empty() ? nullptr : &expect, "codex.patch");
	if (!id) fail(c, cx_last_error()); else ret_int(c, id);
}
static void f_unpatch(XmodCall *c, void *) { int64_t id = 0; iarg(c, 0, id); ret_bool(c, patch_remove((int)id)); }
static void f_patches(XmodCall *c, void *)
{
	XmodValue *a = X->val_array();
	for (auto &p : patch_list()) {
		XmodValue *t = X->val_map();
		X->val_set(t, "id", X->val_int(p.id));
		X->val_set(t, "addr", X->val_int(p.addr));
		X->val_set(t, "orig", X->val_cstr(hex_bytes(p.orig.data(), p.orig.size()).c_str()));
		X->val_set(t, "now", X->val_cstr(hex_bytes(p.now.data(), p.now.size()).c_str()));
		X->val_set(t, "owner", X->val_cstr(p.owner.c_str()));
		X->val_push(a, t);
	}
	ret(c, a);
}

static std::vector<void *> s_allocs;
static void f_alloc(XmodCall *c, void *)
{
	int64_t n = 0;
	if (!iarg(c, 0, n) || n <= 0 || n > 16 * 1024 * 1024) { X->call_error(c, "codex.alloc(size): 1..16M"); return; }
	void *p = calloc(1, (size_t)n);
	if (!p) { fail(c, "out of memory"); return; }
	s_allocs.push_back(p);
	ret_int(c, (int64_t)(uintptr_t)p);
}
static void f_free(XmodCall *c, void *)
{
	int64_t a = 0; iarg(c, 0, a);
	for (size_t i = 0; i < s_allocs.size(); ++i)
		if ((int64_t)(uintptr_t)s_allocs[i] == a) { free(s_allocs[i]); s_allocs.erase(s_allocs.begin() + i); ret_bool(c, true); return; }
	ret_bool(c, false);                                       // only memory from codex.alloc can be freed
}

// ---------------------------------------------------------------------------------------------------------------
// entities, private data, vtables
static void f_pdata(XmodCall *c, void *) { int64_t i = -1; iarg(c, 0, i); uintptr_t p = ent_to_cbase((int)i); if (!p) fail(c, "no private data"); else ret_int(c, p); }
static void f_ent(XmodCall *c, void *) { int64_t p = 0; iarg(c, 0, p); ret_int(c, cbase_to_ent((uintptr_t)p)); }

// codex.get(ent|ptr, offset, type [, linux_offset]) / codex.set(ent|ptr, offset, type, value [, linux_offset]) : byte offsets
static uintptr_t field_addr(XmodCall *c, int off_i, int lin_i)
{
	uintptr_t base = obj_arg(c, 0); if (!base) return 0;
	int64_t off = 0, lin;
	if (!iarg(c, off_i, off)) { X->call_error(c, "codex: byte offset expected"); return 0; }
#ifndef _WIN32
	if (iarg(c, lin_i, lin)) off = lin;
#else
	(void)lin_i; (void)lin;
#endif
	return base + (uintptr_t)off;
}
static void f_get(XmodCall *c, void *)
{
	uintptr_t a = field_addr(c, 1, 3); if (!a) return;
	int sz; MT t = mtype(sarg(c, 2, "i32"), sz);
	if (t == M_BAD || t == M_BYTES) { X->call_error(c, "codex.get: bad type"); return; }
	bool ok; XmodValue *v = mread_one(t, a, ok);
	if (!ok) fail(c, "field not readable"); else ret(c, v);
}
static void f_set(XmodCall *c, void *)
{
	uintptr_t a = field_addr(c, 1, 4); if (!a) return;
	int sz; MT t = mtype(sarg(c, 2, "i32"), sz);
	if (t == M_BAD || t == M_BYTES) { X->call_error(c, "codex.set: bad type"); return; }
	std::string err;
	if (!mwrite_one(t, a, arg(c, 3), err, false)) fail(c, err); else ret_bool(c, true);
}
static void f_vtable(XmodCall *c, void *) { uintptr_t o = obj_arg(c, 0); if (!o) return; uintptr_t v = vtable_of(o); if (!v) fail(c, "no vtable"); else ret_int(c, v); }
static void f_vfunc(XmodCall *c, void *)
{
	uintptr_t o = obj_arg(c, 0); if (!o) return;
	int64_t i = -1; iarg(c, 1, i);
	uintptr_t v = vtable_of(o), f = 0;
	if (!v || i < 0 || !mem_read(v + 4 * (uintptr_t)i, &f, 4) || !mem_executable(f)) { fail(c, "vtable slot is not code"); return; }
	ret_int(c, f);
}
static void f_vindex(XmodCall *c, void *)
{
	uintptr_t o = obj_arg(c, 0); if (!o) return;
	int64_t fn = 0; iarg(c, 1, fn);
	uintptr_t v = vtable_of(o);
	for (int i = 0; v && i < 1024; ++i) {
		uintptr_t f = 0;
		if (!mem_read(v + 4 * i, &f, 4) || !mem_executable(f)) break;
		if ((int64_t)f == fn) { ret_int(c, i); return; }
	}
	fail(c, "function not found in this vtable");
}
static void table_lookup(XmodCall *c, const NameOfs *tab, const void *base)
{
	std::string n = sarg(c, 0);
	if (!base) { fail(c, "table not available"); return; }
	for (const NameOfs *e = tab; e->name; ++e)
		if (n == e->name || n == std::string("pfn") + e->name) {
			uintptr_t f = *(const uintptr_t *)((const char *)base + e->ofs);
			if (!f) { fail(c, n + " is NULL in this engine/game"); return; }
			ret_int(c, f);
			return;
		}
	fail(c, "unknown function table entry '" + n + "'");
}
static void f_engfunc(XmodCall *c, void *) { table_lookup(c, ENG_FUNCS, &g_engfuncs); }
static void f_dllfunc(XmodCall *c, void *)
{
	std::string n = sarg(c, 0);
	for (const NameOfs *e = NEWDLL_FUNCS; e->name; ++e)
		if (n == e->name && gpGamedllFuncs && gpGamedllFuncs->newapi_table) { table_lookup(c, NEWDLL_FUNCS, gpGamedllFuncs->newapi_table); return; }
	table_lookup(c, DLL_FUNCS, gpGamedllFuncs ? gpGamedllFuncs->dllapi_table : nullptr);
}

// ---------------------------------------------------------------------------------------------------------------
// functions
static void f_func(XmodCall *c, void *)
{
	std::string err;
	if (!func_define(arg(c, 0), err)) { s_last_error = err; cx_log("%s", err.c_str()); ret_nil(c); return; }
	ret_bool(c, true);
}
static void call_common(XmodCall *c, bool through)
{
	Func *f = func_arg(c, 0); if (!f) return;
	int n = argc(c) - 1;
	std::vector<const XmodValue *> av(n > 0 ? n : 0);
	for (int i = 0; i < n; ++i) av[i] = arg(c, i + 1);
	XmodValue *r = nullptr; std::string err;
	if (!func_call(*f, av.data(), n, through, &r, err)) { s_last_error = err; X->call_error(c, err.c_str()); return; }
	ret(c, r);
}
static void f_call(XmodCall *c, void *) { call_common(c, false); }
static void f_callh(XmodCall *c, void *) { call_common(c, true); }
static void f_addr(XmodCall *c, void *) { Func *f = func_get(sarg(c, 0)); if (!f || !f->resolved || !f->addr) fail(c, "unknown or unresolved"); else ret_int(c, f->addr); }
static void f_info(XmodCall *c, void *)
{
	Func *f = func_get(sarg(c, 0));
	if (!f) { fail(c, "unknown function"); return; }
	XmodValue *t = X->val_map();
	X->val_set(t, "name", X->val_cstr(f->name.c_str()));
	X->val_set(t, "module", X->val_cstr(f->module.c_str()));
	X->val_set(t, "resolved", X->val_bool(f->resolved));
	if (f->addr) {
		X->val_set(t, "addr", X->val_int(f->addr));
		if (Module *m = mod_of(f->addr)) X->val_set(t, "offset", X->val_int(f->addr - m->base));
	}
	if (f->vindex >= 0) X->val_set(t, "vindex", X->val_int(f->vindex));
	X->val_set(t, "how", X->val_cstr(f->how.c_str()));
	if (!f->why.empty()) X->val_set(t, "why", X->val_cstr(f->why.c_str()));
	if (!f->sig_used.empty()) X->val_set(t, "sig", X->val_cstr(f->sig_used.c_str()));
	X->val_set(t, "faulted", X->val_bool(f->faulted));
	if (f->faulted) X->val_set(t, "fault", X->val_cstr(f->fault.c_str()));
	X->val_set(t, "calls", X->val_int((int64_t)f->calls));
	X->val_set(t, "hooked_calls", X->val_int((int64_t)f->hooked_calls));
	XmodValue *hs = X->val_array();
	for (Hook *h : f->hooks) X->val_push(hs, X->val_cstr(h->vtable ? ("vtable:" + h->label).c_str() : "detour"));
	X->val_set(t, "hooks", hs);
	std::string proto = std::string(type_name(f->ret)) + " " + f->name + "(";
	for (size_t i = 0; i < f->slots.size(); ++i) proto += (i ? ", " : "") + std::string(i == 0 && f->has_this ? "this:" : "") + type_name(f->slots[i].t);
	proto += ")";
	X->val_set(t, "proto", X->val_cstr(proto.c_str()));
	X->val_set(t, "pre_event", X->val_cstr(f->ev_pre.c_str()));
	X->val_set(t, "post_event", X->val_cstr(f->ev_post.c_str()));
	ret(c, t);
}
static void f_list(XmodCall *c, void *)
{
	XmodValue *a = X->val_array();
	for (Func *f : func_all()) X->val_push(a, X->val_cstr(f->name.c_str()));
	ret(c, a);
}
static void f_reset(XmodCall *c, void *) { Func *f = func_arg(c, 0); if (!f) return; f->faulted = false; f->fault.clear(); ret_bool(c, true); }
static void f_hook(XmodCall *c, void *)
{
	Func *f = func_arg(c, 0); if (!f) return;
	std::string err;
	if (!hook_add(*f, X->val_type(arg(c, 1)) == XV_MAP ? arg(c, 1) : nullptr, err)) { s_last_error = err; cx_log("%s", err.c_str()); ret_nil(c); return; }
	if (!g_sync_hooks) cx_log("hook '%s' is observe-only: xmod < 0.4.1 cannot run Lua handlers inside the call (update xmod for full hooks)", f->name.c_str());
	ret_bool(c, true);
}
static void f_unhook(XmodCall *c, void *) { Func *f = func_arg(c, 0); if (!f) return; ret_bool(c, hook_remove(*f, sarg(c, 1).c_str())); }

// inside handlers
static HookFrame *need_frame(XmodCall *c)
{
	HookFrame *fr = frame_top();
	if (!fr) X->call_error(c, "codex: only valid inside a codex hook handler");
	return fr;
}
static void f_arg(XmodCall *c, void *)
{
	HookFrame *fr = need_frame(c); if (!fr) return;
	int64_t n = 1; iarg(c, 0, n);
	std::string err; XmodValue *v = frame_arg(fr, (int)n, err);
	if (!v) X->call_error(c, ("codex.arg: " + err).c_str()); else ret(c, v);
}
static void f_this(XmodCall *c, void *)
{
	HookFrame *fr = need_frame(c); if (!fr) return;
	std::string err; XmodValue *v = frame_arg(fr, 0, err);
	if (!v) X->call_error(c, ("codex.this: " + err).c_str()); else ret(c, v);
}
static void f_set_arg(XmodCall *c, void *)
{
	HookFrame *fr = need_frame(c); if (!fr) return;
	int64_t n = 0; iarg(c, 0, n);
	std::string err;
	if (!frame_set_arg(fr, (int)n, arg(c, 1), err)) X->call_error(c, ("codex.set_arg: " + err).c_str()); else ret_bool(c, true);
}
static void f_ret(XmodCall *c, void *)
{
	HookFrame *fr = need_frame(c); if (!fr) return;
	std::string err; XmodValue *v = frame_ret(fr, err);
	if (!v) X->call_error(c, ("codex.ret: " + err).c_str()); else ret(c, v);
}
static void f_set_ret(XmodCall *c, void *)
{
	HookFrame *fr = need_frame(c); if (!fr) return;
	std::string err;
	if (!frame_set_ret(fr, arg(c, 0), err)) X->call_error(c, ("codex.set_ret: " + err).c_str()); else ret_bool(c, true);
}

// ---------------------------------------------------------------------------------------------------------------
// call graph (graph.cpp): callees, callers, call trees, treemaps, symbol at an address, value search/replace
static XmodValue *opt(XmodCall *c, int i, const char *k) { const XmodValue *o = arg(c, i); return X->val_type(o) == XV_MAP ? (XmodValue *)X->val_get(o, k) : nullptr; }
static int64_t opt_int(XmodCall *c, int i, const char *k, int64_t d) { XmodValue *v = opt(c, i, k); return v && (X->val_type(v) == XV_INT || X->val_type(v) == XV_NUM) ? (int64_t)X->val_get_num(v, (double)d) : d; }
static double opt_num(XmodCall *c, int i, const char *k, double d) { XmodValue *v = opt(c, i, k); return v && (X->val_type(v) == XV_INT || X->val_type(v) == XV_NUM) ? X->val_get_num(v, d) : d; }
static bool opt_bool(XmodCall *c, int i, const char *k, bool d) { XmodValue *v = opt(c, i, k); return v && X->val_type(v) == XV_BOOL ? X->val_get_bool(v, d) != 0 : d; }
static std::string opt_str(XmodCall *c, int i, const char *k, const char *d) { XmodValue *v = opt(c, i, k); const char *t = v ? X->val_get_str(v, nullptr) : nullptr; return t ? t : d; }

// target: address | codex function name | symbol | "module:symbol". 0 = failed (nil returned, last_error set)
static uintptr_t target_arg(XmodCall *c, int i, Module **mo)
{
	std::string err;
	uintptr_t a = 0;
	int64_t v;
	if (X->val_type(arg(c, i)) == XV_INT || X->val_type(arg(c, i)) == XV_NUM) { if (iarg(c, i, v) && v > 0x10000) a = (uintptr_t)v; else err = "address expected"; if (a) *mo = mod_of(a); }
	else a = graph_target(sarg(c, i), nullptr, mo, err);
	if (a && !*mo) { err = "address is not inside a loaded module"; a = 0; }
	if (!a) fail(c, err);
	return a;
}
static void put_name(XmodValue *t, const char *key, Module &m, uintptr_t a)
{
	if (!a || !m.contains(a)) return;
	uintptr_t off = 0;
	std::string n = sym_name_at(m, a, &off);
	if (!n.empty()) X->val_set(t, key, X->val_cstr(off ? (n + "+" + std::to_string(off)).c_str() : n.c_str()));
}
static XmodValue *callsite_val(Module &m, const CallSite &cs)
{
	XmodValue *t = X->val_map();
	X->val_set(t, "at", X->val_int(cs.at));
	if (cs.to) X->val_set(t, "to", X->val_int(cs.to));
	X->val_set(t, "kind", X->val_cstr(call_kind_name(cs.kind)));
	if (cs.vindex >= 0) X->val_set(t, "vindex", X->val_int(cs.vindex));
	if (cs.slot) X->val_set(t, "slot", X->val_int(cs.slot));
	if (!cs.name.empty()) X->val_set(t, "name", X->val_cstr(cs.name.c_str()));
	else if (cs.to) { Module *tm = mod_of(cs.to); if (tm) put_name(t, "name", *tm, cs.to); }
	return t;
}
static void f_callees(XmodCall *c, void *)
{
	Module *m = nullptr; uintptr_t a = target_arg(c, 0, &m); if (!a) return;
	FuncBody b; std::string err;
	if (!analyze_func(*m, a, b, err)) { fail(c, err); return; }
	bool direct = opt_bool(c, 1, "direct", false);
	XmodValue *arr = X->val_array();
	std::set<uintptr_t> seen;
	for (auto &cs : b.calls) {
		if (direct && (cs.kind == CK_INDIRECT || cs.kind == CK_VCALL || !cs.to)) continue;
		if (opt_bool(c, 1, "unique", false) && cs.to && !seen.insert(cs.to).second) continue;
		X->val_push(arr, callsite_val(*m, cs));
	}
	ret(c, arr);
}
static void f_callers(XmodCall *c, void *)
{
	Module *m = nullptr; uintptr_t a = target_arg(c, 0, &m); if (!a) return;
	std::string mn = opt_str(c, 1, "module", "");
	if (!mn.empty()) { m = mod_find(mn.c_str()); if (!m) { fail(c, "module '" + mn + "' is not loaded"); return; } }
	auto refs = find_callers(*m, a, (size_t)std::max<int64_t>(1, opt_int(c, 1, "max", 256)), opt_bool(c, 1, "refs", false));
	bool uniq = opt_bool(c, 1, "unique", false);
	std::set<uintptr_t> seen;
	XmodValue *arr = X->val_array();
	for (auto &r : refs) {
		if (uniq && r.func && !seen.insert(r.func).second) continue;
		XmodValue *t = X->val_map();
		X->val_set(t, "at", X->val_int(r.at));
		if (r.func) { X->val_set(t, "func", X->val_int(r.func)); put_name(t, "name", *m, r.func); }
		X->val_set(t, "kind", X->val_cstr(call_kind_name(r.kind)));
		X->val_set(t, "exact", X->val_bool(r.exact));
		X->val_push(arr, t);
	}
	ret(c, arr);
}
static XmodValue *tree_node(Module &m, uintptr_t a, int depth, bool up, int &budget, std::set<uintptr_t> &path)
{
	XmodValue *t = X->val_map();
	X->val_set(t, "addr", X->val_int(a));
	put_name(t, "name", m, a);
	if (path.count(a)) { X->val_set(t, "cycle", X->val_bool(true)); return t; }
	if (depth <= 0 || budget <= 0) return t;
	path.insert(a);
	XmodValue *kids = X->val_array();
	std::vector<uintptr_t> next;
	if (up) { std::set<uintptr_t> s; for (auto &r : find_callers(m, a, 512, false)) if (r.func && s.insert(r.func).second) next.push_back(r.func); }
	else { FuncBody b; std::string e; if (analyze_func(m, a, b, e)) next = direct_callees(m, b); }
	for (uintptr_t n : next) { if (--budget < 0) { X->val_set(t, "truncated", X->val_bool(true)); break; } X->val_push(kids, tree_node(m, n, depth - 1, up, budget, path)); }
	X->val_set(t, "children", kids);
	path.erase(a);
	return t;
}
static void f_calltree(XmodCall *c, void *)
{
	Module *m = nullptr; uintptr_t a = target_arg(c, 0, &m); if (!a) return;
	int depth = (int)std::max<int64_t>(1, std::min<int64_t>(opt_int(c, 1, "depth", 2), 6));
	bool up = opt_str(c, 1, "dir", "down") == "up";
	int budget = (int)std::max<int64_t>(1, std::min<int64_t>(opt_int(c, 1, "max", 300), 5000));
	std::set<uintptr_t> path;
	ret(c, tree_node(*m, a, depth, up, budget, path));
}
static void f_analyze(XmodCall *c, void *)
{
	Module *m = nullptr; uintptr_t a = target_arg(c, 0, &m); if (!a) return;
	FuncBody b; std::string err;
	if (!analyze_func(*m, a, b, err)) { fail(c, err); return; }
	XmodValue *t = X->val_map();
	X->val_set(t, "start", X->val_int(b.start));
	X->val_set(t, "size", X->val_int(b.end - b.start));
	X->val_set(t, "insns", X->val_int(b.insns));
	X->val_set(t, "branches", X->val_int(b.branches));
	X->val_set(t, "calls", X->val_int((int64_t)b.calls.size()));
	X->val_set(t, "bounded", X->val_bool(b.bounded));
	X->val_set(t, "truncated", X->val_bool(b.truncated));
	X->val_set(t, "module", X->val_cstr(m->name.c_str()));
	put_name(t, "name", *m, a);
	XmodValue *st = X->val_array();
	for (uintptr_t s : b.strings) { std::string txt; if (mem_cstr(s, txt, 512)) X->val_push(st, X->val_cstr(txt.c_str())); }
	X->val_set(t, "strings", st);
	XmodValue *ks = X->val_array();
	for (uint32_t k : b.consts) X->val_push(ks, X->val_int((int64_t)(int32_t)k));
	X->val_set(t, "consts", ks);
	XmodValue *ce = X->val_array();
	for (uintptr_t x : direct_callees(*m, b)) X->val_push(ce, X->val_int(x));
	X->val_set(t, "callees", ce);
	ret(c, t);
}
static void f_treemap(XmodCall *c, void *)
{
	Module *m = nullptr; uintptr_t a = target_arg(c, 0, &m); if (!a) return;
	int64_t depth = 2; if (argc(c) >= 2) iarg(c, 1, depth);
	std::string err, t = treemap_make(*m, a, (int)depth, err);
	if (t.empty()) fail(c, err); else ret(c, X->val_cstr(t.c_str()));
}
static void f_treemap_find(XmodCall *c, void *)
{
	Module *m = module_arg(c, 0); if (!m) { ret_nil(c); return; }
	std::string err;
	auto hits = treemap_find(*m, sarg(c, 1), (size_t)std::max<int64_t>(1, opt_int(c, 2, "max", 5)), opt_num(c, 2, "min", 0.75), err);
	if (hits.empty()) { fail(c, err); return; }
	XmodValue *arr = X->val_array();
	for (auto &h : hits) {
		XmodValue *t = X->val_map();
		X->val_set(t, "addr", X->val_int(h.addr));
		X->val_set(t, "score", X->val_num(std::round(h.score * 1000) / 1000));
		put_name(t, "name", *m, h.addr);
		X->val_push(arr, t);
	}
	ret(c, arr);
}
static void f_symbol_at(XmodCall *c, void *)
{
	uintptr_t a = addr_arg(c, 0); if (!a) return;
	Module *m = mod_of(a);
	if (!m) { fail(c, "no module contains this address"); return; }
	uintptr_t off = 0;
	std::string n = sym_name_at(*m, a, &off);
	if (n.empty()) { fail(c, "no symbol or Codex function at this address (Windows DLLs only name their exports)"); return; }
	XmodValue *t = X->val_map();
	X->val_set(t, "name", X->val_cstr(n.c_str()));
	X->val_set(t, "offset", X->val_int(off));
	X->val_set(t, "module", X->val_cstr(m->name.c_str()));
	ret(c, t);
}

// value -> bytes (+ mask for "bytes" signatures)
static bool value_bytes(XmodCall *c, const std::string &ty, const XmodValue *v, Sig &out, std::string &err)
{
	out.b.clear(); out.m.clear();
	auto put = [&](const void *p, size_t n) { for (size_t i = 0; i < n; ++i) { out.b.push_back(((const uint8_t *)p)[i]); out.m.push_back(1); } };
	int vt = X->val_type(v);
	bool num = vt == XV_INT || vt == XV_NUM;
	if (ty == "str") { size_t n = 0; const char *s = X->val_get_str(v, &n); if (!s || !n) { err = "a non-empty string expected"; return false; } put(s, n); uint8_t z = 0; put(&z, 1); return true; }
	if (ty == "bytes") { size_t n = 0; const char *s = X->val_get_str(v, &n); if (!s) { err = "hex bytes expected (\"8B 45 ??\")"; return false; } return sig_parse(std::string(s, n), out, err); }
	if (!num) { err = "a number expected for type " + ty; return false; }
	double d = X->val_get_num(v, 0); int64_t i = (int64_t)d;
	if (vt == XV_INT) i = X->val_get_int(v, 0);
	if (ty == "i8" || ty == "u8") { uint8_t x = (uint8_t)i; put(&x, 1); }
	else if (ty == "i16" || ty == "u16") { uint16_t x = (uint16_t)i; put(&x, 2); }
	else if (ty == "i32" || ty == "u32" || ty == "int") { uint32_t x = (uint32_t)i; put(&x, 4); }
	else if (ty == "f32" || ty == "float") { float x = (float)d; put(&x, 4); }
	else if (ty == "f64" || ty == "double") { double x = d; put(&x, 8); }
	else { err = "type must be i8 u8 i16 u16 i32 u32 f32 f64 str bytes"; return false; }
	(void)c;
	return true;
}
static std::vector<uintptr_t> value_hits(Module &m, const Sig &s, bool code, bool data, bool whole_str, size_t max, size_t &total)
{
	std::vector<uintptr_t> all, out;
	sig_scan(m, s, all, 200000, false);
	for (uintptr_t h : all) {
		const Segment *sg = m.seg_of(h);
		if (!sg || (sg->x && !code) || (!sg->x && !data)) continue;
		if (whole_str && h > sg->lo && *(const uint8_t *)(h - 1) != 0) continue;
		out.push_back(h);
	}
	total = out.size();
	if (out.size() > max) out.resize(max);
	return out;
}
static void f_find_value(XmodCall *c, void *)
{
	Module *m = module_arg(c, 0); if (!m) { ret_nil(c); return; }
	std::string ty = sarg(c, 1), err; Sig s;
	if (!value_bytes(c, ty, arg(c, 2), s, err)) { X->call_error(c, ("codex.find_value: " + err).c_str()); return; }
	size_t total = 0;
	auto hits = value_hits(*m, s, opt_bool(c, 3, "code", false), opt_bool(c, 3, "data", true), ty == "str", (size_t)std::max<int64_t>(1, opt_int(c, 3, "max", 256)), total);
	XmodValue *arr = X->val_array();
	for (uintptr_t h : hits) X->val_push(arr, X->val_int(h));
	ret(c, arr);
}
static void f_replace_value(XmodCall *c, void *)
{
	Module *m = module_arg(c, 0); if (!m) { ret_nil(c); return; }
	std::string ty = sarg(c, 1), err; Sig so, sn;
	if (!value_bytes(c, ty, arg(c, 2), so, err) || !value_bytes(c, ty, arg(c, 3), sn, err)) { X->call_error(c, ("codex.replace_value: " + err).c_str()); return; }
	for (auto k : sn.m) if (!k) { X->call_error(c, "codex.replace_value: the new bytes cannot contain wildcards"); return; }
	if (ty == "str") { if (sn.b.size() > so.b.size()) { fail(c, "the new string is longer than the old one (it must fit in place)"); return; } while (sn.b.size() < so.b.size()) { sn.b.push_back(0); sn.m.push_back(1); } }
	if (sn.b.size() != so.b.size()) { fail(c, "old and new values must have the same size"); return; }
	int64_t max = opt_int(c, 4, "max", 64);
	size_t total = 0;
	auto hits = value_hits(*m, so, opt_bool(c, 4, "code", false), opt_bool(c, 4, "data", true), ty == "str", (size_t)std::max<int64_t>(1, max), total);
	if (hits.empty()) { fail(c, "value not found in " + m->name); return; }
	if ((int64_t)total > max) { fail(c, "value found " + std::to_string(total) + " times in " + m->name + " (more than max = " + std::to_string(max) + "; pass a larger max if you really mean all of them)"); return; }
	std::vector<int> ids;
	for (uintptr_t h : hits) {
		std::vector<uint8_t> expect(so.b.size());
		if (!mem_read(h, expect.data(), expect.size())) continue;
		bool ok = true; for (size_t i = 0; i < so.b.size(); ++i) if (so.m[i] && expect[i] != so.b[i]) ok = false;
		if (!ok) continue;
		int id = patch_apply(h, sn.b, &expect, "replace_value");
		if (!id) { for (int x : ids) patch_remove(x); fail(c, cx_last_error() + " (nothing was changed)"); return; }
		ids.push_back(id);
	}
	XmodValue *t = X->val_map();
	X->val_set(t, "count", X->val_int((int64_t)ids.size()));
	XmodValue *pa = X->val_array(), *aa = X->val_array();
	for (size_t i = 0; i < ids.size(); ++i) { X->val_push(pa, X->val_int(ids[i])); X->val_push(aa, X->val_int(hits[i])); }
	X->val_set(t, "patches", pa); X->val_set(t, "addrs", aa);
	ret(c, t);
}

// misc
static void f_last_error(XmodCall *c, void *) { ret(c, X->val_cstr(s_last_error.c_str())); }
static void f_safe(XmodCall *c, void *) { if (argc(c) >= 1) g_safe_calls = X->val_get_bool(arg(c, 0), 1) != 0; ret_bool(c, g_safe_calls); }
static void f_cache_clear(XmodCall *c, void *) { cache_clear(); ret_bool(c, true); }
static void f_status(XmodCall *c, void *)
{
	XmodValue *t = X->val_map();
	X->val_set(t, "version", X->val_cstr(CODEX_VERSION));
	X->val_set(t, "sync_hooks", X->val_bool(g_sync_hooks));
	X->val_set(t, "safe_calls", X->val_bool(g_safe_calls));
	int hooks = 0, resolved = 0;
	for (Func *f : func_all()) { hooks += (int)f->hooks.size(); resolved += f->resolved; }
	X->val_set(t, "functions", X->val_int((int64_t)func_all().size()));
	X->val_set(t, "resolved", X->val_int(resolved));
	X->val_set(t, "hooks", X->val_int(hooks));
	X->val_set(t, "patches", X->val_int((int64_t)patch_list().size()));
#ifdef _WIN32
	X->val_set(t, "platform", X->val_cstr("windows"));
#else
	X->val_set(t, "platform", X->val_cstr("linux"));
#endif
	ret(c, t);
}

bool codex_register(const XmodApi *api, XmodExt *ext)
{
	struct F { const char *n; xmod_sync_fn f; };
	static const F funcs[] = {
		{"module", f_module}, {"modules", f_modules}, {"module_of", f_module_of},
		{"find", f_find}, {"find_all", f_find_all}, {"symbol", f_symbol}, {"xref", f_xref}, {"func_start", f_func_start},
		{"sig_make", f_sig_make}, {"insn", f_insn}, {"consts", f_consts}, {"set_const", f_set_const},
		{"read", f_read}, {"write", f_write}, {"readable", f_readable}, {"writable", f_writable},
		{"patch", f_patch}, {"unpatch", f_unpatch}, {"patches", f_patches}, {"alloc", f_alloc}, {"free", f_free},
		{"pdata", f_pdata}, {"ent", f_ent}, {"get", f_get}, {"set", f_set},
		{"vtable", f_vtable}, {"vfunc", f_vfunc}, {"vindex", f_vindex}, {"engfunc", f_engfunc}, {"dllfunc", f_dllfunc},
		{"func", f_func}, {"call", f_call}, {"callh", f_callh}, {"addr", f_addr}, {"info", f_info}, {"list", f_list}, {"reset", f_reset},
		{"hook", f_hook}, {"unhook", f_unhook},
		{"arg", f_arg}, {"this", f_this}, {"set_arg", f_set_arg}, {"ret", f_ret}, {"set_ret", f_set_ret},
		{"callees", f_callees}, {"callers", f_callers}, {"calltree", f_calltree}, {"analyze", f_analyze},
		{"treemap", f_treemap}, {"treemap_find", f_treemap_find}, {"symbol_at", f_symbol_at},
		{"find_value", f_find_value}, {"replace_value", f_replace_value},
		{"last_error", f_last_error}, {"safe", f_safe}, {"cache_clear", f_cache_clear}, {"status", f_status},
	};
	for (auto &f : funcs)
		if (!api->register_func(ext, CX_NS, f.n, f.f, nullptr)) return false;
	return true;
}
