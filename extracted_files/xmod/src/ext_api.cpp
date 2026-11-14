// xmod extension API: lets other Metamod plugins register Lua-callable functions (sync, or async on a worker thread pool).
// Public contract: include/xmod_ext.h.  Everything here except the worker loop runs on the main (game) thread.
#include "xmod.h"
#include "ext_value.h"
#include <atomic>
#include <chrono>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#ifdef _WIN32
#else
#include <pthread.h>
#include <unistd.h>
#endif

// ------------------------------------------------------------------------------------------------ portable primitives
#ifdef _WIN32
struct Mutex { CRITICAL_SECTION cs; Mutex() { InitializeCriticalSection(&cs); } ~Mutex() { DeleteCriticalSection(&cs); } void lock() { EnterCriticalSection(&cs); } void unlock() { LeaveCriticalSection(&cs); } };
struct CondVar { CONDITION_VARIABLE cv; CondVar() { InitializeConditionVariable(&cv); } void wait(Mutex &m) { SleepConditionVariableCS(&cv, &m.cs, INFINITE); } void notify_one() { WakeConditionVariable(&cv); } void notify_all() { WakeAllConditionVariable(&cv); } };
typedef HANDLE ThreadH;
struct ThreadStart { void (*fn)(void *); void *arg; };
static DWORD WINAPI thread_tramp(LPVOID p) { ThreadStart ts = *static_cast<ThreadStart *>(p); delete static_cast<ThreadStart *>(p); ts.fn(ts.arg); return 0; }
static bool thread_start(void (*fn)(void *), void *arg, ThreadH *out) { *out = CreateThread(nullptr, 0, thread_tramp, new ThreadStart{fn, arg}, 0, nullptr); return *out != nullptr; }
static bool thread_join(ThreadH h) { bool ok = WaitForSingleObject(h, 15000) == WAIT_OBJECT_0; CloseHandle(h); return ok; }
static DWORD g_main_tid = 0; static void note_main_thread() { g_main_tid = GetCurrentThreadId(); } static bool on_main_thread() { return GetCurrentThreadId() == g_main_tid; }
static void sleep_ms_(int ms) { Sleep(ms); }
#else
struct Mutex { pthread_mutex_t m; Mutex() { pthread_mutex_init(&m, nullptr); } ~Mutex() { pthread_mutex_destroy(&m); } void lock() { pthread_mutex_lock(&m); } void unlock() { pthread_mutex_unlock(&m); } };
struct CondVar { pthread_cond_t c; CondVar() { pthread_cond_init(&c, nullptr); } ~CondVar() { pthread_cond_destroy(&c); } void wait(Mutex &m) { pthread_cond_wait(&c, &m.m); } void notify_one() { pthread_cond_signal(&c); } void notify_all() { pthread_cond_broadcast(&c); } };
typedef pthread_t ThreadH;
struct ThreadStart { void (*fn)(void *); void *arg; };
static void *thread_tramp(void *p) { ThreadStart ts = *static_cast<ThreadStart *>(p); delete static_cast<ThreadStart *>(p); ts.fn(ts.arg); return nullptr; }
static bool thread_start(void (*fn)(void *), void *arg, ThreadH *out) { return pthread_create(out, nullptr, thread_tramp, new ThreadStart{fn, arg}) == 0; }
static bool thread_join(ThreadH h) { return pthread_join(h, nullptr) == 0; }
static pthread_t g_main_tid; static void note_main_thread() { g_main_tid = pthread_self(); } static bool on_main_thread() { return pthread_equal(pthread_self(), g_main_tid) != 0; }
static void sleep_ms_(int ms) { usleep(ms * 1000); }
#endif
struct Lock { Mutex &m; explicit Lock(Mutex &mm) : m(mm) { m.lock(); } ~Lock() { m.unlock(); } };

