package main

// The package catalog ("npm for xmod"): upstream engine pieces (ReHLDS, ReGameDLL, Metamod-R from their GitHub releases)
// + everything published in the xmod repository (core, extensions, mods, single-file plugins).
//
// Catalog sources, merged in this order:
//   1. built-in knowledge (descriptions, metamod dll names, namespaces of the known packages)
//   2. a scan of the repository root (GitHub contents API): xmod_0.4.4.zip, xmod_codex_1.1.0.zip, modCreator.zip, war3.lua ...
//   3. xcmd.json in the repository root (optional, wins over everything) - see registry_template.json

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"sync"
)

type Pkg struct {
	Name        string   `json:"name"`
	Title       string   `json:"title,omitempty"`
	Version     string   `json:"version,omitempty"`
	Kind        string   `json:"kind,omitempty"` // game engine core extension mod plugin tool
	Description string   `json:"description,omitempty"`
	Tags        []string `json:"tags,omitempty"`
	File        string   `json:"file,omitempty"` // path inside the repository
	URL         string   `json:"url,omitempty"`  // or a full URL
	Sha         string   `json:"sha,omitempty"`
	Size        int64    `json:"size,omitempty"`
	Requires    []string `json:"requires,omitempty"`
	Namespace   string   `json:"namespace,omitempty"` // extension namespace -> configs/extensions-<name>.ini
	Metamod     []string `json:"metamod,omitempty"`   // dll base names: "xmod_mm" -> xmod_mm.dll / xmod_mm_i386.so
	Order       int      `json:"order,omitempty"`
	Plugins     []string `json:"plugins,omitempty"`  // plugin lines for configs/plugins-<name>.ini
	Builtin     string   `json:"builtin,omitempty"`  // shipped inside another package
	Upstream    string   `json:"upstream,omitempty"` // owner/repo with GitHub releases
	Asset       string   `json:"asset,omitempty"`    // release asset name prefix
	Optional    bool     `json:"optional,omitempty"` // not part of "all"
	Available   bool     `json:"-"`
}

func (p *Pkg) Display() string {
	if p.Title != "" {
		return p.Title
	}
	return p.Name
}

var knownPkgs = []*Pkg{
	{Name: "hlds", Title: "HLDS + Counter-Strike 1.6", Kind: "game", Description: "Valve's dedicated server, app 90 (mod cstrike), installed through SteamCMD.", Tags: []string{"server", "steamcmd"}, Available: true},
	{Name: "rehlds", Title: "ReHLDS", Kind: "engine", Upstream: "rehlds/ReHLDS", Asset: "rehlds-bin-", Description: "Reverse-engineered, fixed and faster HLDS engine.", Tags: []string{"engine"}},
	{Name: "regamedll", Title: "ReGameDLL_CS", Kind: "engine", Upstream: "rehlds/ReGameDLL_CS", Asset: "regamedll-bin-", Description: "Reverse-engineered Counter-Strike game library (mp.dll / cs.so) with hundreds of fixes and cvars.", Tags: []string{"engine", "gamedll"}},
	{Name: "metamod", Title: "Metamod-R", Kind: "engine", Upstream: "rehlds/Metamod-R", Asset: "metamod-bin-", Description: "Plugin loader between the engine and the game (needed by xmod).", Tags: []string{"engine", "metamod"}},
	{Name: "xmod", Title: "xmod core", Kind: "core", Metamod: []string{"xmod_mm"}, Order: 0, Description: "The xmod Lua scripting platform: scripts, hooks, Ham, ReAPI, messages, timers, vaults + libraries (nexus, metalog, translator, menu, util).", Tags: []string{"core", "lua"}},
	{Name: "nexus", Title: "Nexus HUD", Kind: "core", Builtin: "xmod", Description: "Nexus custom HUD / UI system - built into the xmod core.", Tags: []string{"hud", "ui"}},
	{Name: "query", Title: "xmod_query", Kind: "extension", Builtin: "xmod", Metamod: []string{"xmod_query_mm"}, Order: 10, Requires: []string{"xmod"}, Optional: true, Description: "SQL (SQLite/MySQL), HTTP and TCP/UDP sockets for scripts (the dll ships with the core).", Tags: []string{"sql", "http", "sockets"}},
	{Name: "codex", Title: "Codex", Kind: "extension", Metamod: []string{"xmod_codex_mm"}, Order: 20, Requires: []string{"xmod"}, Description: "Orpheu 2.0 for xmod: call/hook any game function, signatures, memory, call graphs.", Tags: []string{"reverse", "hooks", "orpheu"}},
	{Name: "xbot", Title: "xbots", Kind: "extension", Metamod: []string{"xmod_bots_mm"}, Order: 25, Requires: []string{"xmod"}, Namespace: "bot", Description: "Bots driven by xmod (navigation, combat, chat) - namespace xmod.bot.", Tags: []string{"bots", "ai"}},
	{Name: "modcreator", Title: "ModCreator", Kind: "extension", Metamod: []string{"modcreator_mm"}, Order: 30, Requires: []string{"xmod"}, Namespace: "mc", Description: "Make whole mods: entities & NPC AI, rounds, WeaponMod, teams & objectives (require(\"modcreator\")).", Tags: []string{"npc", "weapons", "rounds", "framework"}},
	{Name: "xzombie", Title: "XZombie", Kind: "mod", Requires: []string{"xmod"}, Description: "Zombie mod with Nexus HUD, Nemesis mode, effects.", Tags: []string{"zombie", "mod"}},
	{Name: "agent", Title: "xcmd agent", Kind: "tool", Requires: []string{"xmod"}, Available: true, Description: "Tiny xmod plugin that feeds xcmd's live monitor and tactical map (players, money, weapons, positions, kills).", Tags: []string{"xcmd", "monitor"}},
}

