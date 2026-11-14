// diag.cpp - readable script errors: where it happened (file, line, the code itself), what it means in plain words,
// "did you mean ...?" for misspelled xmod functions, and a check of every plugin's xmod.* names at load time.
#include "xmod.h"
#include <algorithm>
#include <cstdarg>
#include <cstring>
#include <map>
#include <set>

static std::string read_rel(const std::string &file) { return read_text(xpath(file.c_str())); }
static std::vector<std::string> split_lines(const std::string &s) {
	std::vector<std::string> v; size_t a = 0;
	while (a <= s.size()) { size_t b = s.find('\n', a); if (b == std::string::npos) b = s.size(); std::string l = s.substr(a, b - a); if (!l.empty() && l.back() == '\r') l.pop_back(); v.push_back(l); a = b + 1; }
	return v;
}
static int lev(const std::string &a, const std::string &b) {
	std::vector<int> d(b.size() + 1); for (size_t j = 0; j <= b.size(); j++) d[j] = (int)j;
	for (size_t i = 1; i <= a.size(); i++) { int prev = d[0]; d[0] = (int)i;
		for (size_t j = 1; j <= b.size(); j++) { int t = d[j]; d[j] = std::min({d[j] + 1, d[j - 1] + 1, prev + (tolower(a[i - 1]) == tolower(b[j - 1]) ? 0 : 1)}); prev = t; } }
	return d[b.size()];
}
// keys of the table at a dotted path ("xmod.msg") in the global environment; empty when it is not a table
static std::vector<std::string> keys_of(const std::string &dotted) {
	std::vector<std::string> out; int top = lua_gettop(L);
	lua_pushglobaltable(L); size_t a = 0;
	while (a < dotted.size()) { size_t b = dotted.find('.', a); if (b == std::string::npos) b = dotted.size();
		if (!lua_istable(L, -1)) { lua_settop(L, top); return out; }
		lua_getfield(L, -1, dotted.substr(a, b - a).c_str()); lua_remove(L, -2); a = b + 1; }
	if (lua_istable(L, -1)) { lua_pushnil(L); while (lua_next(L, -2)) { if (lua_type(L, -2) == LUA_TSTRING) out.push_back(lua_tostring(L, -2)); lua_pop(L, 1); } }
	lua_settop(L, top); std::sort(out.begin(), out.end()); return out;
}
static std::string closest(const std::string &name, const std::vector<std::string> &cands) {
	std::string best; int bd = 99;
	for (auto &c : cands) { int d = lev(name, c); if (d < bd || (d == bd && c.size() < best.size())) { bd = d; best = c; } }
	int limit = name.size() <= 4 ? 1 : name.size() <= 8 ? 2 : 3;
	return bd <= limit ? best : "";
}
static bool rx_name(const std::string &msg, const char *what, std::string &out) {        // "... (field 'name')"
	std::string key = std::string("(") + what + " '"; size_t p = msg.find(key); if (p == std::string::npos) return false;
	p += key.size(); size_t e = msg.find('\'', p); if (e == std::string::npos) return false; out = msg.substr(p, e - p); return true;
}
// the dotted expression in front of ".name" on a source line, e.g. "xmod.msg" for "xmod.msg.chatt("
static std::string prefix_of(const std::string &line, const std::string &name) {
	for (const char *sep : {".", ":"}) {
		size_t p = line.find(std::string(sep) + name);
		while (p != std::string::npos) {
			size_t a = p; while (a > 0 && (isalnum((unsigned char)line[a - 1]) || line[a - 1] == '_' || line[a - 1] == '.')) a--;
			if (a < p) return line.substr(a, p - a);
			p = line.find(std::string(sep) + name, p + 1);
		}
	}
	return "";
}

