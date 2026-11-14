// vault.cpp - xmod.vault: persistent key/value storage, the nVault + nVault-array replacement.
//   Values keep their type (number, string, boolean, nested tables / arrays), every entry has a timestamp (prune old data),
//   files are written atomically (temp file + backup) and checksummed, so a crash during a save never loses the vault.
//   File: addons/xmod/data/vaults/<name>.vault       Saved on map end, shutdown, v:save(), and every 2 minutes when changed.
#include "xmod.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <new>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#endif
#ifdef _WIN32
#include <direct.h>
#define mkdir_p(p) _mkdir(p)
#else
#include <sys/stat.h>
#define mkdir_p(p) mkdir(p, 0755)
#endif

namespace {
struct Entry { std::string blob; uint32_t ts = 0; };
struct Vault { std::map<std::string, Entry> d; bool dirty = false, loaded = false; };
std::map<std::string, Vault> g_v;
double g_last_auto = 0;
const char *MT = "xmod.vault";
const size_t MAX_KEY = 255, MAX_BLOB = 1 << 20;

uint32_t now32() { return (uint32_t)time(nullptr); }
uint32_t fnv(const std::string &s) { uint32_t h = 2166136261u; for (unsigned char c : s) { h ^= c; h *= 16777619u; } return h; }
void put32(std::string &b, uint32_t v) { for (int i = 0; i < 4; i++) b += (char)((v >> (8 * i)) & 255); }
void put16(std::string &b, uint32_t v) { b += (char)(v & 255); b += (char)((v >> 8) & 255); }
bool get32(const std::string &b, size_t &p, uint32_t &v) { if (p + 4 > b.size()) return false; v = 0; for (int i = 0; i < 4; i++) v |= (uint32_t)(unsigned char)b[p + i] << (8 * i); p += 4; return true; }
bool get16(const std::string &b, size_t &p, uint32_t &v) { if (p + 2 > b.size()) return false; v = (unsigned char)b[p] | ((unsigned char)b[p + 1] << 8); p += 2; return true; }

std::string dir() { return xpath("data/vaults"); }
std::string file_of(const std::string &n) { return dir() + "/" + n + ".vault"; }
bool good_name(const char *n) { size_t k = strlen(n); if (!k || k > 48) return false; for (; *n; n++) if (!(isalnum((unsigned char)*n) || *n == '_' || *n == '-')) return false; return true; }

// ---------------------------------------------------------------- value <-> bytes
bool ser(lua_State *l, int idx, std::string &o, int depth, std::string &err) {
	idx = lua_absindex(l, idx);
	switch (lua_type(l, idx)) {
		case LUA_TBOOLEAN: o += 'b'; o += (char)(lua_toboolean(l, idx) ? 1 : 0); return true;
		case LUA_TNUMBER:
			if (lua_isinteger(l, idx)) { int64_t v = lua_tointeger(l, idx); o += 'i'; for (int i = 0; i < 8; i++) o += (char)((v >> (8 * i)) & 255); }
			else { double d = lua_tonumber(l, idx); uint64_t u; memcpy(&u, &d, 8); o += 'd'; for (int i = 0; i < 8; i++) o += (char)((u >> (8 * i)) & 255); }
			return true;
		case LUA_TSTRING: { size_t n; const char *s = lua_tolstring(l, idx, &n); o += 's'; put32(o, (uint32_t)n); o.append(s, n); return true; }
		case LUA_TTABLE: {
			if (depth >= 16) { err = "tables nested more than 16 levels deep (or a table that contains itself)"; return false; }
			luaL_checkstack(l, 6, "vault: table too deep");                 // every level keeps key + value on the C stack
			o += 't'; size_t at = o.size(); put32(o, 0); uint32_t n = 0;
			lua_pushnil(l);
			while (lua_next(l, idx)) {
				int kt = lua_type(l, -2);
				if (kt != LUA_TSTRING && kt != LUA_TNUMBER && kt != LUA_TBOOLEAN) { lua_pop(l, 2); err = "table keys must be strings, numbers or booleans"; return false; }
				if (!ser(l, -2, o, depth + 1, err) || !ser(l, -1, o, depth + 1, err)) { lua_pop(l, 2); return false; }
				lua_pop(l, 1); n++;
			}
			for (int i = 0; i < 4; i++) o[at + i] = (char)((n >> (8 * i)) & 255);
			return true;
		}
		default: err = std::string("a ") + luaL_typename(l, idx) + " cannot be saved (only numbers, strings, booleans and tables of them)"; return false;
	}
}
bool deser(lua_State *l, const std::string &b, size_t &p, int depth) {
	if (p >= b.size() || depth > 17) return false;
	char t = b[p++];
	if (t == 'b') { if (p >= b.size()) return false; lua_pushboolean(l, b[p++] != 0); return true; }
	if (t == 'i' || t == 'd') { if (p + 8 > b.size()) return false; uint64_t u = 0; for (int i = 0; i < 8; i++) u |= (uint64_t)(unsigned char)b[p + i] << (8 * i); p += 8;
		if (t == 'i') lua_pushinteger(l, (lua_Integer)(int64_t)u); else { double d; memcpy(&d, &u, 8); lua_pushnumber(l, d); } return true; }
	if (t == 's') { uint32_t n; if (!get32(b, p, n) || p + n > b.size()) return false; lua_pushlstring(l, b.data() + p, n); p += n; return true; }
	if (t == 't') { uint32_t n; if (!get32(b, p, n)) return false; luaL_checkstack(l, 4, "vault");
		lua_createtable(l, 0, n > 64 ? 64 : (int)n);
		for (uint32_t i = 0; i < n; i++) { if (!deser(l, b, p, depth + 1) || !deser(l, b, p, depth + 1)) { lua_pop(l, 1); return false; } lua_rawset(l, -3); }
		return true; }
	return false;
}

// ---------------------------------------------------------------- files
bool load_file(const std::string &path, Vault &v) {
	std::string b = read_text(path); if (b.size() < 13 || b.compare(0, 4, "XVLT") || b[4] != 1) return false;
	size_t p = b.size() - 4; uint32_t sum; if (!get32(b, p, sum) || fnv(b.substr(0, b.size() - 4)) != sum) return false;
	p = 5; uint32_t n; if (!get32(b, p, n)) return false;
	std::map<std::string, Entry> d;
	for (uint32_t i = 0; i < n; i++) { uint32_t kl, ts, vl; if (!get16(b, p, kl) || p + kl > b.size()) return false; std::string k = b.substr(p, kl); p += kl;
		if (!get32(b, p, ts) || !get32(b, p, vl) || p + vl > b.size() - 4) return false; d[k] = Entry{b.substr(p, vl), ts}; p += vl; }
	v.d.swap(d); return true;
}
Vault &get(const std::string &name) {
	Vault &v = g_v[name];
	if (!v.loaded) {
		v.loaded = true; std::string f = file_of(name);
		if (!load_file(f, v)) {
			bool exists = !read_text(f).empty();
			if (load_file(f + ".bak", v)) { xerr("vault '%s': the file was damaged - restored from the backup (%zu entries)", name.c_str(), v.d.size()); v.dirty = true; }
			else if (exists) xerr("vault '%s': %s is damaged and there is no usable backup - starting empty (the damaged file is kept as .damaged)", name.c_str(), f.c_str()), rename(f.c_str(), (f + ".damaged").c_str());
		}
	}
	return v;
}
bool save(const std::string &name, Vault &v) {
	mkdir_p(xpath("data").c_str()); mkdir_p(dir().c_str());
	std::string b = "XVLT"; b += (char)1; put32(b, (uint32_t)v.d.size());
	for (auto &kv : v.d) { put16(b, (uint32_t)kv.first.size()); b += kv.first; put32(b, kv.second.ts); put32(b, (uint32_t)kv.second.blob.size()); b += kv.second.blob; }
	put32(b, fnv(b));
	std::string f = file_of(name), tmp = f + ".tmp", bak = f + ".bak";
	FILE *fp = fopen(tmp.c_str(), "wb"); if (!fp) { xerr("vault '%s': cannot write %s", name.c_str(), tmp.c_str()); return false; }
	bool ok = fwrite(b.data(), 1, b.size(), fp) == b.size(); ok = (fflush(fp) == 0) && ok; ok = (fclose(fp) == 0) && ok;
	if (!ok) { remove(tmp.c_str()); xerr("vault '%s': write failed (disk full?) - the old file is untouched", name.c_str()); return false; }
	remove(bak.c_str()); rename(f.c_str(), bak.c_str());                     // the previous version becomes the backup
	if (rename(tmp.c_str(), f.c_str()) != 0) { rename(bak.c_str(), f.c_str()); xerr("vault '%s': cannot replace %s", name.c_str(), f.c_str()); return false; }
	v.dirty = false; return true;
}

// ---------------------------------------------------------------- Lua
const std::string &self(lua_State *l) { return *(std::string *)luaL_checkudata(l, 1, MT); }
std::string key_arg(lua_State *l, int i) {
	int t = lua_type(l, i); if (t != LUA_TSTRING && t != LUA_TNUMBER) luaL_argerror(l, i, "key must be a string or a number");
	size_t n; const char *s = lua_tolstring(l, i, &n); if (n == 0 || n > MAX_KEY) luaL_argerror(l, i, "key must be 1..255 bytes"); return std::string(s, n);
}
int v_set(lua_State *l) {
	Vault &v = get(self(l)); std::string k = key_arg(l, 2);
	if (lua_isnoneornil(l, 3)) { v.dirty |= v.d.erase(k) > 0; return 0; }
	std::string blob, err; if (!ser(l, 3, blob, 0, err)) return luaL_error(l, "vault:set(\"%s\"): %s", k.c_str(), err.c_str());
	if (blob.size() > MAX_BLOB) return luaL_error(l, "vault:set(\"%s\"): value too big (%d bytes, max 1 MB)", k.c_str(), (int)blob.size());
	Entry &e = v.d[k]; e.blob.swap(blob); e.ts = lua_isnumber(l, 4) ? (uint32_t)lua_tointeger(l, 4) : now32(); v.dirty = true; return 0;
}
int v_get(lua_State *l) {
	Vault &v = get(self(l)); auto it = v.d.find(key_arg(l, 2));
	if (it == v.d.end()) { lua_settop(l, 3); lua_pushnil(l); return 2; }
	size_t p = 0; if (!deser(l, it->second.blob, p, 0)) { lua_settop(l, 3); lua_pushnil(l); return 2; }
	lua_pushinteger(l, it->second.ts); return 2;
}
int v_has(lua_State *l) { Vault &v = get(self(l)); lua_pushboolean(l, v.d.count(key_arg(l, 2)) > 0); return 1; }
int v_remove(lua_State *l) { Vault &v = get(self(l)); bool r = v.d.erase(key_arg(l, 2)) > 0; v.dirty |= r; lua_pushboolean(l, r); return 1; }
int v_touch(lua_State *l) { Vault &v = get(self(l)); auto it = v.d.find(key_arg(l, 2)); if (it == v.d.end()) { lua_pushboolean(l, 0); return 1; }
	it->second.ts = lua_isnumber(l, 3) ? (uint32_t)lua_tointeger(l, 3) : now32(); v.dirty = true; lua_pushboolean(l, 1); return 1; }
int v_timestamp(lua_State *l) { Vault &v = get(self(l)); auto it = v.d.find(key_arg(l, 2)); if (it == v.d.end()) lua_pushnil(l); else lua_pushinteger(l, it->second.ts); return 1; }
int v_incr(lua_State *l) {             // incr(key [, by=1]) -> new value   (missing or non-number counts as 0)
	Vault &v = get(self(l)); std::string k = key_arg(l, 2); lua_Number by = luaL_optnumber(l, 3, 1), cur = 0; bool isint = lua_isinteger(l, 3) || lua_isnone(l, 3);
	auto it = v.d.find(k);
	if (it != v.d.end()) { size_t p = 0; if (deser(l, it->second.blob, p, 0)) { if (lua_type(l, -1) == LUA_TNUMBER) { if (!lua_isinteger(l, -1)) isint = false; cur = lua_tonumber(l, -1); } lua_pop(l, 1); } }
	if (isint) lua_pushinteger(l, (lua_Integer)cur + (lua_Integer)by); else lua_pushnumber(l, cur + by);
	std::string blob, err; ser(l, -1, blob, 0, err); Entry &e = v.d[k]; e.blob.swap(blob); e.ts = now32(); v.dirty = true; return 1;
}
int v_keys(lua_State *l) {
	Vault &v = get(self(l)); size_t pn = 0; const char *pre = luaL_optlstring(l, 2, "", &pn); lua_newtable(l); int i = 1;
	for (auto it = v.d.lower_bound(std::string(pre, pn)); it != v.d.end() && !it->first.compare(0, pn, pre, pn); ++it) { lua_pushlstring(l, it->first.data(), it->first.size()); lua_rawseti(l, -2, i++); }
	return 1;
}
int v_count(lua_State *l) { lua_pushinteger(l, (lua_Integer)get(self(l)).d.size()); return 1; }
int v_prune(lua_State *l) {            // prune(older_than_seconds)  or  prune(start_ts, end_ts) like nvault_prune;  touch-free entries with ts 0 are permanent
	Vault &v = get(self(l)); uint32_t a, b;
	if (lua_isnumber(l, 3)) { a = (uint32_t)luaL_checkinteger(l, 2); b = (uint32_t)luaL_checkinteger(l, 3); }
	else { lua_Integer secs = luaL_checkinteger(l, 2); a = 1; b = now32() - (uint32_t)(secs < 0 ? 0 : secs); }
	int n = 0; for (auto it = v.d.begin(); it != v.d.end();) { if (it->second.ts && it->second.ts >= a && it->second.ts <= b) { it = v.d.erase(it); n++; } else ++it; }
	v.dirty |= n > 0; lua_pushinteger(l, n); return 1;
}
int v_clear(lua_State *l) { Vault &v = get(self(l)); v.dirty |= !v.d.empty(); v.d.clear(); return 0; }
int v_save(lua_State *l) { const std::string &n = self(l); lua_pushboolean(l, save(n, get(n))); return 1; }
int v_each(lua_State *l) {             // each(function(key, value, ts) [return false to stop] end [, prefix])
	const std::string name = self(l); luaL_checktype(l, 2, LUA_TFUNCTION); size_t pn = 0; const char *pre = luaL_optlstring(l, 3, "", &pn);
	std::vector<std::string> ks; Vault &v = get(name);
	for (auto it = v.d.lower_bound(std::string(pre, pn)); it != v.d.end() && !it->first.compare(0, pn, pre, pn); ++it) ks.push_back(it->first);
	for (auto &k : ks) {                // a snapshot of the keys: the callback may change the vault
		auto it = get(name).d.find(k); if (it == get(name).d.end()) continue;
		lua_pushvalue(l, 2); lua_pushlstring(l, k.data(), k.size()); size_t p = 0; if (!deser(l, it->second.blob, p, 0)) lua_pushnil(l); lua_pushinteger(l, it->second.ts);
		lua_call(l, 3, 1); bool stop = lua_isboolean(l, -1) && !lua_toboolean(l, -1); lua_pop(l, 1); if (stop) break;
	}
	return 0;
}
int v_tostring(lua_State *l) { lua_pushfstring(l, "vault(%s)", self(l).c_str()); return 1; }
int v_gc(lua_State *l) { ((std::string *)luaL_checkudata(l, 1, MT))->~basic_string(); return 0; }
int x_open(lua_State *l) {
	const char *n = luaL_checkstring(l, 1); if (!good_name(n)) return luaL_error(l, "vault name '%s': use 1..48 letters, digits, _ or -", n);
	get(n); new (lua_newuserdatauv(l, sizeof(std::string), 0)) std::string(n); luaL_setmetatable(l, MT); return 1;
}
int x_list(lua_State *l) {             // names of the vaults on disk + open ones
	std::map<std::string, int> names; for (auto &kv : g_v) names[kv.first] = 1;
#ifdef _WIN32
	WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA((dir() + "/*.vault").c_str(), &fd);
	if (h != INVALID_HANDLE_VALUE) { do { std::string s = fd.cFileName; names[s.substr(0, s.size() - 6)] = 1; } while (FindNextFileA(h, &fd)); FindClose(h); }
#else
	if (DIR *d = opendir(dir().c_str())) { while (dirent *e = readdir(d)) { std::string s = e->d_name; if (s.size() > 6 && !s.compare(s.size() - 6, 6, ".vault")) names[s.substr(0, s.size() - 6)] = 1; } closedir(d); }
#endif
	lua_newtable(l); int i = 1; for (auto &kv : names) { lua_pushstring(l, kv.first.c_str()); lua_rawseti(l, -2, i++); } return 1;
}
}  // namespace

