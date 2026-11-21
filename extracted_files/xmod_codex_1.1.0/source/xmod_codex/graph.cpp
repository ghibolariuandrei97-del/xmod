// graph.cpp - code analysis: what a function calls (callees), who calls it (callers), call trees, and treemaps:
// fingerprints of a function together with the functions it calls (the Okapi idea), used to find the same function
// again in another build of the game when signatures break.
//
// Everything reads the code as the game shipped it (our own hooks and patches are undone in the copy) and never
// follows anything outside the executable sections of the module.
#include "codex.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <unordered_map>

const char *call_kind_name(int k)
{
	switch (k) {
	case CK_CALL: return "call";
	case CK_TAIL: return "jmp";
	case CK_INDIRECT: return "indirect";
	case CK_VCALL: return "virtual";
	case CK_IMPORT: return "import";
	case CK_REF: return "ref";
	case CK_PTR: return "ptr";
	}
	return "?";
}

static inline uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

// ---------------------------------------------------------------------------------------------------------------
// reading code: raw bytes, or the original bytes where we have a patch/hook
struct CodeReader {
	Module &m;
	std::vector<std::pair<uintptr_t, uintptr_t>> pr;
	explicit CodeReader(Module &mm) : m(mm) { patch_ranges(m.base, m.base + m.size, pr); }
	bool exec(uintptr_t a) const { const Segment *s = m.seg_of(a); return s && s->x && s->r; }
	// 16 bytes at a (bytes past the segment end read as int3)
	bool get(uintptr_t a, uint8_t *buf) const
	{
		const Segment *s = m.seg_of(a);
		if (!s || !s->r) return false;
		size_t n = (size_t)std::min<uintptr_t>(16, s->hi - a);
		memset(buf, 0xCC, 16);
		for (auto &r : pr) if (a < r.second && r.first < a + n) return mem_read_orig(a, buf, n);
		memcpy(buf, (const void *)a, n);
		return true;
	}
};

static bool read_u32(uintptr_t a, uint32_t &v) { return mem_read(a, &v, 4); }

// a C string the code refers to: at least 3 printable characters, NUL terminated, in a non-executable section
static bool is_string_at(Module &m, uintptr_t a)
{
	const Segment *s = m.seg_of(a);
	if (!s || !s->r || s->x) return false;
	const uint8_t *p = (const uint8_t *)a;
	size_t n = 0;
	while (a + n < s->hi && n < 512) {
		uint8_t c = p[n];
		if (c == 0) break;
		if (c < 0x20 && c != '\n' && c != '\r' && c != '\t') return false;
		++n;
	}
	return n >= 3 && a + n < s->hi;
}

// a stub that only jumps through a pointer: PLT entry (jmp [ebx+disp] / jmp [abs]) or an import thunk (jmp [IAT]).
// to = the function it reaches (0 if unknown), name = imported symbol name when known
static bool stub_target(Module &m, const CodeReader &rd, uintptr_t a, uintptr_t &to, std::string *name)
{
	uint8_t b[16];
	if (!rd.get(a, b)) return false;
	uintptr_t slot = 0;
	if (b[0] == 0xFF && b[1] == 0x25) slot = rd32(b + 2);
	else if (b[0] == 0xFF && b[1] == 0xA3 && m.got) slot = m.got + (int32_t)rd32(b + 2);
	else return false;
	to = 0;
	auto it = m.slots.find(slot);
	if (it != m.slots.end()) { to = it->second.to; if (name) *name = it->second.name; }
	uint32_t v;
	if (!to && read_u32(slot, v) && !(v >= a && v < a + 16) && mem_executable(v)) to = v;   // already bound
	return true;
}

// __x86.get_pc_thunk.*: mov reg, [esp]; ret  (PIC noise, not a real callee)
static bool is_get_pc_thunk(const CodeReader &rd, uintptr_t a)
{
	uint8_t b[16];
	if (!rd.get(a, b)) return false;
	return b[0] == 0x8B && (b[1] & 0xC7) == 0x04 && b[2] == 0x24 && b[3] == 0xC3;
}