// ------------------------------------------------------------------------------------------------ data model
struct ExtHandle { std::string name, version; std::atomic<bool> alive{true}; std::atomic<int> executing{0}; int funcs = 0; };
struct ExtFunc { ExtHandle *ext; std::string ns, name; bool async; xmod_sync_fn sfn; xmod_async_fn afn; void *user; int strand; };
struct XmodCall { std::vector<XmodValue *> args; XmodValue *ret = nullptr; std::string err; bool has_err = false; };
struct XmodJob {
	uint64_t id = 0; ExtFunc *f = nullptr; std::vector<XmodValue *> args; XmodValue *result = nullptr; std::string error; bool finished = false;
	int cb_ref = LUA_NOREF; Plugin *owner = nullptr; unsigned gen = 0; std::atomic<bool> canceled{false}; bool has_strand = false; int64_t strand = 0;
};

static std::vector<ExtHandle *> g_exts;                        // main thread only (entries are never freed: jobs may still point at them)
static std::unordered_map<std::string, ExtFunc *> g_funcs;     // "ns.name" -> func (main thread only; ExtFunc objects are never freed either)
static std::unordered_map<uint64_t, XmodJob *> g_jobs;         // pending jobs, main thread only
static uint64_t g_next_job = 0;
static unsigned g_lua_gen = 0;

struct Pool {
	Mutex mu; CondVar cv;
	std::deque<XmodJob *> runq, done;
	std::unordered_map<int64_t, std::deque<XmodJob *>> strand_wait; std::unordered_set<int64_t> strand_busy;
	std::vector<ThreadH> threads; int idle = 0, running = 0; bool stop = false;
};
static Pool *g_pool = nullptr;
static std::atomic<int> g_max_threads{4}, g_budget_ms{2}, g_queue_max{4096};

static Mutex g_post_mu; static std::vector<std::function<void()>> g_posted;
static void post_std(std::function<void()> f) { Lock l(g_post_mu); g_posted.push_back(std::move(f)); }

static const char *const RESERVED_NS[] = {"core", "player", "ent", "mem", "trace", "msg", "cmd", "cvar", "timer", "kv", "vault", "fs", "sound", "precache", "fullpack",
	"hook", "reapi", "ham", "stats", "ext", "async", "version", "gamedir", nullptr};

// ------------------------------------------------------------------------------------------------ Lua <-> value
static bool lua_to_val(lua_State *l, int idx, int depth, int &budget, XmodValue *&out, char *err) {
	idx = lua_absindex(l, idx);
	if (--budget < 0) { snprintf(err, 256, "value too large to pass to an extension"); return false; }
	switch (lua_type(l, idx)) {
	case LUA_TNIL: out = xv::nil(); return true;
	case LUA_TBOOLEAN: out = xv::boolean(lua_toboolean(l, idx)); return true;
	case LUA_TNUMBER: out = lua_isinteger(l, idx) ? xv::integer(lua_tointeger(l, idx)) : xv::number(lua_tonumber(l, idx)); return true;
	case LUA_TSTRING: { size_t n; const char *s = lua_tolstring(l, idx, &n); out = xv::str(s, n); return true; }
	case LUA_TTABLE: {
		if (depth > 8) { snprintf(err, 256, "table nesting too deep (max 8)"); return false; }
		size_t n = lua_rawlen(l, idx), count = 0; bool arraylike = true;
		lua_pushnil(l);
		while (lua_next(l, idx)) { count++; if (!(lua_isinteger(l, -2) && lua_tointeger(l, -2) >= 1 && (size_t)lua_tointeger(l, -2) <= n)) arraylike = false; lua_pop(l, 1); }
		if (arraylike && count == n) {
			XmodValue *a = xv::array();
			for (size_t i = 1; i <= n; i++) { lua_rawgeti(l, idx, (lua_Integer)i); XmodValue *c = nullptr; bool ok = lua_to_val(l, -1, depth + 1, budget, c, err); lua_pop(l, 1); if (!ok) { delete a; return false; } a->arr.push_back(c); }
			out = a; return true;
		}
		XmodValue *m = xv::map();
		lua_pushnil(l);
		while (lua_next(l, idx)) {
			std::string key;
			if (lua_type(l, -2) == LUA_TSTRING) key = lua_tostring(l, -2);
			else if (lua_type(l, -2) == LUA_TNUMBER) { lua_pushvalue(l, -2); key = lua_tostring(l, -1); lua_pop(l, 1); }
			else { lua_pop(l, 2); delete m; snprintf(err, 256, "table keys must be strings or numbers"); return false; }
			XmodValue *c = nullptr; bool ok = lua_to_val(l, -1, depth + 1, budget, c, err); lua_pop(l, 1);
			if (!ok) { lua_pop(l, 1); delete m; return false; }
			xv::set(m, key.c_str(), c);
		}
		out = m; return true; }
	default: snprintf(err, 256, "cannot pass a %s to an extension", luaL_typename(l, idx)); return false;
	}
}
static void val_to_lua(lua_State *l, const XmodValue *v, int depth = 0) {
	if (!v || depth > 32 || !lua_checkstack(l, 4)) { lua_pushnil(l); return; }
	switch (v->type) {
	case XV_BOOL: lua_pushboolean(l, v->b); break;
	case XV_INT: lua_pushinteger(l, (lua_Integer)v->i); break;
	case XV_NUM: lua_pushnumber(l, v->n); break;
	case XV_STR: lua_pushlstring(l, v->s.data(), v->s.size()); break;
	case XV_ARRAY: lua_createtable(l, (int)v->arr.size(), 0); for (size_t i = 0; i < v->arr.size(); i++) { val_to_lua(l, v->arr[i], depth + 1); lua_rawseti(l, -2, (lua_Integer)i + 1); } break;
	case XV_MAP: lua_createtable(l, 0, (int)v->map.size()); for (auto &p : v->map) { val_to_lua(l, p.second, depth + 1); lua_setfield(l, -2, p.first.c_str()); } break;
	default: lua_pushnil(l);
	}
}

