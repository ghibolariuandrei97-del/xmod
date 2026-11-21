// Codex - call, hook and patch any function of the engine / game DLL by signature, symbol, string reference,
// address or vtable index, from xmod Lua (xmod.codex.*). An xmod extension; the spiritual successor of Orpheu.
//
// Safety model (why Codex does not take the server down when something is wrong):
//   * nothing is touched until it is resolved AND validated (address inside an executable section of a loaded module,
//     unique signature, decodable prologue); every failure is a Lua error / nil + codex.last_error(), never a crash
//   * memory reads/writes check the memory map first and run under a fault guard
//   * codex.call() runs under a fault guard: an access violation inside the called function becomes a Lua error,
//     the function is marked "faulted" and refused until codex.reset(name)
//   * detours relocate the stolen instructions properly (rel32/rel8 branches, PIC get_pc thunks) and refuse
//     prologues they cannot move safely (use a vtable hook then); every patch is undone on unload
//   * codex.call() never re-enters Codex hooks (like ExecuteHam), so a handler calling its own function cannot recurse
#pragma once

#include "mm_sdk.h"
#include "xmod_ext.h"

#include <cstdint>
#include <array>
#include <cstring>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

#define CODEX_VERSION "1.0.1"
#define CX_NS         "codex"

extern const XmodApi *g_xmod;
extern XmodExt       *g_xext;
extern bool           g_sync_hooks;     // the connected xmod has fire_hook_sync (0.4.1+)

void cx_log(const char *fmt, ...);
void cx_err(const char *fmt, ...);
void cx_set_error(const char *fmt, ...);
const std::string &cx_last_error();
std::string cx_data_path(const char *file);     // <gamedir>/addons/xmod/data/codex/<file>

// ---------------------------------------------------------------------------------------------------------------
// modules and memory (mem.cpp)
struct Segment { uintptr_t lo, hi; bool r, w, x; };
struct Module {
	std::string name, path;            // file name (lower case) and full path
	uintptr_t base = 0, size = 0;      // load base, image span
	std::vector<Segment> segs;          // mapped parts (only these may be read)
	bool elf = false;
	uintptr_t got = 0;                  // ELF: _GLOBAL_OFFSET_TABLE_ (PIC references are GOT-relative)
	std::string fingerprint;            // identifies this exact build (cache key)
	// lazily loaded
	bool syms_loaded = false;
	struct Sym { std::string name; uintptr_t addr; uint32_t size; bool func; };
	std::vector<Sym> syms;
	bool relocs_loaded = false;
	std::vector<uintptr_t> relocs;      // PE: sorted absolute addresses of relocated dwords
	bool contains(uintptr_t a) const { return a >= base && a < base + size; }
	const Segment *seg_of(uintptr_t a) const { for (auto &s : segs) if (a >= s.lo && a < s.hi) return &s; return nullptr; }
};
void mod_refresh();
const std::vector<Module> &mod_list();
Module *mod_find(const char *name);          // "engine", "game"/"mod", "metamod", "xmod", or (part of) a file name
Module *mod_of(uintptr_t addr);
bool mem_query(uintptr_t a, size_t n, bool *r, bool *w, bool *x);   // whole range must share the flags
bool mem_readable(uintptr_t a, size_t n);
bool mem_writable(uintptr_t a, size_t n);
bool mem_executable(uintptr_t a);
bool mem_read(uintptr_t a, void *out, size_t n);                    // validated + fault-guarded
bool mem_write(uintptr_t a, const void *src, size_t n);             // validated + fault-guarded (data)
bool mem_write_code(uintptr_t a, const void *src, size_t n);        // unprotects code, writes, restores protection
bool mem_cstr(uintptr_t a, std::string &out, size_t max = 4096);    // safe C string read
void *exec_alloc(size_t n);                                         // RWX memory for stubs and trampolines (never freed)
uint32_t fnv1a(const void *p, size_t n, uint32_t h = 2166136261u);

// patches (mem.cpp): every write to code goes through here and is undone on unload
int  patch_apply(uintptr_t a, const std::vector<uint8_t> &bytes, const std::vector<uint8_t> *expect, const char *owner);
bool patch_remove(int id);
void patch_remove_all();
struct PatchInfo { int id; uintptr_t addr; std::vector<uint8_t> orig, now; std::string owner; };
std::vector<PatchInfo> patch_list();
bool mem_read_orig(uintptr_t a, void *buf, size_t n);                  // our patches/hooks undone in the copy
bool patch_ranges(uintptr_t lo, uintptr_t hi, std::vector<std::pair<uintptr_t, uintptr_t>> &out);

