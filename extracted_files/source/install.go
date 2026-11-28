package main

import (
	"archive/zip"
	"bytes"
	"crypto/sha1"
	"encoding/hex"
	"fmt"
	"io"
	"os"
	"path"
	"path/filepath"
	"runtime"
	"sort"
	"strings"
	"time"
)

// ---------------------------------------------------------------- files

func hashBytes(b []byte) string { h := sha1.Sum(b); return hex.EncodeToString(h[:]) }
func hashFile(p string) string {
	b, err := os.ReadFile(p)
	if err != nil {
		return ""
	}
	return hashBytes(b)
}

func unzipAll(arc, dest string) ([]string, error) {
	zr, err := zip.OpenReader(arc)
	if err != nil {
		return nil, err
	}
	defer zr.Close()
	var files []string
	for _, f := range zr.File {
		p := filepath.Join(dest, filepath.FromSlash(f.Name))
		if !strings.HasPrefix(p, filepath.Clean(dest)) {
			continue
		}
		if f.FileInfo().IsDir() {
			os.MkdirAll(p, 0o755)
			continue
		}
		rc, err := f.Open()
		if err != nil {
			return files, err
		}
		os.MkdirAll(filepath.Dir(p), 0o755)
		o, err := os.OpenFile(p, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, f.Mode()|0o644)
		if err != nil {
			rc.Close()
			return files, err
		}
		io.Copy(o, rc)
		o.Close()
		rc.Close()
		files = append(files, p)
	}
	return files, nil
}

func isConfigFile(rel string) bool {
	r := strings.ToLower(filepath.ToSlash(rel))
	ext := path.Ext(r)
	if ext != ".ini" && ext != ".cfg" && ext != ".txt" && ext != ".json" && ext != ".kv" {
		return false
	}
	return strings.Contains(r, "/configs/") || strings.Contains(r, "addons/metamod/") || strings.HasPrefix(r, "cstrike/") && !strings.Contains(r, "/dlls/")
}

// placeFile writes data to dir/rel, protecting configuration files the user changed.
// prev = hash recorded by the previous install of the same package ("" = none)
func placeFile(dir, rel string, data []byte, mode os.FileMode, prev string, t *Task) (InstalledFile, bool) {
	dst := filepath.Join(dir, filepath.FromSlash(rel))
	h := hashBytes(data)
	if fileExists(dst) && (strings.HasSuffix(rel, "addons/metamod/plugins.ini") || strings.HasSuffix(rel, "addons/metamod/config.ini")) {
		return InstalledFile{Path: rel, Hash: hashFile(dst)}, true // xcmd manages these itself
	}
	if fileExists(dst) && isConfigFile(rel) {
		cur := hashFile(dst)
		if cur == h {
			return InstalledFile{Path: rel, Hash: h}, true
		}
		if prev == "" || cur != prev { // the user's file: keep it, write ours next to it
			os.WriteFile(dst+".xcmd-new", data, 0o644)
			if t != nil {
				t.Log("kept your %s (new default saved as .xcmd-new)", rel)
			}
			return InstalledFile{Path: rel, Hash: cur}, true
		}
	}
	if fileExists(dst) && hashFile(dst) == h {
		return InstalledFile{Path: rel, Hash: h}, true // unchanged
	}
	os.MkdirAll(filepath.Dir(dst), 0o755)
	if mode == 0 {
		mode = 0o644
	}
	if err := os.WriteFile(dst, data, mode); err == nil {
		noteWritten(rel)
	} else {
		// a running server keeps .dll/.so files locked on Windows: write next to it
		if runtime.GOOS == "windows" {
			os.WriteFile(dst+".xcmd-pending", data, mode)
			if t != nil {
				t.Log("%s is in use (server running?) - saved as .xcmd-pending, stop the server and run xcmd update again", rel)
			}
		}
		return InstalledFile{Path: rel, Hash: h}, false
	}
	return InstalledFile{Path: rel, Hash: h}, true
}

func readZipEntry(f *zip.File) ([]byte, error) {
	rc, err := f.Open()
	if err != nil {
		return nil, err
	}
	defer rc.Close()
	return io.ReadAll(rc)
}

// ---------------------------------------------------------------- upstream (GitHub releases)