// ------------------------------------------------------------------------------------------------ worker pool
static void job_finish_locked(Pool *g_pool, XmodJob *j) {         // pool lock held: release the strand, queue for delivery
	if (j->has_strand) {
		auto it = g_pool->strand_wait.find(j->strand);
		if (it != g_pool->strand_wait.end() && !it->second.empty()) { XmodJob *nx = it->second.front(); it->second.pop_front(); g_pool->runq.push_back(nx); g_pool->cv.notify_one(); }
		else { g_pool->strand_busy.erase(j->strand); if (it != g_pool->strand_wait.end()) g_pool->strand_wait.erase(it); }
	}
	g_pool->done.push_back(j);
}
static void worker_main(void *) {
	Pool *p = g_pool; p->mu.lock();
	for (;;) {
		while (p->runq.empty() && !p->stop) { p->idle++; p->cv.wait(p->mu); p->idle--; }
		if (p->stop) break;
		XmodJob *j = p->runq.front(); p->runq.pop_front(); p->running++; p->mu.unlock();
		if (j->canceled.load()) j->error = "canceled";
		else if (!j->f->ext->alive.load()) j->error = "extension unloaded";
		else { j->f->ext->executing++; j->f->afn(j, j->f->user); j->f->ext->executing--; if (!j->finished) j->error = "the extension function returned without a result"; }
		p->mu.lock(); p->running--; job_finish_locked(p, j);
	}
	p->mu.unlock();
}
static bool pool_submit(XmodJob *j, std::string &err) {
	if (!g_pool) g_pool = new Pool();
	Pool *p = g_pool; Lock l(p->mu);
	size_t pending = p->runq.size(); for (auto &kv : p->strand_wait) pending += kv.second.size();
	if (pending >= (size_t)g_queue_max.load()) { err = "too many pending async jobs"; return false; }
	if (j->has_strand && p->strand_busy.count(j->strand)) { p->strand_wait[j->strand].push_back(j); return true; }
	if (j->has_strand) p->strand_busy.insert(j->strand);
	p->runq.push_back(j);
	if (p->idle == 0 && (int)p->threads.size() < g_max_threads.load()) { ThreadH h; if (thread_start(worker_main, nullptr, &h)) p->threads.push_back(h); }
	p->cv.notify_one(); return true;
}
static void free_job(XmodJob *j) { for (auto *a : j->args) delete a; delete j->result; delete j; }
void ext_shutdown() {
	if (!g_pool) return;
	{ Lock l(g_pool->mu); g_pool->stop = true; g_pool->cv.notify_all(); }
	bool all = true; for (ThreadH h : g_pool->threads) all = thread_join(h) && all;
	g_jobs.clear();
	if (!all) { xerr("async: a worker thread is still busy at shutdown - leaving its pool alone"); g_pool = nullptr; return; }   // leak instead of freeing memory a thread still uses
	Lock l(g_pool->mu);
	auto drop = [](std::deque<XmodJob *> &q) { for (XmodJob *j : q) free_job(j); q.clear(); };
	drop(g_pool->runq); drop(g_pool->done); for (auto &kv : g_pool->strand_wait) drop(kv.second);
	g_pool->strand_wait.clear(); g_pool->strand_busy.clear(); g_pool->threads.clear(); g_pool->idle = 0; g_pool->running = 0;
	g_pool->stop = false;                                             // xmod may be attached again later (meta reload): the pool must be usable
	g_jobs.clear();
}

