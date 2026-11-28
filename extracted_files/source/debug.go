package main

// hidden: xcmd __render <tab> <live.json> [map.bsp] [w] [h] - prints one frame of a tab (used by the tests)

import (
	"github.com/charmbracelet/lipgloss"
	"github.com/muesli/termenv"
	"fmt"
	"os"
	"strconv"
	"time"
)

func debugRender(a []string) {
	tab := a[0]
	lipgloss.SetColorProfile(termenv.TrueColor)
	cfg := loadConfig()
	app := newApp(cfg, tab)
	w, h := 150, 46
	if len(a) > 3 {
		w, _ = strconv.Atoi(a[3])
		h, _ = strconv.Atoi(a[4])
	}
	app.w, app.h = w, h
	if len(a) > 1 && a[1] != "-" {
		b, _ := os.ReadFile(a[1])
		l, err := parseLive(b)
		if err != nil {
			fmt.Println("live:", err)
			return
		}
		s := &srv{SavedServer: SavedServer{Addr: "127.0.0.1:27015", Name: "Test Server"}, hist: map[string][]float64{}}
		s.live, s.lsrc = l, "rcon"
		s.info = &ServerInfo{Name: "xmod | Dust Arena #1", Map: l.Map, Players: len(l.Players), MaxPlayers: 32, Ping: 23 * time.Millisecond, OS: 'l'}
		for i := 0; i < 60; i++ {
			s.push("players", float64(10+i%7))
			s.push("ping", float64(20+(i*7)%13))
			s.push("fps", 980+float64((i*37)%40))
			s.push("cpu", 12+float64((i*13)%9))
			s.push("ram", 180+float64(i%5))
		}
		s.proc = ProcStat{Found: true, PID: 4242, CPU: 14.2, RSS: 191 << 20, Started: time.Now().Add(-3 * time.Hour)}
		sharedPool = &pool{list: []*srv{s}}
		mv := app.models["Map"].(*mapView)
		mv.target = s
		if len(a) > 2 && a[2] != "-" {
			hm, err := loadBSP(a[2], 640)
			if err != nil {
				fmt.Println("bsp:", err)
			}
			mv.hm, mv.hmFor = hm, l.Map
		}
	}
	if tab == "Mods" {
		app.models["Mods"].(*modsView).cat = loadCatalog(cfg)
		app.models["Mods"].(*modsView).sel = 9
	}
	fmt.Print(app.View())
}

// hidden: xcmd __query <addr> [rcon]  and  xcmd __browse  (headless checks used by the tests)
func debugQuery(a []string) {
	inf, err := A2SInfo(a[0], 2*time.Second)
	fmt.Printf("info: %+v err=%v\n", inf, err)
	pl, err := A2SPlayers(a[0], 2*time.Second)
	fmt.Printf("players: %+v err=%v\n", pl, err)
	r, err := A2SRules(a[0], 2*time.Second)
	fmt.Printf("rules: %d err=%v xmod_version=%q\n", len(r), err, r["xmod_version"])
	if len(a) > 1 {
		l, err := fetchLiveRcon(a[0], a[1])
		if err != nil {
			fmt.Println("live err:", err)
		} else {
			fmt.Printf("live: map=%s players=%d kills=%d first=%+v\n", l.Map, len(l.Players), len(l.Kills), l.Players[0])
		}
	}
}

func debugBrowse() {
	cfg := loadConfig()
	b := newBrowser(&App{cfg: cfg})
	b.scan()
	for {
		time.Sleep(200 * time.Millisecond)
		b.mu.Lock()
		done := !b.scanning
		b.mu.Unlock()
		if done {
			break
		}
	}
	fmt.Println(b.phase, b.errs)
	for _, e := range b.list {
		fmt.Printf("%-22s %-30s %-10s %d/%d xmod=%q\n", e.info.Addr, e.info.Name, e.info.Map, e.info.Players, e.info.MaxPlayers, e.xmod)
	}
}
