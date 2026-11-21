// mem.cpp - loaded modules (with their mapped segments), memory map queries, safe reads/writes, code patches
// that are always undone on unload, and executable memory for stubs/trampolines.
#include "codex.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <tlhelp32.h>
#else
#include <elf.h>
#include <fstream>
#include <link.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

uint32_t fnv1a(const void *p, size_t n, uint32_t h)
{
	const uint8_t *b = (const uint8_t *)p;
	for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 16777619u; }
	return h;
}

static std::string lower(std::string s) { for (auto &c : s) if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a'; return s; }
static std::string base_name(const std::string &p)
{
	size_t k = p.find_last_of("/\\");
	return lower(k == std::string::npos ? p : p.substr(k + 1));
}

// Module objects never move or disappear (a deque only grows): pointers handed out stay valid across mod_refresh(),
// and lazily loaded data (symbols, GOT slots, fingerprint) is kept. Unloaded modules are only marked.
static std::deque<Module> s_mods;
static std::vector<Module> s_fresh;

// ---------------------------------------------------------------------------------------------------------------
// module enumeration
#ifdef _WIN32
static void add_pe_module(HMODULE h, const char *path)
{
	uintptr_t base = (uintptr_t)h;
	auto *dos = (IMAGE_DOS_HEADER *)base;
	if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
	auto *nt = (IMAGE_NT_HEADERS32 *)(base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE) return;
	Module m;
	m.path = path; m.name = base_name(path); m.base = base; m.size = nt->OptionalHeader.SizeOfImage; m.elf = false;
	m.segs.push_back({base, base + nt->OptionalHeader.SizeOfHeaders, true, false, false});
	auto *sec = IMAGE_FIRST_SECTION(nt);
	for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
		uintptr_t lo = base + sec->VirtualAddress;
		uintptr_t sz = sec->Misc.VirtualSize ? sec->Misc.VirtualSize : sec->SizeOfRawData;
		uint32_t c = sec->Characteristics;
		m.segs.push_back({lo, lo + sz, (c & IMAGE_SCN_MEM_READ) != 0, (c & IMAGE_SCN_MEM_WRITE) != 0, (c & IMAGE_SCN_MEM_EXECUTE) != 0});
	}
	s_fresh.push_back(m);
}
static void merge_fresh();
void mod_refresh()
{
	s_fresh.clear();
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
	if (snap == INVALID_HANDLE_VALUE) return;
	MODULEENTRY32 me; me.dwSize = sizeof me;
	for (BOOL ok = Module32First(snap, &me); ok; ok = Module32Next(snap, &me)) add_pe_module(me.hModule, me.szExePath);
	CloseHandle(snap);
	merge_fresh();
}
#else
static int phdr_cb(struct dl_phdr_info *info, size_t, void *)
{
	Module m;
	std::string path = info->dlpi_name ? info->dlpi_name : "";
	if (path.empty()) {
		char buf[1024] = {0};
		ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
		if (n > 0) path.assign(buf, n);
	}
	if (path.empty() || path.find("linux-gate") != std::string::npos || path.find("linux-vdso") != std::string::npos) return 0;
	m.path = path; m.name = base_name(path); m.base = info->dlpi_addr; m.elf = true;
	uintptr_t lo_all = ~(uintptr_t)0, hi_all = 0;
	for (int i = 0; i < info->dlpi_phnum; ++i) {
		const ElfW(Phdr) &ph = info->dlpi_phdr[i];
		if (ph.p_type != PT_LOAD) continue;
		uintptr_t lo = info->dlpi_addr + ph.p_vaddr, hi = lo + ph.p_memsz;
		m.segs.push_back({lo, hi, (ph.p_flags & PF_R) != 0, (ph.p_flags & PF_W) != 0, (ph.p_flags & PF_X) != 0});
		lo_all = std::min(lo_all, lo); hi_all = std::max(hi_all, hi);
	}
	if (m.segs.empty()) return 0;
	if (m.base == 0) m.base = lo_all & ~(uintptr_t)0xFFF;        // the main executable (non-PIE)
	m.size = hi_all - m.base;
	s_fresh.push_back(m);
	return 0;
}
static void merge_fresh();
void mod_refresh()
{
	s_fresh.clear();
	dl_iterate_phdr(phdr_cb, nullptr);
	merge_fresh();
}
#endif

static void merge_fresh()
{
	for (auto &e : s_mods) e.loaded = false;
	for (auto &f : s_fresh) {
		Module *hit = nullptr;
		for (auto &e : s_mods) if (e.base == f.base && e.size == f.size && e.name == f.name) { hit = &e; break; }
		if (hit) { hit->segs = f.segs; hit->loaded = true; }
		else s_mods.push_back(f);
	}
	s_fresh.clear();
}

