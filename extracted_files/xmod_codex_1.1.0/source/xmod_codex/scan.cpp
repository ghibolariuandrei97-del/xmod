// scan.cpp - signatures (parse / scan / make), symbols, function starts and string references.
#include "codex.h"
#include <algorithm>
#include <cstdio>
#include <cxxabi.h>
#include <fstream>
#ifndef _WIN32
#include <dlfcn.h>
#include <elf.h>
#endif

// ---------------------------------------------------------------------------------------------------------------
// signature text
static int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

bool sig_parse(const std::string &t, Sig &out, std::string &err)
{
	out.b.clear(); out.m.clear();
	if (t.find("\\x") != std::string::npos) {               // SourceMod / Orpheu style: \x55\x8B\x2A (0x2A = wildcard)
		for (size_t i = 0; i < t.size();) {
			if (t[i] == '\\' && i + 3 < t.size() + 0 && (t[i + 1] == 'x' || t[i + 1] == 'X')) {
				int a = hexv(t[i + 2]), b = i + 3 < t.size() ? hexv(t[i + 3]) : -1;
				if (a < 0 || b < 0) { err = "bad \\x escape at " + std::to_string(i); return false; }
				uint8_t v = (uint8_t)(a * 16 + b);
				out.b.push_back(v); out.m.push_back(v != 0x2A);
				i += 4;
			} else if (t[i] == ' ') ++i;
			else { err = "unexpected character in \\x signature"; return false; }
		}
	} else {                                                  // IDA style: 55 8B EC ?? 83 E4 F8 (also ?, *, 2-char tokens)
		size_t i = 0;
		while (i < t.size()) {
			while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == ',')) ++i;
			if (i >= t.size()) break;
			size_t j = i;
			while (j < t.size() && t[j] != ' ' && t[j] != '\t' && t[j] != ',') ++j;
			std::string tok = t.substr(i, j - i);
			i = j;
			if (tok == "?" || tok == "??" || tok == "*" || tok == "**") { out.b.push_back(0); out.m.push_back(0); continue; }
			if (tok.size() == 2 && hexv(tok[0]) >= 0 && hexv(tok[1]) >= 0) { out.b.push_back((uint8_t)(hexv(tok[0]) * 16 + hexv(tok[1]))); out.m.push_back(1); continue; }
			err = "bad token '" + tok + "' (use hex bytes like 8B and ?? for wildcards)";
			return false;
		}
	}
	if (out.b.empty()) { err = "empty signature"; return false; }
	bool any = false;
	for (auto m : out.m) any = any || m;
	if (!any) { err = "signature has only wildcards"; return false; }
	return true;
}

std::string sig_text(const Sig &s)
{
	std::string r;
	char b[4];
	for (size_t i = 0; i < s.b.size(); ++i) {
		if (i) r += ' ';
		if (s.m[i]) { snprintf(b, sizeof b, "%02X", s.b[i]); r += b; } else r += "??";
	}
	return r;
}

static inline bool match_here(const uint8_t *p, const Sig &s)
{
	for (size_t i = 0; i < s.b.size(); ++i) if (s.m[i] && p[i] != s.b[i]) return false;
	return true;
}

bool sig_match_at(uintptr_t a, const Sig &s)
{
	std::vector<uint8_t> buf(s.b.size());
	if (!mem_read_orig(a, buf.data(), buf.size())) return false;
	return match_here(buf.data(), s);
}