// ---------------------------------------------------------------------------------------------------------------
// per-module index: every function start we can see (direct call targets, symbols, code pointers in data)
struct ModIndex {
	uintptr_t base = 0, size = 0;
	bool built = false;
	std::vector<uintptr_t> starts;               // sorted
	std::set<uintptr_t> call_targets;
	struct Node { bool ok = false; uint32_t n = 0, c = 0, b = 0; std::vector<uint32_t> s, k; std::vector<uintptr_t> kids; };
	std::unordered_map<uintptr_t, Node> nodes;
};
static std::map<std::string, ModIndex> s_idx;
void graph_forget() { s_idx.clear(); }

static ModIndex &index_of(Module &m)
{
	ModIndex &ix = s_idx[m.name];
	if (ix.base != m.base || ix.size != m.size) { ix = ModIndex(); ix.base = m.base; ix.size = m.size; }
	return ix;
}

static void build_starts(Module &m, ModIndex &ix)
{
	if (ix.built) return;
	ix.built = true;
	mod_load_symbols(m);
	CodeReader rd(m);
	std::set<uintptr_t> st;
	for (const Segment &s : m.segs) {
		if (!s.x || !s.r || s.hi - s.lo < 8 || !mem_readable(s.lo, s.hi - s.lo)) continue;
		const uint8_t *p = (const uint8_t *)s.lo;
		size_t n = s.hi - s.lo;
		for (size_t i = 0; i + 5 <= n; ++i) {
			if (p[i] != 0xE8) continue;
			uintptr_t t = s.lo + i + 5 + (int32_t)rd32(p + i + 1);
			if (t >= s.lo && t < s.hi && t != s.lo + i + 5) ix.call_targets.insert(t);
		}
	}
	for (uintptr_t t : ix.call_targets) {                     // keep targets whose first instruction decodes
		uint8_t b[16]; Insn in;
		if (rd.get(t, b) && lde_decode(b, in) && in.valid && b[0] != 0xCC && b[0] != 0x00) st.insert(t);
	}
	for (auto &sy : m.syms) if (sy.func && m.contains(sy.addr)) st.insert(sy.addr);
	for (const Segment &s : m.segs) {                         // vtables / function pointer tables
		if (s.x || !s.r || !mem_readable(s.lo, s.hi - s.lo)) continue;
		for (uintptr_t a = (s.lo + 3) & ~(uintptr_t)3; a + 4 <= s.hi; a += 4) {
			uint32_t v = *(const uint32_t *)a;
			if (!rd.exec(v)) continue;
			uint8_t b[16]; Insn in;
			if (rd.get(v, b) && lde_decode(b, in) && in.valid && b[0] != 0xCC && b[0] != 0x00) st.insert(v);
		}
	}
	ix.starts.assign(st.begin(), st.end());
}