static std::string hint_for(const std::string &msg, const std::string &code) {
	std::string n;
	auto has = [&](const char *s) { return msg.find(s) != std::string::npos; };
	if (has("attempt to call a nil value")) {
		if (rx_name(msg, "field", n) || rx_name(msg, "method", n)) {
			std::string pre = prefix_of(code, n);
			if (!pre.empty()) {
				auto ks = keys_of(pre); std::string c = closest(n, ks);
				if (!c.empty()) return "'" + pre + "." + n + "' does not exist - did you mean " + pre + "." + c + "() ?";
				if (!ks.empty()) return "'" + pre + "' has no function '" + n + "' (see xmod_skills.md for the list)";
				if (pre.rfind("xmod.", 0) == 0) return "'" + pre + "' does not exist (an extension that is not loaded? a typo?)";
			}
			return "'" + n + "' is nil: that function does not exist (typo?) or was not defined yet";
		}
		if (rx_name(msg, "global", n)) {
			std::vector<std::string> g = keys_of(""); std::string c = closest(n, g);
			return "'" + n + "' is not defined. Typo?" + (c.empty() ? "" : " Did you mean '" + c + "'?") +
				" A local function must be written ABOVE the code that calls it (or declare it first: local " + n + ")";
		}
		if (rx_name(msg, "local", n) || rx_name(msg, "upvalue", n)) return "the variable '" + n + "' holds nil, not a function";
		return "you called something that is nil (a misspelled function name?)";
	}
	if (has("attempt to index a nil value")) {
		if (rx_name(msg, "field", n)) {
			std::string pre = prefix_of(code, n);
			if (!pre.empty()) { auto ks = keys_of(pre); std::string c = closest(n, ks); if (!c.empty()) return "'" + pre + "." + n + "' does not exist - did you mean " + pre + "." + c + " ?"; }
		}
		if (rx_name(msg, "global", n)) { std::string c = closest(n, keys_of("")); if (!c.empty()) return "'" + n + "' is not defined - did you mean '" + c + "'?"; }
		if (rx_name(msg, "field", n) || rx_name(msg, "global", n) || rx_name(msg, "local", n) || rx_name(msg, "upvalue", n))
			return "'" + n + "' is nil, so '" + n + ".something' / '" + n + "[...]' cannot be read. Check that it was set (and spelled the same everywhere)";
		return "you read x.y or x[...] but x is nil";
	}
	if (has("attempt to index a number value") || has("attempt to index a string value") || has("attempt to index a boolean value"))
		return "you used '.' or '[ ]' on a value that is not a table";
	if (has("attempt to perform arithmetic on a nil value")) return "a number was expected but the value is nil (not set yet?). Default it: (x or 0)";
	if (has("attempt to perform arithmetic on a string value")) return "maths on text: convert it first with tonumber(x)";
	if (has("attempt to concatenate a nil value")) return "you joined text with '..' and one part is nil. Use tostring(x) or (x or \"\")";
	if (has("attempt to concatenate a table value") || has("attempt to concatenate a boolean value")) return "'..' only joins text and numbers: use tostring(x)";
	if (has("attempt to compare")) return "comparing values of different types (for example number < nil, or number < string). Use tonumber() / check for nil first";
	if (has("bad argument #")) return "a function got the wrong kind of value for one of its arguments (the number is the argument's position)";
	if (has("stack overflow")) return "a function keeps calling itself without end (endless recursion)";
	if (has("script timeout") || has("took too long")) return "a loop ran too long and was stopped to protect the server (endless while/for loop?)";
	if (has("'end' expected")) return "a function / if / for / while block is missing its 'end' (the line in the message is where the block started)";
	if (has("'then' expected")) return "write: if <condition> then ... end";
	if (has("'do' expected")) return "write: for ... do ... end  /  while <condition> do ... end";
	if (has("unfinished string")) return "a string is missing its closing quote";
	if (has("'=' expected")) return "a statement is incomplete (a typo, or '==' / '=' mixed up?)";
	if (has("unexpected symbol")) return "something on this line is not valid Lua (a stray character, a missing operator or comma?)";
	if (has("'<eof>' expected")) return "an extra 'end' (or a missing 'function'/'if' before it)";
	if (has("'(' expected near")) return "function definitions need parentheses: function name(args) ... end";
	if (has("')' expected")) return "a parenthesis is not closed (check the line and the one before)";
	if (has("'}' expected")) return "a table { ... } is not closed or a comma is missing between its fields";
	if (has("attempt to assign to const variable")) return "that variable is <const> and cannot be changed";
	return "";
}