// ---------------------------------------------------------------------------------------------------------------
// x86-32 instruction length decoder (lde.cpp)
struct Insn {
	int len = 0;
	bool valid = false;
	int opcode = 0;          // primary opcode; 0x0F00 | b for two-byte, 0x0F3800 | b / 0x0F3A00 | b for three-byte
	int prefixes = 0;        // count
	bool opsize16 = false, addr16 = false;
	bool has_modrm = false;
	int mod = 0, reg = 0, rm = 0;
	int sib_base = -1;
	int disp_off = 0, disp_size = 0; bool disp_abs = false; int32_t disp = 0;
	int imm_off = 0, imm_size = 0; int32_t imm = 0;
	int rel_off = 0, rel_size = 0; int32_t rel = 0;   // relative branch operand
	bool is_ret = false, is_call = false, is_jmp = false, is_jcc = false;   // jmp: unconditional
	bool is_loop = false;    // E0..E3: no rel32 form
	uintptr_t target(uintptr_t at) const { return at + len + rel; }   // only if rel_size
};
bool lde_decode(const uint8_t *p, Insn &in);
std::string hex_bytes(const uint8_t *p, size_t n);

// ---------------------------------------------------------------------------------------------------------------
// signatures, symbols, string references, signature maker (scan.cpp)
struct Sig { std::vector<uint8_t> b, m; };   // m[i] = 1: byte must match
bool sig_parse(const std::string &text, Sig &out, std::string &err);   // "55 8B EC ?? 83" | "\x55\x8B\x2A" (0x2A = wildcard)
std::string sig_text(const Sig &s);
size_t sig_scan(const Module &m, const Sig &s, std::vector<uintptr_t> &out, size_t max, bool code_only = true);
bool sig_match_at(uintptr_t a, const Sig &s);
uintptr_t sym_find(Module &m, const std::string &name, std::string &err);   // mangled, demangled, or "Class::Method"
const Module::Sym *sym_containing(Module &m, uintptr_t a);
uintptr_t func_start(Module &m, uintptr_t a, bool *exact);
struct XRef { uintptr_t ref, func; };
std::vector<XRef> xref_string(Module &m, const std::string &text, size_t max);
struct SigMade { std::string text; int len = 0, matches = 0; bool unique = false; int adjust = 0; };   // text includes " @adjust" when adjust != 0
bool sig_split(const std::string &text, std::string &sig, int &adjust);   // "55 8B .. @-24" -> sig + adjust
bool sig_make(uintptr_t addr, Module &m, SigMade &out, std::string &err, int maxlen = 160);

// ---------------------------------------------------------------------------------------------------------------
// raw calls and the fault guard (guard.cpp + asm in thunks.cpp)
struct CallCtx {             // field offsets are used by the assembly in thunks.cpp: do not reorder
	void *fn;                // 0
	uint32_t nwords;         // 4
	uint32_t *words;         // 8
	uint32_t ecx;            // 12
	uint32_t edx;            // 16
	uint32_t fpu;            // 20  1 = result in st(0)
	uint32_t r_eax;          // 24
	uint32_t r_edx;          // 28
	double r_st0;            // 32
	uint32_t rec_esp;        // 40  (guard bookkeeping)
	uint32_t rec_ebp;        // 44
	uint32_t faulted;        // 48
	uint32_t fault_code;     // 52  Windows exception code / Linux signal number
	uint32_t fault_addr;     // 56  data address of an access violation
	uint32_t fault_ip;       // 60  instruction that faulted
	uint8_t fpu_env[32];     // 64  x87 environment (28 bytes used)
};
struct HookRet {             // filled by codex_hook_dispatch, consumed by codex_hook_entry (asm)
	uint32_t eax;            // 0
	uint32_t edx;            // 4
	double st0;              // 8
	uint32_t fpu;            // 16
	uint32_t pop;            // 20  bytes the callee must pop (stdcall / MSVC thiscall)
};
extern "C" void codex_raw_call(CallCtx *c);            // plain call
bool guard_install();
void guard_remove();
bool guarded_call(CallCtx *c);                         // false = the call faulted (c->fault_*)
bool guarded_copy(void *dst, const void *src, size_t n);
std::string fault_text(const CallCtx &c);
extern "C" void codex_hook_entry();                    // common hook entry (asm), Hook* in eax

