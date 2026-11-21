// Ham-style virtual hooks for CBaseEntity-derived classes (stock CS 1.6 or ReGameDLL).
//   xmod.on("ham:takedamage:player", function(victim, inflictor, attacker, damage, bits) ... end)
//   xmod.on("ham:killed:player:post", function(victim, attacker, gib, ret) ... end)
// Patches the class vtable slot (virtual indices from AMXX CS gamedata). Applied once the map is loaded.
#include "hookutil.h"
#include <cstring>
#include <set>
#ifdef _WIN32
#else
#include <sys/mman.h>
#include <unistd.h>
#include <stdint.h>
#endif

struct HamCbase;
template <> struct is_ent<HamCbase> : std::true_type {};

#if defined(_WIN32) && defined(_MSC_VER)
#define HAM_CC __fastcall
#define HAM_DUMMY int,
#elif defined(_WIN32)
#define HAM_CC __attribute__((thiscall))
#define HAM_DUMMY
#else
#define HAM_CC
#define HAM_DUMMY
#endif

static bool patch_slot(void **slot, void *nv) {
#ifdef _WIN32
	DWORD old; if (!VirtualProtect(slot, sizeof(void *), PAGE_EXECUTE_READWRITE, &old)) return false;
	*slot = nv; VirtualProtect(slot, sizeof(void *), old, &old); return true;
#else
	long ps = sysconf(_SC_PAGESIZE); uintptr_t a = (uintptr_t)slot & ~(uintptr_t)(ps - 1), e = ((uintptr_t)slot + sizeof(void *) + ps - 1) & ~(uintptr_t)(ps - 1);
	if (mprotect((void *)a, e - a, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) return false;   // left writable on purpose (neighbouring data may share the page)
	*slot = nv; return true;
#endif
}

template <class F, class... X> inline auto call_orig(F fn, void *self, X &...x) {
#if defined(_WIN32) && defined(_MSC_VER)
	return fn(self, 0, x...);
#else
	return fn(self, x...);
#endif
}
struct SlotE { std::string cls; Event *pre, *post; };
struct SlotInfo { void **vt; void *orig; std::vector<SlotE> es; };

template <class Tag, class R, class... A> struct HamHook {
	static inline std::vector<SlotInfo> infos;
	using Orig = R(HAM_CC *)(void *, HAM_DUMMY A...);
	static R HAM_CC tramp(void *self, HAM_DUMMY A... a) {
		void **vt = *reinterpret_cast<void ***>(self);
		void *orig = nullptr; Event *pre = nullptr, *post = nullptr;
		for (auto &s : infos) if (s.vt == vt) {
			orig = s.orig;
			if (s.es.size() == 1) { pre = s.es[0].pre; post = s.es[0].post; }
			else { entvars_t *pv = pev_of(self); const char *c = pv ? STRING(pv->classname) : "";
				for (auto &e : s.es) if (e.cls == c) { pre = e.pre; post = e.post; break; } }
			break;
		}
		Orig fn = reinterpret_cast<Orig>(orig);
		std::tuple<std::decay_t<A>...> t(a...);
		auto next = [&](auto &tt) -> R { return std::apply([&](auto &...x) -> R { return call_orig(fn, self, x...); }, tt); };
		return hook_core<R>(pre, post, ent_of(self), t, next);
	}
	static bool install(void **vt, int idx, const std::string &cls, Event *pre, Event *post) {
		for (auto &s : infos) if (s.vt == vt) { for (auto &e : s.es) if (e.cls == cls) return true; s.es.push_back({cls, pre, post}); return true; }
		void *cur = vt[idx];
		if (cur == reinterpret_cast<void *>(&tramp)) {                // this vtable already points at our trampoline (e.g. a copy of a patched table): never wrap ourselves
			if (infos.empty()) return false;
			SlotInfo si; si.vt = vt; si.orig = infos[0].orig; si.es.push_back({cls, pre, post}); infos.push_back(si); return true;
		}
		SlotInfo si; si.vt = vt; si.orig = cur; si.es.push_back({cls, pre, post});
		infos.push_back(si);
		return patch_slot(&vt[idx], reinterpret_cast<void *>(&tramp));
	}
};

struct HamEntry { const char *name; int idx; bool (*install)(void **, int, const std::string &, Event *, Event *); };
#ifdef _WIN32
#define HAM_IDX(W, L) W
#else
#define HAM_IDX(W, L) L
#endif
#define HAM(NAME, W, L, R, ...) struct HamTag_##NAME {};
#include "ham_list.inc"
#undef HAM
#define HAM(NAME, W, L, R, ...) { #NAME, HAM_IDX(W, L), &HamHook<HamTag_##NAME, R, ##__VA_ARGS__>::install },
static HamEntry g_ham[] = {
#include "ham_list.inc"
	{ nullptr, 0, nullptr } };
#undef HAM

struct Pending { const HamEntry *fn; std::string cls; Event *pre, *post; bool done; };
static std::vector<Pending> g_pending;
static bool g_ready = false;

static void **vtable_of_class(const std::string &cls) {
	edict_t *e = CREATE_NAMED_ENTITY(MAKE_STRING(cls.c_str()));
	if (!e || !e->pvPrivateData) { if (e) REMOVE_ENTITY(e); return nullptr; }
	void **vt = *reinterpret_cast<void ***>(e->pvPrivateData);
	REMOVE_ENTITY(e);
	return vt;
}
static std::vector<void **> g_special_vts;       // vtables of bots (CCSBot etc. have their own vtable, a copy of CBasePlayer's with overrides)
static void apply_one(Pending &p) {
	if (p.done) return;
	void **vt = vtable_of_class(p.cls);
	if (!vt) { xerr("ham: cannot create entity class '%s' to find its vtable", p.cls.c_str()); p.done = true; return; }
	if (!p.fn->install(vt, p.fn->idx, p.cls, p.pre, p.post)) xerr("ham: failed to patch vtable of '%s'", p.cls.c_str());
	if (p.cls == "player") for (void **bvt : g_special_vts) p.fn->install(bvt, p.fn->idx, p.cls, p.pre, p.post);
	p.done = true;
}
void ham_apply_pending() { g_ready = true; for (auto &p : g_pending) apply_one(p); }

bool ham_register(const std::string &func, const std::string &cls, std::string &err) {
	const HamEntry *fn = nullptr; for (HamEntry *e = g_ham; e->name; ++e) if (func == e->name) fn = e;
	if (!fn) { err = "unknown/unsupported ham function '" + func + "' (see xmod.ham.list())"; return false; }
	if (cls.empty()) { err = "ham hook needs a classname: ham:" + func + ":<classname>"; return false; }
	for (auto &p : g_pending) if (p.fn == fn && p.cls == cls) return true;       // already installed (e.g. after `xmod reload`)
	Pending p{fn, cls, ev_get(("ham:" + func + ":" + cls).c_str()), ev_get(("ham:" + func + ":" + cls + ":post").c_str()), false};
	g_pending.push_back(p);
	if (g_ready) apply_one(g_pending.back());
	return true;
}
void ham_list(lua_State *l) { lua_newtable(l); int i = 1; for (HamEntry *e = g_ham; e->name; ++e) { lua_pushstring(l, e->name); lua_rawseti(l, -2, i++); } }

// Bots: hooks on "player" must also be installed in the bot class's own vtable (AMXX calls this "special bot"). Called when a bot first thinks.
void ham_hook_special(edict_t *e) {
	if (!g_ready || !e || !e->pvPrivateData) return;
	void **vt = *reinterpret_cast<void ***>(e->pvPrivateData);
	bool known = false; for (void **v : g_special_vts) if (v == vt) known = true;
	if (!known) g_special_vts.push_back(vt);
	for (auto &p : g_pending) if (p.done && p.cls == "player") p.fn->install(vt, p.fn->idx, p.cls, p.pre, p.post);
}