// ---------------------------------------------------------------------------------------------------------------
// function body: recursive descent from the start (conditional and short jumps are followed, switch tables read)
bool analyze_func(Module &m, uintptr_t start, FuncBody &out, std::string &err, int max_insns)
{
	out = FuncBody();
	out.start = start;
	CodeReader rd(m);
	if (!rd.exec(start)) { err = "address is not executable code of " + m.name; return false; }
	mod_load_symbols(m);
	uintptr_t lo = start, hi = 0;
	if (const Module::Sym *sy = sym_containing(m, start)) {
		if (sy->addr == start && sy->size > 0) { hi = start + sy->size; out.bounded = true; }
	}
	ModIndex &ix = index_of(m);
	if (!ix.built) build_starts(m, ix);
	auto is_func_start = [&](uintptr_t t) { return ix.call_targets.count(t) || std::binary_search(ix.starts.begin(), ix.starts.end(), t); };
	auto intra = [&](uintptr_t t) {
		if (!rd.exec(t) || t < lo) return false;
		if (out.bounded) return t < hi;
		return t < start + 0x10000 && !(t != start && is_func_start(t));
	};

	std::set<uintptr_t> seen, consts_set, strings_set;
	std::vector<uintptr_t> work{start};
	uintptr_t pic_table = 0;                 // gcc PIC switch: mov r, [ebx + idx*4 + table@GOTOFF]; add r, ebx; jmp r
	uintptr_t maxend = start;
	while (!work.empty()) {
		uintptr_t a = work.back(); work.pop_back();
		for (;;) {
			if (seen.count(a)) break;
			if (!rd.exec(a) || a < lo || (out.bounded && a >= hi)) break;
			uint8_t b[16]; Insn in;
			if (!rd.get(a, b) || !lde_decode(b, in) || !in.valid) break;
			if (b[0] == 0xCC) break;
			seen.insert(a);
			if (++out.insns > max_insns) { out.truncated = true; work.clear(); break; }
			if (a + in.len > maxend) maxend = a + in.len;
			const uint8_t *op = b + in.prefixes;
			// operands: constants and strings
			if (in.imm_size) {
				uint32_t v = (uint32_t)in.imm;
				bool esp_op = in.has_modrm && in.mod == 3 && in.rm == 4;   // add/sub esp, n (frame size: changes between builds)
				if (m.got && op[0] == 0x81 && in.has_modrm && in.mod == 3 && in.rm == 3 && in.reg == 0) esp_op = true;   // PIC: add ebx, GOT offset
				if (is_string_at(m, v)) strings_set.insert(v);
				else if (!esp_op && !m.contains(v) && !looks_like_address(v) && (int32_t)v != 0 && (int32_t)v != 1 && (int32_t)v != -1) consts_set.insert(v);
			}
			if (in.disp_size == 4) {
				uintptr_t v = (uint32_t)in.disp;
				bool ebx_base = in.has_modrm && in.mod != 3 && ((in.rm == 3) || (in.rm == 4 && in.sib_base == 3));
				if (in.disp_abs && is_string_at(m, v)) strings_set.insert(v);
				else if (ebx_base && m.got && is_string_at(m, m.got + (int32_t)in.disp)) strings_set.insert(m.got + (int32_t)in.disp);
			}
			if (op[0] == 0x8B && in.has_modrm && in.mod != 3 && in.rm == 4 && in.sib_base == 3 && in.disp_size == 4 && m.got && (b[in.prefixes + 2] >> 6) == 2)
				pic_table = m.got + (int32_t)in.disp;
			if (in.is_ret) break;
			if (in.is_call && in.rel_size) {
				uintptr_t t = in.target(a);
				if (t != a + in.len && !is_get_pc_thunk(rd, t)) {
					CallSite cs; cs.at = a; cs.to = t; cs.kind = CK_CALL;
					uintptr_t st; std::string nm;
					if (stub_target(m, rd, t, st, &nm)) { cs.kind = CK_IMPORT; cs.slot = t; cs.to = st; cs.name = nm; }
					out.calls.push_back(cs);
				}
				a += in.len; continue;
			}
			if (op[0] == 0xFF && in.has_modrm && (in.reg == 2 || in.reg == 3)) {      // call [mem] / call reg
				CallSite cs; cs.at = a; cs.kind = CK_INDIRECT;
				if (in.mod != 3 && in.disp_abs) {                                    // call [abs]: IAT / function pointer
					cs.slot = (uint32_t)in.disp; uint32_t v;
					if (read_u32(cs.slot, v) && mem_executable(v)) { cs.to = v; cs.kind = CK_IMPORT; }
				} else if (in.mod != 3 && in.rm == 3 && m.got) {                     // call [ebx+disp]: through the GOT (PIC)
					cs.slot = m.got + (in.disp_size ? in.disp : 0);
					auto it = m.slots.find(cs.slot);
					if (it != m.slots.end()) { cs.to = it->second.to; cs.name = it->second.name; cs.kind = CK_IMPORT; }
				} else if (in.mod != 3 && in.rm != 4 && in.rm != 5) {
					int32_t d = in.disp_size ? in.disp : 0;                           // call [reg+disp]: virtual call
					if (d >= 0 && d < 0x2000 && (d & 3) == 0) { cs.kind = CK_VCALL; cs.vindex = d / 4; }
				}
				out.calls.push_back(cs);
				a += in.len; continue;
			}
			if (op[0] == 0xFF && in.has_modrm && (in.reg == 4 || in.reg == 5)) {      // jmp [mem] / jmp reg
				if (in.mod == 0 && in.rm == 4 && in.disp_abs) {                       // jmp [idx*4 + table]: switch
					uint8_t sib = b[in.prefixes + 2];
					if ((sib >> 6) == 2) {
						uintptr_t tab = (uint32_t)in.disp;
						for (int k = 0; k < 1024; ++k) {
							uint32_t v;
							if (!read_u32(tab + 4 * k, v) || !intra(v)) break;
							work.push_back(v);
						}
					}
				} else if (in.mod == 3 && in.reg == 4 && pic_table) {                 // jmp reg after a PIC table load
					for (int k = 0; k < 1024; ++k) {
						uint32_t v;
						if (!read_u32(pic_table + 4 * k, v)) break;
						uintptr_t t = m.got + (int32_t)v;
						if (!intra(t)) break;
						work.push_back(t);
					}
					pic_table = 0;
				} else if (in.mod == 0 && in.rm == 5) {                               // jmp [abs]: tail call through a pointer
					CallSite cs; cs.at = a; cs.kind = CK_IMPORT; cs.slot = (uint32_t)in.disp; uint32_t v;
					if (read_u32(cs.slot, v) && mem_executable(v)) cs.to = v;
					out.calls.push_back(cs);
				}
				break;
			}
			if (in.is_jcc || in.is_loop) {
				++out.branches;
				uintptr_t t = in.target(a);
				if (intra(t)) work.push_back(t);
				a += in.len; continue;
			}
			if (in.is_jmp) {
				++out.branches;
				uintptr_t t = in.target(a);
				if (op[0] == 0xEB ? (rd.exec(t) && t >= lo && (!out.bounded || t < hi)) : intra(t)) { a = t; continue; }
				CallSite cs; cs.at = a; cs.to = t; cs.kind = CK_TAIL;
				uintptr_t st; std::string nm;
				if (stub_target(m, rd, t, st, &nm)) { cs.kind = CK_IMPORT; cs.slot = t; cs.to = st; cs.name = nm; }
				out.calls.push_back(cs);
				break;
			}
			a += in.len;
		}
	}
	if (out.insns == 0) { err = "no instruction decodes at this address"; return false; }
	out.end = out.bounded ? hi : maxend;
	std::sort(out.calls.begin(), out.calls.end(), [](const CallSite &x, const CallSite &y) { return x.at < y.at; });
	out.consts.assign(consts_set.begin(), consts_set.end());
	out.strings.assign(strings_set.begin(), strings_set.end());
	return true;
}

