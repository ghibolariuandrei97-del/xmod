// func.cpp - function definitions, resolution, typed calls, detour and vtable hooks, the hook dispatcher.
#include "codex.h"
#include <algorithm>
#include <cstdio>

#ifdef _WIN32
static const char *const PLAT = "windows";
#else
static const char *const PLAT = "linux";
#endif

bool g_safe_calls = true;
static std::unordered_map<std::string, Func *> s_funcs;
static std::vector<Func *> s_order;

Func *func_get(const std::string &name) { auto it = s_funcs.find(name); return it == s_funcs.end() ? nullptr : it->second; }
std::vector<Func *> func_all() { return s_order; }

// ---------------------------------------------------------------------------------------------------------------
// types
static const struct { const char *n; CxType t; } TYPES[] = {
	{"void", T_VOID}, {"bool", T_BOOL}, {"char", T_I8}, {"i8", T_I8}, {"int8", T_I8}, {"byte", T_U8}, {"u8", T_U8}, {"uint8", T_U8},
	{"short", T_I16}, {"i16", T_I16}, {"int16", T_I16}, {"ushort", T_U16}, {"u16", T_U16}, {"uint16", T_U16},
	{"int", T_I32}, {"i32", T_I32}, {"int32", T_I32}, {"long", T_I32}, {"uint", T_U32}, {"u32", T_U32}, {"uint32", T_U32}, {"unsigned", T_U32},
	{"i64", T_I64}, {"int64", T_I64}, {"u64", T_I64}, {"uint64", T_I64}, {"float", T_F32}, {"f32", T_F32}, {"double", T_F64}, {"f64", T_F64},
	{"ptr", T_PTR}, {"pointer", T_PTR}, {"void*", T_PTR}, {"string", T_STR}, {"str", T_STR}, {"char*", T_STR}, {"const char*", T_STR},
	{"entity", T_CBASE}, {"cbase", T_CBASE}, {"CBaseEntity*", T_CBASE}, {"edict", T_EDICT}, {"edict_t*", T_EDICT},
	{"entvars", T_ENTVARS}, {"entvars_t*", T_ENTVARS}, {"vector", T_VECPTR}, {"vec", T_VECPTR}, {"Vector*", T_VECPTR}, {"Vector&", T_VECPTR},
	{"vecval", T_VECVAL}, {"Vector", T_VECVAL},
	{"astring", T_ASTR}, {"pstring", T_ASTR}, {"string_t", T_STRT}, {"stringt", T_STRT},
};
bool type_parse(const char *s, CxType &t)
{
	if (!s) return false;
	for (auto &e : TYPES) if (strcmp(e.n, s) == 0) { t = e.t; return true; }
	return false;
}
const char *type_name(CxType t)
{
	for (auto &e : TYPES) if (e.t == t) return e.n;
	return "?";
}
int type_words(CxType t) { return t == T_I64 || t == T_F64 ? 2 : t == T_VECVAL ? 3 : 1; }

// ---------------------------------------------------------------------------------------------------------------
// entities <-> pointers
uintptr_t ent_to_cbase(int idx)
{
	if (idx < 0 || !gpGlobals || idx >= gpGlobals->maxEntities) return 0;
	edict_t *e = g_engfuncs.pfnPEntityOfEntIndex(idx);
	if (!e || e->free || !e->pvPrivateData) return 0;
	return (uintptr_t)e->pvPrivateData;
}
static edict_t *edict_of(int idx)
{
	if (idx < 0 || !gpGlobals || idx >= gpGlobals->maxEntities) return nullptr;
	edict_t *e = g_engfuncs.pfnPEntityOfEntIndex(idx);
	return (e && !e->free) ? e : nullptr;
}
static int edict_index(uintptr_t e)
{
	if (!e) return -1;
	int i = g_engfuncs.pfnIndexOfEdict((const edict_t *)e);
	if (i < 0 || (gpGlobals && i >= gpGlobals->maxEntities)) return -1;
	return g_engfuncs.pfnPEntityOfEntIndex(i) == (edict_t *)e ? i : -1;
}
int cbase_to_ent(uintptr_t p)
{
	if (p < 0x10000) return -1;
	uintptr_t pev = 0, ed = 0;
	if (!mem_read(p + 4, &pev, 4) || pev < 0x10000) return -1;                     // CBaseEntity: [vtable][pev]
	if (!mem_read(pev + offsetof(entvars_t, pContainingEntity), &ed, 4)) return -1;
	int i = edict_index(ed);
	if (i < 0) return -1;
	return ((edict_t *)ed)->pvPrivateData == (void *)p ? i : -1;
}
uintptr_t vtable_of(uintptr_t obj)
{
	uintptr_t vt = 0;
	if (obj < 0x10000 || !mem_read(obj, &vt, 4)) return 0;
	return mem_readable(vt, 4) ? vt : 0;
}

// ---------------------------------------------------------------------------------------------------------------
// engine string pool: strings the game may keep (classnames, models, targetnames) must not point into Lua memory
static uint32_t pool_alloc(const std::string &s)                  // -> string_t (offset from pStringBase)
{
	if (!g_engfuncs.pfnAllocString || !gpGlobals) return 0;
	return (uint32_t)g_engfuncs.pfnAllocString(s.c_str());
}
static uintptr_t pool_ptr(uint32_t st) { return gpGlobals ? (uintptr_t)gpGlobals->pStringBase + st : 0; }

