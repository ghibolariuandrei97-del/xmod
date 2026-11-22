// MySQL / MariaDB backend. The client library is loaded at RUN time (libmariadb / libmysqlclient), so xmod_query has no link-time dependency on it:
// install the 32-bit client library (Linux: libmariadb3:i386 or libmysqlclient, Windows: 32-bit libmariadb.dll / libmysql.dll next to hlds.exe) or set mysql_library= in query.ini.
#include "backends.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#ifdef _WIN32
#include <windows.h>
#define LOADLIB(p) LoadLibraryA(p)
#define GETSYM(h, n) GetProcAddress((HMODULE)(h), n)
#else
#include <dlfcn.h>
#define LOADLIB(p) dlopen(p, RTLD_NOW | RTLD_LOCAL)
#define GETSYM(h, n) dlsym(h, n)
#endif

namespace {
typedef void MYSQL; typedef void MYSQL_RES; typedef char **MYSQL_ROW;
struct MyField { char *name, *org_name, *table, *org_table, *db, *catalog, *def; unsigned long length, max_length;    // layout shared by MySQL and MariaDB Connector/C up to 'type'
	unsigned int name_length, org_name_length, table_length, org_table_length, db_length, catalog_length, def_length, flags, decimals, charsetnr; int type; };
struct MyApi {
	void *lib = nullptr; std::string path;
	int (*library_init)(int, char **, char **) = nullptr; char (*thread_init)(void) = nullptr;
	MYSQL *(*init)(MYSQL *) = nullptr; MYSQL *(*real_connect)(MYSQL *, const char *, const char *, const char *, const char *, unsigned int, const char *, unsigned long) = nullptr;
	void (*close)(MYSQL *) = nullptr; int (*real_query)(MYSQL *, const char *, unsigned long) = nullptr; MYSQL_RES *(*store_result)(MYSQL *) = nullptr; void (*free_result)(MYSQL_RES *) = nullptr;
	unsigned int (*num_fields)(MYSQL_RES *) = nullptr; MYSQL_ROW (*fetch_row)(MYSQL_RES *) = nullptr; unsigned long *(*fetch_lengths)(MYSQL_RES *) = nullptr;
	MyField *(*fetch_field_direct)(MYSQL_RES *, unsigned int) = nullptr; unsigned long long (*affected_rows)(MYSQL *) = nullptr; unsigned long long (*insert_id)(MYSQL *) = nullptr;
	const char *(*error)(MYSQL *) = nullptr; unsigned int (*err_no)(MYSQL *) = nullptr; unsigned long (*escape)(MYSQL *, char *, const char *, unsigned long) = nullptr;
	int (*options)(MYSQL *, int, const void *) = nullptr; int (*set_charset)(MYSQL *, const char *) = nullptr;
};
MyApi g_my; std::once_flag g_my_once; std::string g_my_error;
template <class T> bool sym(T &dst, const char *n) { dst = (T)GETSYM(g_my.lib, n); return dst != nullptr; }
void load_my(const Settings &s) {
	std::vector<std::string> cands;
	if (!s.mysql_library.empty()) cands.push_back(s.mysql_library);
#ifdef _WIN32
	for (const char *n : {"libmariadb.dll", "libmysql.dll"}) cands.push_back(n);
#else
	for (const char *n : {"libmariadb.so.3", "libmysqlclient.so.21", "libmysqlclient.so.20", "libmysqlclient.so.18", "libmariadbclient.so.18", "libmysqlclient.so", "libmariadb.so"}) cands.push_back(n);
#endif
	for (auto &c : cands) { g_my.lib = LOADLIB(c.c_str()); if (g_my.lib) { g_my.path = c; break; } }
	if (!g_my.lib) { g_my_error = "no MySQL/MariaDB client library found (tried: "; for (size_t i = 0; i < cands.size(); i++) g_my_error += (i ? ", " : "") + cands[i]; g_my_error += "). Install the 32-bit client library or set mysql_library= in configs/query.ini"; return; }
	bool ok = sym(g_my.init, "mysql_init") && sym(g_my.real_connect, "mysql_real_connect") && sym(g_my.close, "mysql_close") && sym(g_my.real_query, "mysql_real_query") && sym(g_my.store_result, "mysql_store_result")
		&& sym(g_my.free_result, "mysql_free_result") && sym(g_my.num_fields, "mysql_num_fields") && sym(g_my.fetch_row, "mysql_fetch_row") && sym(g_my.fetch_lengths, "mysql_fetch_lengths")
		&& sym(g_my.fetch_field_direct, "mysql_fetch_field_direct") && sym(g_my.affected_rows, "mysql_affected_rows") && sym(g_my.insert_id, "mysql_insert_id") && sym(g_my.error, "mysql_error")
		&& sym(g_my.err_no, "mysql_errno") && sym(g_my.escape, "mysql_real_escape_string") && sym(g_my.options, "mysql_options");
	if (!ok) { g_my_error = "'" + g_my.path + "' is not a usable MySQL client library (missing symbols)"; g_my.lib = nullptr; return; }
	sym(g_my.library_init, "mysql_server_init"); sym(g_my.thread_init, "mysql_thread_init"); sym(g_my.set_charset, "mysql_set_character_set");
	if (g_my.library_init) g_my.library_init(0, nullptr, nullptr);
}
bool ensure_loaded(const Settings &s, std::string &why) { std::call_once(g_my_once, [&]() { load_my(s); }); if (!g_my.lib) { why = g_my_error; return false; } return true; }

// replace '?' placeholders (outside quotes / backticks / comments) by escaped literals
bool bind_params(MYSQL *conn, const std::string &sql, const std::vector<Cell> &params, std::string &out, std::string &err) {
	size_t pi = 0; char q = 0; out.clear();
	for (size_t i = 0; i < sql.size(); i++) {
		char c = sql[i];
		if (q) { out += c; if (c == '\\' && q != '`' && i + 1 < sql.size()) out += sql[++i]; else if (c == q) q = 0; continue; }
		if (c == '\'' || c == '"' || c == '`') { q = c; out += c; continue; }
		if (c == '#' || (c == '-' && i + 2 < sql.size() && sql[i + 1] == '-' && (sql[i + 2] == ' ' || sql[i + 2] == '\t' || sql[i + 2] == '\n'))) { while (i < sql.size() && sql[i] != '\n') out += sql[i++]; if (i < sql.size()) out += '\n'; continue; }
		if (c == '/' && i + 1 < sql.size() && sql[i + 1] == '*') { size_t e = sql.find("*/", i + 2); e = (e == std::string::npos) ? sql.size() : e + 2; out.append(sql, i, e - i); i = e - 1; continue; }
		if (c != '?') { out += c; continue; }
		if (pi >= params.size()) { err = "mysql: more '?' placeholders than parameters"; return false; }
		const Cell &p = params[pi++];
		switch (p.t) {
		case Cell::Null: out += "NULL"; break;
		case Cell::Int: out += std::to_string((long long)p.i); break;
		case Cell::Real: { char b[64]; snprintf(b, sizeof b, "%.17g", p.d); out += b; break; }
		case Cell::Text: { std::string esc(p.s.size() * 2 + 1, '\0'); unsigned long n = g_my.escape(conn, &esc[0], p.s.data(), (unsigned long)p.s.size()); out += '\''; out.append(esc, 0, n); out += '\''; break; }
		}
	}
	if (pi != params.size()) { err = "mysql: " + std::to_string(params.size()) + " parameters but only " + std::to_string(pi) + " '?' placeholders"; return false; }
	return true;
}

class MyConn : public DbConn {
	MYSQL *c_ = nullptr; DbProfile p_;
	std::string get(const char *k, const char *d) const { auto it = p_.kv.find(k); return it == p_.kv.end() ? d : it->second; }
	bool connect(std::string &err) {
		if (c_) { g_my.close(c_); c_ = nullptr; }
		c_ = g_my.init(nullptr); if (!c_) { err = "mysql: init failed"; return false; }
		unsigned int tmo = (unsigned int)atoi(get("timeout", "5").c_str()); g_my.options(c_, 0 /*MYSQL_OPT_CONNECT_TIMEOUT*/, &tmo);
		unsigned int rw = (unsigned int)atoi(get("read_timeout", "30").c_str()); g_my.options(c_, 11 /*READ_TIMEOUT*/, &rw); g_my.options(c_, 12 /*WRITE_TIMEOUT*/, &rw);
		if (!g_my.real_connect(c_, get("host", "127.0.0.1").c_str(), get("user", "").c_str(), get("pass", "").c_str(), get("database", "").c_str(), (unsigned int)atoi(get("port", "3306").c_str()), nullptr, 0)) {
			err = std::string("mysql: ") + g_my.error(c_); g_my.close(c_); c_ = nullptr; return false; }
		if (g_my.set_charset) g_my.set_charset(c_, get("charset", "utf8mb4").c_str());
		return true;
	}
	bool run(const std::string &sql, size_t max_rows, QResult &out, std::string &err) {
		if (g_my.real_query(c_, sql.data(), (unsigned long)sql.size()) != 0) { err = std::string("mysql: ") + g_my.error(c_); return false; }
		MYSQL_RES *res = g_my.store_result(c_);
		out = QResult();
		if (res) {
			unsigned int nf = g_my.num_fields(res); std::vector<int> types(nf);
			for (unsigned int i = 0; i < nf; i++) { MyField *f = g_my.fetch_field_direct(res, i); out.cols.push_back(f && f->name ? f->name : ""); types[i] = f ? f->type : 253; }
			while (MYSQL_ROW row = g_my.fetch_row(res)) {
				if (out.rows.size() >= max_rows) { out.truncated = true; break; }
				unsigned long *len = g_my.fetch_lengths(res); std::vector<Cell> r(nf);
				for (unsigned int i = 0; i < nf; i++) {
					if (!row[i]) continue;
					Cell &c = r[i]; std::string v(row[i], len ? len[i] : strlen(row[i])); int t = types[i];
					if (t == 1 || t == 2 || t == 3 || t == 8 || t == 9 || t == 13) { c.t = Cell::Int; c.i = strtoll(v.c_str(), nullptr, 10); }
					else if (t == 0 || t == 4 || t == 5 || t == 246) { c.t = Cell::Real; c.d = strtod(v.c_str(), nullptr); }
					else { c.t = Cell::Text; c.s = std::move(v); }
				}
				out.rows.push_back(std::move(r));
			}
			g_my.free_result(res);
		} else if (g_my.err_no(c_) != 0) { err = std::string("mysql: ") + g_my.error(c_); return false; }
		long long aff = (long long)g_my.affected_rows(c_); out.affected = aff < 0 ? 0 : aff; out.insert_id = (long long)g_my.insert_id(c_);
		return true;
	}
public:
	explicit MyConn(const DbProfile &p) : p_(p) {}
	~MyConn() override { if (c_) g_my.close(c_); }
	bool open(std::string &err) { return connect(err); }
	bool exec(const std::string &sql, const std::vector<Cell> &params, size_t max_rows, QResult &out, std::string &err) override {
		if (g_my.thread_init) g_my.thread_init();                       // idempotent; needed by libmysqlclient on foreign threads
		for (int attempt = 0; attempt < 2; attempt++) {
			if (!c_ && !connect(err)) return false;
			std::string final_sql;
			if (!bind_params(c_, sql, params, final_sql, err)) return false;
			if (run(final_sql, max_rows, out, err)) return true;
			unsigned int en = c_ ? g_my.err_no(c_) : 0;
			if (attempt == 0 && (en == 2006 || en == 2013 || en == 2055)) { err.clear(); g_my.close(c_); c_ = nullptr; continue; }   // server gone away: reconnect once
			return false;
		}
		return false;
	}
};
}
bool mysql_available(const Settings &s, std::string &why) { return ensure_loaded(s, why); }
DbConn *mysql_open(const DbProfile &p, const Settings &s, std::string &err) {
	if (!ensure_loaded(s, err)) return nullptr;
	if (g_my.thread_init) g_my.thread_init();
	MyConn *c = new MyConn(p); if (!c->open(err)) { delete c; return nullptr; }
	return c;
}