// file name -> package rules for the repository scan
var fileRules = []struct {
	re   *regexp.Regexp
	name string
}{
	{regexp.MustCompile(`(?i)^xmod[_-]v?(\d+\.\d+(?:\.\d+)*)\.zip$`), "xmod"},
	{regexp.MustCompile(`(?i)^xmod[_-]codex[_-]v?([\d.]+)\.zip$`), "codex"},
	{regexp.MustCompile(`(?i)^xmod[_-]bots?[_-]v?([\d.]+)\.zip$`), "xbot"},
	{regexp.MustCompile(`(?i)^xbots?[_-]v?([\d.]+)\.zip$`), "xbot"},
	{regexp.MustCompile(`(?i)^mod[_-]?creator[_-]?v?([\d.]*)\.zip$`), "modcreator"},
	{regexp.MustCompile(`(?i)^xzombie[_-]?v?([\d.]*)\.zip$`), "xzombie"},
}
var genericZip = regexp.MustCompile(`(?i)^([a-z][a-z0-9_]*?)(?:[_-]v?(\d+(?:\.\d+)*))?\.zip$`)
var genericLua = regexp.MustCompile(`(?i)^([a-z][a-z0-9_]*)\.lua$`)

type Catalog struct {
	Pkgs   map[string]*Pkg
	Source string
	Notes  []string
}

func (c *Catalog) Get(name string) *Pkg { return c.Pkgs[strings.ToLower(name)] }
// Sorted lists the packages; mods/plugins that are not published are left out
func (c *Catalog) Sorted() []*Pkg {
	kindOrder := map[string]int{"game": 0, "engine": 1, "core": 2, "extension": 3, "tool": 4, "mod": 5, "plugin": 6}
	var l []*Pkg
	for _, p := range c.Pkgs {
		if !p.Available && (p.Kind == "mod" || p.Kind == "plugin") {
			continue
		}
		l = append(l, p)
	}
	sort.Slice(l, func(i, j int) bool {
		a, b := kindOrder[l[i].Kind], kindOrder[l[j].Kind]
		if a != b {
			return a < b
		}
		if l[i].Order != l[j].Order {
			return l[i].Order < l[j].Order
		}
		return l[i].Name < l[j].Name
	})
	return l
}

func clonePkg(p *Pkg) *Pkg { c := *p; c.Requires = append([]string(nil), p.Requires...); return &c }

var (
	catOnce   sync.Mutex
	catCached *Catalog
)

// cmpVersion compares dotted versions ("0.4.10" > "0.4.4"); non numeric parts compare as strings
func cmpVersion(a, b string) int {
	pa, pb := strings.Split(strings.TrimPrefix(a, "v"), "."), strings.Split(strings.TrimPrefix(b, "v"), ".")
	for i := 0; i < len(pa) || i < len(pb); i++ {
		var x, y string
		if i < len(pa) {
			x = pa[i]
		}
		if i < len(pb) {
			y = pb[i]
		}
		xi, e1 := strconv.Atoi(x)
		yi, e2 := strconv.Atoi(y)
		if e1 == nil && e2 == nil {
			if xi != yi {
				if xi < yi {
					return -1
				}
				return 1
			}
			continue
		}
		if x != y {
			if x < y {
				return -1
			}
			return 1
		}
	}
	return 0
}

