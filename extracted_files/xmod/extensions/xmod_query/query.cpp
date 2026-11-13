// xmod_query - blocking I/O for Lua without blocking the server: SQL (SQLite bundled, MySQL/MariaDB), HTTP/HTTPS, TCP, UDP, DNS.
// Every I/O function is asynchronous: xmod.query.<fn>(args..., function(err, result) ... end)   (the callback runs later on the game thread).
// Server-side configuration (credentials, allow-lists, limits) lives in addons/xmod/configs/query.ini - scripts never see passwords.
#define XMOD_EXT_NAME    "xmod_query"
#define XMOD_EXT_VERSION "1.0"
#include "xmod_ext_glue.h"
#include "backends.h"
#include "third_party/sqlite3.h"
#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>

static std::shared_ptr<Config> g_cfg; static std::mutex g_cfg_mu;
static std::shared_ptr<Config> cfg() { std::lock_guard<std::mutex> l(g_cfg_mu); return g_cfg; }

struct DbEntry { std::unique_ptr<DbConn> conn; std::string profile; };
struct SockEntry { std::atomic<NetFd> fd{NET_BAD}; };
static std::mutex g_mu;
static std::map<int64_t, std::shared_ptr<DbEntry>> g_dbs;
static std::map<int64_t, std::shared_ptr<SockEntry>> g_socks;
static std::atomic<int64_t> g_next_handle{1};

// ---------------------------------------------------------------------------------------------- argument / result helpers
static bool arg_str(XmodJob *j, int i, std::string &out) { size_t n = 0; const char *s = g_xmod->val_get_str(g_xmod->job_arg(j, i), &n); if (!s) return false; out.assign(s, n); return true; }
static const XmodValue *arg_opts(XmodJob *j, int i) { const XmodValue *v = g_xmod->job_arg(j, i); return (v && g_xmod->val_type(v) == XV_MAP) ? v : nullptr; }
static double opt_num(const XmodValue *o, const char *k, double d) { return o ? g_xmod->val_get_num(g_xmod->val_get(o, k), d) : d; }
static bool opt_bool(const XmodValue *o, const char *k, bool d) { return o ? g_xmod->val_get_bool(g_xmod->val_get(o, k), d) != 0 : d; }
static std::string opt_str(const XmodValue *o, const char *k) { std::string s; if (o) { size_t n; const char *p = g_xmod->val_get_str(g_xmod->val_get(o, k), &n); if (p) s.assign(p, n); } return s; }
static int timeout_ms(const XmodValue *o, const Settings &s) { double t = opt_num(o, "timeout", s.default_timeout); if (t < 0.1) t = 0.1; if (t > s.max_timeout) t = s.max_timeout; return (int)(t * 1000); }
static size_t max_bytes(const XmodValue *o, const Settings &s) { double b = opt_num(o, "max_bytes", (double)s.max_response_bytes); if (b < 1) b = 1; if (b > (double)s.max_response_bytes) b = (double)s.max_response_bytes; return (size_t)b; }
static XmodValue *cell_value(const Cell &c) {
	switch (c.t) { case Cell::Int: return g_xmod->val_int(c.i); case Cell::Real: return g_xmod->val_num(c.d); case Cell::Text: return g_xmod->val_str(c.s.data(), c.s.size()); default: return g_xmod->val_nil(); }
}
static XmodValue *result_value(const QResult &r) {
	XmodValue *m = g_xmod->val_map(), *cols = g_xmod->val_array(), *rows = g_xmod->val_array();
	for (auto &c : r.cols) g_xmod->val_push(cols, g_xmod->val_str(c.data(), c.size()));
	for (auto &row : r.rows) { XmodValue *rm = g_xmod->val_map(); for (size_t i = 0; i < row.size() && i < r.cols.size(); i++) if (row[i].t != Cell::Null) g_xmod->val_set(rm, r.cols[i].c_str(), cell_value(row[i])); g_xmod->val_push(rows, rm); }
	g_xmod->val_set(m, "columns", cols); g_xmod->val_set(m, "rows", rows); g_xmod->val_set(m, "affected", g_xmod->val_int(r.affected));
	g_xmod->val_set(m, "insert_id", g_xmod->val_int(r.insert_id)); g_xmod->val_set(m, "truncated", g_xmod->val_bool(r.truncated));
	return m;
}
static bool read_params(const XmodValue *v, std::vector<Cell> &out, std::string &err) {
	if (!v || g_xmod->val_type(v) == XV_NIL) return true;
	if (g_xmod->val_type(v) == XV_MAP && g_xmod->val_len(v) == 0) return true;                 // {} arrives as an empty table
	if (g_xmod->val_type(v) != XV_ARRAY) { err = "params must be an array table, e.g. {1, \"x\"}"; return false; }
	for (size_t i = 0; i < g_xmod->val_len(v); i++) {
		const XmodValue *p = g_xmod->val_at(v, i); Cell c;
		switch (g_xmod->val_type(p)) {
		case XV_INT: c.t = Cell::Int; c.i = g_xmod->val_get_int(p, 0); break;
		case XV_NUM: c.t = Cell::Real; c.d = g_xmod->val_get_num(p, 0); break;
		case XV_BOOL: c.t = Cell::Int; c.i = g_xmod->val_get_bool(p, 0); break;
		case XV_STR: { size_t n; const char *s = g_xmod->val_get_str(p, &n); c.t = Cell::Text; c.s.assign(s, n); break; }
		case XV_MAP: if (g_xmod->val_get(p, "$null")) { c.t = Cell::Null; break; } /* fallthrough */
		default: err = "param #" + std::to_string(i + 1) + " must be a number, string, boolean or NULL (require(\"query\").NULL)"; return false;
		}
		out.push_back(c);
	}
	return true;
}

