// HTTP client. Backends:  Windows -> WinHTTP (HTTP + HTTPS, system proxy, no dependencies)
//                         Linux   -> libcurl loaded at run time when present (HTTPS, redirects, gzip), otherwise a built-in HTTP/1.1 client (plain http only).
#include "backends.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>
#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#else
#include <dlfcn.h>
#endif

bool parse_url(const std::string &url, std::string &scheme, std::string &host, int &port, std::string &pq) {
	size_t p = url.find("://"); if (p == std::string::npos) return false;
	scheme = lower(url.substr(0, p)); if (scheme != "http" && scheme != "https") return false;
	size_t s = p + 3, e = url.find_first_of("/?#", s); std::string auth = url.substr(s, e == std::string::npos ? std::string::npos : e - s);
	pq = e == std::string::npos ? "/" : url.substr(e); size_t h = pq.find('#'); if (h != std::string::npos) pq.erase(h);
	if (pq.empty() || pq[0] == '?') pq = "/" + pq;
	if (auth.find('@') != std::string::npos) return false;                           // no credentials in URLs
	port = scheme == "https" ? 443 : 80;
	if (!auth.empty() && auth[0] == '[') { size_t r = auth.find(']'); if (r == std::string::npos) return false; host = auth.substr(1, r - 1); if (r + 1 < auth.size() && auth[r + 1] == ':') port = atoi(auth.c_str() + r + 2); }
	else { size_t c = auth.rfind(':'); if (c != std::string::npos) { host = auth.substr(0, c); port = atoi(auth.c_str() + c + 1); } else host = auth; }
	for (char ch : url) if ((unsigned char)ch <= 32) return false;                    // no spaces / control characters (request smuggling)
	return !host.empty() && port > 0 && port < 65536;
}
static bool header_ok(const std::string &s) { return s.find('\r') == std::string::npos && s.find('\n') == std::string::npos && s.find('\0') == std::string::npos; }
static bool validate(const HttpReq &r, std::string &err) {
	for (auto &h : r.headers) if (!header_ok(h.first) || !header_ok(h.second) || h.first.empty() || h.first.find(':') != std::string::npos) { err = "illegal header"; return false; }
	if (r.method.empty()) { err = "illegal HTTP method"; return false; }
	for (char c : r.method) if (!isupper((unsigned char)c)) { err = "illegal HTTP method (use upper case, e.g. GET)"; return false; }
	return true;
}
static void add_header(HttpResp &out, const std::string &k, const std::string &v) {
	for (auto &h : out.headers) if (h.first == k) { h.second += ", " + v; return; }
	out.headers.emplace_back(k, v);
}