std::vector<uintptr_t> direct_callees(Module &m, const FuncBody &b)
{
	std::vector<uintptr_t> r;
	std::set<uintptr_t> seen;
	for (auto &c : b.calls) {
		if ((c.kind != CK_CALL && c.kind != CK_TAIL && c.kind != CK_IMPORT) || !c.to || !m.contains(c.to)) continue;
		if (seen.insert(c.to).second) r.push_back(c.to);
	}
	return r;
}

// ---------------------------------------------------------------------------------------------------------------
// callers: every call / jmp (also through PLT stubs and import thunks) that reaches `target`, plus code pointers
std::vector<CallerRef> find_callers(Module &m, uintptr_t target, size_t max, bool refs)
{
	mod_load_symbols(m);
	CodeReader rd(m);
	std::set<uintptr_t> aliases{target};
	std::vector<CallerRef> out;
	std::set<uintptr_t> sites;
	// PLT stubs / import thunks that lead to the target
	for (const Segment &s : m.segs) {
		if (!s.x || !s.r || !mem_readable(s.lo, s.hi - s.lo)) continue;
		const uint8_t *p = (const uint8_t *)s.lo;
		for (size_t i = 0; i + 6 <= s.hi - s.lo; ++i) {
			if (p[i] != 0xFF || (p[i + 1] != 0x25 && p[i + 1] != 0xA3)) continue;
			uintptr_t to;
			if (stub_target(m, rd, s.lo + i, to, nullptr) && to == target) aliases.insert(s.lo + i);
		}
	}
	std::set<uintptr_t> slots;                                   // GOT slots that hold the target (-fno-plt code calls through them)
	for (auto &kv : m.slots) if (kv.second.to == target) slots.insert(kv.first);
	for (int round = 0; round < 2; ++round) {
		std::set<uintptr_t> new_aliases;
		for (const Segment &s : m.segs) {
			if (!s.x || !s.r || !mem_readable(s.lo, s.hi - s.lo)) continue;
			const uint8_t *p = (const uint8_t *)s.lo;
			size_t n = s.hi - s.lo;
			for (size_t i = 0; i + 5 <= n; ++i) {
				uint8_t o = p[i];
				uintptr_t at = s.lo + i;
				if (o == 0xE8 || o == 0xE9) {
					uintptr_t t = at + 5 + (int32_t)rd32(p + i + 1);
					if (!aliases.count(t) || sites.count(at) || aliases.count(at)) continue;
					CallerRef r; r.at = at; r.kind = o == 0xE8 ? CK_CALL : CK_TAIL;
					r.func = func_start(m, at, &r.exact);
					// an incremental-link thunk (jmp at a function start) is an alias, not a caller
					if (o == 0xE9 && r.func == at) { new_aliases.insert(at); continue; }
					sites.insert(at); out.push_back(r);
				} else if (o == 0xFF && i + 6 <= n && (p[i + 1] == 0x15 || p[i + 1] == 0x93 || p[i + 1] == 0xA3 || p[i + 1] == 0x25)) {
					// call/jmp [abs] or [ebx+disp] (GOT) whose slot holds the target
					uintptr_t slot = (p[i + 1] == 0x93 || p[i + 1] == 0xA3) ? (m.got ? m.got + (int32_t)rd32(p + i + 2) : 0) : rd32(p + i + 2);
					if (!slot || sites.count(at) || aliases.count(at)) continue;
					uint32_t v = 0;
					bool hit = slots.count(slot) || (read_u32(slot, v) && aliases.count(v));
					if (!hit) continue;
					bool is_jmp = p[i + 1] == 0xA3 || p[i + 1] == 0x25;
					CallerRef r; r.at = at; r.kind = is_jmp ? CK_TAIL : CK_INDIRECT; r.func = func_start(m, at, &r.exact);
					if (is_jmp && r.func == at) { new_aliases.insert(at); continue; }   // a PLT stub / import thunk
					sites.insert(at); out.push_back(r);
				}
			}
		}
		if (new_aliases.empty()) break;
		aliases.insert(new_aliases.begin(), new_aliases.end());
	}
	if (refs) {
		uint32_t want[2] = {(uint32_t)target, m.got ? (uint32_t)(target - m.got) : 0};
		for (const Segment &s : m.segs) {
			if (!s.r || !mem_readable(s.lo, s.hi - s.lo)) continue;
			const uint8_t *p = (const uint8_t *)s.lo;
			size_t n = s.hi - s.lo;
			for (size_t i = 0; i + 4 <= n; ++i) {
				if (!s.x && (i & 3)) continue;                         // data: aligned pointers (vtables, tables)
				uint32_t v = rd32(p + i);
				bool hit = v == want[0] || (s.x && m.got && v == want[1]);
				if (!hit) continue;
				uintptr_t at = s.lo + i;
				if (s.x) {                                             // push offset f / mov reg, offset f / lea reg, [ebx+f@GOTOFF]
					bool dup = false;
					for (auto &r : out) if (r.at < at && at < r.at + 6) dup = true;
					if (dup) continue;
					CallerRef r; r.at = at; r.kind = CK_REF; r.func = func_start(m, at, &r.exact); out.push_back(r);
				} else {
					CallerRef r; r.at = at; r.kind = CK_PTR; out.push_back(r);
				}
			}
		}
	}
	std::sort(out.begin(), out.end(), [](const CallerRef &a, const CallerRef &b) { return a.at < b.at; });
	if (out.size() > max) out.resize(max);
	return out;
}

