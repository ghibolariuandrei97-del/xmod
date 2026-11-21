// Call-graph test on real binaries.
//   ELF (cs.so, engine_i486.so): the symbol table gives the truth - callees / callers / treemaps are compared with a
//   linear decode of every function's [start, start+size).
//   PE (mp.dll, swds.dll, mapped by hand): callees/callers of the GiveNamedItem function found by xref, treemap self-find.
#include "codex.h"
#include <dlfcn.h>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <sys/mman.h>

static double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

static bool pc_thunk(uintptr_t t) { const uint8_t *b = (const uint8_t *)t; return b[0] == 0x8B && (b[1] & 0xC7) == 0x04 && b[2] == 0x24 && b[3] == 0xC3; }

static void treemap_test(Module &m, std::vector<uintptr_t> funcs, int want)
{
	std::mt19937 rng(7);
	std::shuffle(funcs.begin(), funcs.end(), rng);
	int tried = 0, rank1 = 0, found = 0; double t0 = now(), tfind = 0; size_t maplen = 0;
	for (uintptr_t f : funcs) {
		if (tried >= want) break;
		FuncBody b; std::string e;
		if (!analyze_func(m, f, b, e) || b.insns < 40 || direct_callees(m, b).size() < 3) continue;
		std::string map = treemap_make(m, f, 2, e);
		if (map.empty()) continue;
		++tried; maplen += map.size();
		double t1 = now();
		auto hits = treemap_find(m, map, 3, 0.5, e);
		tfind += now() - t1;
		bool in = false; for (auto &h : hits) in = in || h.addr == f;
		found += in;
		if (!hits.empty() && hits[0].addr == f && (hits.size() == 1 || hits[0].score - hits[1].score >= 0.02)) ++rank1;
	}
	printf("treemap: %d functions, rank 1 (unique) %d (%.1f%%), in top 3 %d, avg map %zu chars, find %.1f ms avg (first includes indexing), total %.1fs\n",
		tried, rank1, 100.0 * rank1 / std::max(1, tried), found, maplen / std::max(1, tried), 1000 * tfind / std::max(1, tried), now() - t0);
}