// ---------------------------------------------------------------------------------------------- databases
static void j_open(XmodJob *j, void *) {
	std::string name; if (!arg_str(j, 0, name)) { g_xmod->job_fail(j, "usage: xmod.query.open(profile, callback)"); return; }
	auto c = cfg(); auto it = c->dbs.find(lower(name)); if (it == c->dbs.end()) { g_xmod->job_fail(j, ("unknown database profile '" + name + "' (define [db:" + name + "] in configs/query.ini)").c_str()); return; }
	{ std::lock_guard<std::mutex> l(g_mu); int n = 0; for (auto &kv : g_dbs) if (kv.second->profile == it->first) n++; if (n >= c->s.max_connections) { g_xmod->job_fail(j, "too many open connections for this profile (max_connections)"); return; } }
	std::string err; DbConn *conn = it->second.driver == "sqlite" ? sqlite_open(it->second, c->datadir, err) : mysql_open(it->second, c->s, err);
	if (!conn) { g_xmod->job_fail(j, err.c_str()); return; }
	auto e = std::make_shared<DbEntry>(); e->conn.reset(conn); e->profile = it->first; int64_t h = g_next_handle++;
	{ std::lock_guard<std::mutex> l(g_mu); g_dbs[h] = e; }
	g_xmod->job_done(j, g_xmod->val_int(h));
}
static void j_exec(XmodJob *j, void *) {                              // exec(db, sql [, params], cb)
	int64_t h = g_xmod->val_get_int(g_xmod->job_arg(j, 0), 0); std::string sql; if (!arg_str(j, 1, sql)) { g_xmod->job_fail(j, "usage: xmod.query.exec(db, sql [, params], callback)"); return; }
	if (sql.size() > (1u << 20)) { g_xmod->job_fail(j, "SQL text too large"); return; }
	std::shared_ptr<DbEntry> e; { std::lock_guard<std::mutex> l(g_mu); auto it = g_dbs.find(h); if (it != g_dbs.end()) e = it->second; }
	if (!e) { g_xmod->job_fail(j, "invalid or closed database handle"); return; }
	std::vector<Cell> params; std::string err; if (!read_params(g_xmod->job_arg(j, 2), params, err)) { g_xmod->job_fail(j, err.c_str()); return; }
	auto c = cfg(); QResult r; if (!e->conn->exec(sql, params, (size_t)c->s.max_rows, r, err)) { g_xmod->job_fail(j, err.c_str()); return; }
	g_xmod->job_done(j, result_value(r));
}
static void j_close(XmodJob *j, void *) {
	int64_t h = g_xmod->val_get_int(g_xmod->job_arg(j, 0), 0); bool found; { std::lock_guard<std::mutex> l(g_mu); found = g_dbs.erase(h) > 0; }
	if (!found) { g_xmod->job_fail(j, "invalid or closed database handle"); return; }
	g_xmod->job_done(j, g_xmod->val_bool(1));
}