#ifndef _WIN32
// ---------------------------------------------------------------------------------------------- built-in plain HTTP/1.1
static bool builtin_http(const HttpReq &r, HttpResp &out, std::string &err) {
	std::string scheme, host, pq; int port; if (!parse_url(r.url, scheme, host, port, pq)) { err = "bad URL"; return false; }
	if (scheme == "https") { err = "HTTPS needs libcurl: install the 32-bit libcurl (Debian/Ubuntu: dpkg --add-architecture i386 && apt install libcurl4:i386) or set curl_library= in configs/query.ini"; return false; }
	long long deadline = now_ms() + r.timeout_ms;
	NetFd fd = tcp_connect(host, port, r.timeout_ms, err); if (fd == NET_BAD) return false;
	std::string req = r.method + " " + pq + " HTTP/1.1\r\nHost: " + host + ((port != 80) ? ":" + std::to_string(port) : "") + "\r\nUser-Agent: xmod_query/1.0\r\nAccept: */*\r\nConnection: close\r\n";
	bool has_len = false;
	for (auto &h : r.headers) { std::string k = lower(h.first); if (k == "host" || k == "connection") continue; if (k == "content-length") has_len = true; req += h.first + ": " + h.second + "\r\n"; }
	if (!has_len && (!r.body.empty() || r.method == "POST" || r.method == "PUT" || r.method == "PATCH")) req += "Content-Length: " + std::to_string(r.body.size()) + "\r\n";
	req += "\r\n"; req += r.body;
	if (!sock_send_all(fd, req.data(), req.size(), (int)(deadline - now_ms()), err)) { sock_close(fd); return false; }
	std::string raw; char buf[8192]; size_t header_end = std::string::npos; bool done = false;
	while (!done) {
		int left = (int)(deadline - now_ms()); if (left <= 0) { err = "timeout waiting for the response"; sock_close(fd); return false; }
		int n = sock_recv(fd, buf, sizeof buf, left);
		if (n == -2) { err = "timeout waiting for the response"; sock_close(fd); return false; }
		if (n < 0) { err = "receive failed"; sock_close(fd); return false; }
		if (n == 0) break;
		raw.append(buf, (size_t)n);
		if (header_end == std::string::npos) { header_end = raw.find("\r\n\r\n"); if (header_end == std::string::npos && raw.size() > 65536) { err = "response headers too large"; sock_close(fd); return false; } }
		if (header_end != std::string::npos && raw.size() - (header_end + 4) > r.max_bytes * 2 + 65536) { done = true; out.truncated = true; }
	}
	sock_close(fd);
	if (header_end == std::string::npos) { err = "malformed HTTP response"; return false; }
	std::string head = raw.substr(0, header_end), body = raw.substr(header_end + 4);
	size_t le = head.find("\r\n"); std::string status = head.substr(0, le);
	size_t sp = status.find(' '); if (status.compare(0, 5, "HTTP/") != 0 || sp == std::string::npos) { err = "malformed HTTP status line"; return false; }
	out.status = atoi(status.c_str() + sp + 1); out.backend = "builtin";
	bool chunked = false; long long clen = -1; size_t pos = le == std::string::npos ? head.size() : le + 2;
	while (pos < head.size()) {
		size_t nl = head.find("\r\n", pos); if (nl == std::string::npos) nl = head.size();
		std::string line = head.substr(pos, nl - pos); pos = nl + 2; size_t c = line.find(':'); if (c == std::string::npos) continue;
		std::string k = lower(line.substr(0, c)), v = line.substr(c + 1); size_t a = v.find_first_not_of(" \t"); v = a == std::string::npos ? "" : v.substr(a);
		if (k == "transfer-encoding" && lower(v).find("chunked") != std::string::npos) chunked = true; else if (k == "content-length") clen = atoll(v.c_str());
		add_header(out, k, v);
	}
	if (r.method == "HEAD" || out.status == 204 || out.status == 304) body.clear();
	else if (chunked) {
		std::string dec; size_t p = 0;
		while (p < body.size()) {
			size_t e = body.find("\r\n", p); if (e == std::string::npos) break;
			unsigned long sz = strtoul(body.substr(p, e - p).c_str(), nullptr, 16); p = e + 2; if (sz == 0) break;
			if (sz > body.size() - p) sz = (unsigned long)(body.size() - p);
			dec.append(body, p, sz); p += sz + 2; if (dec.size() > r.max_bytes) break;
		}
		body.swap(dec);
	} else if (clen >= 0 && (size_t)clen < body.size()) body.resize((size_t)clen);
	if (body.size() > r.max_bytes) { body.resize(r.max_bytes); out.truncated = true; }
	out.body.swap(body); return true;
}