void vault_flush_all() { for (auto &kv : g_v) if (kv.second.dirty) save(kv.first, kv.second); }
void vault_reset() { g_v.clear(); g_last_auto = 0; }
void vault_frame(double t) { if (t - g_last_auto >= 120.0 || t < g_last_auto) { g_last_auto = t; vault_flush_all(); } }
void api_register_vault(lua_State *l) {
	luaL_newmetatable(l, MT);
	static const luaL_Reg m[] = {{"set", v_set}, {"get", v_get}, {"has", v_has}, {"remove", v_remove}, {"touch", v_touch}, {"timestamp", v_timestamp}, {"incr", v_incr},
		{"keys", v_keys}, {"count", v_count}, {"prune", v_prune}, {"clear", v_clear}, {"save", v_save}, {"each", v_each}, {nullptr, nullptr}};
	lua_newtable(l); luaL_setfuncs(l, m, 0); lua_setfield(l, -2, "__index");
	lua_pushcfunction(l, v_tostring); lua_setfield(l, -2, "__tostring"); lua_pushcfunction(l, v_gc); lua_setfield(l, -2, "__gc"); lua_pop(l, 1);
	lua_getglobal(l, "xmod"); lua_newtable(l);
	static const luaL_Reg f[] = {{"open", x_open}, {"list", x_list}, {nullptr, nullptr}}; luaL_setfuncs(l, f, 0);
	lua_setfield(l, -2, "vault"); lua_pop(l, 1);
}
