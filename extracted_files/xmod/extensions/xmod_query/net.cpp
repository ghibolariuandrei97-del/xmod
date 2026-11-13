// Portable blocking sockets with timeouts (used from worker threads only).
#include "backends.h"
#include <chrono>
#include <cstring>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t; static const sock_t BAD = INVALID_SOCKET;
static int last_err() { return WSAGetLastError(); }
#define WOULDBLOCK(e) ((e) == WSAEWOULDBLOCK)
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t; static const sock_t BAD = -1;
static int last_err() { return errno; }
#define WOULDBLOCK(e) ((e) == EINPROGRESS || (e) == EWOULDBLOCK || (e) == EAGAIN)
#endif

long long now_ms() { return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
bool net_init() {
#ifdef _WIN32
	static bool done = false, ok = false; if (!done) { done = true; WSADATA d; ok = WSAStartup(MAKEWORD(2, 2), &d) == 0; } return ok;
#else
	return true;
#endif
}
static void set_nonblock(sock_t s) {
#ifdef _WIN32
	u_long m = 1; ioctlsocket(s, FIONBIO, &m);
#else
	fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
}
static int wait_fd(sock_t s, bool for_write, int ms) {          // 1 ready, 0 timeout, -1 error
	if (ms < 0) ms = 0;
#ifdef _WIN32
	WSAPOLLFD p; p.fd = s; p.events = for_write ? POLLWRNORM : POLLRDNORM; p.revents = 0; int r = WSAPoll(&p, 1, ms);
#else
	struct pollfd p; p.fd = s; p.events = for_write ? POLLOUT : POLLIN; p.revents = 0; int r = poll(&p, 1, ms);
#endif
	if (r < 0) return -1;
	if (r == 0) return 0;
	return 1;
}
void sock_close(NetFd fd) {
	if (fd == NET_BAD) return;
#ifdef _WIN32
	closesocket((SOCKET)fd);
#else
	close((int)fd);
#endif
}
NetFd tcp_connect(const std::string &host, int port, int timeout_ms, std::string &err) {
	if (!net_init()) { err = "network init failed"; return NET_BAD; }
	struct addrinfo hints; memset(&hints, 0, sizeof hints); hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
	struct addrinfo *res = nullptr; std::string ps = std::to_string(port);
	if (getaddrinfo(host.c_str(), ps.c_str(), &hints, &res) != 0 || !res) { err = "cannot resolve host '" + host + "'"; return NET_BAD; }
	long long deadline = now_ms() + timeout_ms; std::string last = "connection failed";
	for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
		sock_t s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol); if (s == BAD) continue;
		set_nonblock(s);
		int rc = connect(s, ai->ai_addr, (int)ai->ai_addrlen);
		if (rc != 0 && !WOULDBLOCK(last_err())) { last = "connection refused or unreachable"; sock_close((NetFd)s); continue; }
		if (rc != 0) {
			int w = wait_fd(s, true, (int)(deadline - now_ms()));
			if (w <= 0) { last = w == 0 ? "connect timed out" : "connect failed"; sock_close((NetFd)s); continue; }
			int soerr = 0;
#ifdef _WIN32
			int len = sizeof soerr; getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&soerr, &len);
#else
			socklen_t len = sizeof soerr; getsockopt(s, SOL_SOCKET, SO_ERROR, &soerr, &len);
#endif
			if (soerr != 0) { last = "connection refused or unreachable"; sock_close((NetFd)s); continue; }
		}
		int one = 1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
		freeaddrinfo(res); return (NetFd)s;
	}
	freeaddrinfo(res); err = last; return NET_BAD;
}
bool sock_send_all(NetFd fd, const char *data, size_t n, int timeout_ms, std::string &err) {
	long long deadline = now_ms() + timeout_ms; size_t sent = 0;
	while (sent < n) {
		int w = wait_fd((sock_t)fd, true, (int)(deadline - now_ms()));
		if (w == 0) { err = "send timed out"; return false; }
		if (w < 0) { err = "send failed"; return false; }
#ifdef MSG_NOSIGNAL
		int r = (int)send((sock_t)fd, data + sent, (int)(n - sent), MSG_NOSIGNAL);
#else
		int r = (int)send((sock_t)fd, data + sent, (int)(n - sent), 0);
#endif
		if (r < 0) { if (WOULDBLOCK(last_err())) continue; err = "send failed (connection closed?)"; return false; }
		sent += (size_t)r;
	}
	return true;
}
int sock_recv(NetFd fd, char *buf, size_t n, int timeout_ms) {
	int w = wait_fd((sock_t)fd, false, timeout_ms);
	if (w == 0) return -2;
	if (w < 0) return -1;
	int r = (int)recv((sock_t)fd, buf, (int)n, 0);
	if (r < 0) return WOULDBLOCK(last_err()) ? -2 : -1;
	return r;
}
bool resolve_all(const std::string &host, std::vector<std::string> &ips, std::string &err) {
	if (!net_init()) { err = "network init failed"; return false; }
	struct addrinfo hints; memset(&hints, 0, sizeof hints); hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
	struct addrinfo *res = nullptr; if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) { err = "cannot resolve host '" + host + "'"; return false; }
	for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
		char b[64] = {0};
		if (ai->ai_family == AF_INET) inet_ntop(AF_INET, &((struct sockaddr_in *)ai->ai_addr)->sin_addr, b, sizeof b);
		else if (ai->ai_family == AF_INET6) inet_ntop(AF_INET6, &((struct sockaddr_in6 *)ai->ai_addr)->sin6_addr, b, sizeof b);
		else continue;
		std::string s = b; bool dup = false; for (auto &x : ips) if (x == s) dup = true;
		if (!dup) ips.push_back(s);
	}
	freeaddrinfo(res); return !ips.empty();
}
bool udp_exchange(const std::string &host, int port, const std::string &data, int timeout_ms, std::string &reply, std::string &from, std::string &err) {
	if (!net_init()) { err = "network init failed"; return false; }
	struct addrinfo hints; memset(&hints, 0, sizeof hints); hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_DGRAM;
	struct addrinfo *res = nullptr; std::string ps = std::to_string(port);
	if (getaddrinfo(host.c_str(), ps.c_str(), &hints, &res) != 0 || !res) { err = "cannot resolve host '" + host + "'"; return false; }
	sock_t s = socket(res->ai_family, SOCK_DGRAM, 0); if (s == BAD) { freeaddrinfo(res); err = "cannot create socket"; return false; }
	set_nonblock(s);
	if (sendto(s, data.data(), (int)data.size(), 0, res->ai_addr, (int)res->ai_addrlen) < 0) { err = "sendto failed"; sock_close((NetFd)s); freeaddrinfo(res); return false; }
	int w = wait_fd(s, false, timeout_ms);
	if (w <= 0) { err = w == 0 ? "timeout waiting for a UDP reply" : "receive failed"; sock_close((NetFd)s); freeaddrinfo(res); return false; }
	static thread_local char buf[65536]; struct sockaddr_storage ss;
#ifdef _WIN32
	int sl = sizeof ss;
#else
	socklen_t sl = sizeof ss;
#endif
	int r = (int)recvfrom(s, buf, sizeof buf, 0, (struct sockaddr *)&ss, &sl);
	if (r < 0) { err = "receive failed"; sock_close((NetFd)s); freeaddrinfo(res); return false; }
	reply.assign(buf, (size_t)r);
	char ab[64] = {0}; int fport = 0;
	if (ss.ss_family == AF_INET) { inet_ntop(AF_INET, &((struct sockaddr_in *)&ss)->sin_addr, ab, sizeof ab); fport = ntohs(((struct sockaddr_in *)&ss)->sin_port); }
	else if (ss.ss_family == AF_INET6) { inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&ss)->sin6_addr, ab, sizeof ab); fport = ntohs(((struct sockaddr_in6 *)&ss)->sin6_port); }
	from = std::string(ab) + ":" + std::to_string(fport);
	sock_close((NetFd)s); freeaddrinfo(res); return true;
}