// ---------------------------------------------------------------------------------------------- HTTP
static XmodValue *http_value(const HttpResp &r) {
	XmodValue *m = g_xmod->val_map(), *h = g_xmod->val_map();
	for (auto &kv : r.headers) g_xmod->val_set(h, kv.first.c_str(), g_xmod->val_str(kv.second.data(), kv.second.size()));
	g_xmod->val_set(m, "status", g_xmod->val_int(r.status)); g_xmod->val_set(m, "body", g_xmod->val_str(r.body.data(), r.body.size())); g_xmod->val_set(m, "headers", h);
	g_xmod->val_set(m, "truncated", g_xmod->val_bool(r.truncated)); g_xmod->val_set(m, "backend", g_xmod->val_cstr(r.backend.c_str()));
	return m;
}
static void do_http(XmodJob *j, const std::string &method, const std::string &url, const std::string &body, const XmodValue *opts) {
	auto c = cfg(); HttpReq req; req.method = method; req.url = url; req.body = body;
	std::string scheme, host, pq; int port; if (!parse_url(url, scheme, host, port, pq)) { g_xmod->job_fail(j, "bad URL (only http:// and https:// without credentials are supported)"); return; }
	if (!allowed(c->s.http_allow, host, port)) { g_xmod->job_fail(j, ("host '" + host + "' is not allowed by http_allow in configs/query.ini").c_str()); return; }
	if (body.size() > (4u << 20)) { g_xmod->job_fail(j, "request body too large (max 4 MB)"); return; }
	req.timeout_ms = timeout_ms(opts, c->s); req.max_bytes = max_bytes(opts, c->s); req.ca_file = c->s.ca_file; req.curl_library = c->s.curl_library;
	bool restricted = !allowed(c->s.http_allow, "any-host.invalid", 65535);             // with a restrictive allow-list a redirect could leave it: never follow, return the 3xx
	req.follow = !restricted && opt_bool(opts, "follow", true);
	const XmodValue *hv = opts ? g_xmod->val_get(opts, "headers") : nullptr;
	if (hv && g_xmod->val_type(hv) == XV_MAP) for (size_t i = 0; i < g_xmod->val_len(hv); i++) { size_t n; const char *v = g_xmod->val_get_str(g_xmod->val_val(hv, i), &n); if (v) req.headers.emplace_back(g_xmod->val_key(hv, i), std::string(v, n)); }
	if (c->s.log_requests) g_xmod->log(("xmod_query: " + method + " " + scheme + "://" + host + ":" + std::to_string(port)).c_str());
	HttpResp resp; std::string err; if (!http_perform(req, resp, err)) { g_xmod->job_fail(j, err.c_str()); return; }
	g_xmod->job_done(j, http_value(resp));
}
static void j_http(XmodJob *j, void *) {                              // http(method, url [, opts], cb)   opts: headers, body, timeout, max_bytes, follow
	std::string m, u; if (!arg_str(j, 0, m) || !arg_str(j, 1, u)) { g_xmod->job_fail(j, "usage: xmod.query.http(method, url [, opts], callback)"); return; }
	const XmodValue *o = arg_opts(j, 2); do_http(j, m, u, opt_str(o, "body"), o);
}
static void j_http_get(XmodJob *j, void *) { std::string u; if (!arg_str(j, 0, u)) { g_xmod->job_fail(j, "usage: xmod.query.http_get(url [, opts], callback)"); return; } do_http(j, "GET", u, "", arg_opts(j, 1)); }
static void j_http_post(XmodJob *j, void *) {                         // http_post(url, body [, opts], cb)
	std::string u, b; if (!arg_str(j, 0, u) || !arg_str(j, 1, b)) { g_xmod->job_fail(j, "usage: xmod.query.http_post(url, body [, opts], callback)"); return; }
	do_http(j, "POST", u, b, arg_opts(j, 2));
}