static void finish_module(Module &m)
{
	if (!m.fingerprint.empty()) return;
	uint32_t h = fnv1a(&m.size, sizeof m.size);
	for (auto &s : m.segs) if (s.x) { size_t n = std::min<size_t>(s.hi - s.lo, 65536); std::vector<uint8_t> buf(n); if (mem_read_orig(s.lo, buf.data(), n)) h = fnv1a(buf.data(), n, h); break; }   // as shipped: our hooks must not change it
	char b[16]; snprintf(b, sizeof b, "%08x", h);
	m.fingerprint = b;
}

const std::deque<Module> &mod_list()
{
	if (s_mods.empty()) mod_refresh();
	for (auto &m : s_mods) if (m.loaded) finish_module(m);
	return s_mods;
}

Module *mod_of(uintptr_t a)
{
	for (int pass = 0; pass < 2; ++pass) {
		if (s_mods.empty() || pass == 1) mod_refresh();
		for (auto &m : s_mods) if (m.loaded) if (m.contains(a)) { finish_module(m); return &m; }
	}
	return nullptr;
}

Module *mod_find(const char *name)
{
	if (!name || !*name) return nullptr;
	std::string n = lower(name);
	for (int pass = 0; pass < 2; ++pass) {
		if (s_mods.empty() || pass == 1) mod_refresh();
		Module *hit = nullptr;
		if (n == "game" || n == "mod" || n == "gamedll") {
			if (gpGamedllFuncs && gpGamedllFuncs->dllapi_table && gpGamedllFuncs->dllapi_table->pfnGameInit)
				for (auto &m : s_mods) if (m.loaded) if (m.contains((uintptr_t)gpGamedllFuncs->dllapi_table->pfnGameInit)) hit = &m;
		} else if (n == "engine") {
			for (auto &m : s_mods) if (m.loaded)
				if (m.name.find("engine_i") == 0 || m.name.find("engine_amd") == 0 || m.name == "swds.dll" || m.name == "hw.dll" || m.name == "sw.dll") hit = &m;
		} else if (n == "metamod") {
			if (gpMetaUtilFuncs) for (auto &m : s_mods) if (m.loaded) if (m.contains((uintptr_t)gpMetaUtilFuncs->pfnLogConsole)) hit = &m;
		} else if (n == "xmod") {
			for (auto &m : s_mods) if (m.loaded) if (m.name.find("xmod_mm") == 0) hit = &m;
		} else if (n == "codex" || n == "self") {
			for (auto &m : s_mods) if (m.loaded) if (m.contains((uintptr_t)&mod_find)) hit = &m;
		} else {
			for (auto &m : s_mods) if (m.loaded) if (m.name == n) hit = &m;
			if (!hit) for (auto &m : s_mods) if (m.loaded) if (m.name.find(n) != std::string::npos) { hit = &m; break; }
		}
		if (hit) { finish_module(*hit); return hit; }
	}
	return nullptr;
}

