// xmod_query: backend interfaces. Nothing in here knows about xmod - the backends are plain C++ so they can be unit-tested on their own.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

struct Cell { enum Type { Null, Int, Real, Text } t = Null; int64_t i = 0; double d = 0; std::string s; };
struct QResult { std::vector<std::string> cols; std::vector<std::vector<Cell>> rows; int64_t affected = 0, insert_id = 0; bool truncated = false; };

struct DbProfile { std::string name, driver; std::map<std::string, std::string> kv; };
struct Settings {
	int max_rows = 10000; size_t max_response_bytes = 1 << 20; double default_timeout = 10, max_timeout = 60; int max_sockets = 16, max_connections = 8;
	std::vector<std::string> http_allow{"*"}, socket_allow{"*"}; std::string ca_file, mysql_library, curl_library; bool log_requests = false;
};
struct Config { Settings s; std::map<std::string, DbProfile> dbs; std::string datadir; };

bool load_config(const std::string &path, Config &cfg, std::string &err);
std::string lower(std::string s);
bool glob_match(const std::string &pattern, const std::string &text);
bool allowed(const std::vector<std::string> &list, const std::string &host, int port);   // patterns "host" or "host:port", '*' and '?' wildcards

class DbConn { public: virtual ~DbConn() {} virtual bool exec(const std::string &sql, const std::vector<Cell> &params, size_t max_rows, QResult &out, std::string &err) = 0; };
DbConn *sqlite_open(const DbProfile &p, const std::string &datadir, std::string &err);
DbConn *mysql_open(const DbProfile &p, const Settings &s, std::string &err);
bool mysql_available(const Settings &s, std::string &why);

struct HttpReq { std::string method = "GET", url, body; std::vector<std::pair<std::string, std::string>> headers; int timeout_ms = 10000; size_t max_bytes = 1 << 20; bool follow = true; std::string ca_file, curl_library; };
struct HttpResp { int status = 0; std::string body; std::vector<std::pair<std::string, std::string>> headers; bool truncated = false; std::string backend; };
bool http_perform(const HttpReq &req, HttpResp &resp, std::string &err);
bool parse_url(const std::string &url, std::string &scheme, std::string &host, int &port, std::string &path_query);
std::string http_backends_description(const Settings &s);

// ---- sockets (net.cpp). fd is an int on Linux / SOCKET cast to intptr_t on Windows.
typedef intptr_t NetFd;
const NetFd NET_BAD = -1;
bool net_init();
NetFd tcp_connect(const std::string &host, int port, int timeout_ms, std::string &err);
bool sock_send_all(NetFd fd, const char *data, size_t n, int timeout_ms, std::string &err);
int  sock_recv(NetFd fd, char *buf, size_t n, int timeout_ms);     // >0 bytes, 0 closed by peer, -1 error, -2 timeout
void sock_close(NetFd fd);
bool resolve_all(const std::string &host, std::vector<std::string> &ips, std::string &err);
bool udp_exchange(const std::string &host, int port, const std::string &data, int timeout_ms, std::string &reply, std::string &from, std::string &err);
long long now_ms();