func loadCatalog(cfg *Config) *Catalog {
	catOnce.Lock()
	defer catOnce.Unlock()
	if catCached != nil {
		return catCached
	}
	c := &Catalog{Pkgs: map[string]*Pkg{}}
	for _, p := range knownPkgs {
		c.Pkgs[p.Name] = clonePkg(p)
	}
	for _, n := range []string{"rehlds", "regamedll", "metamod"} {
		c.Pkgs[n].Available = true
	}
	sources := []string{}
	scanned := map[string]bool{}
	scanOK := false
	// 2. repository scan
	if list, err := ghContents(cfg.Repo, cfg.Branch, ""); err == nil {
		sources = append(sources, "GitHub "+cfg.Repo)
		var all []ghContent
		all = append(all, list...)
		for _, e := range list { // one level of release folders
			if e.Type == "dir" && regexp.MustCompile(`(?i)^(mods|plugins|releases|dist|packages)$`).MatchString(e.Name) {
				if sub, err := ghContents(cfg.Repo, cfg.Branch, e.Path); err == nil {
					all = append(all, sub...)
				}
			}
		}
		scanOK = true
		for _, e := range all {
			if e.Type != "file" {
				continue
			}
			scanned[e.Path] = true
			c.addFile(e.Path, e.Name, e.Sha, e.Size, e.DownloadURL)
		}
	} else {
		c.Notes = append(c.Notes, "repository scan failed: "+err.Error())
		// 3b. static fallback: the files known to be published
		for _, f := range []string{"xmod_0.4.4.zip", "xmod_codex_1.1.0.zip", "xmod_bots_1.0.3.zip", "modCreator.zip", "war3.lua"} {
			c.addFile(f, f, "", 0, rawURL(cfg.Repo, cfg.Branch, f))
		}
		sources = append(sources, "built-in list")
	}
	// 3. xcmd.json registry
	if b, err := getBytes(rawURL(cfg.Repo, cfg.Branch, "xcmd.json")); err == nil {
		var reg struct {
			Packages []*Pkg `json:"packages"`
		}
		if err := json.Unmarshal(b, &reg); err == nil {
			sources = append(sources, "xcmd.json")
			for _, p := range reg.Packages {
				p.Name = strings.ToLower(p.Name)
				old := c.Pkgs[p.Name]
				if old != nil {
					mergePkg(old, p)
				} else {
					if p.Kind == "" {
						p.Kind = "mod"
					}
					c.Pkgs[p.Name] = p
					old = p
				}
				if old.File != "" || old.URL != "" || old.Builtin != "" {
					old.Available = true
				}
				if old.File != "" && (old.URL == "" || strings.Contains(old.URL, "/"+cfg.Repo+"/")) {
					old.URL = rawURL(cfg.Repo, cfg.Branch, old.File)
					if scanOK && !scanned[old.File] {
						old.Available = false // listed but the file is not in the repository (yet)
					}
				}
			}
		} else {
			c.Notes = append(c.Notes, "xcmd.json: "+err.Error())
		}
	}
	for _, p := range c.Pkgs {
		if p.Builtin != "" {
			if b := c.Pkgs[p.Builtin]; b != nil {
				p.Available = b.Available
				if p.Version == "" {
					p.Version = b.Version
				}
			}
		}
	}
	c.Source = strings.Join(sources, " + ")
	catCached = c
	return c
}

func mergePkg(dst, src *Pkg) {
	if src.Title != "" {
		dst.Title = src.Title
	}
	if src.Version != "" {
		dst.Version = src.Version
	}
	if src.Kind != "" {
		dst.Kind = src.Kind
	}
	if src.Description != "" {
		dst.Description = src.Description
	}
	if len(src.Tags) > 0 {
		dst.Tags = src.Tags
	}
	if src.File != "" {
		dst.File, dst.URL = src.File, ""
	}
	if src.URL != "" {
		dst.URL = src.URL
	}
	if len(src.Requires) > 0 {
		dst.Requires = src.Requires
	}
	if src.Namespace != "" {
		dst.Namespace = src.Namespace
	}
	if len(src.Metamod) > 0 {
		dst.Metamod = src.Metamod
	}
	if src.Order != 0 {
		dst.Order = src.Order
	}
	if len(src.Plugins) > 0 {
		dst.Plugins = src.Plugins
	}
	if src.Builtin != "" {
		dst.Builtin = src.Builtin
	}
	if src.Optional {
		dst.Optional = true
	}
}