// ---------------------------------------------------------------------------------------------------------------
// memory map
#ifdef _WIN32
bool mem_query(uintptr_t a, size_t n, bool *r, bool *w, bool *x)
{
	bool R = true, W = true, X = true;
	uintptr_t end = a + (n ? n : 1);
	if (end < a) return false;
	while (a < end) {
		MEMORY_BASIC_INFORMATION mbi;
		if (!VirtualQuery((void *)a, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
		DWORD p = mbi.Protect & 0xFF;
		if ((mbi.Protect & PAGE_GUARD) || p == PAGE_NOACCESS) return false;
		R = R && (p == PAGE_READONLY || p == PAGE_READWRITE || p == PAGE_WRITECOPY || p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY);
		W = W && (p == PAGE_READWRITE || p == PAGE_WRITECOPY || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY);
		X = X && (p == PAGE_EXECUTE || p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY);
		a = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
	}
	if (r) *r = R; if (w) *w = W; if (x) *x = X;
	return true;
}
#else
struct MapRange { uintptr_t lo, hi; bool r, w, x; };
static std::vector<MapRange> s_maps;
static void maps_reload()
{
	s_maps.clear();
	FILE *f = fopen("/proc/self/maps", "r");
	if (!f) return;
	char line[512];
	while (fgets(line, sizeof line, f)) {
		unsigned long lo, hi; char perm[8] = {0};
		if (sscanf(line, "%lx-%lx %7s", &lo, &hi, perm) != 3) continue;
		s_maps.push_back({(uintptr_t)lo, (uintptr_t)hi, perm[0] == 'r', perm[1] == 'w', perm[2] == 'x'});
	}
	fclose(f);
}
static bool maps_query(uintptr_t a, size_t n, bool *r, bool *w, bool *x)
{
	bool R = true, W = true, X = true;
	uintptr_t end = a + (n ? n : 1);
	if (end < a) return false;
	uintptr_t cur = a;
	while (cur < end) {
		const MapRange *hit = nullptr;
		// binary search (the list is sorted)
		size_t lo = 0, hi = s_maps.size();
		while (lo < hi) { size_t mid = (lo + hi) / 2; if (s_maps[mid].hi <= cur) lo = mid + 1; else hi = mid; }
		if (lo < s_maps.size() && s_maps[lo].lo <= cur && cur < s_maps[lo].hi) hit = &s_maps[lo];
		if (!hit) return false;
		R = R && hit->r; W = W && hit->w; X = X && hit->x;
		cur = hit->hi;
	}
	if (r) *r = R; if (w) *w = W; if (x) *x = X;
	return true;
}
bool mem_query(uintptr_t a, size_t n, bool *r, bool *w, bool *x)
{
	if (s_maps.empty()) maps_reload();
	if (maps_query(a, n, r, w, x)) return true;
	maps_reload();                                  // the map changed (new allocation, dlopen, mprotect)
	return maps_query(a, n, r, w, x);
}
#endif

bool mem_readable(uintptr_t a, size_t n) { bool r; return a >= 0x10000 && mem_query(a, n, &r, nullptr, nullptr) && r; }
bool mem_writable(uintptr_t a, size_t n) { bool w; return a >= 0x10000 && mem_query(a, n, nullptr, &w, nullptr) && w; }
bool mem_executable(uintptr_t a) { bool x; return a >= 0x10000 && mem_query(a, 1, nullptr, nullptr, &x) && x; }

bool mem_read(uintptr_t a, void *out, size_t n)
{
	if (!n) return true;
	if (!mem_readable(a, n)) return false;
	return guarded_copy(out, (const void *)a, n);
}
bool mem_write(uintptr_t a, const void *src, size_t n)
{
	if (!n) return true;
	if (!mem_writable(a, n)) return false;
	return guarded_copy((void *)a, src, n);
}
bool mem_cstr(uintptr_t a, std::string &out, size_t max)
{
	out.clear();
	if (!a) return false;
	char buf[64];
	while (out.size() < max) {
		size_t chunk = 64 - (a & 63);           // never cross into an unmapped page in one read
		if (!mem_read(a, buf, chunk)) return !out.empty();
		for (size_t i = 0; i < chunk; ++i) { if (!buf[i]) return true; out += buf[i]; }
		a += chunk;
	}
	return true;
}

#ifdef _WIN32
bool mem_write_code(uintptr_t a, const void *src, size_t n)
{
	DWORD old;
	if (!VirtualProtect((void *)a, n, PAGE_EXECUTE_READWRITE, &old)) return false;
	bool ok = guarded_copy((void *)a, src, n);
	DWORD tmp;
	VirtualProtect((void *)a, n, old, &tmp);
	FlushInstructionCache(GetCurrentProcess(), (void *)a, n);
	return ok;
}
void *exec_alloc(size_t n)
{
	static uint8_t *cur = nullptr, *end = nullptr;
	n = (n + 15) & ~(size_t)15;
	if (!cur || cur + n > end) {
		size_t sz = std::max<size_t>(65536, n);
		cur = (uint8_t *)VirtualAlloc(nullptr, sz, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
		if (!cur) return nullptr;
		end = cur + sz;
	}
	void *p = cur; cur += n;
	memset(p, 0xCC, n);
	return p;
}
#else
bool mem_write_code(uintptr_t a, const void *src, size_t n)
{
	bool r = true, w = false, x = true;
	mem_query(a, n, &r, &w, &x);
	long pg = sysconf(_SC_PAGESIZE);
	uintptr_t lo = a & ~(uintptr_t)(pg - 1), hi = (a + n + pg - 1) & ~(uintptr_t)(pg - 1);
	if (mprotect((void *)lo, hi - lo, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) return false;
	bool ok = guarded_copy((void *)a, src, n);
	mprotect((void *)lo, hi - lo, (r ? PROT_READ : 0) | (w ? PROT_WRITE : 0) | (x ? PROT_EXEC : 0));
	maps_reload();
	return ok;
}
void *exec_alloc(size_t n)
{
	static uint8_t *cur = nullptr, *end = nullptr;
	n = (n + 15) & ~(size_t)15;
	if (!cur || cur + n > end) {
		size_t sz = std::max<size_t>(65536, n);
		void *p = mmap(nullptr, sz, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (p == MAP_FAILED) return nullptr;
		cur = (uint8_t *)p; end = cur + sz;
		maps_reload();
	}
	void *p = cur; cur += n;
	memset(p, 0xCC, n);
	return p;
}
#endif

// ---------------------------------------------------------------------------------------------------------------
// patches
struct Patch { int id; uintptr_t addr; std::vector<uint8_t> orig, now; std::string owner; };
static std::vector<Patch> s_patches;
static int s_patch_seq = 0;

int patch_apply(uintptr_t a, const std::vector<uint8_t> &bytes, const std::vector<uint8_t> *expect, const char *owner)
{
	if (bytes.empty() || bytes.size() > 4096) { cx_set_error("patch: bad size"); return 0; }
	if (!mem_readable(a, bytes.size())) { cx_set_error("patch: address %08x is not mapped memory", (unsigned)a); return 0; }
	for (auto &p : s_patches)
		if (a < p.addr + p.now.size() && p.addr < a + bytes.size()) { cx_set_error("patch: overlaps patch #%d (%s) at %08x", p.id, p.owner.c_str(), (unsigned)p.addr); return 0; }
	Patch p;
	p.addr = a; p.now = bytes; p.owner = owner ? owner : "";
	p.orig.resize(bytes.size());
	if (!mem_read(a, p.orig.data(), p.orig.size())) { cx_set_error("patch: cannot read %08x", (unsigned)a); return 0; }
	if (expect && !expect->empty()) {
		if (expect->size() > p.orig.size() || memcmp(expect->data(), p.orig.data(), expect->size()) != 0) {
			cx_set_error("patch: expected bytes [%s] at %08x but found [%s] - wrong build or already patched", hex_bytes(expect->data(), expect->size()).c_str(),
				(unsigned)a, hex_bytes(p.orig.data(), std::min(expect->size(), p.orig.size())).c_str());
			return 0;
		}
	}
	bool ok = mem_writable(a, bytes.size()) ? mem_write(a, bytes.data(), bytes.size()) : mem_write_code(a, bytes.data(), bytes.size());
	if (!ok) { cx_set_error("patch: could not write %08x", (unsigned)a); return 0; }
	p.id = ++s_patch_seq;
	s_patches.push_back(p);
	return p.id;
}

static bool patch_undo(const Patch &p)
{
	std::vector<uint8_t> cur(p.now.size());
	if (!mem_read(p.addr, cur.data(), cur.size())) return false;
	if (cur != p.now) {
		cx_err("patch #%d (%s) at %08x was changed by someone else - not restoring it", p.id, p.owner.c_str(), (unsigned)p.addr);
		return false;
	}
	return mem_writable(p.addr, p.orig.size()) ? mem_write(p.addr, p.orig.data(), p.orig.size()) : mem_write_code(p.addr, p.orig.data(), p.orig.size());
}

bool patch_remove(int id)
{
	for (size_t i = 0; i < s_patches.size(); ++i)
		if (s_patches[i].id == id) { bool ok = patch_undo(s_patches[i]); s_patches.erase(s_patches.begin() + i); return ok; }
	return false;
}

void patch_remove_all()
{
	for (size_t i = s_patches.size(); i-- > 0;) patch_undo(s_patches[i]);
	s_patches.clear();
}

// memory as it was before any Codex patch / hook (what signatures and constant searches must look at)
bool mem_read_orig(uintptr_t a, void *buf, size_t n)
{
	if (!mem_read(a, buf, n)) return false;
	uint8_t *b = (uint8_t *)buf;
	for (auto &p : s_patches)
		for (size_t i = 0; i < p.orig.size(); ++i)
			if (p.addr + i >= a && p.addr + i < a + n) b[p.addr + i - a] = p.orig[i];
	return true;
}
bool patch_ranges(uintptr_t lo, uintptr_t hi, std::vector<std::pair<uintptr_t, uintptr_t>> &out)
{
	out.clear();
	for (auto &p : s_patches) if (p.addr < hi && p.addr + p.now.size() > lo) out.push_back({p.addr, p.addr + p.now.size()});
	return !out.empty();
}

std::vector<PatchInfo> patch_list()
{
	std::vector<PatchInfo> v;
	for (auto &p : s_patches) v.push_back({p.id, p.addr, p.orig, p.now, p.owner});
	return v;
}

// ---------------------------------------------------------------------------------------------------------------
std::string cx_data_path(const char *file)
{
	char gd[256] = {0};
	if (g_engfuncs.pfnGetGameDir) g_engfuncs.pfnGetGameDir(gd);
	std::string base = gd[0] ? gd : ".";
	for (const char *part : {"/addons", "/xmod", "/data", "/codex"}) {
		base += part;
#ifdef _WIN32
		_mkdir(base.c_str());
#else
		mkdir(base.c_str(), 0755);
#endif
	}
	return base + "/" + file;
}