std::string sym_name_at(Module &m, uintptr_t a, uintptr_t *off)
{
	if (off) *off = 0;
	mod_load_symbols(m);
	if (const Module::Sym *s = sym_containing(m, a)) {
		if (off) *off = a - s->addr;
		return demangle(s->name);
	}
	for (Func *f : func_all()) if (f->resolved && f->addr && a == f->addr) return f->name;
	return "";
}

// ---------------------------------------------------------------------------------------------------------------
// treemaps
static const char *B36 = "0123456789abcdefghijklmnopqrstuvwxyz";
static std::string b36(uint32_t v) { std::string r; do { r += B36[v % 36]; v /= 36; } while (v); std::reverse(r.begin(), r.end()); return r; }
static uint32_t h20(const void *p, size_t n) { return fnv1a(p, n) >> 12; }

static ModIndex::Node &node_of(Module &m, uintptr_t f)
{
	ModIndex &ix = index_of(m);
	auto it = ix.nodes.find(f);
	if (it != ix.nodes.end()) return it->second;
	ModIndex::Node &nd = ix.nodes[f];
	FuncBody b; std::string e;
	if (!analyze_func(m, f, b, e, 6000)) return nd;
	nd.ok = true;
	nd.n = (uint32_t)b.insns; nd.b = (uint32_t)b.branches; nd.c = (uint32_t)b.calls.size();
	for (uintptr_t s : b.strings) { std::string t; mem_cstr(s, t, 256); nd.s.push_back(h20(t.data(), t.size())); }
	for (uint32_t k : b.consts) nd.k.push_back(h20(&k, 4));
	std::sort(nd.s.begin(), nd.s.end()); nd.s.erase(std::unique(nd.s.begin(), nd.s.end()), nd.s.end());
	std::sort(nd.k.begin(), nd.k.end()); nd.k.erase(std::unique(nd.k.begin(), nd.k.end()), nd.k.end());
	if (nd.s.size() > 24) nd.s.resize(24);
	if (nd.k.size() > 24) nd.k.resize(24);
	for (uintptr_t c : direct_callees(m, b)) { if (nd.kids.size() >= 16) break; nd.kids.push_back(c); }
	return nd;
}

