// Shared marshalling for native hooks (ReAPI hookchains + Ham virtual hooks) -> Lua event bus.
#pragma once
#include "xmod.h"
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

enum { HC_CONTINUE = 0, HC_SUPERCEDE = 1, HC_BREAK = 2 };

struct AV { int kind = 5; double num = 0; const char *str = nullptr; float v[3] = {0, 0, 0}; };   // 0 int,1 float,2 str,3 ent,4 vec,5 nil
struct HookOut { int code = HC_CONTINUE; bool has = false; double num = 0; bool hasstr = false; std::string str; };
struct ModSet { bool set[16] = {false}; double val[16] = {0}; };
extern ModSet *g_curmods;                          // target of xmod.hook.set_arg while a handler runs

HookOut ev_fire_hook(Event *e, const std::vector<AV> &av, ModSet &mods);

template <class T> struct is_ent : std::false_type {};          // specialised for entity classes (generated + manual)
inline entvars_t *pev_of(const void *p) { return p ? *reinterpret_cast<entvars_t *const *>(reinterpret_cast<const char *>(p) + sizeof(void *)) : nullptr; }
inline int ent_of_pev(const entvars_t *pev) { return (pev && pev->pContainingEntity) ? ENTINDEX(pev->pContainingEntity) : 0; }
inline int ent_of_edict(const edict_t *e) { return e ? ENTINDEX(const_cast<edict_t *>(e)) : 0; }
template <class T> inline int ent_of(T *p) { return ent_of_pev(pev_of(p)); }

template <class T> AV pack_one(const T &x) {
	using D = std::decay_t<T>; AV a;
	if constexpr (std::is_same_v<D, bool> || std::is_integral_v<D> || std::is_enum_v<D>) { a.kind = 0; a.num = (double)(long long)x; }
	else if constexpr (std::is_floating_point_v<D>) { a.kind = 1; a.num = (double)x; }
	else if constexpr (std::is_same_v<D, const char *> || std::is_same_v<D, char *>) { a.kind = 2; a.str = x; }
	else if constexpr (std::is_same_v<D, entvars_t *>) { a.kind = 3; a.num = ent_of_pev(x); }
	else if constexpr (std::is_same_v<D, edict_t *> || std::is_same_v<D, const edict_t *>) { a.kind = 3; a.num = ent_of_edict(x); }
	else if constexpr (std::is_same_v<D, Vector>) { a.kind = 4; a.v[0] = x.x; a.v[1] = x.y; a.v[2] = x.z; }
	else if constexpr (std::is_pointer_v<D>) { if constexpr (is_ent<std::remove_pointer_t<D>>::value) { a.kind = 3; a.num = ent_of(x); } }
	return a;
}
template <class Tup, size_t... I> void pack_tuple(std::vector<AV> &av, const Tup &t, std::index_sequence<I...>) { (av.push_back(pack_one(std::get<I>(t))), ...); }
template <class T> void apply_mod(T &slot, const ModSet &m, size_t i) {
	if (i >= 16 || !m.set[i]) return;
	if constexpr (std::is_same_v<T, bool>) slot = m.val[i] != 0;
	else if constexpr (std::is_integral_v<T> || std::is_enum_v<T>) slot = (T)(long long)m.val[i];
	else if constexpr (std::is_floating_point_v<T>) slot = (T)m.val[i];
}
template <class Tup, size_t... I> void apply_mods(Tup &t, const ModSet &m, std::index_sequence<I...>) { (apply_mod(std::get<I>(t), m, I), ...); }
template <size_t... I, class Tup, class... A> void writeback(std::index_sequence<I...>, Tup &t, A &...a) { ((a = std::get<I>(t)), ...); }

template <class R> bool from_lua(R &out, const HookOut &h) {
	if constexpr (std::is_same_v<R, bool>) { if (!h.has) return false; out = h.num != 0; return true; }
	else if constexpr (std::is_integral_v<R> || std::is_enum_v<R>) { if (!h.has) return false; out = (R)(long long)h.num; return true; }
	else if constexpr (std::is_floating_point_v<R>) { if (!h.has) return false; out = (R)h.num; return true; }
	else if constexpr (std::is_same_v<R, const char *>) { static std::string keep; if (!h.hasstr) return false; keep = h.str; out = keep.c_str(); return true; }
	else if constexpr (std::is_pointer_v<R>) {
		if (!h.has) return false; int idx = (int)h.num; if (idx == 0) { out = nullptr; return true; }
		edict_t *e = ent_edict(idx); if (!e || !e->pvPrivateData) return false; out = reinterpret_cast<R>(e->pvPrivateData); return true; }
	else return false;
}
template <class R> void push_ret(std::vector<AV> &av, const R &r) { av.push_back(pack_one(r)); }

// Core of every native hook. `next(tuple&)` calls the next hook / the original with the (possibly modified) args.
template <class R, class Tup, class Next>
R hook_core(Event *pre, Event *post, int self, Tup &tup, Next next) {
	constexpr size_t N = std::tuple_size_v<Tup>; auto seq = std::make_index_sequence<N>{};
	bool hp = pre && ev_has(pre), hq = post && ev_has(post);
	if (!hp && !hq) return next(tup);
	std::vector<AV> av; if (self >= 0) { AV s; s.kind = 3; s.num = self; av.push_back(s); }
	pack_tuple(av, tup, seq);
	if (hp) {
		ModSet mods; HookOut r = ev_fire_hook(pre, av, mods);
		if (r.code == HC_SUPERCEDE) {
			if constexpr (std::is_void_v<R>) return; else { R v{}; from_lua(v, r); return v; }
		}
		apply_mods(tup, mods, seq);
	}
	if constexpr (std::is_void_v<R>) {
		next(tup);
		if (hq) { std::vector<AV> a2; if (self >= 0) { AV s; s.kind = 3; s.num = self; a2.push_back(s); } pack_tuple(a2, tup, seq); ModSet m; ev_fire_hook(post, a2, m); }
	} else {
		R ret = next(tup);
		if (hq) { std::vector<AV> a2; if (self >= 0) { AV s; s.kind = 3; s.num = self; a2.push_back(s); } pack_tuple(a2, tup, seq); push_ret(a2, ret); ModSet m; ev_fire_hook(post, a2, m); }
		return ret;
	}
}

// ---- subsystem entry points
bool reapi_init(const char *gamedll_path);
bool reapi_available();
bool reapi_register(const std::string &name);      // name without "reapi:"/":post"
void reapi_list(lua_State *l);
bool ham_register(const std::string &func, const std::string &cls, std::string &err);
void ham_apply_pending();                          // call once the map is loaded
void ham_list(lua_State *l);
void api_register_hooks(lua_State *l);
bool hooks_on_register(const char *event, std::string &err);   // called from xmod.on