// scans only mapped segments of the module (code segments unless code_only == false).
// Bytes under our own patches and hooks are seen as the game shipped them, so a function hooked by one plugin is still
// found by another plugin's signature (and a constant patched by set_const does not hide its function).
size_t sig_scan(const Module &m, const Sig &s, std::vector<uintptr_t> &out, size_t max, bool code_only)
{
	size_t first = 0;
	while (first < s.m.size() && !s.m[first]) ++first;
	size_t n = s.b.size(), count = 0;
	std::vector<uintptr_t> found;
	std::vector<std::pair<uintptr_t, uintptr_t>> pr;
	for (const Segment &seg : m.segs) {
		if (!seg.r || (code_only && !seg.x)) continue;
		if (seg.hi - seg.lo < n || !mem_readable(seg.lo, seg.hi - seg.lo)) continue;
		bool patched = patch_ranges(seg.lo, seg.hi, pr);
		auto touches = [&](uintptr_t a) { for (auto &r : pr) if (a < r.second && r.first < a + n) return true; return false; };
		const uint8_t *p = (const uint8_t *)seg.lo, *end = (const uint8_t *)seg.hi - n;
		const uint8_t want = s.b[first];
		for (const uint8_t *q = p + first; q <= end + first;) {
			const uint8_t *hit = (const uint8_t *)memchr(q, want, (size_t)(end + first - q) + 1);
			if (!hit) break;
			const uint8_t *start = hit - first;
			if (!(patched && touches((uintptr_t)start)) && match_here(start, s)) found.push_back((uintptr_t)start);
			q = hit + 1;
		}
		if (patched) {                                         // windows that overlap a patch: check the original bytes
			std::vector<uint8_t> buf(n);
			std::vector<uintptr_t> extra;
			for (auto &r : pr) {
				uintptr_t lo = r.first >= seg.lo + n - 1 ? r.first - (n - 1) : seg.lo, hi = std::min<uintptr_t>(r.second, seg.hi - n + 1);
				for (uintptr_t a = lo; a < hi; ++a)
					if (mem_read_orig(a, buf.data(), n) && match_here(buf.data(), s)) extra.push_back(a);
			}
			found.insert(found.end(), extra.begin(), extra.end());
		}
	}
	std::sort(found.begin(), found.end());
	found.erase(std::unique(found.begin(), found.end()), found.end());
	count = found.size();
	for (size_t i = 0; i < found.size() && out.size() < max; ++i) out.push_back(found[i]);
	return count;
}

