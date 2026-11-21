xmod.register("query test", "1", "test")
local q = xmod.query                          -- bound at load time (namespace listed in extensions.ini); the extension attaches later
local Q = require("query")
local R = { pass = 0, fail = 0 }
local function check(name, cond, d) if cond then R.pass = R.pass + 1; xmod.log("PASS " .. name) else R.fail = R.fail + 1; xmod.log("FAIL " .. name .. "  " .. tostring(d or "")) end end
local frames = 0; xmod.on("frame", function() frames = frames + 1 end)
local function has(s, sub) return type(s) == "string" and s:find(sub, 1, true) ~= nil end

local function sql_tests()
  local H
  q.open("testdb", function(err, h)
    check("sqlite open (profile with inline comments in query.ini)", err == nil and math.type(h) == "integer", err); H = h
    q.exec(H, "DROP TABLE IF EXISTS t", function(e1)
    q.exec(H, "CREATE TABLE t(id INTEGER PRIMARY KEY, name TEXT, score REAL, note TEXT)", function(e2)
      check("DDL statements", e1 == nil and e2 == nil, tostring(e1) .. tostring(e2))
      q.exec(H, "INSERT INTO t(name, score, note) VALUES (?, ?, ?)", { "Ann", 1.5, "x" }, function(e3, r3)
        check("insert with params: affected + insert_id", e3 == nil and r3.affected == 1 and r3.insert_id == 1, e3)
        q.exec(H, "INSERT INTO t(name, score, note) VALUES (?, ?, ?)", { "Zo\u{eb} 'quoted'; DROP TABLE t;--", 2, Q.NULL }, function(e4)
          check("sql injection text is just data (bound parameter)", e4 == nil, e4)
          q.exec(H, "SELECT id, name, score, note FROM t ORDER BY id", function(e5, r5)
            check("select rows/columns", e5 == nil and #r5.rows == 2 and r5.columns[2] == "name" and r5.rows[1].name == "Ann" and r5.rows[1].score == 1.5 and r5.rows[1].id == 1, e5)
            check("NULL columns are omitted, floats stay floats, unicode round-trips", r5.rows[2].note == nil and math.type(r5.rows[2].score) == "float" and has(r5.rows[2].name, "Zo\u{eb}") and has(r5.rows[2].name, "DROP TABLE"))
            -- strand: 30 inserts + one count, fired without waiting; the count must see all of them because jobs of one handle run in order
            for i = 1, 30 do q.exec(H, "INSERT INTO t(name) VALUES (?)", { "bulk" .. i }, function() end) end
            q.exec(H, "SELECT count(*) AS n FROM t", function(e6, r6) check("strand: queued statements run in order (count sees all 30 inserts)", e6 == nil and r6.rows[1].n == 32, e6 or r6.rows[1].n) end)
            q.exec(H, "BEGIN", function() end); q.exec(H, "INSERT INTO t(name) VALUES ('rolled back')", function() end); q.exec(H, "ROLLBACK", function() end)
            q.exec(H, "SELECT count(*) AS n FROM t", function(e7, r7) check("transaction: BEGIN/INSERT/ROLLBACK keep their order on one handle", r7.rows[1].n == 32, r7.rows[1].n) end)
            q.exec(H, "SELEC nonsense", function(e8, r8) check("SQL error is reported as err", has(e8, "sqlite:") and r8 == nil, e8) end)
            q.exec(H, "SELECT ?, ?", { 1 }, function(e9) check("placeholder/parameter count mismatch is an error", has(e9, "placeholders"), e9) end)
            q.exec(H, "SELECT ?", function(e9b) check("placeholders without parameters is an error", has(e9b, "placeholders"), e9b) end)
            q.exec(H, "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c WHERE x<200) SELECT x FROM c", function(e10, r10)
              check("max_rows truncates and flags it", e10 == nil and #r10.rows == 50 and r10.truncated == true, e10 or #r10.rows) end)
            q.exec(H, "SELECT 1 AS a; SELECT 2 AS b", function(e11, r11) check("multi-statement script returns the last result set", e11 == nil and r11.rows[1].b == 2, e11) end)
            q.close(H, function(e12, ok12)
              check("close", e12 == nil and ok12 == true)
              q.exec(H, "SELECT 1", function(e13) check("exec on a closed handle fails clearly", has(e13, "closed"), e13) end)
            end)
          end)
        end)
      end)
    end) end)
  end)
  q.open("nosuchprofile", function(err) check("unknown profile is an error", has(err, "unknown database profile"), err) end)
  q.open("badfile", function(err) check("sqlite path traversal is refused", has(err, "relative") or has(err, "illegal"), err) end)
  q.open("mymysql", function(err) check("mysql profile: no client library / server down gives an actionable error", has(err, "client library") or has(err, "mysql:"), err) end)
  q.open("mem", function(e1, a) q.open("mem", function(e2, b)
    check("two in-memory handles", e1 == nil and e2 == nil and a ~= b)
    q.exec(a, "CREATE TABLE only_a(x)", function() q.exec(b, "SELECT * FROM only_a", function(e3) check("connections are independent", has(e3, "no such table"), e3) end) end)
    q.open("mem", function(e4, c) q.open("mem", function(e5) check("max_connections per profile is enforced", has(e5, "too many open connections"), e5) end) end)
  end) end)
  Q.with("testdb", function(db, done) q.exec(db, "SELECT 7 AS v", function(e, r) check("lib/query.lua: Q.with opens, runs and closes", e == nil and r.rows[1].v == 7, e); done() end) end)
end

local function http_tests()
  local B = "http://127.0.0.1:18080"
  q.http_get(B .. "/hello", function(err, r) check("http_get 200 + body + headers", err == nil and r.status == 200 and r.body == "hello world" and r.headers["x-test"] == "yes", err or r.status); xmod.log("INFO http backend: " .. tostring(r and r.backend)) end)
  q.http_get(B .. "/missing", function(err, r) check("http 404 is a normal result (status), not an error", err == nil and r.status == 404 and r.body == "nope", err) end)
  q.http_post(B .. "/echo", '{"a":1}', { headers = { ["Content-Type"] = "application/json" } }, function(err, r) check("http_post echoes the body", err == nil and r.body == '{"a":1}' and r.headers["x-echo-len"] == "7" and r.headers["x-method"] == "POST", err) end)
  q.http("PUT", B .. "/echo", { body = "put-body" }, function(err, r) check("http(method, url, opts{body})", err == nil and r.body == "put-body" and r.headers["x-method"] == "PUT", err) end)
  q.http_get(B .. "/headers", { headers = { ["X-Custom"] = "42" } }, function(err, r) check("custom request headers are sent", err == nil and has(r.body, '"x-custom": "42"'), err) end)
  q.http_get(B .. "/chunked", function(err, r) check("chunked transfer encoding", err == nil and r.body == "chunk-one,chunk-two", err) end)
  q.http_get(B .. "/redir", function(err, r) check("restricted http_allow: redirects are NOT followed (302 + Location returned)", err == nil and r.status == 302 and r.headers["location"] == "/hello", err or r.status) end)
  q.http_get(B .. "/big", function(err, r) check("response larger than max_response_bytes is truncated and flagged", err == nil and #r.body == 100000 and r.truncated == true, err or #r.body) end)
  q.http_get("http://example.com/", function(err) check("host outside http_allow is denied", has(err, "not allowed"), err) end)
  q.http_get("ftp://127.0.0.1/x", function(err) check("non-http scheme rejected", err ~= nil, err) end)
  q.http_get("http://user:pw@127.0.0.1:18080/hello", function(err) check("credentials in URL rejected", err ~= nil, err) end)
  q.http_get(B .. "/hello HTTP/1.1\r\nX-Evil: 1", function(err) check("request smuggling via URL (space/CRLF) rejected", err ~= nil, err) end)
  q.http_get(B .. "/hello", { headers = { ["X-Bad"] = "a\r\nInjected: 1" } }, function(err) check("header injection (CRLF) rejected", has(err, "illegal header"), err) end)
  q.http_get("http://127.0.0.1:18099/", function(err) check("connection refused is an error", err ~= nil, err) end)
  local f0 = frames
  q.http_get(B .. "/slow", { timeout = 0.2 }, function(err) check("timeout aborts a slow request", err ~= nil and (has(err, "timeout") or has(err, "timed out") or has(err, "Timeout")), err) end)
  q.http_get(B .. "/slow", function(err, r) check("slow request completes", err == nil and r.body == "slow done", err); check("game thread kept running during the 0.5 s HTTP request", frames - f0 > 60, frames - f0) end)
  local done8 = 0
  for i = 1, 8 do q.http_get(B .. "/slow?" .. i, function(err, r) if err == nil and r.body == "slow done" then done8 = done8 + 1 end end) end
  xmod.timer.after(8, function() check("8 parallel slow requests all completed", done8 == 8, done8); check("pool never exceeds xmod_async_threads", xmod.async.stats().threads <= 4, xmod.async.stats().threads) end)
end

local function socket_tests()
  q.tcp_request("127.0.0.1", 18081, "hello\n", { read_until = "\n" }, function(err, r) check("tcp_request line echo", err == nil and r.data == "ECHO:HELLO\n", err) end)
  q.tcp_request("127.0.0.1", 18081, "no newline", { timeout = 0.3 }, function(err, r) check("tcp_request: silent peer -> timed_out flag with empty data", err == nil and r.timed_out == true and r.data == "", err) end)
  q.tcp_request("127.0.0.1", 18090, "x", function(err) check("connection refused on an allowed port is an error", err ~= nil and not has(err, "not allowed"), err) end)
  q.tcp_request("127.0.0.1", 18099, "x", function(err) check("port outside socket_allow is denied", has(err, "not allowed"), err) end)
  q.tcp_open("127.0.0.1", 18081, function(err, h)
    check("tcp_open", err == nil and math.type(h) == "integer", err)
    q.tcp_send(h, "first\nsecond\n", function(e1, n) check("tcp_send returns bytes", e1 == nil and n == 13, e1) end)
    q.tcp_recv(h, { read_until = "ECHO:SECOND\n" }, function(e2, r) check("tcp_recv until terminator (persistent socket)", e2 == nil and r.data == "ECHO:FIRST\nECHO:SECOND\n", e2 or r.data) end)
    q.tcp_close(h, function(e3) check("tcp_close", e3 == nil)
      q.tcp_send(h, "x", function(e4) check("send on closed socket handle fails", has(e4, "closed"), e4) end) end)
  end)
  q.udp_request("127.0.0.1", 18082, "ping", function(err, r) check("udp_request", err == nil and r.data == "PONG:ping" and has(r.from, "127.0.0.1:18082"), err) end)
  q.resolve("localhost", function(err, ips) local found = false; for _, ip in ipairs(ips or {}) do if ip == "127.0.0.1" then found = true end end; check("resolve localhost", err == nil and found, err) end)
  q.resolve("no-such-host.invalid", function(err) check("resolve failure is an error", err ~= nil, err) end)
end

xmod.on("config", function()
  local st = q.status()
  check("status()", st.sqlite and st.version == "1.0" and type(st.profiles) == "table", st.sqlite)
  xmod.log("INFO status: sqlite " .. tostring(st.sqlite) .. ", mysql=" .. tostring(st.mysql) .. ", http=" .. tostring(st.http))
  local pr = q.profiles(); local leak = false; for _, p in ipairs(pr) do for k in pairs(p) do if k ~= "name" and k ~= "driver" then leak = true end end end
  check("profiles() lists names/drivers only (no credentials)", #pr >= 3 and not leak)
  sql_tests(); http_tests(); socket_tests()
  xmod.timer.after(11, function() xmod.log(("QUERY TEST DONE: %d passed, %d failed"):format(R.pass, R.fail)) end)
end)