// ---------------------------------------------------------------------------------------------- libcurl (loaded at run time)
namespace {
typedef void CURL; struct curl_slist;
struct CurlApi {
	void *lib = nullptr; std::string path; int (*global_init)(long) = nullptr; CURL *(*easy_init)(void) = nullptr; int (*setopt)(CURL *, int, ...) = nullptr; int (*perform)(CURL *) = nullptr;
	int (*getinfo)(CURL *, int, ...) = nullptr; void (*cleanup)(CURL *) = nullptr; curl_slist *(*slist_append)(curl_slist *, const char *) = nullptr; void (*slist_free)(curl_slist *) = nullptr;
	const char *(*strerror)(int) = nullptr;
} g_curl; std::once_flag g_curl_once;
void load_curl(const std::string &explicit_path) {
	std::vector<std::string> c; if (!explicit_path.empty()) c.push_back(explicit_path);
	for (const char *n : {"libcurl.so.4", "libcurl-gnutls.so.4", "libcurl.so", "libcurl.so.3"}) c.push_back(n);
	for (auto &p : c) { g_curl.lib = dlopen(p.c_str(), RTLD_NOW | RTLD_LOCAL); if (g_curl.lib) { g_curl.path = p; break; } }
	if (!g_curl.lib) return;
#define CSYM(f, n) *(void **)(&g_curl.f) = dlsym(g_curl.lib, n)
	CSYM(global_init, "curl_global_init"); CSYM(easy_init, "curl_easy_init"); CSYM(setopt, "curl_easy_setopt"); CSYM(perform, "curl_easy_perform"); CSYM(getinfo, "curl_easy_getinfo");
	CSYM(cleanup, "curl_easy_cleanup"); CSYM(slist_append, "curl_slist_append"); CSYM(slist_free, "curl_slist_free_all"); CSYM(strerror, "curl_easy_strerror");
	if (!g_curl.global_init || !g_curl.easy_init || !g_curl.setopt || !g_curl.perform || !g_curl.getinfo || !g_curl.cleanup || !g_curl.slist_append || !g_curl.slist_free) { dlclose(g_curl.lib); g_curl.lib = nullptr; return; }
	g_curl.global_init(3);
}
bool curl_ready(const std::string &p) { std::call_once(g_curl_once, [&]() { load_curl(p); }); return g_curl.lib != nullptr; }
struct CurlCtx { std::string *body; size_t max; bool *truncated; HttpResp *resp; };
size_t write_cb(char *p, size_t sz, size_t n, void *ud) {
	CurlCtx *c = (CurlCtx *)ud; size_t total = sz * n;
	if (c->body->size() + total > c->max) { size_t room = c->max > c->body->size() ? c->max - c->body->size() : 0; c->body->append(p, room); *c->truncated = true; return 0; }
	c->body->append(p, total); return total;
}
size_t header_cb(char *p, size_t sz, size_t n, void *ud) {
	CurlCtx *c = (CurlCtx *)ud; size_t total = sz * n; std::string line(p, total);
	if (line.compare(0, 5, "HTTP/") == 0) { c->resp->headers.clear(); return total; }       // a new response (redirect chain): start over
	size_t col = line.find(':'); if (col == std::string::npos) return total;
	std::string k = lower(line.substr(0, col)), v = line.substr(col + 1); size_t a = v.find_first_not_of(" \t"), b = v.find_last_not_of(" \t\r\n"); v = a == std::string::npos ? "" : v.substr(a, b - a + 1);
	add_header(*c->resp, k, v); return total;
}
bool curl_http(const HttpReq &r, HttpResp &out, std::string &err) {
	CURL *h = g_curl.easy_init(); if (!h) { err = "curl init failed"; return false; }
	CurlCtx ctx{&out.body, r.max_bytes, &out.truncated, &out}; curl_slist *hdrs = nullptr;
	for (auto &x : r.headers) hdrs = g_curl.slist_append(hdrs, (x.first + ": " + x.second).c_str());
	g_curl.setopt(h, 10002, r.url.c_str()); g_curl.setopt(h, 20011, write_cb); g_curl.setopt(h, 10001, &ctx); g_curl.setopt(h, 20079, header_cb); g_curl.setopt(h, 10029, &ctx);
	g_curl.setopt(h, 99, 1L); g_curl.setopt(h, 155, (long)r.timeout_ms); g_curl.setopt(h, 156, (long)std::min(r.timeout_ms, 10000)); g_curl.setopt(h, 10018, "xmod_query/1.0");
	g_curl.setopt(h, 181, 3L); g_curl.setopt(h, 182, 3L);                    // CURLOPT_PROTOCOLS / REDIR_PROTOCOLS = http|https only (never file://, ftp://, ...)
	g_curl.setopt(h, 52, r.follow ? 1L : 0L); g_curl.setopt(h, 68, 5L); g_curl.setopt(h, 10102, "");   // follow (max 5), accept gzip/deflate
	if (!r.ca_file.empty()) g_curl.setopt(h, 10065, r.ca_file.c_str());       // certificate verification itself is always on (curl default)
	if (hdrs) g_curl.setopt(h, 10023, hdrs);
	if (r.method == "HEAD") g_curl.setopt(h, 44, 1L);
	else if (r.method != "GET") { g_curl.setopt(h, 10036, r.method.c_str()); g_curl.setopt(h, 10015, r.body.c_str()); g_curl.setopt(h, 60, (long)r.body.size()); }
	int rc = g_curl.perform(h);
	if (rc != 0 && !(out.truncated && rc == 23)) err = std::string("curl: ") + (g_curl.strerror ? g_curl.strerror(rc) : "error");
	else { long code = 0; g_curl.getinfo(h, 0x200002, &code); out.status = (int)code; out.backend = "curl"; }
	if (hdrs) g_curl.slist_free(hdrs);
	g_curl.cleanup(h);
	return err.empty();
}
}
#endif