func (c *Catalog) addFile(path, name, sha string, size int64, url string) {
	if strings.Contains(path, "extracted_files/") {
		return
	}
	var pname, ver string
	for _, r := range fileRules {
		if m := r.re.FindStringSubmatch(name); m != nil {
			pname, ver = r.name, m[1]
			break
		}
	}
	isLua := false
	if pname == "" {
		if m := genericLua.FindStringSubmatch(name); m != nil {
			pname, isLua = strings.ToLower(m[1]), true
		} else if m := genericZip.FindStringSubmatch(name); m != nil {
			pname, ver = strings.ToLower(m[1]), m[2]
			if k := c.Pkgs[pname]; k != nil && k.Kind != "mod" && k.Kind != "plugin" {
				return // e.g. "xmod_04.zip": an old core build without a proper version
			}
		} else {
			return
		}
	}
	if ver == "" && sha != "" {
		ver = "git-" + sha[:7]
	} else if ver == "" {
		ver = "latest"
	}
	p := c.Pkgs[pname]
	if p == nil {
		p = &Pkg{Name: pname, Kind: "mod", Requires: []string{"xmod"}, Description: "Published in the xmod repository (" + name + ")."}
		if isLua {
			p.Kind, p.Description = "plugin", "Single-file xmod plugin ("+name+")."
		}
		c.Pkgs[pname] = p
	} else if p.File != "" && p.Version != "" && !strings.HasPrefix(ver, "git-") && cmpVersion(ver, p.Version) <= 0 {
		return // keep the newest file of a package
	}
	p.File, p.Version, p.Sha, p.Size, p.URL, p.Available = path, ver, sha, size, url, true
}

// ---------------------------------------------------------------- resolve

// depsOf returns the declared dependencies (inferred ones are added after the download)
func (c *Catalog) depsOf(name string) []string {
	p := c.Get(name)
	if p == nil {
		return nil
	}
	return p.Requires
}

// plan returns the packages to install in dependency order
func (c *Catalog) plan(names []string, st *State, withInstalled bool) ([]*Pkg, error) {
	var order []*Pkg
	seen := map[string]int{}
	var visit func(n string, chain []string) error
	visit = func(n string, chain []string) error {
		n = strings.ToLower(n)
		switch seen[n] {
		case 1:
			return fmt.Errorf("dependency cycle: %s", strings.Join(append(chain, n), " -> "))
		case 2:
			return nil
		}
		p := c.Get(n)
		if p == nil {
			return fmt.Errorf("unknown package %q (try: xcmd search)", n)
		}
		seen[n] = 1
		for _, d := range c.depsOf(n) {
			if !withInstalled {
				if _, ok := st.Packages[d]; ok && d != "hlds" {
					continue
				}
			}
			if d == "hlds" && hldsInstalled(st.Dir) {
				continue
			}
			if err := visit(d, append(chain, n)); err != nil {
				return err
			}
		}
		seen[n] = 2
		order = append(order, p)
		return nil
	}
	for _, n := range names {
		if err := visit(n, nil); err != nil {
			return nil, err
		}
	}
	return order, nil
}

// lua source -> extra dependencies
var luaDeps = []struct {
	re  *regexp.Regexp
	pkg string
}{
	{regexp.MustCompile(`require\s*\(?\s*["']modcreator["']|xmod\.mc\.`), "modcreator"},
	{regexp.MustCompile(`xmod\.codex\.|require\s*\(?\s*["']codex["']`), "codex"},
	{regexp.MustCompile(`xmod\.bot\.`), "xbot"},
	{regexp.MustCompile(`xmod\.query\.`), "query"},
}

func inferLuaDeps(src string) []string {
	var out []string
	for _, d := range luaDeps {
		if d.re.MatchString(src) {
			out = append(out, d.pkg)
		}
	}
	return out
}

func cacheDir() string { d := filepath.Join(homeDir(), "cache"); os.MkdirAll(d, 0o755); return d }
