// ReAPI-style hookchains: binds ReGameDLL's IReGameHookchains to the xmod event bus.
//   xmod.on("reapi:CBasePlayer_TakeDamage", function(victim, inflictor, attacker, damage, bits) ... end)
//   xmod.on("reapi:CBasePlayer_TakeDamage:post", function(victim, inflictor, attacker, damage, bits, ret) ... end)
#include "hookutil.h"
#include "reapi_decl.h"
#include <cstring>
#ifdef _WIN32
#else
#include <dlfcn.h>
#endif

static IReGameApi *g_api = nullptr;
static IReGameHookchains *g_hc = nullptr;

template <class Tag, class Chain> struct HookImpl;
template <class Tag, class R, class C, class... A> struct HookImpl<Tag, IHookChainClass<R, C, A...>> {
	static R fn(IHookChainClass<R, C, A...> *ch, C *self, A... a) {
		if constexpr (std::is_reference_v<R>) return ch->callNext(self, a...);
		else {
		std::tuple<std::decay_t<A>...> t(a...);
		auto next = [&](auto &tt) -> R {
			if constexpr (std::is_void_v<R>) { std::apply([&](auto &...x) { ch->callNext(self, x...); }, tt); writeback(std::index_sequence_for<A...>{}, tt, a...); }
			else { R r = std::apply([&](auto &...x) { return ch->callNext(self, x...); }, tt); writeback(std::index_sequence_for<A...>{}, tt, a...); return r; }
		};
		return hook_core<R>(Tag::pre, Tag::post, ent_of(self), t, next);
		}
	}
};
template <class Tag, class R, class... A> struct HookImpl<Tag, IHookChain<R, A...>> {
	static R fn(IHookChain<R, A...> *ch, A... a) {
		if constexpr (std::is_reference_v<R>) return ch->callNext(a...);
		else {
		std::tuple<std::decay_t<A>...> t(a...);
		auto next = [&](auto &tt) -> R {
			if constexpr (std::is_void_v<R>) { std::apply([&](auto &...x) { ch->callNext(x...); }, tt); writeback(std::index_sequence_for<A...>{}, tt, a...); }
			else { R r = std::apply([&](auto &...x) { return ch->callNext(x...); }, tt); writeback(std::index_sequence_for<A...>{}, tt, a...); return r; }
		};
		return hook_core<R>(Tag::pre, Tag::post, -1, t, next);
		}
	}
};

struct RgEntry { const char *name; bool (*reg)(); Event **pre; Event **post; bool registered; };
#define RG_HOOK(NAME) \
	struct Tag_##NAME { static inline Event *pre = nullptr, *post = nullptr; }; \
	static bool reg_##NAME() { g_hc->NAME()->registerHook(&HookImpl<Tag_##NAME, IReGameHook_##NAME>::fn, HC_PRIORITY_DEFAULT); return true; }
#include "reapi_list.inc"
#undef RG_HOOK
#define RG_HOOK(NAME) { #NAME, &reg_##NAME, &Tag_##NAME::pre, &Tag_##NAME::post, false },
static RgEntry g_table[] = {
#include "reapi_list.inc"
	{ nullptr, nullptr, nullptr, nullptr, false } };
#undef RG_HOOK

bool reapi_available() { return g_hc != nullptr; }

bool reapi_init(const char *gamedll_path) {
	typedef void *(*CreateInterface_t)(const char *, int *);
	CreateInterface_t ci = nullptr;
#ifdef _WIN32
	HMODULE h = GetModuleHandleA(gamedll_path);
	if (h) ci = (CreateInterface_t)GetProcAddress(h, "CreateInterface");
#else
	void *h = dlopen(gamedll_path, RTLD_NOW | RTLD_NOLOAD);
	if (h) ci = (CreateInterface_t)dlsym(h, "CreateInterface");
#endif
	if (!ci) return false;
	int rc = 0; g_api = (IReGameApi *)ci("VRE_GAMEDLL_API_VERSION001", &rc);
	if (!g_api) return false;
	g_hc = g_api->GetHookchains();
	if (!g_hc) return false;
	xlog("ReGameDLL API %d.%d detected - reapi:* hooks enabled", g_api->GetMajorVersion(), g_api->GetMinorVersion());
	return true;
}

bool reapi_register(const std::string &name) {
	if (!g_hc) return false;
	for (RgEntry *e = g_table; e->name; ++e) if (name == e->name) {
		if (!e->registered) { e->reg(); e->registered = true; }
		*e->pre = ev_get((std::string("reapi:") + name).c_str());
		*e->post = ev_get((std::string("reapi:") + name + ":post").c_str());
		return true;
	}
	return false;
}
void reapi_list(lua_State *l) {
	lua_newtable(l); int i = 1;
	for (RgEntry *e = g_table; e->name; ++e) { lua_pushstring(l, e->name); lua_rawseti(l, -2, i++); }
}
