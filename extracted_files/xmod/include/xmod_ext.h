/* ============================================================================================================
 *  xmod_ext.h  -  C ABI for Metamod plugins that extend xmod ("xmod extensions")
 *
 *  An extension is a normal Metamod plugin (listed in addons/metamod/plugins.ini AFTER xmod). In Meta_Attach it calls
 *  xmod_ext_connect() and registers functions; xmod exposes them to Lua as  xmod.<namespace>.<name>(...).
 *
 *  THREADING RULES (read this):
 *    * sync functions run on the MAIN (game) thread. Keep them fast; never block.
 *    * async functions run on a WORKER thread of xmod's pool. They may block (sockets, SQL, HTTP, files ...).
 *      Inside a worker you may ONLY call: job_* accessors, val_* functions, post_main, emit_event, log, log_error,
 *      color_chat, client_print, is_main_thread.  NEVER call engine functions (g_engfuncs, MESSAGE_BEGIN, ...) or touch game state.
 *    * results are handed back to Lua on the main thread (budgeted per frame), so Lua callbacks never run concurrently.
 *  The API is versioned: struct XmodApi only ever grows at the END; check api->size before using newer fields.
 * ============================================================================================================ */
#ifndef XMOD_EXT_H
#define XMOD_EXT_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <link.h>
#endif
#ifdef __cplusplus
extern "C" {
#endif

#define XMOD_EXT_ABI 1

typedef struct XmodValue XmodValue;   /* opaque value tree: nil, bool, int, number, string, array, map(string -> value) */
typedef struct XmodCall  XmodCall;    /* one synchronous call (main thread) */
typedef struct XmodJob   XmodJob;     /* one asynchronous job (worker thread) */
typedef struct XmodExt   XmodExt;     /* an extension handle returned by ext_register */

enum { XV_NIL = 0, XV_BOOL = 1, XV_INT = 2, XV_NUM = 3, XV_STR = 4, XV_ARRAY = 5, XV_MAP = 6 };

typedef void (*xmod_sync_fn)(XmodCall *call, void *user);     /* main thread */
typedef void (*xmod_async_fn)(XmodJob *job, void *user);      /* worker thread */
typedef void (*xmod_main_fn)(void *user);                     /* main thread (post_main) */

typedef struct XmodApi {
	uint32_t abi_version;                 /* XMOD_EXT_ABI */
	uint32_t size;                        /* sizeof(XmodApi) of the xmod that built it */

	/* ---- lifecycle (call from Meta_Attach / Meta_Detach, main thread) */
	XmodExt *(*ext_register)(const char *name, const char *version);
	void     (*ext_unregister)(XmodExt *ext);    /* removes the functions, waits (max 5 s) for running jobs of this extension, fails queued ones */

	/* ---- function registration (main thread, in Meta_Attach).  ns = Lua namespace (xmod.<ns>), name = function name.
	 *   register_async: Lua passes a callback as the LAST argument: callback(err, result).  strand_arg > 0 makes jobs whose
	 *   argument #strand_arg (1-based, an integer) is equal run strictly one at a time, in submission order (use it for DB / socket handles).
	 *   returns 1 on success, 0 on failure (reserved namespace, duplicate, bad args) */
	int (*register_func)(XmodExt *ext, const char *ns, const char *name, xmod_sync_fn fn, void *user);
	int (*register_async)(XmodExt *ext, const char *ns, const char *name, xmod_async_fn fn, void *user, int strand_arg);

	/* ---- values: constructors (thread-safe) */
	XmodValue *(*val_nil)(void);
	XmodValue *(*val_bool)(int b);
	XmodValue *(*val_int)(int64_t i);
	XmodValue *(*val_num)(double n);
	XmodValue *(*val_str)(const char *s, size_t len);     /* copies; may contain NULs */
	XmodValue *(*val_cstr)(const char *s);                /* copies; NULL -> nil */
	XmodValue *(*val_array)(void);
	XmodValue *(*val_map)(void);
	void       (*val_push)(XmodValue *array, XmodValue *v);          /* takes ownership of v */
	void       (*val_set)(XmodValue *map, const char *key, XmodValue *v);   /* takes ownership of v (replaces an existing key) */
	void       (*val_free)(XmodValue *v);                 /* only for values you did NOT hand over */
	/* ---- values: accessors (thread-safe, read-only) */
	int         (*val_type)(const XmodValue *v);
	int64_t     (*val_get_int)(const XmodValue *v, int64_t dflt);   /* ints and integral numbers */
	double      (*val_get_num)(const XmodValue *v, double dflt);
	int         (*val_get_bool)(const XmodValue *v, int dflt);
	const char *(*val_get_str)(const XmodValue *v, size_t *len);    /* NULL if not a string; always NUL-terminated */
	size_t      (*val_len)(const XmodValue *v);                     /* array length / map size */
	const XmodValue *(*val_at)(const XmodValue *array, size_t index);      /* 0-based */
	const XmodValue *(*val_get)(const XmodValue *map, const char *key);    /* NULL if missing */
	const char *(*val_key)(const XmodValue *map, size_t index);            /* iterate a map: key #index */
	const XmodValue *(*val_val)(const XmodValue *map, size_t index);       /* iterate a map: value #index */

	/* ---- sync call context (main thread). args are 0-based. */
	int              (*call_argc)(XmodCall *call);
	const XmodValue *(*call_arg)(XmodCall *call, int index);
	void             (*call_return)(XmodCall *call, XmodValue *v);  /* result for Lua (takes ownership) */
	void             (*call_error)(XmodCall *call, const char *msg);/* raises a Lua error */

	/* ---- async job context (worker thread). args are 0-based, WITHOUT the callback. */
	int              (*job_argc)(XmodJob *job);
	const XmodValue *(*job_arg)(XmodJob *job, int index);
	void             (*job_done)(XmodJob *job, XmodValue *result);  /* Lua callback gets (nil, result)   (takes ownership) */
	void             (*job_fail)(XmodJob *job, const char *error);  /* Lua callback gets (error, nil) */

	/* ---- thread-safe helpers */
	void (*post_main)(xmod_main_fn fn, void *user);                 /* run fn(user) on the main thread at the next frame */
	void (*emit_event)(const char *event, XmodValue *args_array);   /* fire a Lua event (xmod.on) on the main thread; takes ownership; array elements become the handler args */
	void (*log)(const char *msg);                                   /* server log line, tagged with xmod */
	void (*log_error)(const char *msg);
	int  (*is_main_thread)(void);

	/* ---- game helpers (thread-safe: queued to the main thread when called from a worker) */
	void (*color_chat)(int player_id, const char *text);            /* chat message, 0 = every human player. Colour tags: {y} yellow/default, {t} team colour, {g} green. Max ~188 chars. */
	void (*client_print)(int player_id, const char *kind, const char *text);   /* kind: "console" | "center" | "chat"; 0 = everybody */

	/* ---- (future fields are appended here) */
} XmodApi;

#ifndef _WIN32
static inline int xmod_ext__find_cb(struct dl_phdr_info *info, size_t sz, void *data) {
	(void)sz; if (info->dlpi_name && strstr(info->dlpi_name, "xmod_mm_i386.so")) { *(const char **)data = info->dlpi_name; return 1; } return 0;
}
#endif
/* Locate xmod's module (it must already be loaded: list xmod BEFORE your plugin in plugins.ini) and fetch the API.
 * Returns NULL if xmod is not loaded or is too old. */
static inline const XmodApi *xmod_ext_connect(void) {
	typedef const XmodApi *(*xmod_get_api_t)(uint32_t);
	xmod_get_api_t getter = 0;
#ifdef _WIN32
	HMODULE h = GetModuleHandleA("xmod_mm.dll");
	if (h) getter = (xmod_get_api_t)GetProcAddress(h, "xmod_get_api");
#else
	const char *name = 0; dl_iterate_phdr(xmod_ext__find_cb, &name);
	if (name) { void *h = dlopen(name, RTLD_NOW | RTLD_NOLOAD); if (h) getter = (xmod_get_api_t)dlsym(h, "xmod_get_api"); }
#endif
	if (!getter) return 0;
	const XmodApi *api = getter(XMOD_EXT_ABI);
	if (!api || api->abi_version != XMOD_EXT_ABI || api->size < offsetof(XmodApi, client_print) + sizeof(void *)) return 0;
	return api;
}

#ifdef __cplusplus
}
#endif
#endif /* XMOD_EXT_H */