// ---------------------------------------------------------------------------------------------------------------
// symbols
#ifndef _WIN32
static void load_elf_symbols(Module &m)
{
	m.syms_loaded = true;
	std::ifstream f(m.path, std::ios::binary);
	if (!f) return;
	std::vector<char> file((std::istreambuf_iterator<char>(f)), {});
	if (file.size() < sizeof(Elf32_Ehdr)) return;
	auto *eh = (const Elf32_Ehdr *)file.data();
	if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 || eh->e_ident[EI_CLASS] != ELFCLASS32) return;
	if (eh->e_shoff == 0 || eh->e_shoff + (size_t)eh->e_shnum * sizeof(Elf32_Shdr) > file.size()) return;
	auto *sh = (const Elf32_Shdr *)(file.data() + eh->e_shoff);
	const char *shstr = eh->e_shstrndx < eh->e_shnum ? file.data() + sh[eh->e_shstrndx].sh_offset : nullptr;
	uintptr_t load = (eh->e_type == ET_DYN) ? m.base : 0;
	for (int i = 0; i < eh->e_shnum; ++i) {
		if (shstr && (strcmp(shstr + sh[i].sh_name, ".got.plt") == 0 || (!m.got && strcmp(shstr + sh[i].sh_name, ".got") == 0)))
			m.got = load + sh[i].sh_addr;
		if (sh[i].sh_type != SHT_SYMTAB && sh[i].sh_type != SHT_DYNSYM) continue;
		if (sh[i].sh_link >= eh->e_shnum) continue;
		const Elf32_Shdr &strs = sh[sh[i].sh_link];
		if (sh[i].sh_offset + sh[i].sh_size > file.size() || strs.sh_offset + strs.sh_size > file.size()) continue;
		auto *sym = (const Elf32_Sym *)(file.data() + sh[i].sh_offset);
		size_t cnt = sh[i].sh_size / sizeof(Elf32_Sym);
		for (size_t k = 0; k < cnt; ++k) {
			int type = ELF32_ST_TYPE(sym[k].st_info);
			if ((type != STT_FUNC && type != STT_OBJECT) || sym[k].st_shndx == SHN_UNDEF || !sym[k].st_value) continue;
			if (sym[k].st_name >= strs.sh_size) continue;
			const char *nm = file.data() + strs.sh_offset + sym[k].st_name;
			m.syms.push_back({nm, load + sym[k].st_value, sym[k].st_size, type == STT_FUNC});
		}
	}
	// GOT slots: PLT stubs and PIC code jump/call through them; with lazy binding the slot still points back into the
	// PLT, so the relocation tells which function a stub really reaches
	for (int i = 0; i < eh->e_shnum; ++i) {
		if (sh[i].sh_type != SHT_REL || sh[i].sh_link >= eh->e_shnum) continue;
		const Elf32_Shdr &st = sh[sh[i].sh_link];
		if (st.sh_link >= eh->e_shnum) continue;
		const Elf32_Shdr &ss = sh[st.sh_link];
		if (sh[i].sh_offset + sh[i].sh_size > file.size() || st.sh_offset + st.sh_size > file.size() || ss.sh_offset + ss.sh_size > file.size()) continue;
		auto *rel = (const Elf32_Rel *)(file.data() + sh[i].sh_offset);
		auto *sym = (const Elf32_Sym *)(file.data() + st.sh_offset);
		size_t nsym = st.sh_size / sizeof(Elf32_Sym);
		for (size_t k = 0; k < sh[i].sh_size / sizeof(Elf32_Rel); ++k) {
			int type = ELF32_R_TYPE(rel[k].r_info); size_t si = ELF32_R_SYM(rel[k].r_info);
			if ((type != R_386_JMP_SLOT && type != R_386_GLOB_DAT && type != R_386_32) || !si || si >= nsym) continue;
			if (sym[si].st_name >= ss.sh_size) continue;
			Module::Slot sl;
			sl.name = file.data() + ss.sh_offset + sym[si].st_name;
			sl.to = (sym[si].st_shndx != SHN_UNDEF && sym[si].st_value) ? load + sym[si].st_value : 0;
			m.slots[load + rel[k].r_offset] = sl;
		}
	}
	std::sort(m.syms.begin(), m.syms.end(), [](const Module::Sym &a, const Module::Sym &b) { return a.addr < b.addr; });
	m.syms.erase(std::unique(m.syms.begin(), m.syms.end(), [](const Module::Sym &a, const Module::Sym &b) { return a.addr == b.addr && a.name == b.name; }), m.syms.end());
}
#endif

void mod_load_symbols(Module &m)
{
#ifndef _WIN32
	if (!m.syms_loaded) load_elf_symbols(m);
#else
	m.syms_loaded = true;
#endif
}

std::string demangle(const std::string &n)
{
	if (n.size() < 2 || n[0] != '_' || n[1] != 'Z') return n;
	int st = 0;
	char *d = abi::__cxa_demangle(n.c_str(), nullptr, nullptr, &st);
	if (!d) return n;
	std::string r = d;
	free(d);
	return r;
}

uintptr_t sym_export(Module &m, const std::string &name)
{
#ifdef _WIN32
	FARPROC p = GetProcAddress((HMODULE)m.base, name.c_str());
	return p ? (uintptr_t)p : 0;
#else
	uintptr_t r = 0;
	if (void *h = dlopen(m.path.c_str(), RTLD_LAZY | RTLD_NOLOAD)) {
		void *p = dlsym(h, name.c_str());
		dlclose(h);
		if (p && m.contains((uintptr_t)p)) r = (uintptr_t)p;
	}
	return r;
#endif
}