static const size_t KIDS[] = {16, 16, 8, 4};

static void enc(Module &m, uintptr_t f, int depth, int level, std::string &out, std::set<uintptr_t> &path)
{
	ModIndex::Node nd = node_of(m, f);     // copy: node_of may rehash
	out += '(' + b36(nd.n) + ',' + b36(nd.c) + ',' + b36(nd.b) + ',';
	for (size_t i = 0; i < nd.s.size(); ++i) { if (i) out += '.'; out += b36(nd.s[i]); }
	out += ',';
	for (size_t i = 0; i < nd.k.size(); ++i) { if (i) out += '.'; out += b36(nd.k[i]); }
	if (depth > 0 && !path.count(f)) {
		path.insert(f);
		size_t cap = KIDS[std::min(level + 1, 3)];
		for (size_t i = 0; i < nd.kids.size() && i < cap; ++i) enc(m, nd.kids[i], depth - 1, level + 1, out, path);
		path.erase(f);
	}
	out += ')';
}

std::string treemap_make(Module &m, uintptr_t f, int depth, std::string &err)
{
	CodeReader rd(m);
	if (!rd.exec(f)) { err = "address is not executable code of " + m.name; return ""; }
	if (!index_of(m).built) build_starts(m, index_of(m));
	if (!node_of(m, f).ok) { err = "the function cannot be analysed"; return ""; }
	depth = std::max(0, std::min(depth, 3));
	std::set<uintptr_t> path;
	std::string out = "cx1:" + std::to_string(depth);
	enc(m, f, depth, 0, out, path);
	return out;
}