func installUpstream(p *Pkg, dir string, st *State, t *Task) error {
	t.Log("checking %s releases", p.Upstream)
	rel, err := ghLatestRelease(p.Upstream)
	if err != nil {
		return err
	}
	var asset *ghAsset
	for i, a := range rel.Assets {
		if strings.HasPrefix(a.Name, p.Asset) && strings.HasSuffix(a.Name, ".zip") {
			asset = &rel.Assets[i]
			break
		}
	}
	if asset == nil {
		return fmt.Errorf("no %s*.zip in release %s", p.Asset, rel.Tag)
	}
	if old := st.Packages[p.Name]; old != nil && old.Version == rel.Tag && os.Getenv("XCMD_FORCE") == "" {
		return t.Skip(rel.Tag + " already installed")
	}
	arc := filepath.Join(cacheDir(), asset.Name)
	if !fileExists(arc) {
		t.Log("downloading %s", asset.Name)
		if err := download(asset.URL, arc, t.DownloadProgress); err != nil {
			return err
		}
	}
	t.Progress(-1, "extracting "+asset.Name)
	zr, err := zip.OpenReader(arc)
	if err != nil {
		os.Remove(arc)
		return err
	}
	defer zr.Close()
	osdirs := targetOSDirs(dir)
	prev := map[string]string{}
	if old := st.Packages[p.Name]; old != nil {
		for _, f := range old.Files {
			prev[f.Path] = f.Hash
		}
	}
	inst := &Installed{Name: p.Name, Version: rel.Tag, Source: asset.URL, When: time.Now(), Enabled: true}
	for _, f := range zr.File {
		if f.FileInfo().IsDir() {
			continue
		}
		name := f.Name
		var rel string
		switch p.Name {
		case "rehlds", "regamedll": // bin/win32/... and/or bin/linux32/... -> server root (regamedll: cstrike/...)
			for _, od := range osdirs {
				if pre := "bin/" + od + "/"; strings.HasPrefix(name, pre) {
					rel = strings.TrimPrefix(name, pre)
				}
			}
			if rel == "" {
				continue
			}
		case "metamod":
			i := strings.Index(name, "addons/metamod/")
			if i < 0 {
				continue
			}
			rel = "cstrike/" + name[i:]
		default:
			continue
		}
		data, err := readZipEntry(f)
		if err != nil {
			return err
		}
		mode := os.FileMode(0o644)
		if runtime.GOOS != "windows" && (strings.HasSuffix(rel, "hlds_linux") || strings.HasSuffix(rel, "hltv")) {
			mode = 0o755
		}
		if p.Name == "regamedll" && (strings.HasSuffix(rel, "game.cfg") || strings.HasSuffix(rel, "game_init.cfg")) && fileExists(filepath.Join(dir, rel)) {
			continue // the server's own configs win
		}
		fi, _ := placeFile(dir, rel, data, mode, prev[rel], t)
		inst.Files = append(inst.Files, fi)
	}
	if len(inst.Files) == 0 {
		return fmt.Errorf("%s: nothing for %s in %s", p.Name, strings.Join(osdirs, "+"), asset.Name)
	}
	st.Packages[p.Name] = inst
	t.Note(rel.Tag)
	if p.Name == "metamod" {
		if err := patchLiblist(dir, t); err != nil {
			return err
		}
	}
	return st.Save()
}