// ---------------------------------------------------------------------------------------------------------------
// value conversion
XmodValue *native_to_val(CxType t, const uint32_t *w, double)
{
	const XmodApi *X = g_xmod;
	switch (t) {
	case T_VOID: return X->val_nil();
	case T_BOOL: return X->val_bool((w[0] & 0xFF) != 0);
	case T_I8: return X->val_int((int8_t)w[0]);
	case T_U8: return X->val_int((uint8_t)w[0]);
	case T_I16: return X->val_int((int16_t)w[0]);
	case T_U16: return X->val_int((uint16_t)w[0]);
	case T_I32: return X->val_int((int32_t)w[0]);
	case T_U32: case T_PTR: return X->val_int((int64_t)w[0]);
	case T_I64: return X->val_int((int64_t)(((uint64_t)w[1] << 32) | w[0]));
	case T_F32: { float f; memcpy(&f, w, 4); return X->val_num(f); }
	case T_F64: { double d; memcpy(&d, w, 8); return X->val_num(d); }
	case T_STR: case T_ASTR: { std::string s; if (w[0] && mem_cstr(w[0], s)) return X->val_str(s.data(), s.size()); return X->val_nil(); }
	case T_STRT: { std::string s; if (w[0] && gpGlobals && mem_cstr(pool_ptr(w[0]), s)) return X->val_str(s.data(), s.size()); return X->val_str("", 0); }
	case T_CBASE: return X->val_int(cbase_to_ent(w[0]));
	case T_EDICT: return X->val_int(edict_index(w[0]));
	case T_ENTVARS: {
		uintptr_t ed = 0;
		if (w[0] < 0x10000 || !mem_read(w[0] + offsetof(entvars_t, pContainingEntity), &ed, 4)) return X->val_int(-1);
		return X->val_int(edict_index(ed));
	}
	case T_VECPTR: case T_VECVAL: {
		float v[3];
		if (t == T_VECPTR) { if (!w[0] || !mem_read(w[0], v, 12)) return X->val_nil(); }
		else memcpy(v, w, 12);
		XmodValue *m = X->val_map();
		X->val_set(m, "x", X->val_num(v[0])); X->val_set(m, "y", X->val_num(v[1])); X->val_set(m, "z", X->val_num(v[2]));
		return m;
	}
	default: return X->val_nil();
	}
}

static bool vec_from_val(const XmodValue *v, float out[3])
{
	const XmodApi *X = g_xmod;
	if (X->val_type(v) == XV_MAP) {
		const XmodValue *x = X->val_get(v, "x"), *y = X->val_get(v, "y"), *z = X->val_get(v, "z");
		if (!x || !y || !z) return false;
		out[0] = (float)X->val_get_num(x, 0); out[1] = (float)X->val_get_num(y, 0); out[2] = (float)X->val_get_num(z, 0);
		return true;
	}
	if (X->val_type(v) == XV_ARRAY && X->val_len(v) >= 3) {
		for (int i = 0; i < 3; ++i) out[i] = (float)X->val_get_num(X->val_at(v, i), 0);
		return true;
	}
	return false;
}

bool val_to_words(CxType t, const XmodValue *v, uint32_t *w, std::deque<std::string> &strs, std::deque<std::array<float, 3>> &vecs, std::string &err)
{
	const XmodApi *X = g_xmod;
	int vt = X->val_type(v);
	auto as_int = [&](int64_t d) -> int64_t {
		if (vt == XV_BOOL) return X->val_get_bool(v, 0);
		if (vt == XV_NUM) return (int64_t)X->val_get_num(v, 0);
		return X->val_get_int(v, d);
	};
	switch (t) {
	case T_BOOL: w[0] = (vt == XV_BOOL ? X->val_get_bool(v, 0) : as_int(0)) ? 1 : 0; return true;
	case T_I8: case T_U8: case T_I16: case T_U16: case T_I32: case T_U32: case T_PTR:
		if (vt != XV_INT && vt != XV_NUM && vt != XV_BOOL && vt != XV_NIL) { err = std::string("expected a number for ") + type_name(t); return false; }
		w[0] = (uint32_t)as_int(0); return true;
	case T_I64: { uint64_t q = (uint64_t)as_int(0); w[0] = (uint32_t)q; w[1] = (uint32_t)(q >> 32); return true; }
	case T_F32: { float f = (float)X->val_get_num(v, 0); memcpy(w, &f, 4); return true; }
	case T_F64: { double d = X->val_get_num(v, 0); memcpy(w, &d, 8); return true; }
	case T_STR: {
		if (vt == XV_NIL) { w[0] = 0; return true; }
		if (vt == XV_INT || vt == XV_NUM) { w[0] = (uint32_t)as_int(0); return true; }   // a raw char* address
		size_t len = 0; const char *s = X->val_get_str(v, &len);
		if (!s) { err = "expected a string"; return false; }
		strs.emplace_back(s, len);
		w[0] = (uint32_t)(uintptr_t)strs.back().c_str();
		return true;
	}
	case T_ASTR: case T_STRT: {
		if (vt == XV_NIL) { w[0] = 0; return true; }
		if (vt == XV_INT || vt == XV_NUM) { w[0] = (uint32_t)as_int(0); return true; }   // raw pointer / string_t
		size_t len = 0; const char *s = X->val_get_str(v, &len);
		if (!s) { err = "expected a string"; return false; }
		uint32_t st = pool_alloc(std::string(s, len));
		if (!st && len) { err = "the engine string pool is not available"; return false; }
		w[0] = t == T_STRT ? st : (uint32_t)pool_ptr(st);
		return true;
	}
	case T_CBASE: case T_EDICT: case T_ENTVARS: {
		if (vt == XV_NIL) { w[0] = 0; return true; }
		int64_t i = as_int(-1);
		if (i > 0x10000) { w[0] = (uint32_t)i; return true; }                        // already a pointer
		if (i < 0) { w[0] = 0; return true; }                                         // -1 = NULL
		if (t == T_CBASE) { w[0] = (uint32_t)ent_to_cbase((int)i); if (!w[0]) { err = "entity " + std::to_string(i) + " has no private data (not a valid entity)"; return false; } return true; }
		edict_t *e = edict_of((int)i);
		if (!e) { err = "entity " + std::to_string(i) + " is not valid"; return false; }
		w[0] = t == T_EDICT ? (uint32_t)(uintptr_t)e : (uint32_t)(uintptr_t)&e->v;
		return true;
	}
	case T_VECPTR: case T_VECVAL: {
		if (t == T_VECPTR && (vt == XV_INT || vt == XV_NUM)) { w[0] = (uint32_t)as_int(0); return true; }   // raw Vector*
		if (t == T_VECPTR && vt == XV_NIL) { w[0] = 0; return true; }
		float f[3];
		if (!vec_from_val(v, f)) { err = "expected a vector {x=, y=, z=}"; return false; }
		if (t == T_VECVAL) { memcpy(w, f, 12); return true; }
		vecs.push_back({f[0], f[1], f[2]});
		w[0] = (uint32_t)(uintptr_t)vecs.back().data();
		return true;
	}
	default: err = "bad argument type"; return false;
	}
}

