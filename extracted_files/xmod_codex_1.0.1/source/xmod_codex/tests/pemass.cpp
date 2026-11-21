// Map a PE image (MSVC-built mp.dll / swds.dll) into memory on Linux and run the trampoline/detour/signature test on it.
#include "codex.h"
#include <cstdio>
#include <fstream>
#include <set>
#include <map>
#include <sys/mman.h>
#pragma pack(push, 1)
struct DosH { uint16_t e_magic; uint8_t pad[58]; int32_t e_lfanew; };
struct FileH { uint16_t Machine, NumberOfSections; uint32_t TimeDateStamp, PtrSym, NumSym; uint16_t SizeOpt, Chars; };
struct DataDir { uint32_t VirtualAddress, Size; };
struct OptH { uint16_t Magic; uint8_t a, b; uint32_t SizeOfCode, SizeInit, SizeUninit, Entry, BaseOfCode, BaseOfData, ImageBase, SectAlign, FileAlign;
              uint16_t v[6]; uint32_t Win32, SizeOfImage, SizeOfHeaders, CheckSum; uint16_t Subsys, DllChars; uint32_t s[4], LoaderFlags, NumRva; DataDir dd[16]; };
struct SecH { char Name[8]; uint32_t VirtualSize, VirtualAddress, SizeOfRawData, PtrRaw, PtrReloc, PtrLine; uint16_t NReloc, NLine; uint32_t Chars; };
#pragma pack(pop)
int main(int argc, char **argv) {
	guard_install();
	for (int ai = 1; ai < argc; ++ai) {
		std::ifstream f(argv[ai], std::ios::binary); std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), {});
		auto *dos = (DosH *)file.data(); auto *fh = (FileH *)(file.data() + dos->e_lfanew + 4); auto *oh = (OptH *)(fh + 1); auto *sh = (SecH *)((uint8_t *)oh + fh->SizeOpt);
		uint8_t *img = (uint8_t *)mmap(nullptr, oh->SizeOfImage, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		memcpy(img, file.data(), oh->SizeOfHeaders);
		Module m; m.name = argv[ai]; m.base = (uintptr_t)img; m.size = oh->SizeOfImage; m.elf = false; m.syms_loaded = true;
		for (int i = 0; i < fh->NumberOfSections; ++i) {
			memcpy(img + sh[i].VirtualAddress, file.data() + sh[i].PtrRaw, std::min(sh[i].SizeOfRawData, sh[i].VirtualSize ? sh[i].VirtualSize : sh[i].SizeOfRawData));
			bool x = sh[i].Chars & 0x20000000;
			m.segs.push_back({(uintptr_t)img + sh[i].VirtualAddress, (uintptr_t)img + sh[i].VirtualAddress + (sh[i].VirtualSize ? sh[i].VirtualSize : sh[i].SizeOfRawData), true, !x, x});
		}
		// base relocations (we are not at ImageBase)
		uint32_t delta = (uint32_t)(uintptr_t)img - oh->ImageBase; DataDir rd = oh->dd[5]; uint32_t nrel = 0;
		for (uint8_t *p = img + rd.VirtualAddress, *e = p + rd.Size; p + 8 <= e;) {
			uint32_t va = *(uint32_t *)p, sz = *(uint32_t *)(p + 4); if (sz < 8) break;
			for (uint16_t *r = (uint16_t *)(p + 8); (uint8_t *)r < p + sz; ++r) if ((*r >> 12) == 3) { *(uint32_t *)(img + va + (*r & 0xFFF)) += delta; ++nrel; }
			p += sz;
		}
		if (getenv("XREF")) {
			auto r = xref_string(m, getenv("XREF"), 8);
			printf("xref \"%s\": %zu refs\n", getenv("XREF"), r.size());
			for (auto &x : r) { printf("  ref +%X func +%X\n", (unsigned)(x.ref - m.base), (unsigned)(x.func ? x.func - m.base : 0));
				if (x.func) { SigMade sm; std::string e; bool u = sig_make(x.func, m, sm, e, 160); printf("  sig %s (%s)\n", sm.text.c_str(), u ? "unique" : e.c_str()); } }
			continue;
		}
		std::set<uintptr_t> starts;
		for (auto &s : m.segs) if (s.x) for (uintptr_t p = s.lo; p + 16 < s.hi;) { Insn in; if (!lde_decode((const uint8_t *)p, in)) { ++p; continue; } if (in.is_call && in.rel_size == 4) { uintptr_t t = in.target(p); if (t >= s.lo && t < s.hi) starts.insert(t); } p += in.len; }
		int ok = 0, refused = 0, wrong = 0, restored = 0, sig_ok = 0, sig_tries = 0, k = 0, movedi = 0, sehpro = 0;
		std::map<std::string, int> why;
		for (uintptr_t fn : starts) {
			const uint8_t *b = (const uint8_t *)fn;
			if (b[0] == 0x8B && b[1] == 0xFF) ++movedi;
			if (b[0] == 0x6A && b[1] == 0xFF && b[2] == 0x68) ++sehpro;
			uint8_t *t = nullptr; int stolen = 0; std::string err;
			if (!build_trampoline(fn, t, stolen, err)) { ++refused; why[err]++; continue; }
			int oo = 0; uint8_t *tp = t; bool good = true;
			while (oo < stolen) { Insn a, c; lde_decode(b + oo, a); lde_decode(tp, c);
				if (a.rel_size) { if (!(a.is_call && !c.rel_size) && (!c.rel_size || c.target((uintptr_t)tp) != a.target(fn + oo))) good = false; }
				else if (a.len != c.len || memcmp(b + oo, tp, a.len)) good = false;
				oo += a.len; tp += c.len; }
			Insn j; lde_decode(tp, j); if (!(tp[0] == 0xE9 && j.target((uintptr_t)tp) == fn + stolen)) good = false;
			if (!good) { ++wrong; continue; }
			++ok;
			std::vector<uint8_t> before(stolen), bytes(stolen, 0x90); memcpy(before.data(), b, stolen);
			bytes[0] = 0xE9; int32_t r = (int32_t)((uintptr_t)t - (fn + 5)); memcpy(&bytes[1], &r, 4);
			int id = patch_apply(fn, bytes, &before, "mass"); if (id && patch_remove(id) && !memcmp(before.data(), b, stolen)) ++restored;
			if ((++k % 9) == 0 && sig_tries < 400) { ++sig_tries; SigMade sm; std::string e2, st; int adj;
				if (sig_make(fn, m, sm, e2, 128)) { sig_split(sm.text, st, adj); Sig sg; sig_parse(st, sg, e2); std::vector<uintptr_t> hits; if (sig_scan(m, sg, hits, 4) == 1 && hits[0] + adj == fn) ++sig_ok; } }
		}
		printf("%s (MSVC, %u relocations applied): %zu functions | prologues: %d 'mov edi,edi', %d SEH frames | trampolines OK %d, refused %d, WRONG %d | detour+restore %d/%d | unique sigs %d/%d\n",
		       argv[ai], nrel, starts.size(), movedi, sehpro, ok, refused, wrong, restored, ok, sig_ok, sig_tries);
		for (auto &w : why) printf("    refused (%d): %s\n", w.second, w.first.c_str());
	}
}