// liblist.gam -> load Metamod
func patchLiblist(dir string, t *Task) error {
	lp := filepath.Join(dir, "cstrike", "liblist.gam")
	b, err := os.ReadFile(lp)
	if err != nil {
		// an upload folder: ship the standard CS 1.6 liblist.gam that loads Metamod
		os.MkdirAll(filepath.Dir(lp), 0o755)
		t.Log("wrote a standard cstrike/liblist.gam that loads Metamod")
		noteWritten("cstrike/liblist.gam")
		return os.WriteFile(lp, []byte(defaultLiblist), 0o644)
	}
	if !fileExists(lp + ".xcmd-bak") {
		os.WriteFile(lp+".xcmd-bak", b, 0o644)
	}
	var out []string
	hasW, hasL := false, false
	for _, l := range strings.Split(strings.ReplaceAll(string(b), "\r\n", "\n"), "\n") {
		k := strings.ToLower(strings.TrimSpace(l))
		if strings.HasPrefix(k, "gamedll_linux") {
			l, hasL = `gamedll_linux "addons/metamod/metamod_i386.so"`, true
		} else if strings.HasPrefix(k, "gamedll ") || strings.HasPrefix(k, "gamedll\t") || k == "gamedll" {
			l, hasW = `gamedll "addons\metamod\metamod.dll"`, true
		}
		out = append(out, l)
	}
	if !hasW {
		out = append(out, `gamedll "addons\metamod\metamod.dll"`)
	}
	if !hasL {
		out = append(out, `gamedll_linux "addons/metamod/metamod_i386.so"`)
	}
	t.Log("liblist.gam now loads Metamod")
	noteWritten("cstrike/liblist.gam")
	return os.WriteFile(lp, []byte(strings.Join(out, "\r\n")), 0o644)
}

// ---------------------------------------------------------------- repository packages

// fetchPkg downloads the package file into the cache and returns its local path
func fetchPkg(p *Pkg, t *Task) (string, error) {
	if p.URL == "" {
		return "", fmt.Errorf("%s has no download", p.Name)
	}
	ver := p.Version
	if p.Sha != "" {
		ver += "-" + p.Sha[:7]
	}
	local := filepath.Join(cacheDir(), p.Name+"-"+strings.ReplaceAll(ver, "/", "_")+path.Ext(p.URL))
	if fileExists(local) {
		return local, nil
	}
	if t != nil {
		t.Log("downloading %s", p.URL)
		if err := download(p.URL, local, t.DownloadProgress); err != nil {
			return "", err
		}
	} else if err := download(p.URL, local, nil); err != nil {
		return "", err
	}
	return local, nil
}

// inspect a downloaded package for lua dependencies
func inspectDeps(local string) []string {
	set := map[string]bool{}
	if strings.HasSuffix(strings.ToLower(local), ".lua") {
		b, _ := os.ReadFile(local)
		for _, d := range inferLuaDeps(string(b)) {
			set[d] = true
		}
	} else if zr, err := zip.OpenReader(local); err == nil {
		for _, f := range zr.File {
			if strings.HasSuffix(strings.ToLower(f.Name), ".lua") && strings.Contains(f.Name, "plugins/") {
				if b, err := readZipEntry(f); err == nil {
					for _, d := range inferLuaDeps(string(b)) {
						set[d] = true
					}
				}
			}
		}
		zr.Close()
	}
	var out []string
	for d := range set {
		out = append(out, d)
	}
	sort.Strings(out)
	return out
}