// ---------------------------------------------------------------------------------------------------------------
// layout: which stack words / registers carry which argument, and who pops the stack
bool func_layout(Func &f, std::string &err)
{
	f.slots.clear();
	int w = 0, regs = 0;
#ifdef _WIN32
	bool this_in_ecx = f.conv == CONV_THISCALL;
#else
	bool this_in_ecx = false;                       // GCC: 'this' is the first stack argument
#endif
	if (f.has_this) {
		if (this_in_ecx) f.slots.push_back({f.this_t, -1, 1, 1});
		else f.slots.push_back({f.this_t, w++, 1, 0});
	}
	for (CxType t : f.args) {
		if (t == T_VOID) { err = "an argument cannot be void"; return false; }
		int n = type_words(t);
		if (f.conv == CONV_FASTCALL && n == 1 && t != T_F32 && regs < 2) { f.slots.push_back({t, -1, 1, 1 + regs}); ++regs; continue; }
		f.slots.push_back({t, w, n, 0});
		w += n;
	}
	if (w > 64) { err = "too many argument words (max 64)"; return false; }
	f.stack_words = w;
#ifdef _WIN32
	bool callee_pops = f.conv == CONV_STDCALL || f.conv == CONV_THISCALL || f.conv == CONV_FASTCALL;
#else
	bool callee_pops = f.conv == CONV_STDCALL || f.conv == CONV_FASTCALL;
#endif
	f.pop_bytes = callee_pops ? w * 4 : 0;
	return true;
}

// ---------------------------------------------------------------------------------------------------------------
// definition and resolution
static const XmodValue *plat(const XmodValue *v)
{
	if (g_xmod->val_type(v) == XV_MAP) { const XmodValue *p = g_xmod->val_get(v, PLAT); return p; }
	return v;
}
static void strings_of(const XmodValue *v, std::vector<std::string> &out)
{
	v = plat(v);
	if (!v) return;
	if (g_xmod->val_type(v) == XV_STR) { out.push_back(g_xmod->val_get_str(v, nullptr)); return; }
	if (g_xmod->val_type(v) == XV_ARRAY)
		for (size_t i = 0; i < g_xmod->val_len(v); ++i) { const char *s = g_xmod->val_get_str(g_xmod->val_at(v, i), nullptr); if (s) out.push_back(s); }
}
static bool int_of(const XmodValue *v, int64_t &out)
{
	v = plat(v);
	if (!v) return false;
	int t = g_xmod->val_type(v);
	if (t == XV_INT) { out = g_xmod->val_get_int(v, 0); return true; }
	if (t == XV_NUM) { out = (int64_t)g_xmod->val_get_num(v, 0); return true; }
	if (t == XV_STR) { const char *s = g_xmod->val_get_str(v, nullptr); char *e; long long n = strtoll(s, &e, 0); if (e != s) { out = n; return true; } }
	return false;
}
static std::string str_of(const XmodValue *v, const char *dflt = "")
{
	v = plat(v);
	const char *s = v ? g_xmod->val_get_str(v, nullptr) : nullptr;
	return s ? s : dflt;
}

static bool valid_code(Module &m, uintptr_t a)
{
	const Segment *s = m.seg_of(a);
	return s && s->x && mem_executable(a);
}

static uintptr_t apply_post(uintptr_t a, int64_t adjust, const std::string &follow, std::string &err)
{
	a += (uintptr_t)adjust;
	if (follow.empty()) return a;
	uint8_t b[6];
	if (!mem_read(a, b, 6)) { err = "follow: cannot read"; return 0; }
	int32_t rel;
	if (b[0] == 0xE8 || b[0] == 0xE9) { memcpy(&rel, b + 1, 4); return a + 5 + rel; }
	if (b[0] == 0x0F && (b[1] & 0xF0) == 0x80) { memcpy(&rel, b + 2, 4); return a + 6 + rel; }
	if (follow == "rel32") { memcpy(&rel, b, 4); return a + 4 + rel; }
	err = "follow: no call/jmp at the match";
	return 0;
}

