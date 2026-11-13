// xmod_hello - the TEMPLATE extension. Copy this directory to start a new extension (see module_skillz.md).
// Shows: sync functions, async functions on the worker pool, a strand (ordered per-handle jobs), color_chat from a worker thread,
// events pushed to Lua from a worker thread, and nested value trees.
#define XMOD_EXT_NAME    "xmod_hello"
#define XMOD_EXT_VERSION "1.0"
#include "xmod_ext_glue.h"
#include <map>
#include <mutex>
#include <string>
#ifdef _WIN32
static void ms_sleep(int ms) { Sleep(ms); }
#else
#include <unistd.h>
static void ms_sleep(int ms) { usleep(ms * 1000); }
#endif

// ---- sync (main thread, must be fast):   xmod.hello.add(a, b) -> a + b
static void f_add(XmodCall *c, void *) {
	if (g_xmod->call_argc(c) < 2) { g_xmod->call_error(c, "usage: xmod.hello.add(a, b)"); return; }
	g_xmod->call_return(c, g_xmod->val_num(g_xmod->val_get_num(g_xmod->call_arg(c, 0), 0) + g_xmod->val_get_num(g_xmod->call_arg(c, 1), 0)));
}
// ---- sync, returns a table:   xmod.hello.info() -> { name=..., abi=..., is_main=true }
static void f_info(XmodCall *c, void *) {
	XmodValue *m = g_xmod->val_map();
	g_xmod->val_set(m, "name", g_xmod->val_cstr(XMOD_EXT_NAME)); g_xmod->val_set(m, "abi", g_xmod->val_int(g_xmod->abi_version));
	g_xmod->val_set(m, "is_main", g_xmod->val_bool(g_xmod->is_main_thread()));
	g_xmod->call_return(c, m);
}
// ---- async (worker thread, may block):   xmod.hello.echo(ms, text, function(err, res) end)  -> res = { text=..., waited=ms, on_main=false }
static void j_echo(XmodJob *j, void *) {
	int ms = (int)g_xmod->val_get_int(g_xmod->job_arg(j, 0), 0); if (ms < 0 || ms > 5000) { g_xmod->job_fail(j, "ms must be 0..5000"); return; }
	size_t n = 0; const char *s = g_xmod->val_get_str(g_xmod->job_arg(j, 1), &n);
	if (!s) { g_xmod->job_fail(j, "text must be a string"); return; }
	ms_sleep(ms);                                                      // blocking is fine here: this is not the game thread
	XmodValue *r = g_xmod->val_map(); g_xmod->val_set(r, "text", g_xmod->val_str(s, n)); g_xmod->val_set(r, "waited", g_xmod->val_int(ms));
	g_xmod->val_set(r, "on_main", g_xmod->val_bool(g_xmod->is_main_thread()));
	g_xmod->job_done(j, r);
}
// ---- async + strand on argument #1:   xmod.hello.counter_add(handle, n, cb) -> value after adding.  Jobs of one handle run one at a time, in order.
static std::mutex g_cmu; static std::map<int64_t, int64_t> g_counters; static std::map<int64_t, int> g_active;
static void j_counter_add(XmodJob *j, void *) {
	int64_t h = g_xmod->val_get_int(g_xmod->job_arg(j, 0), 0), add = g_xmod->val_get_int(g_xmod->job_arg(j, 1), 0);
	{ std::lock_guard<std::mutex> l(g_cmu); if (++g_active[h] > 1) { g_active[h]--; g_xmod->job_fail(j, "STRAND VIOLATION: two jobs of one handle ran concurrently"); return; } }
	ms_sleep(2);                                                       // widen the window in which a violation would show
	int64_t v; { std::lock_guard<std::mutex> l(g_cmu); v = (g_counters[h] += add); g_active[h]--; }
	g_xmod->job_done(j, g_xmod->val_int(v));
}
// ---- thread-safe game helper from a worker:   xmod.hello.shout(text, cb)  -> colour chat to everybody, sent safely via the main thread
static void j_shout(XmodJob *j, void *) {
	const char *s = g_xmod->val_get_str(g_xmod->job_arg(j, 0), nullptr); if (!s) { g_xmod->job_fail(j, "text must be a string"); return; }
	g_xmod->color_chat(0, s); g_xmod->job_done(j, g_xmod->val_bool(1));
}
// ---- events pushed from a worker:  xmod.hello.ticker(count, interval_ms, "my_event", cb)  fires  xmod.on("my_event", function(i, total) end)
static void j_ticker(XmodJob *j, void *) {
	int count = (int)g_xmod->val_get_int(g_xmod->job_arg(j, 0), 0), iv = (int)g_xmod->val_get_int(g_xmod->job_arg(j, 1), 10);
	const char *ev = g_xmod->val_get_str(g_xmod->job_arg(j, 2), nullptr); if (!ev || count < 1 || count > 100) { g_xmod->job_fail(j, "bad arguments"); return; }
	std::string event = ev;
	for (int i = 1; i <= count; i++) { ms_sleep(iv); XmodValue *a = g_xmod->val_array(); g_xmod->val_push(a, g_xmod->val_int(i)); g_xmod->val_push(a, g_xmod->val_int(count)); g_xmod->emit_event(event.c_str(), a); }
	g_xmod->job_done(j, g_xmod->val_int(count));
}
// ---- nested values in and out:  xmod.hello.reverse({1,2,{a="x"}}, cb) -> same structure, arrays reversed
static XmodValue *rev(const XmodValue *v) {
	switch (g_xmod->val_type(v)) {
	case XV_ARRAY: { XmodValue *a = g_xmod->val_array(); for (size_t i = g_xmod->val_len(v); i > 0; i--) g_xmod->val_push(a, rev(g_xmod->val_at(v, i - 1))); return a; }
	case XV_MAP: { XmodValue *m = g_xmod->val_map(); for (size_t i = 0; i < g_xmod->val_len(v); i++) g_xmod->val_set(m, g_xmod->val_key(v, i), rev(g_xmod->val_val(v, i))); return m; }
	case XV_STR: { size_t n; const char *s = g_xmod->val_get_str(v, &n); return g_xmod->val_str(s, n); }
	case XV_INT: return g_xmod->val_int(g_xmod->val_get_int(v, 0));
	case XV_NUM: return g_xmod->val_num(g_xmod->val_get_num(v, 0));
	case XV_BOOL: return g_xmod->val_bool(g_xmod->val_get_bool(v, 0));
	default: return g_xmod->val_nil();
	}
}
static void j_reverse(XmodJob *j, void *) { g_xmod->job_done(j, rev(g_xmod->job_arg(j, 0))); }
static void j_never_replies(XmodJob *, void *) { /* a buggy extension function: xmod turns this into an error for the callback */ }

static bool xext_init(const XmodApi *api, XmodExt *ext) {
	bool ok = api->register_func(ext, "hello", "add", f_add, nullptr) && api->register_func(ext, "hello", "info", f_info, nullptr)
		&& api->register_async(ext, "hello", "echo", j_echo, nullptr, 0) && api->register_async(ext, "hello", "counter_add", j_counter_add, nullptr, 1)
		&& api->register_async(ext, "hello", "shout", j_shout, nullptr, 0) && api->register_async(ext, "hello", "ticker", j_ticker, nullptr, 0)
		&& api->register_async(ext, "hello", "reverse", j_reverse, nullptr, 0) && api->register_async(ext, "hello", "never_replies", j_never_replies, nullptr, 0);
	if (!ok) { api->log_error("xmod_hello: registering functions failed"); return false; }
	api->log("xmod_hello ready: xmod.hello.add/info/echo/counter_add/shout/ticker/reverse");
	return true;
}
static void xext_shutdown(void) {}