func installRepoPkg(p *Pkg, cat *Catalog, dir string, st *State, t *Task) error {
	if p.Builtin != "" {
		base := st.Packages[p.Builtin]
		if base == nil {
			return fmt.Errorf("%s comes with %s, which is not installed", p.Name, p.Builtin)
		}
		inst := &Installed{Name: p.Name, Version: base.Version, Source: "builtin:" + p.Builtin, When: time.Now(), Enabled: true, Metamod: p.Metamod, Order: p.Order, Requires: p.Requires}
		if p.Namespace != "" {
			ini := "cstrike/addons/xmod/configs/extensions-" + p.Name + ".ini"
			if !fileExists(filepath.Join(dir, ini)) {
				fi, _ := placeFile(dir, ini, []byte("; namespace of the "+p.Display()+" extension (written by xcmd)\n"+p.Namespace+"\n"), 0, "", t)
				inst.Files = append(inst.Files, fi)
			}
			inst.ExtIni = append(inst.ExtIni, ini)
		}
		st.Packages[p.Name] = inst
		t.Note("included in " + p.Builtin)
		return st.Save()
	}
	old := st.Packages[p.Name]
	if old != nil && old.Version == p.Version && (p.Sha == "" || old.Sha == p.Sha) && os.Getenv("XCMD_FORCE") == "" {
		return t.Skip(p.Version + " already installed")
	}
	local, err := fetchPkg(p, t)
	if err != nil {
		return err
	}
	prev := map[string]string{}
	if old != nil {
		for _, f := range old.Files {
			prev[f.Path] = f.Hash
		}
	}
	inst := &Installed{Name: p.Name, Version: p.Version, Sha: p.Sha, Source: p.URL, When: time.Now(), Enabled: true, Metamod: p.Metamod, Order: p.Order, Requires: p.Requires}
	if old != nil {
		inst.Enabled = old.Enabled
	}
	t.Progress(-1, "installing files")
	if strings.HasSuffix(strings.ToLower(local), ".lua") {
		data, _ := os.ReadFile(local)
		base := filepath.Base(p.File)
		if base == "" || base == "." {
			base = p.Name + ".lua"
		}
		fi, _ := placeFile(dir, "cstrike/addons/xmod/plugins/"+base, data, 0, prev["cstrike/addons/xmod/plugins/"+base], t)
		inst.Files = append(inst.Files, fi)
		p.Plugins = []string{base}
	} else {
		zr, err := zip.OpenReader(local)
		if err != nil {
			os.Remove(local)
			return fmt.Errorf("bad archive: %v", err)
		}
		defer zr.Close()
		for _, f := range zr.File {
			if f.FileInfo().IsDir() {
				continue
			}
			n := f.Name
			var rel string
			if i := strings.Index(n, "addons/"); i >= 0 && (i == 0 || n[i-1] == '/') {
				rel = "cstrike/" + n[i:]
			} else if i := strings.Index(n, "cstrike/"); i >= 0 && (i == 0 || n[i-1] == '/') {
				rel = n[i:]
			} else {
				low := strings.ToLower(path.Base(n))
				if strings.Count(strings.Trim(n, "/"), "/") <= 1 && (strings.HasSuffix(low, ".md") || strings.HasSuffix(low, ".txt")) {
					rel = ".xcmd/docs/" + p.Name + "/" + path.Base(n)
				} else {
					continue // sources, tests ...
				}
			}
			data, err := readZipEntry(f)
			if err != nil {
				return err
			}
			mode := os.FileMode(0o644)
			if strings.HasSuffix(rel, ".so") {
				mode = 0o755
			}
			fi, _ := placeFile(dir, rel, data, mode, prev[rel], t)
			inst.Files = append(inst.Files, fi)
			low := strings.ToLower(rel)
			if strings.Contains(low, "/configs/plugins-") && strings.HasSuffix(low, ".ini") {
				inst.PluginIni = append(inst.PluginIni, rel)
			}
			if strings.Contains(low, "/configs/extensions-") && strings.HasSuffix(low, ".ini") {
				inst.ExtIni = append(inst.ExtIni, rel)
			}
		}
	}
	if len(inst.Files) == 0 {
		return fmt.Errorf("the package has no addons/ folder")
	}
	// plugin list for packages that do not ship their own plugins-*.ini
	if len(inst.PluginIni) == 0 && len(p.Plugins) > 0 {
		ini := "cstrike/addons/xmod/configs/plugins-" + p.Name + ".ini"
		fi, _ := placeFile(dir, ini, []byte("; "+p.Display()+" (installed by xcmd)\n"+strings.Join(p.Plugins, "\n")+"\n"), 0, prev[ini], t)
		inst.Files = append(inst.Files, fi)
		inst.PluginIni = append(inst.PluginIni, ini)
	}
	// extension namespace ini when the package does not ship one (e.g. xbots -> "bot")
	if p.Namespace != "" && len(inst.ExtIni) == 0 {
		ini := "cstrike/addons/xmod/configs/extensions-" + p.Name + ".ini"
		fi, _ := placeFile(dir, ini, []byte("; namespace of the "+p.Display()+" extension (written by xcmd)\n"+p.Namespace+"\n"), 0, prev[ini], t)
		inst.Files = append(inst.Files, fi)
		inst.ExtIni = append(inst.ExtIni, ini)
	}
	// files of the old version that are gone now
	if old != nil {
		keep := map[string]bool{}
		for _, f := range inst.Files {
			keep[f.Path] = true
		}
		for _, f := range old.Files {
			if !keep[f.Path] && !isConfigFile(f.Path) && hashFile(filepath.Join(dir, f.Path)) == f.Hash {
				os.Remove(filepath.Join(dir, f.Path))
			}
		}
	}
	if p.Kind == "mod" || p.Kind == "plugin" {
		for _, d := range inspectDeps(local) {
			if d != p.Name {
				inst.Requires = append(inst.Requires, d)
			}
		}
	}
	st.Packages[p.Name] = inst
	if !inst.Enabled {
		setEnabled(dir, st, p.Name, false)
	}
	t.Note(p.Version)
	return st.Save()
}