// ------------------------------------------------------------------------------------------------ main-thread pump (called every frame)
static void deliver(XmodJob *j) {
	g_jobs.erase(j->id);
	bool same_gen = (j->gen == g_lua_gen) && L;
	if (same_gen && !j->canceled.load() && j->owner->enabled) {
		int top = lua_gettop(L);
		lua_rawgeti(L, LUA_REGISTRYINDEX, j->cb_ref);
		if (!j->error.empty()) { lua_pushlstring(L, j->error.data(), j->error.size()); lua_pushnil(L); }
		else { lua_pushnil(L); val_to_lua(L, j->result); }
		safe_call(j->owner, 2, 0, (j->f->ns + "." + j->f->name + " callback").c_str());
		lua_settop(L, top);
	}
	if (same_gen) luaL_unref(L, LUA_REGISTRYINDEX, j->cb_ref);
	free_job(j);
}
void ext_pump() {
	{ std::vector<std::function<void()>> q; { Lock l(g_post_mu); q.swap(g_posted); }
	  for (auto &f : q) f(); }
	if (!g_pool) return;
	unsigned long long t0 = now_ns(), budget = (unsigned long long)g_budget_ms.load() * 1000000ULL;
	for (;;) {
		XmodJob *j = nullptr;
		{ Lock l(g_pool->mu); if (g_pool->done.empty()) break; j = g_pool->done.front(); g_pool->done.pop_front(); }
		deliver(j);
		if (now_ns() - t0 > budget) break;                            // the rest is delivered next frame
	}
}
void ext_set_limits(int threads, int budget_ms, int queue_max) {
	g_max_threads = threads < 1 ? 1 : threads > 16 ? 16 : threads; g_budget_ms = budget_ms < 0 ? 0 : budget_ms > 50 ? 50 : budget_ms; g_queue_max = queue_max < 16 ? 16 : queue_max;
}