// ---------------------------------------------------------------------------------------------- sockets
static bool read_loop(NetFd fd, size_t max, const std::string &until, long long deadline, std::string &data, bool &timed_out, bool &closed) {
	char buf[8192]; timed_out = closed = false;
	while (data.size() < max) {
		int left = (int)(deadline - now_ms()); if (left <= 0) { timed_out = true; return true; }
		int n = sock_recv(fd, buf, std::min(sizeof buf, max - data.size()), left);
		if (n == -2) { timed_out = true; return true; }
		if (n < 0) return false;
		if (n == 0) { closed = true; return true; }
		data.append(buf, (size_t)n);
		if (!until.empty() && data.find(until) != std::string::npos) return true;
	}
	return true;
}
static XmodValue *data_value(const std::string &data, bool timed_out, bool closed) {
	XmodValue *m = g_xmod->val_map(); g_xmod->val_set(m, "data", g_xmod->val_str(data.data(), data.size())); g_xmod->val_set(m, "bytes", g_xmod->val_int((int64_t)data.size()));
	g_xmod->val_set(m, "timed_out", g_xmod->val_bool(timed_out)); g_xmod->val_set(m, "closed", g_xmod->val_bool(closed)); return m;
}
static bool sock_allowed(XmodJob *j, const std::string &host, int port, const Settings &s) {
	if (port < 1 || port > 65535) { g_xmod->job_fail(j, "bad port"); return false; }
	if (!allowed(s.socket_allow, host, port)) { g_xmod->job_fail(j, ("'" + host + ":" + std::to_string(port) + "' is not allowed by socket_allow in configs/query.ini").c_str()); return false; }
	return true;
}
static void j_tcp_request(XmodJob *j, void *) {                       // tcp_request(host, port, data [, opts], cb)  opts: timeout, max_bytes, read_until, read(bool, default true)
	std::string host, data; if (!arg_str(j, 0, host) || !arg_str(j, 2, data)) { g_xmod->job_fail(j, "usage: xmod.query.tcp_request(host, port, data [, opts], callback)"); return; }
	int port = (int)g_xmod->val_get_int(g_xmod->job_arg(j, 1), 0); auto c = cfg(); if (!sock_allowed(j, host, port, c->s)) return;
	const XmodValue *o = arg_opts(j, 3); int tmo = timeout_ms(o, c->s); long long deadline = now_ms() + tmo;
	if (data.size() > (4u << 20)) { g_xmod->job_fail(j, "data too large (max 4 MB)"); return; }
	std::string err; NetFd fd = tcp_connect(host, port, tmo, err); if (fd == NET_BAD) { g_xmod->job_fail(j, err.c_str()); return; }
	if (!data.empty() && !sock_send_all(fd, data.data(), data.size(), (int)(deadline - now_ms()), err)) { sock_close(fd); g_xmod->job_fail(j, err.c_str()); return; }
	std::string out; bool to = false, closed = false;
	if (opt_bool(o, "read", true) && !read_loop(fd, max_bytes(o, c->s), opt_str(o, "read_until"), deadline, out, to, closed)) { sock_close(fd); g_xmod->job_fail(j, "receive failed"); return; }
	sock_close(fd); g_xmod->job_done(j, data_value(out, to, closed));
}
static void j_udp_request(XmodJob *j, void *) {                       // udp_request(host, port, data [, opts], cb) -> { data, from }
	std::string host, data; if (!arg_str(j, 0, host) || !arg_str(j, 2, data)) { g_xmod->job_fail(j, "usage: xmod.query.udp_request(host, port, data [, opts], callback)"); return; }
	int port = (int)g_xmod->val_get_int(g_xmod->job_arg(j, 1), 0); auto c = cfg(); if (!sock_allowed(j, host, port, c->s)) return;
	if (data.size() > 65000) { g_xmod->job_fail(j, "datagram too large"); return; }
	std::string reply, from, err; if (!udp_exchange(host, port, data, timeout_ms(arg_opts(j, 3), c->s), reply, from, err)) { g_xmod->job_fail(j, err.c_str()); return; }
	XmodValue *m = g_xmod->val_map(); g_xmod->val_set(m, "data", g_xmod->val_str(reply.data(), reply.size())); g_xmod->val_set(m, "from", g_xmod->val_cstr(from.c_str())); g_xmod->job_done(j, m);
}
static void j_resolve(XmodJob *j, void *) {
	std::string host; if (!arg_str(j, 0, host)) { g_xmod->job_fail(j, "usage: xmod.query.resolve(host, callback)"); return; }
	std::vector<std::string> ips; std::string err; if (!resolve_all(host, ips, err)) { g_xmod->job_fail(j, err.c_str()); return; }
	XmodValue *a = g_xmod->val_array(); for (auto &ip : ips) g_xmod->val_push(a, g_xmod->val_cstr(ip.c_str())); g_xmod->job_done(j, a);
}
static std::shared_ptr<SockEntry> get_sock(int64_t h) { std::lock_guard<std::mutex> l(g_mu); auto it = g_socks.find(h); return it == g_socks.end() ? nullptr : it->second; }
static void j_tcp_open(XmodJob *j, void *) {                          // tcp_open(host, port [, opts], cb) -> handle
	std::string host; if (!arg_str(j, 0, host)) { g_xmod->job_fail(j, "usage: xmod.query.tcp_open(host, port [, opts], callback)"); return; }
	int port = (int)g_xmod->val_get_int(g_xmod->job_arg(j, 1), 0); auto c = cfg(); if (!sock_allowed(j, host, port, c->s)) return;
	{ std::lock_guard<std::mutex> l(g_mu); if ((int)g_socks.size() >= c->s.max_sockets) { g_xmod->job_fail(j, "too many open sockets (max_sockets)"); return; } }
	std::string err; NetFd fd = tcp_connect(host, port, timeout_ms(arg_opts(j, 2), c->s), err); if (fd == NET_BAD) { g_xmod->job_fail(j, err.c_str()); return; }
	auto s = std::make_shared<SockEntry>(); s->fd = fd; int64_t h = g_next_handle++; { std::lock_guard<std::mutex> l(g_mu); g_socks[h] = s; }
	g_xmod->job_done(j, g_xmod->val_int(h));
}
static void j_tcp_send(XmodJob *j, void *) {                          // tcp_send(h, data [, opts], cb) -> bytes sent
	auto s = get_sock(g_xmod->val_get_int(g_xmod->job_arg(j, 0), 0)); std::string data; if (!s) { g_xmod->job_fail(j, "invalid or closed socket handle"); return; }
	if (!arg_str(j, 1, data)) { g_xmod->job_fail(j, "usage: xmod.query.tcp_send(handle, data [, opts], callback)"); return; }
	auto c = cfg(); std::string err; if (!sock_send_all(s->fd.load(), data.data(), data.size(), timeout_ms(arg_opts(j, 2), c->s), err)) { g_xmod->job_fail(j, err.c_str()); return; }
	g_xmod->job_done(j, g_xmod->val_int((int64_t)data.size()));
}
static void j_tcp_recv(XmodJob *j, void *) {                          // tcp_recv(h [, opts], cb)  opts: timeout, max_bytes (default 4096), read_until
	auto s = get_sock(g_xmod->val_get_int(g_xmod->job_arg(j, 0), 0)); if (!s) { g_xmod->job_fail(j, "invalid or closed socket handle"); return; }
	auto c = cfg(); const XmodValue *o = arg_opts(j, 1); size_t mx = (size_t)opt_num(o, "max_bytes", 4096); mx = std::min(std::max<size_t>(mx, 1), c->s.max_response_bytes);
	std::string until = opt_str(o, "read_until"), out; bool to = false, closed = false; long long deadline = now_ms() + timeout_ms(o, c->s);
	if (until.empty()) {                                                // no terminator: return as soon as some data arrived
		char buf[8192]; int n = sock_recv(s->fd.load(), buf, std::min(sizeof buf, mx), (int)(deadline - now_ms()));
		if (n == -2) to = true; else if (n < 0) { g_xmod->job_fail(j, "receive failed"); return; } else if (n == 0) closed = true; else out.assign(buf, (size_t)n);
	} else if (!read_loop(s->fd.load(), mx, until, deadline, out, to, closed)) { g_xmod->job_fail(j, "receive failed"); return; }
	g_xmod->job_done(j, data_value(out, to, closed));
}
static void j_tcp_close(XmodJob *j, void *) {
	int64_t h = g_xmod->val_get_int(g_xmod->job_arg(j, 0), 0); std::shared_ptr<SockEntry> s; { std::lock_guard<std::mutex> l(g_mu); auto it = g_socks.find(h); if (it != g_socks.end()) { s = it->second; g_socks.erase(it); } }
	if (!s) { g_xmod->job_fail(j, "invalid or closed socket handle"); return; }
	sock_close(s->fd.exchange(NET_BAD)); g_xmod->job_done(j, g_xmod->val_bool(1));
}