// ---------------------------------------------------------------------------------------------------------------
// functions, hooks (func.cpp)
enum CxType {
	T_VOID, T_BOOL, T_I8, T_U8, T_I16, T_U16, T_I32, T_U32, T_I64, T_F32, T_F64, T_PTR, T_STR,
	T_CBASE, T_EDICT, T_ENTVARS, T_VECPTR, T_VECVAL,
	T_ASTR,      // char* in the engine string pool (AllocString): lives until the map changes - for strings the game keeps
	T_STRT,      // string_t (an offset into the engine string pool), e.g. pev->classname
	T__COUNT
};
enum CxConv { CONV_CDECL, CONV_STDCALL, CONV_THISCALL, CONV_FASTCALL };
bool type_parse(const char *s, CxType &t);
const char *type_name(CxType t);

struct ArgSlot { CxType t; int word; int nwords; int reg; };   // reg: 0 none, 1 ecx, 2 edx
struct Hook;
struct Func {
	std::string name, module;
	CxConv conv = CONV_CDECL;
	bool has_this = false; CxType this_t = T_CBASE;
	std::vector<CxType> args; CxType ret = T_VOID;
	// computed layout
	std::vector<ArgSlot> slots;      // slot 0 = this when has_this
	int stack_words = 0, pop_bytes = 0;
	// resolution
	uintptr_t addr = 0; int vindex = -1; std::string how, sig_used; bool resolved = false; std::string why;
	// runtime
	bool faulted = false; std::string fault;
	uint64_t calls = 0, hooked_calls = 0;
	std::vector<Hook *> hooks;
	std::string ev_pre, ev_post;
};
struct Hook {
	Func *f = nullptr;
	bool vtable = false;
	uintptr_t target = 0;            // detour: function address; vtable: slot address
	uintptr_t original = 0;          // what to call for "the original" (trampoline or old slot value)
	int patch_id = 0;                // detour patch
	uint8_t *stub = nullptr;         // mov eax, this; jmp codex_hook_entry
	int depth = 0;
	bool removed = false;
	std::string label;               // class name for vtable hooks
	std::string ret_str;             // storage for string returns
};
Func *func_get(const std::string &name);
std::vector<Func *> func_all();
bool func_define(const XmodValue *def, std::string &err);
bool func_layout(Func &f, std::string &err);
bool func_call(Func &f, const XmodValue *const *argv, int argc, bool through_hooks, XmodValue **ret, std::string &err);
bool hook_add(Func &f, const XmodValue *opts, std::string &err);
bool hook_remove(Func &f, const char *label);
void hook_remove_all();
uintptr_t vtable_of(uintptr_t obj);
uintptr_t ent_to_cbase(int idx);
int cbase_to_ent(uintptr_t p);
bool build_trampoline(uintptr_t fn, uint8_t *&tramp, int &stolen, std::string &err);

// inside a running hook handler (Lua: codex.arg / set_arg / ret / set_ret / this)
struct HookFrame;
HookFrame *frame_top();
XmodValue *frame_arg(HookFrame *fr, int n, std::string &err);   // n = 0: this
bool frame_set_arg(HookFrame *fr, int n, const XmodValue *v, std::string &err);
XmodValue *frame_ret(HookFrame *fr, std::string &err);
bool frame_set_ret(HookFrame *fr, const XmodValue *v, std::string &err);

// value conversion helpers
XmodValue *native_to_val(CxType t, const uint32_t *w, double st0);
bool val_to_words(CxType t, const XmodValue *v, uint32_t *w, std::deque<std::string> &strs, std::deque<std::array<float, 3>> &vecs, std::string &err);
int type_words(CxType t);
extern bool g_safe_calls;

// ---------------------------------------------------------------------------------------------------------------
// resolution cache (cache.cpp): remembers where functions were and their auto-generated signatures
void cache_load();
void cache_save();
bool cache_get(const Module &m, const std::string &func, uintptr_t &off, std::string &sig);
std::vector<std::string> cache_sigs_any_build(const std::string &modname, const std::string &func);
void cache_put(const Module &m, const std::string &func, uintptr_t off, const std::string &sig);
void cache_clear();

// Lua API (api.cpp)
bool codex_register(const XmodApi *api, XmodExt *ext);
const char *const *engfunc_names();
const char *const *dllfunc_names();