struct TNode { uint32_t n = 0, c = 0, b = 0; std::vector<uint32_t> s, k; std::vector<TNode> kids; };

static bool parse_list(const std::string &t, size_t &i, std::vector<uint32_t> &out)
{
	while (i < t.size() && t[i] != ',' && t[i] != '(' && t[i] != ')') {
		uint32_t v = 0; size_t st = i;
		while (i < t.size() && isalnum((unsigned char)t[i])) { const char *p = strchr(B36, tolower(t[i])); if (!p) return false; v = v * 36 + (uint32_t)(p - B36); ++i; }
		if (i == st) return false;
		out.push_back(v);
		if (i < t.size() && t[i] == '.') ++i;
	}
	return true;
}
static bool parse_node(const std::string &t, size_t &i, TNode &nd, int guard)
{
	if (guard > 6 || i >= t.size() || t[i] != '(') return false;
	++i;
	std::vector<uint32_t> nums;
	for (int k = 0; k < 3; ++k) {
		std::vector<uint32_t> one;
		if (!parse_list(t, i, one) || one.size() != 1 || i >= t.size() || t[i] != ',') return false;
		nums.push_back(one[0]); ++i;
	}
	nd.n = nums[0]; nd.c = nums[1]; nd.b = nums[2];
	if (!parse_list(t, i, nd.s) || i >= t.size() || t[i] != ',') return false;
	++i;
	if (!parse_list(t, i, nd.k)) return false;
	while (i < t.size() && t[i] == '(') { nd.kids.emplace_back(); if (!parse_node(t, i, nd.kids.back(), guard + 1)) return false; }
	if (i >= t.size() || t[i] != ')') return false;
	++i;
	return true;
}

static double ratio(uint32_t a, uint32_t b) { if (a == b) return 1.0; return (double)std::min(a, b) / (double)std::max(a, b); }
static double jacc(const std::vector<uint32_t> &a, const std::vector<uint32_t> &b)
{
	if (a.empty() && b.empty()) return -1;
	size_t i = 0, j = 0, both = 0;
	while (i < a.size() && j < b.size()) { if (a[i] == b[j]) { ++both; ++i; ++j; } else if (a[i] < b[j]) ++i; else ++j; }
	return (double)both / (double)(a.size() + b.size() - both);
}
static double node_sim(const TNode &a, const ModIndex::Node &b)
{
	double w = 0, s = 0;
	auto add = [&](double v, double wt) { if (v < 0) return; s += v * wt; w += wt; };
	add(ratio(a.n, b.n), 0.30); add(ratio(a.c, b.c), 0.20); add(ratio(a.b, b.b), 0.10);
	add(jacc(a.s, b.s), 0.25); add(jacc(a.k, b.k), 0.15);
	return w > 0 ? s / w : 0;
}
static double tree_sim(Module &m, const TNode &a, uintptr_t f, int level)
{
	ModIndex::Node nd = node_of(m, f);
	if (!nd.ok) return 0;
	double ns = node_sim(a, nd);
	if (a.kids.empty()) return nd.kids.empty() ? ns : ns * 0.9;
	size_t cap = KIDS[std::min(level + 1, 3)];
	size_t na = a.kids.size(), nb = std::min(nd.kids.size(), cap);
	if (nb == 0) return ns * 0.5;
	// monotonic alignment of the callee lists (call order is usually kept between builds)
	std::vector<std::vector<double>> dp(na + 1, std::vector<double>(nb + 1, 0));
	for (size_t i = 1; i <= na; ++i)
		for (size_t j = 1; j <= nb; ++j) {
			double v = tree_sim(m, a.kids[i - 1], nd.kids[j - 1], level + 1);
			dp[i][j] = std::max({dp[i - 1][j], dp[i][j - 1], dp[i - 1][j - 1] + v});
		}
	double cs = dp[na][nb] / (double)std::max(na, nb);
	return 0.5 * ns + 0.5 * cs;
}