// ---------------------------------------------------------------- Metamod plugins.ini (managed block)

const mmBegin = "; ---- xcmd managed: edit with 'xcmd enable/disable' ----"
const mmEnd = "; ---- end xcmd ----"

func syncMetamod(dir string, st *State) error {
	mdir := filepath.Join(dir, "cstrike", "addons", "metamod")
	if !fileExists(mdir) {
		return nil
	}
	type ent struct {
		order int
		name  string
		dll   string
	}
	var list []ent
	for _, ip := range st.Packages {
		if !ip.Enabled {
			continue
		}
		for _, d := range ip.Metamod {
			list = append(list, ent{ip.Order, ip.Name, d})
		}
	}
	sort.Slice(list, func(i, j int) bool {
		if list[i].order != list[j].order {
			return list[i].order < list[j].order
		}
		return list[i].dll < list[j].dll
	})
	ours := map[string]bool{}
	for _, ip := range st.Packages {
		for _, d := range ip.Metamod {
			ours[strings.ToLower(d)] = true
		}
	}
	pp := filepath.Join(mdir, "plugins.ini")
	b, _ := os.ReadFile(pp)
	var keep []string
	in := false
	for _, l := range strings.Split(strings.ReplaceAll(string(b), "\r\n", "\n"), "\n") {
		if strings.TrimSpace(l) == mmBegin {
			in = true
			continue
		}
		if strings.TrimSpace(l) == mmEnd {
			in = false
			continue
		}
		if in {
			continue
		}
		// a hand-written line for one of our dlls would load it twice: comment it out
		low := strings.ToLower(l)
		tl := strings.TrimSpace(low)
		if tl != "" && !strings.HasPrefix(tl, ";") && !strings.HasPrefix(tl, "#") && !strings.HasPrefix(tl, "//") {
			for d := range ours {
				if strings.Contains(low, "/"+d+".dll") || strings.Contains(low, "\\"+d+".dll") || strings.Contains(low, "/"+d+"_i386.so") {
					l = "; (moved into the xcmd block) " + l
					break
				}
			}
		}
		keep = append(keep, l)
	}
	for len(keep) > 0 && strings.TrimSpace(keep[len(keep)-1]) == "" {
		keep = keep[:len(keep)-1]
	}
	block := []string{"", mmBegin}
	for _, e := range list {
		block = append(block, fmt.Sprintf("win32 addons/xmod/dlls/%s.dll", e.dll), fmt.Sprintf("linux addons/xmod/dlls/%s_i386.so", e.dll))
	}
	block = append(block, mmEnd, "")
	data := []byte(strings.Join(append(keep, block...), "\n"))
	if old, _ := os.ReadFile(pp); string(old) != string(data) {
		noteWritten("cstrike/addons/metamod/plugins.ini")
	}
	return os.WriteFile(pp, data, 0o644)
}

// ---------------------------------------------------------------- enable / disable / remove

func setEnabled(dir string, st *State, name string, on bool) error {
	ip := st.Packages[name]
	if ip == nil {
		return fmt.Errorf("%s is not installed", name)
	}
	ip.Enabled = on
	for _, ini := range append(append([]string{}, ip.PluginIni...), ip.ExtIni...) {
		p := filepath.Join(dir, ini)
		if on && fileExists(p+".disabled") {
			os.Rename(p+".disabled", p)
		} else if !on && fileExists(p) {
			os.Remove(p + ".disabled")
			os.Rename(p, p+".disabled")
		}
	}
	if err := syncMetamod(dir, st); err != nil {
		return err
	}
	return st.Save()
}