bool func_define(const XmodValue *def, std::string &err)
{
	const XmodApi *X = g_xmod;
	if (X->val_type(def) != XV_MAP) { err = "codex.func expects a table"; return false; }
	std::string name = str_of(X->val_get(def, "name"));
	if (name.empty()) { err = "codex.func: 'name' is required"; return false; }
	if (Func *old = func_get(name)) {
		bool replace = X->val_get_bool(X->val_get(def, "replace"), 0) != 0;
		if (!replace && old->resolved) return true;       // shared gamedata: first definition wins
		if (!old->hooks.empty()) { err = "codex.func: '" + name + "' is hooked - unhook it before redefining"; return false; }
	}
	Func *f = new Func();
	f->name = name;
	f->module = str_of(X->val_get(def, "module"), "game");
	std::string conv = str_of(X->val_get(def, "conv"), "cdecl");
	if (conv == "cdecl") f->conv = CONV_CDECL;
	else if (conv == "stdcall") f->conv = CONV_STDCALL;
	else if (conv == "thiscall" || conv == "method") f->conv = CONV_THISCALL;
	else if (conv == "fastcall") f->conv = CONV_FASTCALL;
	else { err = "codex.func: unknown conv '" + conv + "' (cdecl, stdcall, thiscall, fastcall)"; delete f; return false; }
	const XmodValue *thisv = X->val_get(def, "this");
	f->has_this = f->conv == CONV_THISCALL;
	if (thisv) {
		if (X->val_type(thisv) == XV_BOOL) f->has_this = X->val_get_bool(thisv, 0) != 0;
		else { CxType t; if (!type_parse(X->val_get_str(thisv, nullptr), t) || (t != T_CBASE && t != T_PTR)) { err = "codex.func: this must be \"entity\" or \"ptr\""; delete f; return false; } f->has_this = true; f->this_t = t; }
	}
	const XmodValue *args = X->val_get(def, "args");
	for (size_t i = 0; args && i < X->val_len(args); ++i) {
		CxType t;
		const char *ts = X->val_get_str(X->val_at(args, i), nullptr);
		if (!type_parse(ts, t)) { err = "codex.func '" + name + "': unknown argument type '" + std::string(ts ? ts : "?") + "'"; delete f; return false; }
		f->args.push_back(t);
	}
	std::string rets = str_of(X->val_get(def, "ret"), "void");
	if (!type_parse(rets.c_str(), f->ret) || f->ret == T_VECVAL) { err = "codex.func '" + name + "': unknown or unsupported return type '" + rets + "'"; delete f; return false; }
	if (!func_layout(*f, err)) { err = "codex.func '" + name + "': " + err; delete f; return false; }
	f->ev_pre = "codex:" + name;
	f->ev_post = "codex:" + name + ":post";

	// register now (even unresolved: codex.info shows why)
	if (Func *old = func_get(name)) { s_order.erase(std::remove(s_order.begin(), s_order.end(), old), s_order.end()); delete old; }
	s_funcs[name] = f;
	s_order.push_back(f);

	int64_t vi;
	if (int_of(X->val_get(def, "vindex"), vi)) {
		f->vindex = (int)vi; f->resolved = true; f->how = "vtable[" + std::to_string(vi) + "]";
		if (!f->has_this) { f->resolved = false; f->why = "a virtual function needs conv = \"thiscall\""; err = f->why; return false; }
		return true;
	}

	Module *m = mod_find(f->module.c_str());
	if (!m) { f->why = "module '" + f->module + "' is not loaded"; err = f->why; return false; }
	int64_t adjust = 0;
	int_of(X->val_get(def, "adjust"), adjust);
	std::string follow = str_of(X->val_get(def, "follow"));
	int64_t want_index = 0;
	int_of(X->val_get(def, "index"), want_index);
	std::string notes;
	uintptr_t a = 0;

	// 1) cache of this exact build
	{
		uintptr_t off; std::string csig;
		if (cache_get(*m, name, off, csig)) {
			Sig s; std::string e2;
			uintptr_t c = m->base + off;
			std::string cs; int cadj = 0;
			sig_split(csig, cs, cadj);
			if (valid_code(*m, c) && (csig.empty() || (sig_parse(cs, s, e2) && sig_match_at(c - cadj, s)))) { a = c; f->how = "cache"; f->sig_used = csig; }
		}
	}
	// 2) address / offset
	int64_t v;
	if (!a && int_of(X->val_get(def, "address"), v)) { a = (uintptr_t)v; f->how = "address"; }
	if (!a && int_of(X->val_get(def, "offset"), v)) { a = m->base + (uintptr_t)v; f->how = "offset"; }
	// 3) symbols
	if (!a) {
		std::vector<std::string> syms;
		strings_of(X->val_get(def, "symbol"), syms);
		for (auto &s : syms) {
			std::string e2;
			uintptr_t r = sym_find(*m, s, e2);
			if (r) { a = r; f->how = "symbol " + s; break; }
			notes += e2 + "; ";
		}
	}
	// 4) signatures (this platform), 5) signatures learned from other builds
	auto try_sigs = [&](const std::vector<std::string> &sigs, const char *how) {
		for (auto &full : sigs) {
			Sig s; std::string e2, st; int sadj = 0;
			sig_split(full, st, sadj);
			if (!sig_parse(st, s, e2)) { notes += "bad signature: " + e2 + "; "; continue; }
			std::vector<uintptr_t> hits;
			size_t n = sig_scan(*m, s, hits, 64, true);
			if (n == 0) { notes += "signature not found; "; continue; }
			if (n > 1 && want_index <= 0) { notes += "signature matches " + std::to_string(n) + " places (make it longer or set index); "; continue; }
			size_t k = want_index > 0 ? (size_t)want_index - 1 : 0;
			if (k >= hits.size()) { notes += "index out of range; "; continue; }
			a = hits[k] + sadj; f->how = how; f->sig_used = full;
			return true;
		}
		return false;
	};
	if (!a) { std::vector<std::string> sigs; strings_of(X->val_get(def, "sig"), sigs); strings_of(X->val_get(def, "signature"), sigs); try_sigs(sigs, "signature"); }
	if (!a) try_sigs(cache_sigs_any_build(m->name, name), "learned signature");
	// 6) string reference
	if (!a) {
		const XmodValue *xr = X->val_get(def, "xref");
		std::string text = X->val_type(xr) == XV_MAP ? str_of(X->val_get(xr, "string")) : str_of(xr);
		int64_t xi = 1;
		if (X->val_type(xr) == XV_MAP) int_of(X->val_get(xr, "index"), xi);
		if (!text.empty()) {
			auto refs = xref_string(*m, text, 16);
			if (refs.empty()) notes += "no function references \"" + text + "\"; ";
			else if (refs.size() > 1 && xi <= 0) notes += "several functions reference \"" + text + "\"; ";
			else if ((size_t)xi > refs.size() || !refs[xi - 1].func) notes += "xref index out of range / function start unknown; ";
			else { a = refs[xi - 1].func; f->how = "xref \"" + text + "\""; }
		}
	}
	if (!a) {
		f->why = notes.empty() ? "nothing to resolve it with (give sig, symbol, xref, address, offset or vindex)" : notes;
		err = "codex: cannot resolve '" + name + "' in " + m->name + ": " + f->why;
		return false;
	}
	if (f->how != "cache") {
		std::string e2;
		a = apply_post(a, adjust, follow, e2);
		if (!a) { f->why = e2; err = "codex: '" + name + "': " + e2; return false; }
	}
	if (!valid_code(*m, a)) {
		f->why = "resolved address is not executable code of " + m->name;
		err = "codex: '" + name + "': " + f->why;
		return false;
	}
	f->addr = a;
	f->resolved = true;
	if (f->how != "cache") {                       // remember it, with a signature generated for this build
		SigMade sm; std::string e2;
		std::string sig = sig_make(a, *m, sm, e2, 96) ? sm.text : "";
		if (sig.empty()) { uint8_t b[16]; if (mem_read(a, b, 16)) sig = hex_bytes(b, 16); }
		cache_put(*m, name, a - m->base, sig);
	}
	return true;
}