static void elf_test(const char *path)
{
	void *h = dlopen(path, RTLD_LAZY | RTLD_LOCAL);
	if (!h) { printf("dlopen %s: %s\n", path, dlerror()); return; }
	mod_refresh();
	std::string bn = path; bn = bn.substr(bn.find_last_of('/') + 1);
	Module *m = mod_find(bn.c_str());
	mod_load_symbols(*m);
	printf("== %s: %zu symbols, %zu GOT slots\n", bn.c_str(), m->syms.size(), m->slots.size());
	// truth: linear decode of every sized function symbol
	std::map<uintptr_t, std::set<uintptr_t>> truth_callees, truth_callers;
	std::vector<uintptr_t> funcs;
	auto resolve = [&](uintptr_t t) -> uintptr_t {          // PLT stub -> function (by relocation)
		const uint8_t *b = (const uint8_t *)t;
		if (b[0] == 0xFF && b[1] == 0xA3 && m->got) { int32_t d; memcpy(&d, b + 2, 4); auto it = m->slots.find(m->got + d); if (it != m->slots.end()) return it->second.to; return 0; }
		if (b[0] == 0xFF && b[1] == 0x25) { uint32_t s; memcpy(&s, b + 2, 4); auto it = m->slots.find(s); if (it != m->slots.end()) return it->second.to; return 0; }
		return t;
	};
	std::set<uintptr_t> starts;
	for (auto &s : m->syms) if (s.func && s.size >= 16 && s.size < 200000 && m->contains(s.addr)) starts.insert(s.addr);
	for (uintptr_t f : starts) {
		const Module::Sym *sy = sym_containing(*m, f);
		if (!sy || sy->addr != f) continue;
		funcs.push_back(f);
		for (uintptr_t p = f; p < f + sy->size;) {
			Insn in; if (!lde_decode((const uint8_t *)p, in) || !in.valid) { ++p; continue; }
			if (in.is_call && in.rel_size) {
				uintptr_t t = in.target(p);
				if (t != p + in.len && m->contains(t) && !pc_thunk(t)) { uintptr_t r = resolve(t); if (r && m->contains(r)) { truth_callees[f].insert(r); truth_callers[r].insert(f); } }
			}
			const uint8_t *q = (const uint8_t *)p + in.prefixes;
			if (q[0] == 0xFF && q[1] == 0x93 && m->got) {        // call [ebx+disp] (-fno-plt)
				int32_t d; memcpy(&d, q + 2, 4); auto it = m->slots.find(m->got + d);
				if (it != m->slots.end() && it->second.to && m->contains(it->second.to)) { truth_callees[f].insert(it->second.to); truth_callers[it->second.to].insert(f); }
			}
			p += in.len;
		}
	}
	// callees
	double t0 = now();
	int same = 0, sub = 0, total = 0; size_t missing = 0, extra = 0, ntruth = 0;
	for (uintptr_t f : funcs) {
		FuncBody b; std::string e;
		if (!analyze_func(*m, f, b, e)) continue;
		std::set<uintptr_t> got;
		for (auto &c : b.calls) { const uint8_t *q = (const uint8_t *)c.at; bool is_call = q[0] == 0xE8 || (q[0] == 0xFF && ((q[1] >> 3) & 7) == 2);
			if (is_call && (c.kind == CK_CALL || c.kind == CK_IMPORT) && c.to && m->contains(c.to)) got.insert(c.to); }
		auto &tr = truth_callees[f];
		++total; ntruth += tr.size();
		size_t miss = 0, ex = 0;
		for (auto x : tr) if (!got.count(x)) ++miss;
		for (auto x : got) if (!tr.count(x)) ++ex;
		missing += miss; extra += ex;
		static int shown = 0; if (getenv("SHOW"))
		if ((getenv("SHOW")[0]==0x6d ? miss : (miss || ex)) && shown < 8) {
			++shown; uintptr_t o;
			printf("  %s:", sym_name_at(*m, f, &o).c_str());
			for (auto x : tr) if (!got.count(x)) printf(" MISS %s", sym_name_at(*m, x, &o).c_str());
			for (auto &c : b.calls) if (c.to && m->contains(c.to) && !tr.count(c.to)) printf(" EXTRA[%s at+%X] %s", call_kind_name(c.kind), (unsigned)(c.at - f), sym_name_at(*m, c.to, &o).c_str());
			printf("\n");
		}
		if (!miss && !ex) ++same; else if (!ex) ++sub;
	}
	printf("callees: %d functions in %.2fs, identical %d (%.2f%%), only missing some %d; calls: truth %zu, missed %zu, extra %zu\n",
		total, now() - t0, same, 100.0 * same / std::max(1, total), sub, ntruth, missing, extra);
	// callers
	std::vector<uintptr_t> targets;
	for (auto &kv : truth_callers) targets.push_back(kv.first);
	std::mt19937 rng(3); std::shuffle(targets.begin(), targets.end(), rng);
	int ct = 0, cexact = 0; size_t cmiss = 0, cextra = 0, ctr = 0; t0 = now();
	for (uintptr_t t : targets) {
		if (ct >= 300) break;
		++ct;
		std::set<uintptr_t> got;
		for (auto &r : find_callers(*m, t, 100000, false)) if (r.func && (r.kind == CK_CALL || r.kind == CK_INDIRECT)) got.insert(r.func);
		auto &tr = truth_callers[t]; ctr += tr.size();
		size_t miss = 0, ex = 0;
		for (auto x : tr) if (!got.count(x)) ++miss;
		for (auto x : got) if (!tr.count(x)) ++ex;
		cmiss += miss; cextra += ex; if (!miss && !ex) ++cexact;
	}
	printf("callers: %d targets in %.2fs (%.1f ms each), identical %d (%.1f%%); caller functions: truth %zu, missed %zu, extra %zu\n",
		ct, now() - t0, 1000 * (now() - t0) / std::max(1, ct), cexact, 100.0 * cexact / std::max(1, ct), ctr, cmiss, cextra);
	// named example
	std::string e;
	if (uintptr_t g = sym_find(*m, "CBasePlayer::GiveNamedItem", e)) {
		FuncBody b; analyze_func(*m, g, b, e);
		printf("CBasePlayer::GiveNamedItem: %d insns, %zu calls:", b.insns, b.calls.size());
		for (auto &c : b.calls) { uintptr_t o; std::string n = c.name.empty() && c.to && m->contains(c.to) ? sym_name_at(*m, c.to, &o) : c.name; printf(" [%s %s%s]", call_kind_name(c.kind), n.c_str(), c.vindex >= 0 ? ("v" + std::to_string(c.vindex)).c_str() : ""); }
		printf("\n  callers:");
		for (auto &r : find_callers(*m, g, 20, true)) { uintptr_t o; printf(" [%s %s]", call_kind_name(r.kind), r.func ? sym_name_at(*m, r.func, &o).c_str() : "-"); }
		printf("\n  treemap: %s\n", treemap_make(*m, g, 2, e).substr(0, 160).c_str());
	}
	treemap_test(*m, funcs, 300);
}

#pragma pack(push, 1)
struct DosH { uint16_t e_magic; uint8_t pad[58]; int32_t e_lfanew; };
struct FileH { uint16_t Machine, NumberOfSections; uint32_t TimeDateStamp, PtrSym, NumSym; uint16_t SizeOpt, Chars; };
struct DataDir { uint32_t VirtualAddress, Size; };
struct OptH { uint16_t Magic; uint8_t a, b; uint32_t SizeOfCode, SizeInit, SizeUninit, Entry, BaseOfCode, BaseOfData, ImageBase, SectAlign, FileAlign;
              uint16_t v[6]; uint32_t Win32, SizeOfImage, SizeOfHeaders, CheckSum; uint16_t Subsys, DllChars; uint32_t s[4], LoaderFlags, NumRva; DataDir dd[16]; };