func removePkg(dir string, st *State, name string) (kept []string, err error) {
	ip := st.Packages[name]
	if ip == nil {
		return nil, fmt.Errorf("%s is not installed", name)
	}
	for n, o := range st.Packages {
		if n == name {
			continue
		}
		for _, r := range o.Requires {
			if r == name {
				return nil, fmt.Errorf("%s needs %s (remove %s first)", n, name, n)
			}
		}
	}
	for _, f := range ip.Files {
		p := filepath.Join(dir, f.Path)
		for _, cand := range []string{p, p + ".disabled"} {
			if !fileExists(cand) {
				continue
			}
			if isConfigFile(f.Path) && hashFile(cand) != f.Hash {
				kept = append(kept, f.Path)
				continue
			}
			os.Remove(cand)
			// drop folders the package left empty
			for d := filepath.Dir(cand); len(d) > len(dir)+1; d = filepath.Dir(d) {
				if os.Remove(d) != nil {
					break
				}
			}
		}
	}
	delete(st.Packages, name)
	syncMetamod(dir, st)
	return kept, st.Save()
}

// ---------------------------------------------------------------- the agent (embedded)

func installAgent(dir string, st *State, t *Task) error {
	inst := &Installed{Name: "agent", Version: agentVersion, Source: "embedded", When: time.Now(), Enabled: true, Requires: []string{"xmod"}}
	prev := map[string]string{}
	if o := st.Packages["agent"]; o != nil {
		for _, f := range o.Files {
			prev[f.Path] = f.Hash
		}
		inst.Enabled = o.Enabled
	}
	for rel, data := range map[string][]byte{
		"cstrike/addons/xmod/plugins/xcmd_agent.lua":   []byte(agentLua),
		"cstrike/addons/xmod/configs/plugins-xcmd.ini": []byte("; xcmd live agent (monitor + tactical map). Turn off: xcmd disable agent\nxcmd_agent.lua\n"),
	} {
		fi, _ := placeFile(dir, rel, data, 0, prev[rel], t)
		inst.Files = append(inst.Files, fi)
	}
	inst.PluginIni = []string{"cstrike/addons/xmod/configs/plugins-xcmd.ini"}
	st.Packages["agent"] = inst
	t.Note("v" + agentVersion)
	return st.Save()
}

// ---------------------------------------------------------------- steps for a whole install

func installSteps(names []string, dir string, cfg *Config, update bool) ([]Step, error) {
	st := loadState(dir)
	cat := loadCatalog(cfg)
	plan, err := cat.plan(names, st, update)
	if err != nil {
		return nil, err
	}
	var steps []Step
	// resolve: download repository packages, discover lua dependencies, re-plan
	steps = append(steps, Step{Title: "Resolve packages", Run: func(t *Task) error {
		for round := 0; round < 4; round++ {
			changed := false
			for _, p := range plan {
				if p.URL == "" || p.Upstream != "" || p.Builtin != "" {
					continue
				}
				local, err := fetchPkg(p, t)
				if err != nil {
					return fmt.Errorf("%s: %v", p.Name, err)
				}
				if p.Kind != "mod" && p.Kind != "plugin" {
					continue // core / extensions declare their dependencies
				}
				for _, d := range inspectDeps(local) {
					if d == p.Name {
						continue
					}
					found := false
					for _, r := range p.Requires {
						if r == d {
							found = true
						}
					}
					if !found {
						p.Requires = append(p.Requires, d)
						t.Log("%s uses %s -> added as a dependency", p.Name, d)
						changed = true
					}
				}
			}
			if !changed {
				break
			}
			np, err := cat.plan(names, st, update)
			if err != nil {
				return err
			}
			plan = np
		}
		var n []string
		for _, p := range plan {
			n = append(n, p.Name)
		}
		t.Note(strings.Join(n, ", "))
		return nil
	}})
	// the rest is built lazily from the final plan
	steps = append(steps, Step{Title: "__expand__"})
	return expandSteps(steps, &plan, dir, cfg, update), nil
}