uintptr_t sym_find(Module &m, const std::string &name, std::string &err)
{
#ifdef _WIN32
	FARPROC p = GetProcAddress((HMODULE)m.base, name.c_str());
	if (p) return (uintptr_t)p;
	err = "symbol '" + name + "' is not exported by " + m.name + " (Windows DLLs only have exports - use a signature)";
	return 0;
#else
	if (void *h = dlopen(m.path.c_str(), RTLD_LAZY | RTLD_NOLOAD)) {
		void *p = dlsym(h, name.c_str());
		dlclose(h);
		if (p && m.contains((uintptr_t)p)) return (uintptr_t)p;
	}
	if (!m.syms_loaded) load_elf_symbols(m);
	for (auto &s : m.syms) if (s.name == name) return s.addr;
	// demangled: "CBasePlayer::GiveNamedItem(char const*)" or without the parameter list
	std::vector<const Module::Sym *> hits;
	bool with_params = name.find('(') != std::string::npos;
	for (auto &s : m.syms) {
		if (!s.func || s.name.size() < 2 || s.name[0] != '_' || s.name[1] != 'Z') continue;
		std::string d = demangle(s.name);
		if (!with_params) { size_t k = d.find('('); if (k != std::string::npos) d.resize(k); }
		if (d == name) hits.push_back(&s);
	}
	if (hits.size() == 1) return hits[0]->addr;
	if (hits.size() > 1) {
		// the same function can be listed twice (versioned aliases): same address is fine
		bool same = true;
		for (auto *h : hits) same = same && h->addr == hits[0]->addr;
		if (same) return hits[0]->addr;
		err = "symbol '" + name + "' is ambiguous (" + std::to_string(hits.size()) + " overloads) - add the parameter list, e.g. " + demangle(hits[0]->name);
		return 0;
	}
	err = "symbol '" + name + "' not found in " + m.name;
	return 0;
#endif
}

const Module::Sym *sym_containing(Module &m, uintptr_t a)
{
#ifndef _WIN32
	if (!m.syms_loaded) load_elf_symbols(m);
	const Module::Sym *best = nullptr;
	for (auto &s : m.syms) {
		if (s.addr > a) break;
		if (s.func && a < s.addr + std::max<uint32_t>(s.size, 1)) best = &s;
	}
	return best;
#else
	(void)m; (void)a;
	return nullptr;
#endif
}

// start of the function containing 'a': exact from ELF symbols, otherwise a heuristic (padding before a 16-aligned start)
uintptr_t func_start(Module &m, uintptr_t a, bool *exact)
{
	if (exact) *exact = false;
	if (const Module::Sym *s = sym_containing(m, a)) { if (exact) *exact = true; return s->addr; }
	const Segment *seg = m.seg_of(a);
	if (!seg || !seg->x) return 0;
	uintptr_t lo = std::max(seg->lo + 1, a > 0x10000 ? a - 0x10000 : seg->lo + 1);
	for (uintptr_t p = a & ~(uintptr_t)15; p >= lo; p -= 16) {
		uint8_t prev = *(const uint8_t *)(p - 1), cur = *(const uint8_t *)p;
		if (prev != 0xCC && prev != 0x90 && prev != 0xC3 && prev != 0x00) continue;
		// plausible first instructions: push ebp / push reg / sub esp / mov edi,edi / push -1 (SEH) / mov eax,[esp+]
		if (cur == 0x55 || cur == 0x53 || cur == 0x56 || cur == 0x57 || cur == 0x51 || cur == 0x83 || cur == 0x81 ||
		    cur == 0x8B || cur == 0x6A || cur == 0xA1 || cur == 0xB8 || cur == 0x64 || cur == 0x33 || cur == 0xD9 || cur == 0x68)
			return p;
		if (p - 16 < lo) break;
	}
	return 0;
}

// ---------------------------------------------------------------------------------------------------------------
// string references: functions that use a given string literal
std::vector<XRef> xref_string(Module &m, const std::string &text, size_t max)
{
	std::vector<XRef> out;
	if (text.empty()) return out;
	if (text.back() != '\n') {                         // game messages nearly always end in "\n": accept the text without it
		out = xref_string(m, text + "\n", max);
		if (out.size() >= max) return out;
	}
#ifndef _WIN32
	if (!m.syms_loaded) load_elf_symbols(m);   // also finds the GOT
#endif
	Sig s;
	for (char c : text) { s.b.push_back((uint8_t)c); s.m.push_back(1); }
	s.b.push_back(0); s.m.push_back(1);
	std::vector<uintptr_t> strs;
	sig_scan(m, s, strs, 32, false);
	for (uintptr_t sa : strs) {
		if (sa > m.base && mem_readable(sa - 1, 1) && *(const uint8_t *)(sa - 1) != 0) continue;   // must be a whole string
		uint32_t vals[2] = {(uint32_t)sa, m.got ? (uint32_t)(sa - m.got) : 0};
		for (int k = 0; k < (m.got ? 2 : 1); ++k) {
			Sig r;
			for (int i = 0; i < 4; ++i) { r.b.push_back((uint8_t)(vals[k] >> (8 * i))); r.m.push_back(1); }
			std::vector<uintptr_t> refs;
			sig_scan(m, r, refs, 256, true);
			for (uintptr_t ref : refs) {
				bool exact = false;
				uintptr_t f = func_start(m, ref, &exact);
				bool dup = false;
				for (auto &x : out) dup = dup || x.func == f || x.ref == ref;
				if (!dup) out.push_back({ref, f});
				if (out.size() >= max) return out;
			}
		}
	}
	return out;
}