void report_error(Plugin *p, const char *what, const char *raw) {
	std::string msg = raw ? raw : "(no message)", head = msg, trace;
	size_t tb = msg.find("\nstack traceback:"); if (tb != std::string::npos) { head = msg.substr(0, tb); trace = msg.substr(tb + 17); }
	// first "file.lua:LINE:" in the message
	std::string file; int line = 0;
	{ size_t q = head.find(".lua:"); if (q != std::string::npos) { size_t a = head.rfind(' ', q); a = a == std::string::npos ? 0 : a + 1; while (a < q && (head[a] == '[' || head[a] == '"')) a++;
		file = head.substr(a, q + 4 - a); line = atoi(head.c_str() + q + 5); if (!file.empty() && file[0] == '@') file.erase(0, 1); } }
	std::string pname = p ? p->name : "core", pfile = p ? p->file : "";
	xerr("ERROR in %s \"%s\"%s%s%s - %s", (p && p->version == "lib") ? "library" : "plugin", pname.c_str(), pfile.empty() ? "" : " (", pfile.c_str(), pfile.empty() ? "" : ")", what);
	xerr("    %s", head.c_str());
	std::string code;
	if (!file.empty() && line > 0) {
		auto lines = split_lines(read_rel(file));
		if (line <= (int)lines.size()) {
			code = lines[line - 1];
			for (int k = std::max(1, line - 1); k <= std::min((int)lines.size(), line + 1); k++) {
				std::string t = lines[k - 1]; for (char &c : t) if (c == '\t') c = ' '; if (t.size() > 150) t = t.substr(0, 147) + "...";
				xerr("  %s %4d | %s", k == line ? ">>" : "  ", k, t.c_str());
			}
		}
	}
	std::string h = hint_for(head, code);
	if (!h.empty()) xerr("    hint: %s", h.c_str());
	if (!trace.empty()) {                                // compact call chain: only lines in plugin / library files
		std::string chain; size_t a = 0; int n = 0;
		while (a < trace.size() && n < 6) { size_t b = trace.find('\n', a); if (b == std::string::npos) b = trace.size(); std::string t = trace.substr(a, b - a); a = b + 1;
			size_t s = t.find_first_not_of(" \t"); if (s == std::string::npos) continue; t = t.substr(s);
			if (t.find(".lua:") == std::string::npos || t.rfind("[C]", 0) == 0) continue;
			size_t c = t.find(": in "); std::string where = c == std::string::npos ? t : t.substr(0, c), fn = c == std::string::npos ? "" : t.substr(c + 5);
			if (fn.find("function <") == 0) fn = "a function"; else if (fn.find("main chunk") != std::string::npos) fn = "the file itself";
			else for (const char *k : {"upvalue '", "local '", "field '", "method '", "global '", "function '"}) if (fn.rfind(k, 0) == 0) { fn = "function " + fn.substr(strlen(k) - 1); break; }
			chain += (n ? "  <-  " : "") + where + (fn.empty() ? "" : " (" + fn + ")"); n++; }
		if (n > 1) xerr("    called from: %s", chain.c_str());
	}
}