// ---------------------------------------------------------------------------------------------- sync functions (main thread, no I/O except reload_config's small file read)
static std::string gamedir() { char gd[256] = {0}; GET_GAME_DIR(gd); return gd; }
static bool reload_cfg(std::string &err) {
	auto c = std::make_shared<Config>(); std::string base = gamedir() + "/addons/xmod/"; c->datadir = base + "data/";
	if (!load_config(base + "configs/query.ini", *c, err)) return false;
	std::lock_guard<std::mutex> l(g_cfg_mu); g_cfg = c; return true;
}
static void f_status(XmodCall *call, void *) {
	auto c = cfg(); XmodValue *m = g_xmod->val_map();
	g_xmod->val_set(m, "version", g_xmod->val_cstr(XMOD_EXT_VERSION)); g_xmod->val_set(m, "sqlite", g_xmod->val_cstr(sqlite3_libversion()));
	std::string why; bool my = mysql_available(c->s, why); g_xmod->val_set(m, "mysql", g_xmod->val_bool(my)); if (!my) g_xmod->val_set(m, "mysql_note", g_xmod->val_cstr(why.c_str()));
	std::string hb = http_backends_description(c->s); g_xmod->val_set(m, "http", g_xmod->val_cstr(hb.c_str()));
	XmodValue *pl = g_xmod->val_array(); for (auto &kv : c->dbs) g_xmod->val_push(pl, g_xmod->val_cstr(kv.first.c_str())); g_xmod->val_set(m, "profiles", pl);
	{ std::lock_guard<std::mutex> l(g_mu); g_xmod->val_set(m, "open_databases", g_xmod->val_int((int64_t)g_dbs.size())); g_xmod->val_set(m, "open_sockets", g_xmod->val_int((int64_t)g_socks.size())); }
	g_xmod->call_return(call, m);
}
static void f_profiles(XmodCall *call, void *) {
	auto c = cfg(); XmodValue *a = g_xmod->val_array();
	for (auto &kv : c->dbs) { XmodValue *m = g_xmod->val_map(); g_xmod->val_set(m, "name", g_xmod->val_cstr(kv.first.c_str())); g_xmod->val_set(m, "driver", g_xmod->val_cstr(kv.second.driver.c_str())); g_xmod->val_push(a, m); }   // never exposes credentials
	g_xmod->call_return(call, a);
}
static void f_reload_config(XmodCall *call, void *) { std::string err; if (!reload_cfg(err)) { g_xmod->call_error(call, err.c_str()); return; } g_xmod->call_return(call, g_xmod->val_bool(1)); }