// expandSteps turns the placeholder into one step per package once Resolve ran
func expandSteps(steps []Step, plan *[]*Pkg, dir string, cfg *Config, update bool) []Step {
	out := []Step{steps[0]}
	// we cannot change the step list while running, so one step per possible package of the initial plan
	// + a final "configure" step; packages added by Resolve are installed inside the step of their dependant.
	installed := map[string]bool{}
	var runPkg func(p *Pkg, t *Task) error
	runPkg = func(p *Pkg, t *Task) error {
		if installed[p.Name] {
			return t.Skip("done")
		}
		installed[p.Name] = true
		st := loadState(dir)
		cat := loadCatalog(cfg)
		// dependencies discovered late
		for _, d := range p.Requires {
			if dp := cat.Get(d); dp != nil && !installed[d] && st.Packages[d] == nil && !(d == "hlds" && hldsInstalled(dir)) {
				t.Log("installing dependency %s", d)
				if err := runPkg(dp, t); err != nil && err != errSkip {
					return fmt.Errorf("dependency %s: %v", d, err)
				}
				st = loadState(dir)
			}
		}
		switch {
		case p.Name == "hlds":
			if hldsInstalled(dir) && !update {
				return t.Skip("already installed")
			}
			if err := ensureSteamCmd(dir, t); err != nil && err != errSkip {
				return err
			}
			t.Progress(-1, "starting SteamCMD")
			if err := installHLDS(dir, update, t); err != nil {
				return err
			}
			st.Packages["hlds"] = &Installed{Name: "hlds", Version: "app 90", Source: "steamcmd", When: time.Now(), Enabled: true}
			return st.Save()
		case p.Name == "agent":
			return installAgent(dir, st, t)
		case p.Upstream != "":
			return installUpstream(p, dir, st, t)
		default:
			if !p.Available {
				return fmt.Errorf("%s is not published in %s", p.Name, cfg.Repo)
			}
			return installRepoPkg(p, loadCatalog(cfg), dir, st, t)
		}
	}
	for _, p := range *plan {
		p := p
		out = append(out, Step{Title: p.Display(), Run: func(t *Task) error {
			// the plan may have grown: install anything new that precedes p
			for _, q := range *plan {
				if q.Name == p.Name {
					break
				}
				if !installed[q.Name] {
					if err := runPkg(q, t); err != nil && err != errSkip {
						return err
					}
				}
			}
			return runPkg(p, t)
		}, Optional: p.Optional})
	}
	out = append(out, Step{Title: "Configure Metamod + xmod", Run: func(t *Task) error {
		st := loadState(dir)
		for _, p := range *plan {
			if !installed[p.Name] {
				if err := runPkg(p, t); err != nil && err != errSkip {
					return err
				}
				st = loadState(dir)
			}
		}
		if err := syncMetamod(dir, st); err != nil {
			return err
		}
		var on []string
		for _, n := range st.Names() {
			if ip := st.Packages[n]; ip.Enabled && len(ip.Metamod) > 0 {
				on = append(on, n)
			}
		}
		if len(on) > 0 {
			t.Note("metamod: " + strings.Join(on, ", "))
		}
		return nil
	}})
	return out
}

var _ = bytes.MinRead


// targetOS: which server binaries to put in the folder ("windows", "linux" or "both").
// Default: what the folder already contains, else both (a folder made for FTP upload).
var targetOS = ""

func targetOSDirs(dir string) []string {
	os := targetOS
	if os == "" {
		switch {
		case fileExists(filepath.Join(dir, "hlds_linux")) || fileExists(filepath.Join(dir, "cstrike", "dlls", "cs.so")):
			os = "linux"
		case fileExists(filepath.Join(dir, "hlds.exe")) || fileExists(filepath.Join(dir, "cstrike", "dlls", "mp.dll")):
			os = "windows"
		default:
			os = "both"
		}
	}
	switch os {
	case "linux":
		return []string{"linux32"}
	case "windows", "win", "win32":
		return []string{"win32"}
	}
	return []string{"win32", "linux32"}
}

const defaultLiblist = "game \"Counter-Strike\"\r\nurl_info \"www.counter-strike.net\"\r\nurl_dl \"\"\r\nversion \"1.6\"\r\nsize \"184000000\"\r\nsvonly \"0\"\r\nsecure \"1\"\r\ntype \"multiplayer_only\"\r\ncldll \"1\"\r\nhlversion \"1111\"\r\nnomodels \"1\"\r\nnohimodel \"1\"\r\nmpentity \"info_player_start\"\r\ngamedll \"addons\\metamod\\metamod.dll\"\r\ngamedll_linux \"addons/metamod/metamod_i386.so\"\r\ntrainmap \"tr_1\"\r\nedicts \"900\"\r\n"