// ------------------------------------------------------------------------------------------------ Lua binding of registered functions
static int ext_lua_call(lua_State *l) {
	ExtFunc *f = static_cast<ExtFunc *>(lua_touserdata(l, lua_upvalueindex(1)));
	char ebuf[400]; ebuf[0] = 0; int nret = 0;
	const char *ns = f->ns.c_str(), *fn = f->name.c_str();
	if (!f->ext->alive.load()) snprintf(ebuf, sizeof ebuf, "xmod.%s.%s is not available: the extension '%s' was unloaded", ns, fn, f->ext->name.c_str());
	else {
		int n = lua_gettop(l), budget = 200000; char err[256]; err[0] = 0;
		if (!f->async) {
			XmodCall *call = new XmodCall;
			for (int i = 1; i <= n; i++) { XmodValue *v = nullptr; if (!lua_to_val(l, i, 0, budget, v, err)) { snprintf(ebuf, sizeof ebuf, "xmod.%s.%s argument #%d: %s", ns, fn, i, err); break; } call->args.push_back(v); }
			if (!ebuf[0]) { f->sfn(call, f->user); if (call->has_err) snprintf(ebuf, sizeof ebuf, "xmod.%s.%s: %s", ns, fn, call->err.c_str()); else if (call->ret) { val_to_lua(l, call->ret); nret = 1; } }
			for (auto *a : call->args) delete a; delete call->ret; delete call;
		} else if (n < 1 || !lua_isfunction(l, n)) snprintf(ebuf, sizeof ebuf, "xmod.%s.%s is asynchronous: pass a callback function(err, result) as the LAST argument", ns, fn);
		else if (!caller_plugin(l)) snprintf(ebuf, sizeof ebuf, "async call outside of a plugin context");
		else {
			XmodJob *j = new XmodJob; j->f = f; j->owner = caller_plugin(l); j->gen = g_lua_gen;
			for (int i = 1; i < n; i++) { XmodValue *v = nullptr; if (!lua_to_val(l, i, 0, budget, v, err)) { snprintf(ebuf, sizeof ebuf, "xmod.%s.%s argument #%d: %s", ns, fn, i, err); break; } j->args.push_back(v); }
			if (!ebuf[0] && f->strand > 0) {
				const XmodValue *sa = (size_t)f->strand <= j->args.size() ? j->args[f->strand - 1] : nullptr;
				if (!sa || sa->type != XV_INT) snprintf(ebuf, sizeof ebuf, "xmod.%s.%s argument #%d must be an integer handle", ns, fn, f->strand);
				else { j->has_strand = true; j->strand = sa->i; }
			}
			if (!ebuf[0]) {
				j->id = ++g_next_job; lua_pushvalue(l, n); j->cb_ref = luaL_ref(l, LUA_REGISTRYINDEX);
				std::string perr;
				if (!pool_submit(j, perr)) { luaL_unref(l, LUA_REGISTRYINDEX, j->cb_ref); snprintf(ebuf, sizeof ebuf, "xmod.%s.%s: %s", ns, fn, perr.c_str()); }
				else { g_jobs[j->id] = j; lua_pushinteger(l, (lua_Integer)j->id); nret = 1; j = nullptr; }
			}
			if (j) free_job(j);
		}
	}
	if (ebuf[0]) return luaL_error(l, "%s", ebuf);
	return nret;
}
static int ns_missing(lua_State *l) {                              // __index of an extension namespace: the function does not exist (yet / any more)
	const char *ns = lua_tostring(l, lua_upvalueindex(1));
	return luaL_error(l, "xmod.%s.%s is not available: the extension providing '%s' is not loaded (list it in metamod's plugins.ini AFTER xmod and check the server log)",
		ns, lua_tostring(l, 2) ? lua_tostring(l, 2) : "?", ns);
}
static bool ns_reserved(const std::string &ns) { for (const char *const *r = RESERVED_NS; *r; ++r) if (ns == *r) return true; return false; }
static void ns_table(lua_State *l, const std::string &ns) {      // xmod table at -1 on entry; leaves xmod.<ns> on top
	lua_getfield(l, -1, ns.c_str());
	if (lua_istable(l, -1)) return;
	lua_pop(l, 1); lua_newtable(l);
	lua_newtable(l); lua_pushstring(l, ns.c_str()); lua_pushcclosure(l, ns_missing, 1); lua_setfield(l, -2, "__index"); lua_setmetatable(l, -2);
	lua_pushvalue(l, -1); lua_setfield(l, -3, ns.c_str());
}
static void bind_func(ExtFunc *f) {
	if (!L) return;
	lua_getglobal(L, "xmod"); ns_table(L, f->ns);
	lua_pushstring(L, f->name.c_str()); lua_pushlightuserdata(L, f); lua_pushcclosure(L, ext_lua_call, 1); lua_rawset(L, -3);
	lua_pop(L, 2);
}
static void unbind_func(ExtFunc *f) {
	if (!L) return;
	lua_getglobal(L, "xmod"); lua_getfield(L, -1, f->ns.c_str());
	if (lua_istable(L, -1)) { lua_pushstring(L, f->name.c_str()); lua_pushnil(L); lua_rawset(L, -3); }
	lua_pop(L, 2);
}