std::vector<TreemapHit> treemap_find(Module &m, const std::string &map, size_t max, double min_score, std::string &err)
{
	std::vector<TreemapHit> hits;
	if (map.compare(0, 4, "cx1:") != 0) { err = "not a Codex treemap (it starts with \"cx1:\")"; return hits; }
	size_t i = 4;
	while (i < map.size() && isdigit((unsigned char)map[i])) ++i;
	TNode root;
	if (!parse_node(map, i, root, 0) || i != map.size()) { err = "treemap text is damaged"; return hits; }
	ModIndex &ix = index_of(m);
	build_starts(m, ix);
	for (uintptr_t f : ix.starts) {
		ModIndex::Node nd = node_of(m, f);
		if (!nd.ok || ratio(root.n, nd.n) < 0.5 || ratio(root.c, nd.c) < 0.5) continue;
		if (node_sim(root, nd) < min_score * 0.8) continue;
		double sc = tree_sim(m, root, f, 0);
		if (sc >= min_score) hits.push_back({f, sc});
	}
	std::sort(hits.begin(), hits.end(), [](const TreemapHit &a, const TreemapHit &b) { return a.score > b.score || (a.score == b.score && a.addr < b.addr); });
	if (hits.size() > max) hits.resize(max);
	if (hits.empty()) err = "no function in " + m.name + " is similar enough (best below " + std::to_string(min_score) + ")";
	return hits;
}

// ---------------------------------------------------------------------------------------------------------------
// "CBasePlayer::GiveNamedItem" | codex function name | "engine:SV_DropClient" | "0x1234ABCD"
uintptr_t graph_target(const std::string &text, Module *pref, Module **mod_out, std::string &err)
{
	if (mod_out) *mod_out = nullptr;
	if (text.empty()) { err = "empty name"; return 0; }
	char *e = nullptr;
	unsigned long long n = strtoull(text.c_str(), &e, 0);
	if (e && !*e && n > 0x10000) { if (mod_out) *mod_out = mod_of((uintptr_t)n); return (uintptr_t)n; }
	if (Func *f = func_get(text)) {
		if (!f->resolved || !f->addr) { err = "'" + text + "' is not resolved: " + f->why; return 0; }
		if (mod_out) *mod_out = mod_of(f->addr);
		return f->addr;
	}
	std::string modname, sym = text;
	size_t c = text.find(':');
	if (c != std::string::npos && c + 1 < text.size() && text[c + 1] != ':' && (c == 0 || text[c - 1] != ':')) { modname = text.substr(0, c); sym = text.substr(c + 1); }
	std::vector<Module *> mods;
	if (!modname.empty()) { Module *m = mod_find(modname.c_str()); if (!m) { err = "module '" + modname + "' is not loaded"; return 0; } mods.push_back(m); }
	else {
		if (pref) mods.push_back(pref);
		for (const char *d : {"game", "engine"}) { Module *m = mod_find(d); if (m && std::find(mods.begin(), mods.end(), m) == mods.end()) mods.push_back(m); }
	}
	std::string notes;
	for (Module *m : mods) {
		std::string e2;
		uintptr_t a = sym_find(*m, sym, e2);
		if (a) { if (mod_out) *mod_out = m; return a; }
		if (notes.empty() || e2.find("ambiguous") != std::string::npos) notes = e2;
	}
	if (modname.empty()) {                       // any other module: exported names only (reading every symbol table is slow)
		for (auto &mm : mod_list()) {
			if (!mm.loaded || std::find(mods.begin(), mods.end(), &mm) != mods.end()) continue;
			uintptr_t a = sym_export(const_cast<Module &>(mm), sym);
			if (a) { if (mod_out) *mod_out = const_cast<Module *>(&mm); return a; }
		}
	}
	err = notes.empty() ? "'" + text + "' is not a Codex function, a symbol or an address" : notes + " (and no Codex function has that name; for another module write \"module:name\")";
	return 0;
}
