package main

import (
	_ "embed"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"
)

//go:embed agent/xcmd_agent.lua
var agentLua string

const agentVersion = "1.0.1"

// Live snapshot written / printed by the agent
type LivePlayer struct {
	ID      int
	Name    string
	Team    int
	Alive   bool
	HP, AP  int
	Money   int
	Frags   int
	Deaths  int
	Ping    int
	X, Y, Z float64
	Yaw     float64
	Weapon  int
	Weapons int64
	Bot     bool
}

type LiveKill struct {
	Time            int64
	Killer, Victim  string
	KTeam, VTeam    int
	Headshot        bool
	Weapon          string
}

type Live struct {
	V       int       `json:"v"`
	Agent   string    `json:"agent"`
	Xmod    string    `json:"xmod"`
	Host    string    `json:"host"`
	Map     string    `json:"map"`
	Max     float64   `json:"max"`
	Time    float64   `json:"time"`
	GT      float64   `json:"gt"`
	LeftF   float64   `json:"left"`
	CTF     float64   `json:"ct"`
	TF      float64   `json:"t"`
	Left    int       `json:"-"`
	CT      int       `json:"-"`
	T       int       `json:"-"`
	DL      string    `json:"dl"`
	Raw     [][]interface{} `json:"p"`
	RawK    [][]interface{} `json:"k"`
	FPS     float64   `json:"fps"`
	FMS     float64   `json:"fms"`
	FMax    float64   `json:"fmax"`
	LuaKB   float64   `json:"lua"`
	Players []LivePlayer `json:"-"`
	Kills   []LiveKill   `json:"-"`
	At      time.Time    `json:"-"`
}

func num(v interface{}) float64 {
	switch x := v.(type) {
	case float64:
		return x
	case bool:
		if x {
			return 1
		}
	}
	return 0
}
func str(v interface{}) string { s, _ := v.(string); return s }
func boolv(v interface{}) bool {
	switch x := v.(type) {
	case bool:
		return x
	case float64:
		return x != 0
	}
	return false
}

func parseLive(b []byte) (*Live, error) {
	s := strings.TrimSpace(string(b))
	if i := strings.Index(s, "XCMD{"); i >= 0 {
		s = s[i+4:]
	}
	if j := strings.LastIndex(s, "}"); j >= 0 {
		s = s[:j+1]
	}
	var l Live
	if err := json.Unmarshal([]byte(s), &l); err != nil {
		return nil, err
	}
	for _, r := range l.Raw {
		if len(r) < 17 {
			continue
		}
		l.Players = append(l.Players, LivePlayer{ID: int(num(r[0])), Name: str(r[1]), Team: int(num(r[2])), Alive: boolv(r[3]), HP: int(num(r[4])), AP: int(num(r[5])),
			Money: int(num(r[6])), Frags: int(num(r[7])), Deaths: int(num(r[8])), Ping: int(num(r[9])), X: num(r[10]), Y: num(r[11]), Z: num(r[12]), Yaw: num(r[13]),
			Weapon: int(num(r[14])), Weapons: int64(num(r[15])), Bot: boolv(r[16])})
	}
	for _, r := range l.RawK {
		if len(r) < 7 {
			continue
		}
		l.Kills = append(l.Kills, LiveKill{Time: int64(num(r[0])), Killer: str(r[1]), Victim: str(r[2]), KTeam: int(num(r[3])), VTeam: int(num(r[4])), Headshot: boolv(r[5]), Weapon: str(r[6])})
	}
	l.Left, l.CT, l.T = int(l.LeftF), int(l.CTF), int(l.TF)
	l.At = time.Now()
	return &l, nil
}

func readLiveFile(dir string) (*Live, error) {
	p := filepath.Join(dir, "cstrike", "addons", "xmod", "data", "xcmd_live.json")
	st, err := os.Stat(p)
	if err != nil {
		return nil, err
	}
	if time.Since(st.ModTime()) > 5*time.Second {
		return nil, fmt.Errorf("live file is old (server stopped?)")
	}
	b, err := os.ReadFile(p)
	if err != nil {
		return nil, err
	}
	return parseLive(b)
}

func fetchLiveRcon(addr, pass string) (*Live, error) {
	out, err := rconExec(addr, pass, "xcmd_dump", 1500*time.Millisecond)
	if err != nil {
		return nil, err
	}
	if !strings.Contains(out, "XCMD{") {
		if strings.Contains(strings.ToLower(out), "bad rcon") {
			return nil, fmt.Errorf("bad rcon password")
		}
		return nil, fmt.Errorf("agent not installed on the server (xcmd install agent)")
	}
	return parseLive([]byte(out))
}

// CS weapon ids -> short names
var csWeapons = map[int]string{1: "p228", 3: "scout", 4: "hegren", 5: "xm1014", 6: "c4", 7: "mac10", 8: "aug", 9: "smoke", 10: "elite", 11: "fiveseven",
	12: "ump45", 13: "sg550", 14: "galil", 15: "famas", 16: "usp", 17: "glock", 18: "awp", 19: "mp5", 20: "m249", 21: "m3", 22: "m4a1", 23: "tmp",
	24: "g3sg1", 25: "flash", 26: "deagle", 27: "sg552", 28: "ak47", 29: "knife", 30: "p90"}

func weaponName(id int) string {
	if n, ok := csWeapons[id]; ok {
		return n
	}
	return ""
}

// primary / pistol of a weapons bitmask
func loadout(bits int64) (primary, pistol string, nades int) {
	for id, n := range csWeapons {
		if bits&(1<<uint(id)) == 0 {
			continue
		}
		switch id {
		case 1, 10, 11, 16, 17, 26:
			pistol = n
		case 4, 9, 25:
			nades++
		case 6, 29:
		default:
			primary = n
		}
	}
	return
}
