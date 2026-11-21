// Mass test on real binaries: trampolines + detour install/remove for every exported function, signatures for a sample.
#include "codex.h"
#include <dlfcn.h>
#include <cstdio>
#include <map>
static bool check_tramp(uintptr_t fn, uint8_t *t, int stolen, std::string &why) {
	// walk the trampoline: every relocated branch must hit the same absolute target as the original instruction
	int oo = 0; uint8_t *tp = t;
	while (oo < stolen) {
		Insn a, b; lde_decode((const uint8_t *)fn + oo, a); lde_decode(tp, b);
		if (a.rel_size) {
			uintptr_t ta = a.target(fn + oo);
			if (a.is_call && !b.rel_size) { /* get_pc_thunk / call $+5 rewritten as mov/push imm32 */
				uint32_t imm; memcpy(&imm, tp + 1, 4);
				if (imm != fn + oo + a.len) { why = "pc thunk value"; return false; }
			} else if (!b.rel_size || b.target((uintptr_t)tp) != ta) { why = "branch target moved"; return false; }
		} else if (a.len != b.len || memcmp((const void *)(fn + oo), tp, a.len)) { why = "copied bytes differ"; return false; }
		oo += a.len; tp += b.len;
	}
	Insn j; lde_decode(tp, j);
	if (!(tp[0] == 0xE9 && j.target((uintptr_t)tp) == fn + stolen)) { why = "no jump back"; return false; }
	return true;
}
int main(int argc, char **argv) {
	guard_install();
	for (int ai = 1; ai < argc; ++ai) {
		void *h = dlopen(argv[ai], RTLD_LAZY | RTLD_LOCAL);
		if (!h) { printf("dlopen %s: %s\n", argv[ai], dlerror()); continue; }
		mod_refresh();
		std::string bn = argv[ai]; bn = bn.substr(bn.find_last_of('/') + 1);
		Module *m = mod_find(bn.c_str());
		std::string err; sym_find(*m, "__nothing__", err);   // loads the symbol table
		if (getenv("XREF")) {
			auto r = xref_string(*m, getenv("XREF"), 8);
			printf("xref \"%s\": %zu refs\n", getenv("XREF"), r.size());
			for (auto &x : r) { const Module::Sym *sy = sym_containing(*m, x.func); printf("  ref +%X func +%X %s\n", (unsigned)(x.ref - m->base), (unsigned)(x.func - m->base), sy ? sy->name.c_str() : "?"); }
			for (const char *q : {"CBasePlayer::GiveNamedItem", "CBasePlayer::AddAccount", "CBasePlayer::AddAccount_OrigFunc"}) { std::string e; uintptr_t a = sym_find(*m, q, e); printf("  sym %s -> +%X %s\n", q, a ? (unsigned)(a - m->base) : 0, e.c_str());
				if (a) { SigMade sm; std::string e2; bool u = sig_make(a, *m, sm, e2, 160); printf("    sig %s (%s)\n", sm.text.c_str(), u ? "unique" : e2.c_str()); } }
			continue;
		}
		std::map<std::string, int> reasons; int funcs = 0, ok = 0, bad = 0, sig_ok = 0, sig_tries = 0, restored = 0;
		for (auto &s : m->syms) {
			if (!s.func || s.size < 6) continue;
			++funcs;
			uint8_t *t = nullptr; int stolen = 0; std::string why;
			if (!build_trampoline(s.addr, t, stolen, why)) { reasons[why]++; continue; }
			if (!check_tramp(s.addr, t, stolen, why)) { ++bad; if (bad < 5) printf("  BAD %s: %s\n", s.name.c_str(), why.c_str()); continue; }
			++ok;
			// install a detour (jmp to the trampoline itself = behaviour preserved) and remove it
			std::vector<uint8_t> before(stolen), bytes(stolen, 0x90);
			memcpy(before.data(), (void *)s.addr, stolen);
			bytes[0] = 0xE9; int32_t r = (int32_t)((uintptr_t)t - (s.addr + 5)); memcpy(&bytes[1], &r, 4);
			int id = patch_apply(s.addr, bytes, &before, "mass");
			if (id && patch_remove(id) && memcmp(before.data(), (void *)s.addr, stolen) == 0) ++restored;
			if ((funcs % 7) == 0 && sig_tries < 600) {
				++sig_tries; SigMade sm; std::string e2;
				if (sig_make(s.addr, *m, sm, e2, 128)) { Sig sg; std::string st; int adj; sig_split(sm.text, st, adj); sig_parse(st, sg, e2); std::vector<uintptr_t> hits; if (sig_scan(*m, sg, hits, 4) == 1 && hits[0] + adj == s.addr) ++sig_ok; else printf("  ROUNDTRIP FAIL %s\n", sm.text.c_str()); }
			}
		}
		printf("%s: %d functions | trampolines OK %d, refused %d, WRONG %d | detour install+restore %d/%d | unique signatures %d/%d\n",
		       bn.c_str(), funcs, ok, funcs - ok - bad, bad, restored, ok, sig_ok, sig_tries);
		for (auto &kv : reasons) printf("    refused (%d): %s\n", kv.second, kv.first.c_str());
	}
	return 0;
}