// ---------------------------------------------------------------------------------------------- WinHTTP
#ifdef _WIN32
static std::wstring widen(const std::string &s) { if (s.empty()) return L""; int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0); std::wstring w((size_t)n, L'\0'); MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n); return w; }
static bool winhttp_http(const HttpReq &r, HttpResp &out, std::string &err) {
	std::string scheme, host, pq; int port; if (!parse_url(r.url, scheme, host, port, pq)) { err = "bad URL"; return false; }
	HINTERNET ses = WinHttpOpen(L"xmod_query/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (!ses) { err = "WinHttpOpen failed"; return false; }
	WinHttpSetTimeouts(ses, r.timeout_ms, std::min(r.timeout_ms, 10000), r.timeout_ms, r.timeout_ms);
	HINTERNET con = WinHttpConnect(ses, widen(host).c_str(), (INTERNET_PORT)port, 0); if (!con) { err = "WinHttpConnect failed"; WinHttpCloseHandle(ses); return false; }
	HINTERNET req = WinHttpOpenRequest(con, widen(r.method).c_str(), widen(pq).c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, scheme == "https" ? WINHTTP_FLAG_SECURE : 0);
	bool ok = req != nullptr; if (!ok) err = "WinHttpOpenRequest failed";
	if (ok && !r.follow) { DWORD opt = WINHTTP_DISABLE_REDIRECTS; WinHttpSetOption(req, WINHTTP_OPTION_DISABLE_FEATURE, &opt, sizeof opt); }
	if (ok) for (auto &h : r.headers) { std::wstring line = widen(h.first + ": " + h.second); WinHttpAddRequestHeaders(req, line.c_str(), (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE); }
	if (ok && !WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, r.body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)r.body.data(), (DWORD)r.body.size(), (DWORD)r.body.size(), 0)) { ok = false; err = "request failed (connection/TLS error or timeout)"; }
	if (ok && !WinHttpReceiveResponse(req, nullptr)) { ok = false; err = "no response (timeout or TLS error)"; }
	if (ok) {
		DWORD code = 0, sz = sizeof code; WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz, WINHTTP_NO_HEADER_INDEX); out.status = (int)code; out.backend = "winhttp";
		DWORD hl = 0; WinHttpQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &hl, WINHTTP_NO_HEADER_INDEX);
		if (hl > 0) { std::wstring wh(hl / sizeof(wchar_t) + 1, L'\0'); if (WinHttpQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, &wh[0], &hl, WINHTTP_NO_HEADER_INDEX)) {
			std::string h(wh.begin(), wh.end()); size_t pos = h.find("\r\n"); pos = pos == std::string::npos ? h.size() : pos + 2;
			while (pos < h.size()) { size_t nl = h.find("\r\n", pos); if (nl == std::string::npos) nl = h.size(); std::string line = h.substr(pos, nl - pos); pos = nl + 2; size_t c = line.find(':'); if (c == std::string::npos) continue;
				std::string k = lower(line.substr(0, c)), v = line.substr(c + 1); size_t a = v.find_first_not_of(" \t"); v = a == std::string::npos ? "" : v.substr(a); add_header(out, k, v); } } }
		for (;;) {
			DWORD avail = 0; if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
			size_t room = r.max_bytes > out.body.size() ? r.max_bytes - out.body.size() : 0; if (room == 0) { out.truncated = true; break; }
			std::string chunk(std::min<size_t>(avail, room), '\0'); DWORD got = 0; if (!WinHttpReadData(req, &chunk[0], (DWORD)chunk.size(), &got) || got == 0) break;
			out.body.append(chunk, 0, got);
		}
	}
	if (req) WinHttpCloseHandle(req);
	WinHttpCloseHandle(con); WinHttpCloseHandle(ses);
	return ok;
}
#endif

bool http_perform(const HttpReq &req, HttpResp &resp, std::string &err) {
	err.clear();
	if (!validate(req, err)) return false;
#ifdef _WIN32
	return winhttp_http(req, resp, err);
#else
	if (curl_ready(req.curl_library)) return curl_http(req, resp, err);
	return builtin_http(req, resp, err);
#endif
}
std::string http_backends_description(const Settings &s) {
#ifdef _WIN32
	(void)s; return "winhttp (http+https)";
#else
	return curl_ready(s.curl_library) ? "curl " + g_curl.path + " (http+https)" : "builtin (plain http only; install 32-bit libcurl for https)";
#endif
}
