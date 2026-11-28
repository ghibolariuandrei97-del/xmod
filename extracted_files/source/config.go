package main

// User config (~/.xcmd/config.json) and per-server state (<server>/.xcmd/state.json).

import (
	"encoding/json"
	"os"
	"path/filepath"
	"runtime"
	"sort"
	"time"
)

const xcmdVersion = "1.1.0"

type SavedServer struct {
	Name string `json:"name"`
	Addr string `json:"addr"`
	Rcon string `json:"rcon,omitempty"`
	Dir  string `json:"dir,omitempty"` // local install dir (live file + CPU/RAM)
}

type Config struct {
	DefaultDir string        `json:"default_dir"`
	Repo       string        `json:"repo"`   // owner/name of the xmod release repo
	Branch     string        `json:"branch"` // its branch
	Servers    []SavedServer `json:"servers"`
	Masters    []string      `json:"masters"`
	MapsDir    string        `json:"maps_dir,omitempty"` // your CS 1.6 client's cstrike/maps (for the tactical map)
}

func homeDir() string {
	if d := os.Getenv("XCMD_HOME"); d != "" {
		return d
	}
	h, err := os.UserHomeDir()
	if err != nil {
		h = "."
	}
	return filepath.Join(h, ".xcmd")
}

func defaultConfig() *Config {
	dir := "hlds"
	if runtime.GOOS == "windows" {
		dir = `C:\hlds`
	} else if h, err := os.UserHomeDir(); err == nil {
		dir = filepath.Join(h, "hlds")
	}
	return &Config{DefaultDir: dir, Repo: "ghibolariuandrei97-del/xmod", Branch: "main",
		Masters: []string{"hl1master.steampowered.com:27011", "hl2master.steampowered.com:27011"}}
}

func loadConfig() *Config {
	c := defaultConfig()
	b, err := os.ReadFile(filepath.Join(homeDir(), "config.json"))
	if err == nil {
		json.Unmarshal(b, c)
	}
	if v := os.Getenv("XCMD_REPO"); v != "" {
		c.Repo = v
	}
	if c.Branch == "" {
		c.Branch = "main"
	}
	if len(c.Masters) == 0 {
		c.Masters = defaultConfig().Masters
	}
	return c
}

func (c *Config) Save() error {
	os.MkdirAll(homeDir(), 0o755)
	b, _ := json.MarshalIndent(c, "", "  ")
	return os.WriteFile(filepath.Join(homeDir(), "config.json"), b, 0o600)
}

func (c *Config) AddServer(s SavedServer) {
	for i, x := range c.Servers {
		if x.Addr == s.Addr || (s.Name != "" && x.Name == s.Name) {
			if s.Rcon == "" {
				s.Rcon = x.Rcon
			}
			if s.Dir == "" {
				s.Dir = x.Dir
			}
			c.Servers[i] = s
			return
		}
	}
	c.Servers = append(c.Servers, s)
}

// ---- per-server installation state

type InstalledFile struct {
	Path string `json:"path"` // relative to the server dir
	Hash string `json:"hash"`
}

type Installed struct {
	Name      string          `json:"name"`
	Version   string          `json:"version"`
	Sha       string          `json:"sha,omitempty"`
	Source    string          `json:"source"`
	When      time.Time       `json:"when"`
	Enabled   bool            `json:"enabled"`
	Files     []InstalledFile `json:"files"`
	Metamod   []string        `json:"metamod,omitempty"` // dll base names (e.g. xmod_mm)
	Order     int             `json:"order"`
	PluginIni []string        `json:"plugin_ini,omitempty"` // configs/plugins-*.ini owned by the package
	ExtIni    []string        `json:"ext_ini,omitempty"`
	Requires  []string        `json:"requires,omitempty"`
}

type State struct {
	Dir      string                `json:"-"`
	Packages map[string]*Installed `json:"packages"`
}

func stateFile(dir string) string { return filepath.Join(dir, ".xcmd", "state.json") }

func loadState(dir string) *State {
	s := &State{Dir: dir, Packages: map[string]*Installed{}}
	if b, err := os.ReadFile(stateFile(dir)); err == nil {
		json.Unmarshal(b, s)
		if s.Packages == nil {
			s.Packages = map[string]*Installed{}
		}
	}
	s.Dir = dir
	return s
}

func (s *State) Save() error {
	os.MkdirAll(filepath.Join(s.Dir, ".xcmd"), 0o755)
	b, _ := json.MarshalIndent(s, "", "  ")
	return os.WriteFile(stateFile(s.Dir), b, 0o644)
}

func (s *State) Names() []string {
	var n []string
	for k := range s.Packages {
		n = append(n, k)
	}
	sort.Strings(n)
	return n
}

func gameDir(dir string) string { return filepath.Join(dir, "cstrike") }

func hldsBinary(dir string) string {
	if runtime.GOOS == "windows" {
		return filepath.Join(dir, "hlds.exe")
	}
	return filepath.Join(dir, "hlds_linux")
}

func fileExists(p string) bool { _, err := os.Stat(p); return err == nil }
