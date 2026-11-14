// Backend tests for xmod_query (no xmod needed). Built natively (64-bit) and run against a local MariaDB/MySQL, libcurl and local HTTP(S) servers:
//   see tests/run_backend_tests.sh
#include "../backends.h"
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>
#include <atomic>
static int pass = 0, fail = 0;
#define CHECK(name, cond) do { if (cond) { pass++; printf("PASS %s\n", name); } else { fail++; printf("FAIL %s  (line %d)\n", name, __LINE__); } } while (0)
static Cell I(long long v) { Cell c; c.t = Cell::Int; c.i = v; return c; }
static Cell R(double v) { Cell c; c.t = Cell::Real; c.d = v; return c; }
static Cell T(const std::string &v) { Cell c; c.t = Cell::Text; c.s = v; return c; }
static Cell N() { return Cell(); }

int main() {
	std::string mysql_pass = "p@ss;word#1", err;
	// ---------------------------------------------------------------- config + allow lists
	CHECK("glob: * and ?", glob_match("*.example.com", "api.example.com") && !glob_match("*.example.com", "example.org") && glob_match("10.0.?.1", "10.0.5.1"));
	std::vector<std::string> al{"127.0.0.1", "*.example.com:443", "db.local:3306"};
	CHECK("allowed: host-only pattern matches any port", allowed(al, "127.0.0.1", 8080));
	CHECK("allowed: host:port pattern is exact on the port", allowed(al, "api.example.com", 443) && !allowed(al, "api.example.com", 80));
	CHECK("allowed: unknown host denied, empty list denies all", !allowed(al, "evil.com", 80) && !allowed({}, "127.0.0.1", 80));
	{ FILE *f = fopen("/tmp/xq_test.ini", "w"); fprintf(f, "; c\n[settings]\nmax_rows = 7   ; inline comment\nhttp_allow = a.com, b.com:8080\n[db:Main]\ndriver = MySQL\npass = p@ss;word#1\nuser=u # another comment\nfile = x.db\t; tab comment\n"); fclose(f); }
	Config cfg; CHECK("load_config", load_config("/tmp/xq_test.ini", cfg, err));
	CHECK("config: inline ' ; ' comments stripped, ';' and '#' inside a value kept, keys case-insensitive",
		cfg.s.max_rows == 7 && cfg.s.http_allow.size() == 2 && cfg.dbs["main"].driver == "mysql" && cfg.dbs["main"].kv["pass"] == "p@ss;word#1" && cfg.dbs["main"].kv["user"] == "u" && cfg.dbs["main"].kv["file"] == "x.db");
	{ FILE *f = fopen("/tmp/xq_bad.ini", "w"); fprintf(f, "[db:x]\ndriver = oracle\n"); fclose(f); Config c2; CHECK("bad driver rejected", !load_config("/tmp/xq_bad.ini", c2, err)); }
	{ std::string s, h, pq; int p; CHECK("parse_url", parse_url("https://Example.com:8443/a/b?x=1#frag", s, h, p, pq) && s == "https" && h == "Example.com" && p == 8443 && pq == "/a/b?x=1");
	  CHECK("parse_url rejects credentials, spaces, other schemes", !parse_url("http://u:p@h/", s, h, p, pq) && !parse_url("http://h/a b", s, h, p, pq) && !parse_url("file:///etc/passwd", s, h, p, pq)); }

	// ---------------------------------------------------------------- MySQL / MariaDB
	Settings st; std::string why; bool have = mysql_available(st, why);
	CHECK("mysql client library loads at run time", have);
	if (have) {
		DbProfile p; p.name = "t"; p.driver = "mysql"; p.kv = {{"host", "127.0.0.1"}, {"port", "3306"}, {"user", "xmod"}, {"pass", mysql_pass}, {"database", "xmodtest"}};
		DbConn *c = mysql_open(p, st, err); CHECK("mysql open (password with ';' '#' '@')", c != nullptr); if (!c) { printf("  %s\n", err.c_str()); return 1; }
		QResult r; std::vector<Cell> none;
		CHECK("drop+create table", c->exec("DROP TABLE IF EXISTS t", none, 100, r, err) && c->exec("CREATE TABLE t(id INT AUTO_INCREMENT PRIMARY KEY, name VARCHAR(200), score DOUBLE, big BIGINT, price DECIMAL(10,2), d DATE, note TEXT) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4", none, 100, r, err));
		std::string nasty = "O'Brien \"x\" \\ ; DROP TABLE t; -- \xF0\x9F\x98\x80 \xC3\xA9";
		CHECK("insert with params (quotes, backslash, injection text, emoji)", c->exec("INSERT INTO t(name, score, big, price, d, note) VALUES (?, ?, ?, ?, ?, ?)", {T(nasty), R(1.5), I(123456789012LL), T("12.34"), T("2024-01-02"), N()}, 100, r, err) && r.affected == 1 && r.insert_id == 1);
		CHECK("select: one row back", c->exec("SELECT * FROM t", none, 100, r, err) && r.rows.size() == 1);
		if (!r.rows.empty()) { auto &row = r.rows[0];
			CHECK("INT -> Int, DOUBLE -> Real, BIGINT -> Int, DECIMAL -> Real", row[0].t == Cell::Int && row[0].i == 1 && row[2].t == Cell::Real && row[2].d == 1.5 && row[3].t == Cell::Int && row[3].i == 123456789012LL && row[4].t == Cell::Real && row[4].d == 12.34);
			CHECK("text round-trips exactly (escaping is correct)", row[1].t == Cell::Text && row[1].s == nasty);
			CHECK("DATE -> text, NULL -> Null", row[5].t == Cell::Text && row[5].s == "2024-01-02" && row[6].t == Cell::Null); }
		CHECK("? inside a string literal / comment is not a placeholder", c->exec("SELECT 'what?' AS q, ? AS p /* ? */", {I(5)}, 100, r, err) && r.rows[0][0].s == "what?" && r.rows[0][1].i == 5);
		CHECK("a--b is arithmetic, not a comment", c->exec("SELECT 5--3 AS v, ? AS p", {I(1)}, 100, r, err) && r.rows[0][0].i == 8);
		CHECK("placeholder count mismatch (too few)", !c->exec("SELECT ?, ?", {I(1)}, 100, r, err) && err.find("placeholders") != std::string::npos);
		CHECK("placeholder count mismatch (too many)", !c->exec("SELECT 1", {I(1)}, 100, r, err) && err.find("parameters") != std::string::npos);
		CHECK("placeholder without parameters is an error (never sent to the server)", !c->exec("SELECT ?", none, 100, r, err) && err.find("placeholders") != std::string::npos);
		CHECK("syntax error is reported", !c->exec("SELEC 1", none, 100, r, err) && err.find("mysql:") != std::string::npos);
		c->exec("DROP TABLE IF EXISTS nums", none, 100, r, err); c->exec("CREATE TABLE nums(x INT)", none, 100, r, err);
		c->exec("INSERT INTO nums VALUES (1),(2),(3),(4),(5),(6),(7),(8),(9),(10)", none, 100, r, err);
		CHECK("max_rows truncation", c->exec("SELECT x FROM nums", none, 4, r, err) && r.rows.size() == 4 && r.truncated);
		CHECK("affected rows for UPDATE", c->exec("UPDATE nums SET x = x + 1 WHERE x <= 3", none, 100, r, err) && r.affected == 3);
		DbConn *c2 = mysql_open(p, st, err); QResult idr; c->exec("SELECT CONNECTION_ID() AS id", none, 10, idr, err); long long id = idr.rows[0][0].i;
		CHECK("KILL the first connection from the second", c2 && c2->exec("KILL " + std::to_string(id), none, 10, r, err));
		CHECK("server-gone-away is repaired by one transparent reconnect", c->exec("SELECT 42 AS v", none, 10, r, err) && r.rows[0][0].i == 42);
		delete c2;
		DbProfile bad = p; bad.kv["pass"] = "wrong"; DbConn *bc = mysql_open(bad, st, err); CHECK("wrong password -> clear error", bc == nullptr && err.find("mysql:") != std::string::npos);
		DbProfile down = p; down.kv["port"] = "3399"; down.kv["timeout"] = "2"; DbConn *dc = mysql_open(down, st, err); CHECK("server down -> error", dc == nullptr && !err.empty());
		c->exec("DROP TABLE IF EXISTS thr", none, 10, r, err); c->exec("CREATE TABLE thr(w INT, i INT)", none, 10, r, err);
		std::atomic<int> errors{0}; std::vector<std::thread> ts;
		for (int w = 0; w < 4; w++) ts.emplace_back([&, w]() { std::string e; DbConn *tc = mysql_open(p, st, e); if (!tc) { errors += 1000; return; } QResult q;
			for (int i = 0; i < 150; i++) if (!tc->exec("INSERT INTO thr VALUES (?, ?)", {I(w), I(i)}, 10, q, e)) errors++; delete tc; });
		for (auto &t : ts) t.join();
		CHECK("4 threads x 150 inserts on 4 connections", errors == 0 && c->exec("SELECT count(*) FROM thr", none, 10, r, err) && r.rows[0][0].i == 600);
		c->exec("DROP TABLE t", none, 10, r, err); c->exec("DROP TABLE nums", none, 10, r, err); c->exec("DROP TABLE thr", none, 10, r, err); delete c;
	}

	// ---------------------------------------------------------------- libcurl HTTP(S)   (test/servers.py on :18080, TLS server on :18443)
	{ HttpReq q; q.url = "http://127.0.0.1:18080/hello"; HttpResp h; CHECK("curl GET", http_perform(q, h, err) && h.status == 200 && h.body == "hello world" && h.backend == "curl"); }
	{ HttpReq a; a.url = "http://127.0.0.1:18080/redir"; HttpResp b; CHECK("curl follows redirects", http_perform(a, b, err) && b.status == 200 && b.body == "hello world");
	  a.follow = false; HttpResp b2; CHECK("follow=false returns 302", http_perform(a, b2, err) && b2.status == 302); }
	{ HttpReq a; a.url = "http://127.0.0.1:18080/chunked"; HttpResp b; CHECK("curl chunked", http_perform(a, b, err) && b.body == "chunk-one,chunk-two"); }
	{ HttpReq a; a.url = "http://127.0.0.1:18080/echo"; a.method = "POST"; a.body = std::string("bin\0data", 8); a.headers = {{"Content-Type", "application/octet-stream"}}; HttpResp b;
	  CHECK("curl POST binary body", http_perform(a, b, err) && b.body == a.body); a.method = "PUT"; HttpResp b2; CHECK("curl PUT", http_perform(a, b2, err) && b2.body == a.body);
	  a.body.clear(); a.method = "POST"; HttpResp b3; CHECK("curl POST with an empty body", http_perform(a, b3, err) && b3.status == 200 && b3.body.empty()); }
	{ HttpReq a; a.url = "http://127.0.0.1:18080/big"; a.max_bytes = 100000; HttpResp b; CHECK("curl truncation at max_bytes", http_perform(a, b, err) && b.body.size() == 100000 && b.truncated); }
	{ HttpReq a; a.url = "http://127.0.0.1:18080/slow"; a.timeout_ms = 200; HttpResp b; long long t0 = now_ms(); bool ok = http_perform(a, b, err); CHECK("curl timeout (fast failure)", !ok && now_ms() - t0 < 1500); }
	{ HttpReq a; a.url = "http://127.0.0.1:18080/headers"; a.headers = {{"X-Custom", "42"}}; HttpResp b; CHECK("curl custom headers", http_perform(a, b, err) && b.body.find("\"x-custom\": \"42\"") != std::string::npos); }
	{ HttpReq a; a.url = "file:///etc/passwd"; HttpResp b; CHECK("curl never reads file:// URLs", !http_perform(a, b, err)); }
	{ HttpReq a; a.url = "https://127.0.0.1:18443/hello"; HttpResp b; CHECK("HTTPS with an untrusted certificate is REJECTED (verification on)", !http_perform(a, b, err) && !err.empty()); printf("  (%s)\n", err.c_str());
	  a.ca_file = "/tmp/xq_cert.pem"; HttpResp b2; CHECK("HTTPS succeeds when ca_file trusts the certificate", http_perform(a, b2, err) && b2.status == 200 && b2.body == "secure hello"); }
	printf("\nBACKEND TESTS: %d passed, %d failed\n", pass, fail); return fail ? 1 : 0;
}