// ------------------------------------------------------------------------------------------------ xmod.ext / xmod.async (Lua)
static int l_ext_list(lua_State *l) {
	lua_newtable(l); int i = 1;
	for (ExtHandle *e : g_exts) {
		lua_newtable(l);
		lua_pushstring(l, e->name.c_str()); lua_setfield(l, -2, "name"); lua_pushstring(l, e->version.c_str()); lua_setfield(l, -2, "version");
		lua_pushinteger(l, e->funcs); lua_setfield(l, -2, "functions"); lua_pushboolean(l, e->alive.load()); lua_setfield(l, -2, "loaded");
		lua_rawseti(l, -2, i++);
	}
	return 1;
}
static int l_ext_available(lua_State *l) {
	std::string ns = luaL_checkstring(l, 1); const char *fn = luaL_optstring(l, 2, nullptr);
	if (fn) { lua_pushboolean(l, g_funcs.count(ns + "." + fn) > 0); return 1; }
	for (auto &kv : g_funcs) if (kv.second->ns == ns) { lua_pushboolean(l, 1); return 1; }
	lua_pushboolean(l, 0); return 1;
}
static int l_async_cancel(lua_State *l) {
	auto it = g_jobs.find((uint64_t)luaL_checkinteger(l, 1));
	if (it == g_jobs.end()) { lua_pushboolean(l, 0); return 1; }
	it->second->canceled = true; lua_pushboolean(l, 1); return 1;
}
static int l_async_stats(lua_State *l) {
	int threads = 0, idle = 0, running = 0; size_t queued = 0, done = 0;
	if (g_pool) { Lock lk(g_pool->mu); threads = (int)g_pool->threads.size(); idle = g_pool->idle; running = g_pool->running; queued = g_pool->runq.size(); for (auto &kv : g_pool->strand_wait) queued += kv.second.size(); done = g_pool->done.size(); }
	lua_createtable(l, 0, 8);
	lua_pushinteger(l, threads); lua_setfield(l, -2, "threads"); lua_pushinteger(l, g_max_threads.load()); lua_setfield(l, -2, "max_threads");
	lua_pushinteger(l, idle); lua_setfield(l, -2, "idle"); lua_pushinteger(l, running); lua_setfield(l, -2, "running");
	lua_pushinteger(l, (lua_Integer)queued); lua_setfield(l, -2, "queued"); lua_pushinteger(l, (lua_Integer)done); lua_setfield(l, -2, "waiting_for_delivery");
	lua_pushinteger(l, (lua_Integer)g_jobs.size()); lua_setfield(l, -2, "pending"); lua_pushinteger(l, g_budget_ms.load()); lua_setfield(l, -2, "budget_ms");
	return 1;
}
void ext_console_list() {
	char buf[256];
	snprintf(buf, sizeof buf, "extensions: %zu   async: pool max %d threads, %zu pending jobs\n", g_exts.size(), g_max_threads.load(), g_jobs.size()); SERVER_PRINT(buf);
	for (ExtHandle *e : g_exts) { snprintf(buf, sizeof buf, "  %-20s %-10s %d functions %s\n", e->name.c_str(), e->version.c_str(), e->funcs, e->alive.load() ? "" : "[UNLOADED]"); SERVER_PRINT(buf); }
}

// Called by core_init once the Lua state and the xmod table exist (before any plugin script runs) and again after `xmod reload`.
void ext_lua_init() {
	note_main_thread(); g_lua_gen++; g_jobs.clear();
	lua_getglobal(L, "xmod");
	lua_newtable(L); lua_pushcfunction(L, l_ext_list); lua_setfield(L, -2, "list"); lua_pushcfunction(L, l_ext_available); lua_setfield(L, -2, "available"); lua_setfield(L, -2, "ext");
	lua_newtable(L); lua_pushcfunction(L, l_async_cancel); lua_setfield(L, -2, "cancel"); lua_pushcfunction(L, l_async_stats); lua_setfield(L, -2, "stats"); lua_setfield(L, -2, "async");
	// namespaces named in configs/extensions.ini exist from the start (stable tables) so scripts can bind `local q = xmod.query` at load time
	std::string text; { FILE *fp = fopen(xpath("configs/extensions.ini").c_str(), "rb"); if (fp) { char b[1024]; size_t n; while ((n = fread(b, 1, sizeof b, fp)) > 0) text.append(b, n); fclose(fp); } }
	size_t pos = 0;
	while (pos < text.size()) {
		size_t nl = text.find('\n', pos); if (nl == std::string::npos) nl = text.size();
		std::string line = text.substr(pos, nl - pos); pos = nl + 1;
		size_t c = line.find_first_of(";#"); if (c != std::string::npos) line.erase(c);
		size_t a = line.find_first_not_of(" \t\r"), b = line.find_last_not_of(" \t\r"); if (a == std::string::npos) continue;
		line = line.substr(a, b - a + 1);
		bool ok = !line.empty(); for (char ch : line) if (!(isalnum((unsigned char)ch) || ch == '_')) ok = false;
		if (!ok || ns_reserved(line)) { xerr("extensions.ini: bad or reserved namespace '%s'", line.c_str()); continue; }
		ns_table(L, line); lua_pop(L, 1);
	}
	lua_pop(L, 1);
	for (auto &kv : g_funcs) bind_func(kv.second);                    // functions registered before a reload
}
void ext_lua_closing() { g_jobs.clear(); }                         // pending jobs keep running; their callbacks are dropped (generation mismatch)