// ---------------------------------------------------------------------------------------------------------------
// signature maker
#ifdef _WIN32
static void load_pe_relocs(Module &m)
{
	m.relocs_loaded = true;
	auto *dos = (IMAGE_DOS_HEADER *)m.base;
	auto *nt = (IMAGE_NT_HEADERS32 *)(m.base + dos->e_lfanew);
	auto &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
	if (!dir.VirtualAddress || !dir.Size) return;
	uintptr_t p = m.base + dir.VirtualAddress, end = p + dir.Size;
	while (p + 8 <= end) {
		auto *blk = (IMAGE_BASE_RELOCATION *)p;
		if (blk->SizeOfBlock < 8 || p + blk->SizeOfBlock > end) break;
		auto *e = (const uint16_t *)(p + 8);
		size_t n = (blk->SizeOfBlock - 8) / 2;
		for (size_t i = 0; i < n; ++i)
			if ((e[i] >> 12) == IMAGE_REL_BASED_HIGHLOW) m.relocs.push_back(m.base + blk->VirtualAddress + (e[i] & 0xFFF));
		p += blk->SizeOfBlock;
	}
	std::sort(m.relocs.begin(), m.relocs.end());
}
static bool is_reloc(Module &m, uintptr_t a)   // is any of the 4 bytes at a..a+3 part of a relocated dword?
{
	if (!m.relocs_loaded) load_pe_relocs(m);
	auto it = std::lower_bound(m.relocs.begin(), m.relocs.end(), a > 3 ? a - 3 : 0);
	return it != m.relocs.end() && *it <= a + 3;
}
#endif

bool looks_like_address(uint32_t v)
{
	for (auto &mm : mod_list()) if (mm.loaded && mm.contains(v)) return true;
	return false;
}

