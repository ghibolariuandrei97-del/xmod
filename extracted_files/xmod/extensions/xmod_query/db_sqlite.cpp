// SQLite backend (bundled amalgamation). One DbConn = one sqlite3* ; xmod serialises all jobs of a handle (strand), the connection is also FULLMUTEX.
#include "backends.h"
#include "third_party/sqlite3.h"
#include <cstring>

namespace {
class SqliteConn : public DbConn {
	sqlite3 *db_;
public:
	explicit SqliteConn(sqlite3 *d) : db_(d) {}
	~SqliteConn() override { if (db_) sqlite3_close(db_); }
	bool exec(const std::string &sql, const std::vector<Cell> &params, size_t max_rows, QResult &out, std::string &err) override {
		out = QResult(); const char *tail = sql.c_str(); int stmts = 0;
		while (tail && *tail) {
			sqlite3_stmt *st = nullptr; const char *next = nullptr;
			int rc = sqlite3_prepare_v2(db_, tail, -1, &st, &next);
			if (rc != SQLITE_OK) { err = std::string("sqlite: ") + sqlite3_errmsg(db_); return false; }
			if (!st) { tail = next; continue; }                                  // whitespace / comment only
			stmts++;
			if (!params.empty()) {
				if (stmts > 1 || (next && std::string(next).find_first_not_of(" \t\r\n;") != std::string::npos)) { sqlite3_finalize(st); err = "sqlite: parameters are only allowed with exactly one statement"; return false; }
				int expected = sqlite3_bind_parameter_count(st);
				if (expected != (int)params.size()) { sqlite3_finalize(st); err = "sqlite: statement has " + std::to_string(expected) + " placeholders but " + std::to_string(params.size()) + " parameters were given"; return false; }
				for (size_t i = 0; i < params.size(); i++) {
					const Cell &c = params[i]; int idx = (int)i + 1;
					switch (c.t) {
					case Cell::Null: sqlite3_bind_null(st, idx); break;
					case Cell::Int: sqlite3_bind_int64(st, idx, c.i); break;
					case Cell::Real: sqlite3_bind_double(st, idx, c.d); break;
					case Cell::Text: sqlite3_bind_text(st, idx, c.s.data(), (int)c.s.size(), SQLITE_TRANSIENT); break;
					}
				}
			} else if (sqlite3_bind_parameter_count(st) > 0) { sqlite3_finalize(st); err = "sqlite: statement has placeholders but no parameters were given"; return false; }
			QResult cur; int ncol = sqlite3_column_count(st);
			for (int i = 0; i < ncol; i++) cur.cols.push_back(sqlite3_column_name(st, i) ? sqlite3_column_name(st, i) : "");
			for (;;) {
				rc = sqlite3_step(st);
				if (rc == SQLITE_ROW) {
					if (cur.rows.size() >= max_rows) { cur.truncated = true; continue; }          // keep stepping so the statement completes
					std::vector<Cell> row(ncol);
					for (int i = 0; i < ncol; i++) {
						Cell &c = row[i];
						switch (sqlite3_column_type(st, i)) {
						case SQLITE_INTEGER: c.t = Cell::Int; c.i = sqlite3_column_int64(st, i); break;
						case SQLITE_FLOAT: c.t = Cell::Real; c.d = sqlite3_column_double(st, i); break;
						case SQLITE_TEXT: case SQLITE_BLOB: { c.t = Cell::Text; const void *p = sqlite3_column_blob(st, i); int n = sqlite3_column_bytes(st, i); if (p && n > 0) c.s.assign((const char *)p, (size_t)n); break; }
						default: break;
						}
					}
					cur.rows.push_back(std::move(row));
				} else if (rc == SQLITE_DONE) break;
				else { err = std::string("sqlite: ") + sqlite3_errmsg(db_); sqlite3_finalize(st); return false; }
			}
			sqlite3_finalize(st);
			cur.affected = sqlite3_changes(db_); cur.insert_id = sqlite3_last_insert_rowid(db_);
			if (!cur.cols.empty() || stmts == 1) out = std::move(cur); else { out.affected = cur.affected; out.insert_id = cur.insert_id; }   // keep the last result set that returned columns
			tail = next;
		}
		return true;
	}
};
}
DbConn *sqlite_open(const DbProfile &p, const std::string &datadir, std::string &err) {
	auto it = p.kv.find("file"); std::string file = it == p.kv.end() ? "" : it->second;
	if (file.empty()) { err = "sqlite profile '" + p.name + "' has no file = ..."; return nullptr; }
	std::string path;
	if (file == ":memory:") path = file;
	else {
		for (char c : file) if (!(isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.' || c == '/')) { err = "sqlite profile '" + p.name + "': illegal character in file name"; return nullptr; }
		if (file.find("..") != std::string::npos || file[0] == '/') { err = "sqlite profile '" + p.name + "': file must be relative to addons/xmod/data (no '..')"; return nullptr; }
		path = datadir + file;
	}
	sqlite3 *db = nullptr;
	int rc = sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
	if (rc != SQLITE_OK) { err = std::string("sqlite: cannot open '") + file + "': " + (db ? sqlite3_errmsg(db) : "out of memory"); if (db) sqlite3_close(db); return nullptr; }
	sqlite3_busy_timeout(db, 5000);
	sqlite3_exec(db, "PRAGMA foreign_keys=ON;", nullptr, nullptr, nullptr);
	auto w = p.kv.find("wal"); if (path != ":memory:" && (w == p.kv.end() || w->second != "0")) sqlite3_exec(db, "PRAGMA journal_mode=WAL;PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
	return new SqliteConn(db);
}
