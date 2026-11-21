#include "backends.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

std::string lower(std::string s) { for (auto &c : s) c = (char)tolower((unsigned char)c); return s; }
static std::string trim(const std::string &s) { size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n"); return a == std::string::npos ? "" : s.substr(a, b - a + 1); }
// value comments: " ;" or " #" (preceded by whitespace) starts a comment; a ';' or '#' glued to text is part of the value (passwords!)
static std::string strip_value_comment(const std::string &v) {
	for (size_t i = 1; i < v.size(); i++) if ((v[i] == ';' || v[i] == '#') && (v[i - 1] == ' ' || v[i - 1] == '\t') && (i + 1 >= v.size() || v[i + 1] == ' ' || v[i + 1] == '\t')) return trim(v.substr(0, i));
	return v;
}
bool glob_match(const std::string &p, const std::string &t) {
	size_t pi = 0, ti = 0, star = std::string::npos, mark = 0;
	while (ti < t.size()) {
		if (pi < p.size() && (p[pi] == '?' || p[pi] == t[ti])) { pi++; ti++; }
		else if (pi < p.size() && p[pi] == '*') { star = pi++; mark = ti; }
		else if (star != std::string::npos) { pi = star + 1; ti = ++mark; }
		else return false;
	}
	while (pi < p.size() && p[pi] == '*') pi++;
	return pi == p.size();
}
bool allowed(const std::vector<std::string> &list, const std::string &host, int port) {
	std::string h = lower(host), hp = h + ":" + std::to_string(port);
	for (const std::string &e : list) { std::string pat = lower(e); if (pat.empty()) continue; if (glob_match(pat, hp)) return true; if (pat.find(':') == std::string::npos && glob_match(pat, h)) return true; }
	return false;
}
static std::vector<std::string> split_list(const std::string &v) {
	std::vector<std::string> out; size_t p = 0;
	while (p <= v.size()) { size_t c = v.find(',', p); if (c == std::string::npos) c = v.size(); std::string t = trim(v.substr(p, c - p)); if (!t.empty()) out.push_back(t); p = c + 1; }
	return out;
}
bool load_config(const std::string &path, Config &cfg, std::string &err) {
	FILE *f = fopen(path.c_str(), "rb"); if (!f) { err = "cannot read " + path; return false; }
	std::string text; char buf[1024]; size_t n; while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n); fclose(f);
	std::string section = ""; size_t pos = 0; int lineno = 0;
	while (pos <= text.size()) {
		size_t nl = text.find('\n', pos); if (nl == std::string::npos) nl = text.size();
		std::string line = trim(text.substr(pos, nl - pos)); pos = nl + 1; lineno++;
		if (line.empty() || line[0] == ';' || line[0] == '#') { if (nl >= text.size()) break; continue; }
		if (line[0] == '[') {
			size_t e = line.find(']'); if (e == std::string::npos) { err = "query.ini line " + std::to_string(lineno) + ": missing ]"; return false; }
			section = lower(trim(line.substr(1, e - 1)));
			if (section.rfind("db:", 0) == 0) { std::string name = section.substr(3); if (name.empty()) { err = "query.ini: empty db profile name"; return false; } cfg.dbs[name].name = name; }
		} else {
			size_t eq = line.find('='); if (eq == std::string::npos) { err = "query.ini line " + std::to_string(lineno) + ": expected key = value"; return false; }
			std::string k = lower(trim(line.substr(0, eq))), v = strip_value_comment(trim(line.substr(eq + 1)));
			if (section == "settings") {
				Settings &s = cfg.s;
				if (k == "max_rows") s.max_rows = std::max(1, atoi(v.c_str()));
				else if (k == "max_response_bytes") s.max_response_bytes = (size_t)std::max(1024, atoi(v.c_str()));
				else if (k == "default_timeout") s.default_timeout = atof(v.c_str());
				else if (k == "max_timeout") s.max_timeout = atof(v.c_str());
				else if (k == "max_sockets") s.max_sockets = std::max(1, atoi(v.c_str()));
				else if (k == "max_connections") s.max_connections = std::max(1, atoi(v.c_str()));
				else if (k == "http_allow") s.http_allow = split_list(v);
				else if (k == "socket_allow") s.socket_allow = split_list(v);
				else if (k == "ca_file") s.ca_file = v;
				else if (k == "mysql_library") s.mysql_library = v;
				else if (k == "curl_library") s.curl_library = v;
				else if (k == "log_requests") s.log_requests = (v == "1" || lower(v) == "true");
			} else if (section.rfind("db:", 0) == 0) {
				DbProfile &p = cfg.dbs[section.substr(3)];
				if (k == "driver") p.driver = lower(v); else p.kv[k] = v;
			}
		}
		if (nl >= text.size()) break;
	}
	for (auto &kv : cfg.dbs) if (kv.second.driver != "sqlite" && kv.second.driver != "mysql") { err = "query.ini [db:" + kv.first + "]: driver must be sqlite or mysql"; return false; }
	if (cfg.s.default_timeout <= 0) cfg.s.default_timeout = 10;
	if (cfg.s.max_timeout < cfg.s.default_timeout) cfg.s.max_timeout = cfg.s.default_timeout;
	return true;
}