// ------------------------------------------------------------------------------------------------ static check of xmod.* names
// removes comments and string contents, so only code is scanned
static std::string code_only(const std::string &s) {
	std::string o; o.reserve(s.size()); size_t i = 0, n = s.size();
	auto long_bracket = [&](size_t at, size_t &level) -> bool { if (s[at] != '[') return false; size_t k = at + 1; level = 0; while (k < n && s[k] == '=') { level++; k++; } return k < n && s[k] == '['; };
	while (i < n) {
		size_t lv;
		if (s[i] == '-' && i + 1 < n && s[i + 1] == '-') {
			if (i + 2 < n && long_bracket(i + 2, lv)) { std::string close = "]" + std::string(lv, '=') + "]"; size_t e = s.find(close, i); e = e == std::string::npos ? n : e + close.size(); for (size_t k = i; k < e; k++) o += s[k] == '\n' ? '\n' : ' '; i = e; continue; }
			while (i < n && s[i] != '\n') { o += ' '; i++; } continue;
		}
		if (long_bracket(i, lv)) { std::string close = "]" + std::string(lv, '=') + "]"; size_t e = s.find(close, i); e = e == std::string::npos ? n : e + close.size(); for (size_t k = i; k < e; k++) o += s[k] == '\n' ? '\n' : ' '; i = e; continue; }
		if (s[i] == '"' || s[i] == '\'') { char q = s[i]; o += ' '; i++; while (i < n && s[i] != q && s[i] != '\n') { if (s[i] == '\\' && i + 1 < n) { o += ' '; i++; } o += ' '; i++; } if (i < n) { o += ' '; i++; } continue; }
		o += s[i++];
	}
	return o;
}
// warns about every xmod.a / xmod.a.b in the plugin's code that does not exist. Returns the number of warnings.
int diag_check_api(Plugin *p, bool quiet_ok) {
	std::string src = read_rel(p->file); if (src.empty()) return 0;
	std::string c = code_only(src); auto lines = split_lines(src);
	std::set<std::string> done; int warns = 0, line = 1;
	for (size_t i = 0; i + 5 < c.size(); i++) {
		if (c[i] == '\n') { line++; continue; }
		if (c.compare(i, 5, "xmod.") || (i > 0 && (isalnum((unsigned char)c[i - 1]) || c[i - 1] == '_' || c[i - 1] == '.' || c[i - 1] == ':'))) continue;
		size_t a = i + 5, b = a; while (b < c.size() && (isalnum((unsigned char)c[b]) || c[b] == '_' || c[b] == '.')) b++;
		std::string path = c.substr(a, b - a); while (!path.empty() && path.back() == '.') path.pop_back();
		i = b - 1; if (path.empty()) continue;
		std::vector<std::string> parts; size_t s = 0; while (s <= path.size()) { size_t e = path.find('.', s); if (e == std::string::npos) e = path.size(); parts.push_back(path.substr(s, e - s)); s = e + 1; }
		std::string cur = "xmod";
		for (size_t k = 0; k < parts.size() && k < 2; k++) {
			auto ks = keys_of(cur); if (ks.empty()) break;                         // not a table (a value / function): nothing to check below it
			if (std::find(ks.begin(), ks.end(), parts[k]) == ks.end()) {
				std::string full = cur + "." + parts[k]; if (done.count(full)) break; done.insert(full);
				std::string sug = closest(parts[k], ks);
				xerr("[%s] %s:%d: '%s' does not exist%s", p->name.c_str(), p->file.c_str(), line, full.c_str(), sug.empty() ? (cur == "xmod" ? " (an extension that is not loaded?)" : "") : (" - did you mean " + cur + "." + sug + " ?").c_str());
				warns++; break;
			}
			cur += "." + parts[k];
		}
	}
	if (!warns && !quiet_ok) xlog("check: %s - ok", p->file.c_str());
	return warns;
}
// console "xmod check": syntax of every plugin in plugins.ini (also the ones that failed to load) + the name check of the loaded ones
void diag_check_all() {
	int bad = 0, n = 0;
	for (Plugin *p : g_plugins) {
		n++;
		std::string src = read_rel(p->file);
		if (src.empty()) { xerr("check: %s - cannot read the file", p->file.c_str()); bad++; continue; }
		std::string chunk = "@" + p->file;
		if (luaL_loadbufferx(L, src.data(), src.size(), chunk.c_str(), "t") != LUA_OK) { report_error(p, "syntax check", lua_tostring(L, -1)); lua_pop(L, 1); bad++; continue; }
		lua_pop(L, 1);
		bad += diag_check_api(p, false) ? 1 : 0;
	}
	xlog("check: %d files, %d with problems", n, bad);
}