static bool sig_make_at(uintptr_t addr, Module &m, SigMade &out, std::string &err, int maxlen)
{
	const Segment *seg = m.seg_of(addr);
	if (!seg || !seg->x) { err = "address is not inside an executable section of " + m.name; return false; }
	Sig s;
	uintptr_t cur = addr;
	std::vector<uintptr_t> cands;
	bool scanned = false, prev_call = false;
	while ((int)s.b.size() < maxlen) {
		if (cur + 16 > seg->hi) break;
		Insn in;
		uint8_t ob[16];                                       // original bytes (a hooked function gets its real signature)
		if (!mem_read_orig(cur, ob, 16) || !lde_decode(ob, in)) { err = "undecodable instruction at " + std::to_string(cur - addr); break; }
		const uint8_t *p = ob;
		std::vector<uint8_t> wild(in.len, 0);
		if (in.rel_size == 4 || in.rel_size == 2) for (int k = 0; k < in.rel_size; ++k) wild[in.rel_off + k] = 1;   // rel32 branches move
		if (in.disp_size == 4) {
			bool w = in.disp_abs || looks_like_address((uint32_t)in.disp);
#ifndef _WIN32
			// PIC code addresses data relative to the GOT through any register: those offsets change with every build
			if (m.got && in.has_modrm && in.mod == 2 && (in.rm == 3 || in.sib_base == 3 || in.disp > 0x2000 || in.disp < -0x2000)) w = true;
#endif
			if (w) for (int k = 0; k < 4; ++k) wild[in.disp_off + k] = 1;
		}
		if (in.imm_size == 4) {
			bool w = looks_like_address((uint32_t)in.imm);
#ifndef _WIN32
			// add reg, imm32 right after call __x86.get_pc_thunk.reg: the GOT delta (different in every build)
			if (prev_call && ((in.opcode == 0x81 && in.has_modrm && in.mod == 3 && in.reg == 0) || in.opcode == 0x05)) w = true;
#endif
			if (w) for (int k = 0; k < 4; ++k) wild[in.imm_off + k] = 1;
		}
#ifdef _WIN32
		for (int k = 0; k + 3 < in.len; ++k) if (is_reloc(m, cur + k)) { for (int j = 0; j < 4; ++j) wild[k + j] = 1; k += 3; }
#endif
		for (int k = 0; k < in.len; ++k) { s.b.push_back(wild[k] ? 0 : p[k]); s.m.push_back(!wild[k]); }
		cur += in.len;
		prev_call = in.is_call;
		if (s.b.size() < 6) continue;
		// candidate filtering: one full scan, then only re-check the candidates
		if (!scanned) {
			sig_scan(m, s, cands, 200000, true);
			scanned = true;
		} else {
			std::vector<uintptr_t> keep;
			for (uintptr_t c : cands) if (c + s.b.size() <= m.base + m.size && sig_match_at(c, s)) keep.push_back(c);
			cands.swap(keep);
		}
		if (cands.size() <= 1) break;
		if (in.is_ret && s.b.size() >= 24) break;     // do not run into the next function for nothing
	}
	// trim trailing wildcards
	while (!s.m.empty() && !s.m.back()) { s.m.pop_back(); s.b.pop_back(); }
	out.text = sig_text(s);
	out.len = (int)s.b.size();
	out.matches = (int)cands.size();
	out.unique = cands.size() == 1 && cands[0] == addr;
	if (!out.unique && err.empty()) err = "no unique signature within " + std::to_string(maxlen) + " bytes (" + std::to_string(cands.size()) + " matches)";
	return out.unique;
}

bool sig_split(const std::string &text, std::string &sig, int &adjust)
{
	adjust = 0;
	size_t at = text.rfind('@');
	if (at == std::string::npos) { sig = text; return true; }
	char *e = nullptr;
	long v = strtol(text.c_str() + at + 1, &e, 0);
	if (e == text.c_str() + at + 1) { sig = text; return true; }
	adjust = (int)v;
	sig = text.substr(0, at);
	while (!sig.empty() && sig.back() == ' ') sig.pop_back();
	return true;
}

// a unique signature for the function at addr; when its first bytes are shared with other functions, anchor the
// signature on a unique instruction sequence further inside the function ("... @-k": function = match - k)
bool sig_make(uintptr_t addr, Module &m, SigMade &out, std::string &err, int maxlen)
{
	if (sig_make_at(addr, m, out, err, maxlen)) return true;
	SigMade first = out;
	std::string first_err = err;
	const Segment *seg = m.seg_of(addr);
	uint32_t limit = 512;
	if (const Module::Sym *s = sym_containing(m, addr)) if (s->addr == addr && s->size) limit = std::min<uint32_t>(limit, s->size);
	uintptr_t cur = addr;
	for (int tries = 0; tries < 48 && seg && cur + 16 < seg->hi && cur - addr < limit; ++tries) {
		Insn in;
		uint8_t ob[16];
		if (!mem_read_orig(cur, ob, 16) || !lde_decode(ob, in)) break;
		cur += in.len;
		if (cur - addr >= limit) break;
		SigMade sm; std::string e2;
		if (sig_make_at(cur, m, sm, e2, std::min(maxlen, 96))) {
			int k = (int)(cur - addr);
			out = sm;
			out.adjust = -k;
			out.text = sm.text + " @-" + std::to_string(k);
			err.clear();
			return true;
		}
	}
	out = first;
	err = first_err;
	if (first.matches > 1) err += " - the function body is shared with other functions (use a symbol, or index)";
	return false;
}
