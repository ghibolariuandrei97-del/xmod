// cache.cpp - data/codex/cache.ini: where each function was found, per module build (fingerprint), together with a
// signature Codex generated itself. Same build -> instant and verified. New build -> the learned signatures are tried
// as fallbacks, so a game/engine update that only moves code is survived without touching any gamedata.
#include "codex.h"
#include <cstdio>
#include <fstream>
#include <sstream>

struct CacheEntry { std::string mod, fp, func, sig; uintptr_t off; };
static std::vector<CacheEntry> s_cache;
static bool s_loaded = false, s_dirty = false, s_save_queued = false;

void cache_load()
{
	s_loaded = true;
	s_cache.clear();
	std::ifstream f(cx_data_path("cache.ini"));
	std::string line;
	while (std::getline(f, line)) {
		if (line.empty() || line[0] == '#' || line[0] == ';') continue;
		std::vector<std::string> parts;
		std::stringstream ss(line);
		std::string p;
		while (std::getline(ss, p, '|')) parts.push_back(p);
		if (parts.size() < 4) continue;
		CacheEntry e;
		e.mod = parts[0]; e.fp = parts[1]; e.func = parts[2];
		e.off = (uintptr_t)strtoul(parts[3].c_str(), nullptr, 0);
		e.sig = parts.size() > 4 ? parts[4] : "";
		s_cache.push_back(e);
	}
}

void cache_save()
{
	if (!s_dirty) return;
	std::string path = cx_data_path("cache.ini"), tmp = path + ".tmp";
	FILE *f = fopen(tmp.c_str(), "w");
	if (!f) return;
	fprintf(f, "# Codex resolution cache - generated, safe to delete. module|build fingerprint|function|offset|auto signature\n");
	for (auto &e : s_cache) fprintf(f, "%s|%s|%s|0x%x|%s\n", e.mod.c_str(), e.fp.c_str(), e.func.c_str(), (unsigned)e.off, e.sig.c_str());
	bool ok = !ferror(f);
	fclose(f);
	if (!ok) return;
	remove(path.c_str());
	rename(tmp.c_str(), path.c_str());
	s_dirty = false;
}

bool cache_get(const Module &m, const std::string &func, uintptr_t &off, std::string &sig)
{
	if (!s_loaded) cache_load();
	for (auto &e : s_cache)
		if (e.func == func && e.mod == m.name && e.fp == m.fingerprint) { off = e.off; sig = e.sig; return true; }
	return false;
}

std::vector<std::string> cache_sigs_any_build(const std::string &modname, const std::string &func)
{
	if (!s_loaded) cache_load();
	std::vector<std::string> v;
	for (auto &e : s_cache) if (e.func == func && e.mod == modname && !e.sig.empty()) v.push_back(e.sig);
	return v;
}

static void save_later(void *) { s_save_queued = false; cache_save(); }

void cache_put(const Module &m, const std::string &func, uintptr_t off, const std::string &sig)
{
	if (!s_loaded) cache_load();
	for (auto &e : s_cache)
		if (e.func == func && e.mod == m.name && e.fp == m.fingerprint) {
			if (e.off == off && e.sig == sig) return;
			e.off = off; e.sig = sig; s_dirty = true;
			goto queued;
		}
	s_cache.push_back({m.name, m.fingerprint, func, sig, off});
	s_dirty = true;
queued:
	if (g_xmod && !s_save_queued) { s_save_queued = true; g_xmod->post_main(save_later, nullptr); }
}

void cache_clear()
{
	s_cache.clear();
	s_loaded = true;
	s_dirty = true;
	cache_save();
}
