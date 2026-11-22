#!/usr/bin/env python3
"""Local servers for the xmod_query tests: HTTP :18080, TCP line echo :18081, UDP echo :18082."""
import http.server, socketserver, threading, socket, time, json
class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def _send(self, code, body, ctype="text/plain", extra=None):
        b = body if isinstance(body, bytes) else body.encode()
        self.send_response(code); self.send_header("Content-Type", ctype); self.send_header("Content-Length", str(len(b)))
        for k, v in (extra or {}).items(): self.send_header(k, v)
        self.send_header("Connection", "close"); self.end_headers(); self.wfile.write(b)
    def do_GET(self):
        if self.path == "/hello": self._send(200, "hello world", extra={"X-Test": "yes"})
        elif self.path == "/big": self._send(200, "a" * 300000)
        elif self.path.startswith("/slow"): time.sleep(0.5); self._send(200, "slow done")
        elif self.path == "/redir": self._send(302, "", extra={"Location": "/hello"})
        elif self.path == "/headers": self._send(200, json.dumps({k.lower(): v for k, v in self.headers.items()}), "application/json")
        elif self.path == "/chunked":
            self.send_response(200); self.send_header("Transfer-Encoding", "chunked"); self.send_header("Connection", "close"); self.end_headers()
            for part in (b"chunk-one,", b"chunk-two"): self.wfile.write(b"%x\r\n%s\r\n" % (len(part), part))
            self.wfile.write(b"0\r\n\r\n")
        else: self._send(404, "nope")
    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0)); data = self.rfile.read(n)
        self._send(200, data, extra={"X-Echo-Len": str(n), "X-Method": self.command})
    do_PUT = do_POST
class TS(socketserver.ThreadingMixIn, http.server.HTTPServer): daemon_threads = True; allow_reuse_address = True
def tcp_echo():
    s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); s.bind(("127.0.0.1", 18081)); s.listen(16)
    def serve(c):
        buf = b""
        try:
            while True:
                d = c.recv(4096)
                if not d: break
                buf += d
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1); c.sendall(b"ECHO:" + line.upper() + b"\n")
        finally: c.close()
    while True:
        c, _ = s.accept(); threading.Thread(target=serve, args=(c,), daemon=True).start()
def udp_echo():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.bind(("127.0.0.1", 18082))
    while True:
        d, a = s.recvfrom(65535); s.sendto(b"PONG:" + d, a)
threading.Thread(target=tcp_echo, daemon=True).start(); threading.Thread(target=udp_echo, daemon=True).start()
srv = TS(("127.0.0.1", 18080), H); print("READY", flush=True); srv.serve_forever()