// ---------------------------------------------------------------------------------------------------------------
// calls
static uintptr_t resolve_target(Func &f, uintptr_t this_ptr, bool through_hooks, std::string &err)
{
	uintptr_t fn = f.addr;
	if (f.vindex >= 0) {
		uintptr_t vt = vtable_of(this_ptr);
		if (!vt) { err = "virtual call: 'this' has no readable vtable"; return 0; }
		if (!mem_read(vt + 4 * f.vindex, &fn, 4) || !mem_executable(fn)) { err = "virtual call: vtable slot " + std::to_string(f.vindex) + " is not code (wrong index?)"; return 0; }
	}
	if (!through_hooks)
		for (Hook *h : f.hooks) {
			if (!h->vtable && fn == f.addr) return h->original;           // detour: call the trampoline
			if (h->vtable && fn == (uintptr_t)h->stub) return h->original; // this object's class slot is hooked
		}
	return fn;
}

bool func_call(Func &f, const XmodValue *const *argv, int argc, bool through_hooks, XmodValue **ret, std::string &err)
{
	if (!f.resolved) { err = "'" + f.name + "' is not resolved: " + f.why; return false; }
	if (f.faulted) { err = "'" + f.name + "' crashed earlier (" + f.fault + ") - fix the definition and call codex.reset(\"" + f.name + "\")"; return false; }
	int want = (int)f.slots.size();
	if (argc != want) { err = "'" + f.name + "' expects " + std::to_string(want) + " arguments" + (f.has_this ? " (including this)" : "") + ", got " + std::to_string(argc); return false; }
	uint32_t words[64] = {0}, ecx = 0, edx = 0;
	std::deque<std::string> strs;
	std::deque<std::array<float, 3>> vecs;
	uintptr_t this_ptr = 0;
	for (int i = 0; i < want; ++i) {
		const ArgSlot &s = f.slots[i];
		uint32_t tmp[3] = {0, 0, 0};
		std::string e2;
		if (!val_to_words(s.t, argv[i], tmp, strs, vecs, e2)) { err = "'" + f.name + "' argument #" + std::to_string(i + 1) + ": " + e2; return false; }
		if (i == 0 && f.has_this) {
			this_ptr = tmp[0];
			if (!this_ptr) { err = "'" + f.name + "': 'this' is NULL / not a valid entity"; return false; }
		}
		if (s.reg == 1) ecx = tmp[0];
		else if (s.reg == 2) edx = tmp[0];
		else memcpy(&words[s.word], tmp, 4 * s.nwords);
	}
	uintptr_t fn = resolve_target(f, this_ptr, through_hooks, err);
	if (!fn) return false;
	CallCtx c;
	memset(&c, 0, sizeof c);
	c.fn = (void *)fn; c.nwords = f.stack_words; c.words = words; c.ecx = ecx; c.edx = edx;
	c.fpu = (f.ret == T_F32 || f.ret == T_F64) ? 1 : 0;
	f.calls++;
	if (g_safe_calls) {
		if (!guarded_call(&c)) {
			f.faulted = true;
			f.fault = fault_text(c);
			err = "'" + f.name + "' crashed: " + f.fault + " - the call was aborted, the server keeps running. Check the definition (args, conv, this).";
			cx_err("%s", err.c_str());
			return false;
		}
	} else {
		codex_raw_call(&c);
	}
	uint32_t rw[2] = {c.r_eax, c.r_edx};
	if (f.ret == T_F32 || f.ret == T_F64) *ret = g_xmod->val_num(c.r_st0);
	else *ret = native_to_val(f.ret, rw, 0);
	return true;
}

