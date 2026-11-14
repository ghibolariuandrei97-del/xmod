#!/bin/sh
# Native (64-bit) test of the xmod_query backends against a real MariaDB/MySQL, libcurl (incl. HTTPS certificate checks) and local servers.
# Needs: g++, python3, openssl, libcurl.so.4, libmariadb.so.3 (or libmysqlclient) and a server on 127.0.0.1:3306 prepared with:
#   CREATE DATABASE xmodtest; CREATE USER 'xmod'@'127.0.0.1' IDENTIFIED BY 'p@ss;word#1'; GRANT ALL ON xmodtest.* TO 'xmod'@'127.0.0.1';
cd "$(dirname "$0")"; Q=..
openssl req -x509 -newkey rsa:2048 -nodes -keyout /tmp/xq_key.pem -out /tmp/xq_cert.pem -days 2 -subj "/CN=127.0.0.1" -addext "subjectAltName=IP:127.0.0.1" >/dev/null 2>&1
cat > /tmp/xq_tls.py <<'PY'
import http.server, ssl
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        b = b"secure hello"; self.send_response(200); self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
s = http.server.HTTPServer(("127.0.0.1", 18443), H); c = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); c.load_cert_chain("/tmp/xq_cert.pem", "/tmp/xq_key.pem"); s.socket = c.wrap_socket(s.socket, server_side=True); s.serve_forever()
PY
python3 ../../../test/servers.py >/dev/null 2>&1 & P1=$!; python3 /tmp/xq_tls.py >/dev/null 2>&1 & P2=$!; sleep 1.5
gcc -O1 -c $Q/third_party/sqlite3.c -o /tmp/xq_sqlite3.o -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_THREADSAFE=1 || exit 1
g++ -std=gnu++17 -O1 -w -I$Q backend_test.cpp $Q/config.cpp $Q/net.cpp $Q/http.cpp $Q/db_sqlite.cpp $Q/db_mysql.cpp /tmp/xq_sqlite3.o -ldl -pthread -o /tmp/xq_backend_test || exit 1
/tmp/xq_backend_test; RC=$?; kill $P1 $P2 2>/dev/null; exit $RC