// ------------------------------------------------------------------------------------------------ the C API
static void pin_module() {
#ifdef _WIN32
	static bool pinned = false; if (pinned) return; pinned = true;   // an extension may detach AFTER xmod and still call into this API: never unmap xmod while extensions exist
	HMODULE h = nullptr; GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCSTR)&pin_module, &h);
#endif              // Linux: xmod_ext_connect() holds a dlopen reference that is never released
}
static XmodExt *api_ext_register(const char *name, const char *version) {
	if (!on_main_thread() || !name || !*name) return nullptr;
	for (ExtHandle *e : g_exts) if (e->alive.load() && e->name == name) return nullptr;
	pin_module();
	ExtHandle *e = new ExtHandle; e->name = name; e->version = version ? version : ""; g_exts.push_back(e);
	xlog("extension '%s' %s connected", e->name.c_str(), e->version.c_str());
	return reinterpret_cast<XmodExt *>(e);
}
static int register_common(XmodExt *x, const char *ns, const char *name, bool async, xmod_sync_fn sfn, xmod_async_fn afn, void *user, int strand) {
	ExtHandle *e = reinterpret_cast<ExtHandle *>(x);
	if (!e || !on_main_thread() || !e->alive.load() || !ns || !name || !*ns || !*name) return 0;
	std::string sns = ns, sname = name;
	for (char ch : sns) if (!(isalnum((unsigned char)ch) || ch == '_')) return 0;
	for (char ch : sname) if (!(isalnum((unsigned char)ch) || ch == '_')) return 0;
	if (ns_reserved(sns)) { xerr("extension '%s': namespace '%s' is reserved by xmod", e->name.c_str(), ns); return 0; }
	if (g_funcs.count(sns + "." + sname)) { xerr("extension '%s': xmod.%s.%s is already registered", e->name.c_str(), ns, name); return 0; }
	if ((!async && !sfn) || (async && !afn)) return 0;
	ExtFunc *f = new ExtFunc{e, sns, sname, async, sfn, afn, user, strand};
	g_funcs[sns + "." + sname] = f; e->funcs++; bind_func(f); return 1;
}
static int api_register_func(XmodExt *x, const char *ns, const char *name, xmod_sync_fn fn, void *user) { return register_common(x, ns, name, false, fn, nullptr, user, 0); }
static int api_register_async(XmodExt *x, const char *ns, const char *name, xmod_async_fn fn, void *user, int strand) { return register_common(x, ns, name, true, nullptr, fn, user, strand); }
static void api_ext_unregister(XmodExt *x) {
	ExtHandle *e = reinterpret_cast<ExtHandle *>(x); if (!e || !e->alive.exchange(false)) return;
	for (auto it = g_funcs.begin(); it != g_funcs.end();) { if (it->second->ext == e) { unbind_func(it->second); it = g_funcs.erase(it); } else ++it; }
	// queued jobs of this extension are failed by the workers ("extension unloaded") when their turn comes, so strands keep their order
	for (int waited = 0; e->executing.load() > 0 && waited < 5000; waited += 5) sleep_ms_(5);   // running code of this extension must finish before its module is unloaded
	if (e->executing.load() > 0) xerr("extension '%s' still has running jobs after 5 s - unloading anyway", e->name.c_str());
	xlog("extension '%s' unloaded", e->name.c_str());
}
static int api_call_argc(XmodCall *c) { return (int)c->args.size(); }
static const XmodValue *api_call_arg(XmodCall *c, int i) { return (i >= 0 && (size_t)i < c->args.size()) ? c->args[i] : nullptr; }
static void api_call_return(XmodCall *c, XmodValue *v) { delete c->ret; c->ret = v; }
static void api_call_error(XmodCall *c, const char *m) { c->has_err = true; c->err = m ? m : "error"; }
static int api_job_argc(XmodJob *j) { return (int)j->args.size(); }
static const XmodValue *api_job_arg(XmodJob *j, int i) { return (i >= 0 && (size_t)i < j->args.size()) ? j->args[i] : nullptr; }
static void api_job_done(XmodJob *j, XmodValue *r) { if (j->finished) { delete r; return; } j->finished = true; j->result = r; }
static void api_job_fail(XmodJob *j, const char *e) { if (j->finished) return; j->finished = true; j->error = (e && *e) ? e : "failed"; }
static void api_post_main(xmod_main_fn fn, void *user) { if (fn) post_std([fn, user]() { fn(user); }); }
static void api_emit_event(const char *event, XmodValue *args) {
	std::string ev = event ? event : "";
	post_std([ev, args]() {
		if (L && !ev.empty()) {
			Event *e = ev_get(ev.c_str()); size_t n = args ? (args->type == XV_ARRAY ? args->arr.size() : 1) : 0;
			ev_fire(e, (int)n, [&](lua_State *l) { if (!args) return 0; if (args->type == XV_ARRAY) { for (auto *c : args->arr) val_to_lua(l, c); return (int)args->arr.size(); } val_to_lua(l, args); return 1; });
		}
		delete args;
	});
}
static void api_log(const char *m) { std::string s = m ? m : ""; if (on_main_thread()) xlog("%s", s.c_str()); else post_std([s]() { xlog("%s", s.c_str()); }); }
static void api_log_error(const char *m) { std::string s = m ? m : ""; if (on_main_thread()) xerr("%s", s.c_str()); else post_std([s]() { xerr("%s", s.c_str()); }); }
static int api_is_main_thread() { return on_main_thread() ? 1 : 0; }
static void api_color_chat(int id, const char *text) { std::string t = text ? text : ""; if (on_main_thread()) xmod_color_chat(id, t); else post_std([id, t]() { xmod_color_chat(id, t); }); }
static void api_client_print(int id, const char *kind, const char *text) {
	std::string k = kind ? kind : "console", t = text ? text : ""; if (on_main_thread()) xmod_client_print(id, k, t); else post_std([id, k, t]() { xmod_client_print(id, k, t); });
}

static XmodApi g_api;
extern "C" C_DLLEXPORT const XmodApi *xmod_get_api(uint32_t abi) {
	if (abi != XMOD_EXT_ABI) return nullptr;
	static bool init = false;
	if (!init) {
		init = true; memset(&g_api, 0, sizeof g_api); g_api.abi_version = XMOD_EXT_ABI; g_api.size = (uint32_t)sizeof(XmodApi);
		g_api.ext_register = api_ext_register; g_api.ext_unregister = api_ext_unregister; g_api.register_func = api_register_func; g_api.register_async = api_register_async;
		xmod_fill_value_api(&g_api);
		g_api.call_argc = api_call_argc; g_api.call_arg = api_call_arg; g_api.call_return = api_call_return; g_api.call_error = api_call_error;
		g_api.job_argc = api_job_argc; g_api.job_arg = api_job_arg; g_api.job_done = api_job_done; g_api.job_fail = api_job_fail;
		g_api.post_main = api_post_main; g_api.emit_event = api_emit_event; g_api.log = api_log; g_api.log_error = api_log_error; g_api.is_main_thread = api_is_main_thread;
		g_api.color_chat = api_color_chat; g_api.client_print = api_client_print;
	}
	return &g_api;
}