// ---------------------------------------------------------------------------------------------------------------
// trampolines: copy the first >= 5 bytes elsewhere, fixing everything that is position dependent
bool build_trampoline(uintptr_t fn, uint8_t *&tramp, int &stolen, std::string &err)
{
	if (!mem_readable(fn, 64)) { err = "function bytes are not readable"; return false; }
	const uint8_t *p = (const uint8_t *)fn;
	std::vector<Insn> ins;
	int total = 0;
	while (total < 5) {
		Insn in;
		if (!lde_decode(p + total, in)) { err = "cannot decode the instruction at +" + std::to_string(total); return false; }
		if (in.is_loop) { err = "loop/jecxz in the first bytes - use a vtable hook"; return false; }
		if ((in.rel_size && in.opsize16)) { err = "16-bit relative branch in the first bytes"; return false; }
		if (in.is_ret && total + in.len < 5) { err = "function is shorter than 5 bytes"; return false; }
		if (in.is_jmp && total + in.len < 5) { err = "unconditional jump inside the first 5 bytes - cannot detour safely"; return false; }
		ins.push_back(in);
		total += in.len;
	}
	// nothing may jump back into the stolen bytes (we look at the next instructions of the function)
	{
		int off = total;
		for (int k = 0; k < 48 && off < 512; ++k) {
			Insn in;
			if (!mem_readable(fn + off, 16) || !lde_decode(p + off, in)) break;
			if (in.rel_size) {
				uintptr_t t = in.target(fn + off);
				if (t > fn && t < fn + (uintptr_t)total) { err = "a later branch jumps into the first bytes - use a vtable hook"; return false; }
			}
			off += in.len;
			if (in.is_ret || (in.is_jmp && in.opcode == 0xE9)) break;
		}
	}
	uint8_t *t = (uint8_t *)exec_alloc(total * 4 + 16);
	if (!t) { err = "out of executable memory"; return false; }
	uint8_t *o = t;
	int off = 0;
	for (const Insn &in : ins) {
		uintptr_t at = fn + off;
		const uint8_t *src = p + off;
		if (in.rel_size) {
			uintptr_t target = in.target(at);
			if (target > fn && target < fn + (uintptr_t)total) { err = "a branch inside the first bytes points into them"; return false; }
			if (in.is_call) {
				uint8_t tb[4] = {0};
				mem_read(target, tb, 4);
				if (tb[0] == 0x8B && (tb[1] & 0xC7) == 0x04 && tb[2] == 0x24 && tb[3] == 0xC3) {
					// call __x86.get_pc_thunk.REG  ->  mov REG, <original return address>
					int reg = (tb[1] >> 3) & 7;
					*o++ = (uint8_t)(0xB8 + reg);
					uint32_t v = (uint32_t)(at + in.len); memcpy(o, &v, 4); o += 4;
				} else if (target == at + in.len) {
					// call $+5 (pop reg follows): push the original address instead
					*o++ = 0x68;
					uint32_t v = (uint32_t)(at + in.len); memcpy(o, &v, 4); o += 4;
				} else {
					*o++ = 0xE8;
					int32_t r = (int32_t)(target - ((uintptr_t)o + 4)); memcpy(o, &r, 4); o += 4;
				}
			} else if (in.is_jmp) {
				*o++ = 0xE9;
				int32_t r = (int32_t)(target - ((uintptr_t)o + 4)); memcpy(o, &r, 4); o += 4;
			} else {                                          // conditional jump -> 0F 8x rel32
				uint8_t cc = in.opcode >= 0x70 && in.opcode <= 0x7F ? (uint8_t)(in.opcode - 0x70) : (uint8_t)(in.opcode & 0x0F);
				*o++ = 0x0F; *o++ = (uint8_t)(0x80 + cc);
				int32_t r = (int32_t)(target - ((uintptr_t)o + 4)); memcpy(o, &r, 4); o += 4;
			}
		} else {
			memcpy(o, src, in.len);
			o += in.len;
		}
		off += in.len;
	}
	*o++ = 0xE9;
	int32_t r = (int32_t)((fn + total) - ((uintptr_t)o + 4));
	memcpy(o, &r, 4);
	tramp = t;
	stolen = total;
	return true;
}

// ---------------------------------------------------------------------------------------------------------------
// hooks
static uint8_t *make_stub(Hook *h)
{
	uint8_t *s = (uint8_t *)exec_alloc(16);
	if (!s) return nullptr;
	s[0] = 0xB8;                                        // mov eax, Hook*
	uint32_t hp = (uint32_t)(uintptr_t)h; memcpy(s + 1, &hp, 4);
	s[5] = 0xE9;                                        // jmp codex_hook_entry
	int32_t r = (int32_t)((uintptr_t)&codex_hook_entry - ((uintptr_t)s + 10)); memcpy(s + 6, &r, 4);
	return s;
}

static uintptr_t object_for_class(const std::string &cls, bool &temp_removed, std::string &err)
{
	temp_removed = false;
	edict_t *e = g_engfuncs.pfnFindEntityByString(nullptr, "classname", cls.c_str());
	if (e && !FNullEnt(e) && e->pvPrivateData) return (uintptr_t)e->pvPrivateData;
	e = g_engfuncs.pfnCreateNamedEntity(g_engfuncs.pfnAllocString(cls.c_str()));
	if (!e || FNullEnt(e) || !e->pvPrivateData) { err = "class '" + cls + "' does not exist in the game DLL"; return 0; }
	uintptr_t vt = vtable_of((uintptr_t)e->pvPrivateData);
	static uintptr_t holder;                            // the vtable outlives the entity; return a pointer to it
	holder = vt;
	g_engfuncs.pfnRemoveEntity(e);
	temp_removed = true;
	return (uintptr_t)&holder;
}

bool hook_add(Func &f, const XmodValue *opts, std::string &err)
{
	const XmodApi *X = g_xmod;
	if (!f.resolved) { err = "'" + f.name + "' is not resolved: " + f.why; return false; }
	std::string cls = opts ? str_of(X->val_get(opts, "class")) : "";
	int64_t ent = -1, objp = 0;
	if (opts) { int_of(X->val_get(opts, "entity"), ent); int_of(X->val_get(opts, "object"), objp); }
	bool vt = f.vindex >= 0;
	if (vt) {
		if (cls.empty() && ent < 0 && objp <= 0) { err = "'" + f.name + "' is virtual: hook it with {class = \"weapon_ak47\"}, {entity = id} or {object = ptr}"; return false; }
		uintptr_t obj = 0; bool temp = false;
		if (objp > 0) { obj = (uintptr_t)objp; if (cls.empty()) { char b[24]; snprintf(b, sizeof b, "object@%08x", (unsigned)obj); cls = b; } }
		else if (ent >= 0) { obj = ent_to_cbase((int)ent); if (!obj) { err = "entity " + std::to_string(ent) + " is not valid"; return false; } if (cls.empty()) cls = "entity#" + std::to_string(ent); }
		else { obj = object_for_class(cls, temp, err); if (!obj) return false; }
		uintptr_t table = vtable_of(obj);
		if (!table) { err = "no vtable for " + cls; return false; }
		uintptr_t slot = table + 4 * (uintptr_t)f.vindex, old = 0;
		if (!mem_read(slot, &old, 4) || !mem_executable(old)) { err = "vtable slot " + std::to_string(f.vindex) + " of " + cls + " is not code (wrong vindex?)"; return false; }
		for (Hook *h : f.hooks) if (h->vtable && h->target == slot) return true;   // already hooked (same vtable)
		for (Func *g : s_order) for (Hook *h : g->hooks) if (h->vtable && h->target == slot) { err = "that vtable slot is already hooked by '" + g->name + "'"; return false; }
		Hook *h = new Hook();
		h->f = &f; h->vtable = true; h->target = slot; h->original = old; h->label = cls;
		h->stub = make_stub(h);
		uint32_t nv = (uint32_t)(uintptr_t)h->stub;
		std::vector<uint8_t> bytes((uint8_t *)&nv, (uint8_t *)&nv + 4), expect((uint8_t *)&old, (uint8_t *)&old + 4);
		h->patch_id = h->stub ? patch_apply(slot, bytes, &expect, ("codex vhook " + f.name).c_str()) : 0;
		if (!h->patch_id) { err = "could not patch the vtable: " + cx_last_error(); delete h; return false; }
		f.hooks.push_back(h);
		return true;
	}
	for (Hook *h : f.hooks) if (!h->vtable) return true;      // already detoured
	uint8_t *tramp = nullptr; int stolen = 0;
	if (!build_trampoline(f.addr, tramp, stolen, err)) { err = "cannot detour '" + f.name + "': " + err; return false; }
	Hook *h = new Hook();
	h->f = &f; h->target = f.addr; h->original = (uintptr_t)tramp;
	h->stub = make_stub(h);
	if (!h->stub) { err = "out of executable memory"; delete h; return false; }
	std::vector<uint8_t> bytes(stolen, 0x90), expect(stolen);
	bytes[0] = 0xE9;
	int32_t r = (int32_t)((uintptr_t)h->stub - (f.addr + 5)); memcpy(&bytes[1], &r, 4);
	memcpy(expect.data(), (const void *)f.addr, stolen);
	h->patch_id = patch_apply(f.addr, bytes, &expect, ("codex hook " + f.name).c_str());
	if (!h->patch_id) { err = "could not write the detour: " + cx_last_error(); delete h; return false; }
	f.hooks.push_back(h);
	return true;
}