static bool xext_init(const XmodApi *api, XmodExt *ext) {
	net_init(); std::string err;
	if (!reload_cfg(err)) {
		api->log_error(("xmod_query: " + err + " - using defaults (no database profiles)").c_str());
		auto c = std::make_shared<Config>(); c->datadir = gamedir() + "/addons/xmod/data/"; std::lock_guard<std::mutex> l(g_cfg_mu); g_cfg = c;
	}
	bool ok = api->register_func(ext, "query", "status", f_status, nullptr) && api->register_func(ext, "query", "profiles", f_profiles, nullptr) && api->register_func(ext, "query", "reload_config", f_reload_config, nullptr)
		&& api->register_async(ext, "query", "open", j_open, nullptr, 0) && api->register_async(ext, "query", "exec", j_exec, nullptr, 1) && api->register_async(ext, "query", "close", j_close, nullptr, 1)
		&& api->register_async(ext, "query", "http", j_http, nullptr, 0) && api->register_async(ext, "query", "http_get", j_http_get, nullptr, 0) && api->register_async(ext, "query", "http_post", j_http_post, nullptr, 0)
		&& api->register_async(ext, "query", "tcp_request", j_tcp_request, nullptr, 0) && api->register_async(ext, "query", "udp_request", j_udp_request, nullptr, 0)
		&& api->register_async(ext, "query", "resolve", j_resolve, nullptr, 0) && api->register_async(ext, "query", "tcp_open", j_tcp_open, nullptr, 0)
		&& api->register_async(ext, "query", "tcp_send", j_tcp_send, nullptr, 1) && api->register_async(ext, "query", "tcp_recv", j_tcp_recv, nullptr, 1) && api->register_async(ext, "query", "tcp_close", j_tcp_close, nullptr, 1);
	if (!ok) { api->log_error("xmod_query: registering functions failed"); return false; }
	auto c = cfg(); api->log(("xmod_query ready: " + std::to_string(c->dbs.size()) + " database profile(s); http backend: " + http_backends_description(c->s)).c_str());
	return true;
}
static void xext_shutdown(void) {                                     // close sockets so blocked workers return; DB connections are freed when the last job drops its reference
	std::map<int64_t, std::shared_ptr<DbEntry>> dbs; std::map<int64_t, std::shared_ptr<SockEntry>> socks;
	{ std::lock_guard<std::mutex> l(g_mu); dbs.swap(g_dbs); socks.swap(g_socks); }
	for (auto &kv : socks) sock_close(kv.second->fd.exchange(NET_BAD));
}