struct SecH { char Name[8]; uint32_t VirtualSize, VirtualAddress, SizeOfRawData, PtrRaw, PtrReloc, PtrLine; uint16_t NReloc, NLine; uint32_t Chars; };
#pragma pack(pop)
static Module g_pe;
static void pe_test(const char *path)
{
	std::ifstream f(path, std::ios::binary); std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), {});
	auto *dos = (DosH *)file.data(); auto *fh = (FileH *)(file.data() + dos->e_lfanew + 4); auto *oh = (OptH *)(fh + 1); auto *sh = (SecH *)((uint8_t *)oh + fh->SizeOpt);
	uint8_t *img = (uint8_t *)mmap(nullptr, oh->SizeOfImage, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	memcpy(img, file.data(), oh->SizeOfHeaders);
	Module &m = g_pe; m.name = "pe"; m.base = (uintptr_t)img; m.size = oh->SizeOfImage; m.elf = false; m.syms_loaded = true;
	for (int i = 0; i < fh->NumberOfSections; ++i) {
		memcpy(img + sh[i].VirtualAddress, file.data() + sh[i].PtrRaw, std::min(sh[i].SizeOfRawData, sh[i].VirtualSize ? sh[i].VirtualSize : sh[i].SizeOfRawData));
		bool x = sh[i].Chars & 0x20000000;
		m.segs.push_back({(uintptr_t)img + sh[i].VirtualAddress, (uintptr_t)img + sh[i].VirtualAddress + (sh[i].VirtualSize ? sh[i].VirtualSize : sh[i].SizeOfRawData), true, !x, x});
	}
	uint32_t delta = (uint32_t)(uintptr_t)img - oh->ImageBase; DataDir rd = oh->dd[5];
	for (uint8_t *p = img + rd.VirtualAddress, *e = p + rd.Size; p + 8 <= e;) {
		uint32_t va = *(uint32_t *)p, sz = *(uint32_t *)(p + 4); if (sz < 8) break;
		for (uint16_t *r = (uint16_t *)(p + 8); (uint8_t *)r < p + sz; ++r) if ((*r >> 12) == 3) *(uint32_t *)(img + va + (*r & 0xFFF)) += delta;
		p += sz;
	}
	printf("== %s (PE)\n", path);
	auto refs = xref_string(m, "NULL Ent in GiveNamedItem!", 4);
	if (!refs.empty() && refs[0].func) {
		uintptr_t g = refs[0].func; FuncBody b; std::string e;
		analyze_func(m, g, b, e);
		printf("GiveNamedItem (xref) +%X: %d insns, size %u, %zu calls:", (unsigned)(g - m.base), b.insns, (unsigned)(b.end - b.start), b.calls.size());
		for (auto &c : b.calls) printf(" [%s +%X%s]", call_kind_name(c.kind), c.to ? (unsigned)(c.to - m.base) : 0, c.vindex >= 0 ? (" v" + std::to_string(c.vindex)).c_str() : "");
		auto cr = find_callers(m, g, 50, true);
		printf("\n  %zu callers:", cr.size());
		for (auto &r : cr) printf(" [%s at +%X func +%X]", call_kind_name(r.kind), (unsigned)(r.at - m.base), r.func ? (unsigned)(r.func - m.base) : 0);
		printf("\n");
	}
	// callers vs callees consistency: for sampled functions, every direct callee must list the function as a caller
	std::set<uintptr_t> st;
	for (auto &s : m.segs) if (s.x) for (uintptr_t p = s.lo; p + 5 < s.hi; ++p) { const uint8_t *q = (const uint8_t *)p; if (*q == 0xE8) { int32_t r; memcpy(&r, q + 1, 4); uintptr_t t = p + 5 + r; if (t >= s.lo && t < s.hi) st.insert(t); } }
	std::vector<uintptr_t> funcs(st.begin(), st.end());
	std::mt19937 rng(5); std::shuffle(funcs.begin(), funcs.end(), rng);
	int n = 0, ok = 0, edges = 0, back = 0; double t0 = now();
	for (uintptr_t fn : funcs) {
		if (n >= 150) break;
		FuncBody b; std::string e;
		if (!analyze_func(m, fn, b, e) || b.insns < 10) continue;
		++n; bool good = true;
		for (uintptr_t c : direct_callees(m, b)) {
			++edges; bool has = false;
			for (auto &r : find_callers(m, c, 100000, false)) if (r.func == fn || (r.at >= b.start && r.at < b.end)) { has = true; break; }
			back += has; good = good && has;
		}
		ok += good;
	}
	printf("consistency: %d functions, %d callee edges, %d found back by callers (%.1f%%), %.1fs\n", n, edges, back, 100.0 * back / std::max(1, edges), now() - t0);
	treemap_test(m, funcs, 200);
}

int main(int argc, char **argv)
{
	guard_install();
	for (int i = 1; i < argc; ++i) {
		std::string p = argv[i];
		if (p.size() > 4 && (p.substr(p.size() - 4) == ".dll" || p.substr(p.size() - 4) == ".DLL")) pe_test(argv[i]);
		else elf_test(argv[i]);
	}
	return 0;
}