bool hook_remove(Func &f, const char *label)
{
	bool any = false;
	for (size_t i = f.hooks.size(); i-- > 0;) {
		Hook *h = f.hooks[i];
		if (label && *label && h->label != label) continue;
		patch_remove(h->patch_id);
		h->removed = true;                                 // stub/trampoline stay allocated: a call may still be inside them
		f.hooks.erase(f.hooks.begin() + i);
		any = true;
	}
	return any;
}

void hook_remove_all()
{
	for (Func *f : s_order) hook_remove(*f, nullptr);
}

// ---------------------------------------------------------------------------------------------------------------
// the dispatcher (called by codex_hook_entry)
struct HookFrame {
	Func *f; Hook *h;
	uint32_t words[64]; int nwords;
	uint32_t ecx, edx;
	std::deque<std::string> strs;
	std::deque<std::array<float, 3>> vecs;
	bool has_ret = false, ret_set = false, post = false;
	uint32_t ret[2] = {0, 0}; double ret_st0 = 0;
};
static std::vector<HookFrame *> s_frames;
HookFrame *frame_top() { return s_frames.empty() ? nullptr : s_frames.back(); }

static uint32_t *slot_words(HookFrame &fr, const ArgSlot &s)
{
	if (s.reg == 1) return &fr.ecx;
	if (s.reg == 2) return &fr.edx;
	return &fr.words[s.word];
}

static XmodValue *frame_values(HookFrame &fr, bool with_ret)
{
	const XmodApi *X = g_xmod;
	XmodValue *a = X->val_array();
	for (const ArgSlot &s : fr.f->slots) X->val_push(a, native_to_val(s.t, slot_words(fr, s), 0));
	if (with_ret && fr.f->ret != T_VOID) {
		if (fr.f->ret == T_F32 || fr.f->ret == T_F64) X->val_push(a, X->val_num(fr.ret_st0));
		else X->val_push(a, native_to_val(fr.f->ret, fr.ret, 0));
	}
	return a;
}

static void set_from_number(HookFrame &fr, const ArgSlot &s, double v)
{
	uint32_t *w = slot_words(fr, s);
	switch (s.t) {
	case T_F32: { float f = (float)v; memcpy(w, &f, 4); break; }
	case T_F64: memcpy(w, &v, 8); break;
	case T_I64: { int64_t q = (int64_t)v; memcpy(w, &q, 8); break; }
	case T_CBASE: w[0] = (uint32_t)ent_to_cbase((int)v); break;
	case T_EDICT: { edict_t *e = edict_of((int)v); w[0] = (uint32_t)(uintptr_t)e; break; }
	case T_ENTVARS: { edict_t *e = edict_of((int)v); w[0] = e ? (uint32_t)(uintptr_t)&e->v : 0; break; }
	case T_STR: case T_ASTR: case T_VECPTR: case T_VECVAL: break;   // not numeric: use codex.set_arg
	default: w[0] = (uint32_t)(int64_t)v; break;
	}
}

static void set_ret_number(HookFrame &fr, double v)
{
	Func &f = *fr.f;
	fr.ret[0] = fr.ret[1] = 0; fr.ret_st0 = 0;
	switch (f.ret) {
	case T_F32: case T_F64: fr.ret_st0 = v; break;
	case T_I64: { int64_t q = (int64_t)v; memcpy(fr.ret, &q, 8); break; }
	case T_CBASE: fr.ret[0] = (uint32_t)ent_to_cbase((int)v); break;
	case T_EDICT: fr.ret[0] = (uint32_t)(uintptr_t)edict_of((int)v); break;
	case T_ENTVARS: { edict_t *e = edict_of((int)v); fr.ret[0] = e ? (uint32_t)(uintptr_t)&e->v : 0; break; }
	default: fr.ret[0] = (uint32_t)(int64_t)v; break;
	}
	fr.ret_set = true;
}

extern "C" __attribute__((used, visibility("hidden"))) void codex_hook_dispatch(Hook *h, uint32_t *sargs, uint32_t ecx, uint32_t edx, HookRet *out)
{
	Func &f = *h->f;
	HookFrame fr;
	fr.f = &f; fr.h = h; fr.ecx = ecx; fr.edx = edx;
	fr.nwords = f.stack_words;
	memcpy(fr.words, sargs, 4 * fr.nwords);
	bool is_float = f.ret == T_F32 || f.ret == T_F64;
	out->pop = f.pop_bytes; out->fpu = is_float ? 1 : 0;
	f.hooked_calls++;

	const XmodApi *X = g_xmod;
	bool main = X && g_xext && X->is_main_thread() && !h->removed && h->depth < 8;
	bool pre = main && g_sync_hooks && X->has_handlers(f.ev_pre.c_str());
	bool post = main && g_sync_hooks && X->has_handlers(f.ev_post.c_str());
	h->depth++;
	s_frames.push_back(&fr);
	bool supercede = false;
	if (pre) {
		XmodValue *a = frame_values(fr, false);
		XmodHookOut o;
		X->fire_hook_sync(f.ev_pre.c_str(), a, &o);
		X->val_free(a);
		int base = f.has_this ? 1 : 0;
		for (int i = 0; i < 16; ++i)
			if ((o.set_mask & (1u << i)) && base + i < (int)f.slots.size()) set_from_number(fr, f.slots[base + i], o.set_val[i]);
		if (o.code == 1 /* HC_SUPERCEDE */) {
			supercede = true;
			if (o.has_num) set_ret_number(fr, o.num);
			else if (o.has_str && f.ret == T_STR) { h->ret_str = o.str; fr.ret[0] = (uint32_t)(uintptr_t)h->ret_str.c_str(); fr.ret_set = true; }
			else if (o.has_str && (f.ret == T_ASTR || f.ret == T_STRT)) { uint32_t st = pool_alloc(o.str); fr.ret[0] = f.ret == T_STRT ? st : (uint32_t)pool_ptr(st); fr.ret_set = true; }
		}
	}
	if (!supercede) {
		CallCtx c;
		memset(&c, 0, sizeof c);
		c.fn = (void *)h->original; c.nwords = fr.nwords; c.words = fr.words; c.ecx = fr.ecx; c.edx = fr.edx; c.fpu = is_float ? 1 : 0;
		codex_raw_call(&c);
		fr.ret[0] = c.r_eax; fr.ret[1] = c.r_edx; fr.ret_st0 = c.r_st0;
		fr.ret_set = false;
	}
	fr.has_ret = true;
	if (post) {
		fr.post = true;
		XmodValue *a = frame_values(fr, true);
		XmodHookOut o;
		X->fire_hook_sync(f.ev_post.c_str(), a, &o);
		X->val_free(a);
		if (o.code == 1 && o.has_num) set_ret_number(fr, o.num);
		else if (o.code == 1 && o.has_str && f.ret == T_STR) { h->ret_str = o.str; fr.ret[0] = (uint32_t)(uintptr_t)h->ret_str.c_str(); }
		else if (o.code == 1 && o.has_str && (f.ret == T_ASTR || f.ret == T_STRT)) { uint32_t st = pool_alloc(o.str); fr.ret[0] = f.ret == T_STRT ? st : (uint32_t)pool_ptr(st); }
	} else if (main && !g_sync_hooks) {
		// xmod < 0.4.1: observe only (next frame, cannot change anything)
		X->emit_event(f.ev_post.c_str(), frame_values(fr, true));
	}
	s_frames.pop_back();
	h->depth--;
	out->eax = fr.ret[0]; out->edx = fr.ret[1]; out->st0 = fr.ret_st0;
}

// ---------------------------------------------------------------------------------------------------------------
// handler helpers
static const ArgSlot *frame_slot(HookFrame *fr, int n, std::string &err)
{
	int idx = n == 0 ? 0 : n - 1 + (fr->f->has_this ? 1 : 0);
	if (n == 0 && !fr->f->has_this) { err = "this function has no 'this'"; return nullptr; }
	if (n < 0 || idx >= (int)fr->f->slots.size()) { err = "argument index out of range"; return nullptr; }
	return &fr->f->slots[idx];
}

XmodValue *frame_arg(HookFrame *fr, int n, std::string &err)
{
	const ArgSlot *s = frame_slot(fr, n, err);
	return s ? native_to_val(s->t, slot_words(*fr, *s), 0) : nullptr;
}

bool frame_set_arg(HookFrame *fr, int n, const XmodValue *v, std::string &err)
{
	if (fr->has_ret) { err = "set_arg only works in a pre-hook (the original already ran)"; return false; }
	if (n == 0) { err = "cannot replace 'this'"; return false; }
	const ArgSlot *s = frame_slot(fr, n, err);
	if (!s) return false;
	uint32_t tmp[3] = {0, 0, 0};
	if (!val_to_words(s->t, v, tmp, fr->strs, fr->vecs, err)) return false;
	memcpy(slot_words(*fr, *s), tmp, 4 * s->nwords);
	return true;
}

XmodValue *frame_ret(HookFrame *fr, std::string &err)
{
	if (!fr->has_ret && !fr->ret_set) { err = "the return value is only known in a post-hook (or after codex.set_ret)"; return nullptr; }
	if (fr->f->ret == T_F32 || fr->f->ret == T_F64) return g_xmod->val_num(fr->ret_st0);
	return native_to_val(fr->f->ret, fr->ret, 0);
}

bool frame_set_ret(HookFrame *fr, const XmodValue *v, std::string &err)
{
	Func &f = *fr->f;
	if (f.ret == T_VOID) { err = "the function returns void"; return false; }
	if (f.ret == T_VECPTR && g_xmod->val_type(v) == XV_MAP) { err = "cannot return a new Vector* from a hook (it would point to freed memory) - pass a raw pointer number or change the vector the original returned"; return false; }
	if (f.ret == T_F32 || f.ret == T_F64) { fr->ret_st0 = g_xmod->val_get_num(v, 0); fr->ret_set = true; return true; }
	if (f.ret == T_STR) {
		size_t len = 0; const char *s = g_xmod->val_get_str(v, &len);
		if (!s) { fr->ret[0] = 0; fr->ret_set = true; return true; }
		fr->h->ret_str.assign(s, len);
		fr->ret[0] = (uint32_t)(uintptr_t)fr->h->ret_str.c_str();
		fr->ret_set = true;
		return true;
	}
	uint32_t tmp[3] = {0, 0, 0};
	if (!val_to_words(f.ret, v, tmp, fr->strs, fr->vecs, err)) return false;
	fr->ret[0] = tmp[0]; fr->ret[1] = tmp[1];
	fr->ret_set = true;
	return true;
}
