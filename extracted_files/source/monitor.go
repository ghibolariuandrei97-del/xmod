package main

// Live server monitor (htop-like): every saved server + the local install, A2S queries, the xcmd agent (local file or
// rcon), CPU/RAM of a local hlds process, sparklines, player table.

import (
	"fmt"
	"sort"
	"strings"
	"sync"
	"time"

	"github.com/charmbracelet/bubbles/textinput"
	tea "github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/lipgloss"
)

type srv struct {
	mu        sync.Mutex
	SavedServer
	local     bool
	info      *ServerInfo
	ierr      error
	live      *Live
	lerr      error
	lsrc      string
	players   []A2SPlayer
	proc      ProcStat
	hist      map[string][]float64
	lastInfo  time.Time
	lastLive  time.Time
	busyInfo  bool
	busyLive  bool
	fast      bool // the map is open on this server
}

func (s *srv) push(k string, v float64) {
	h := s.hist[k]
	h = append(h, v)
	if len(h) > 120 {
		h = h[len(h)-120:]
	}
	s.hist[k] = h
}

func (s *srv) title() string {
	if s.Name != "" {
		return s.Name
	}
	if s.info != nil && s.info.Name != "" {
		return s.info.Name
	}
	if s.live != nil && s.live.Host != "" {
		return s.live.Host
	}
	return s.Addr
}

// poll runs in the background every frame; it starts the queries that are due
func (s *srv) poll() {
	s.mu.Lock()
	now := time.Now()
	if !s.busyInfo && now.Sub(s.lastInfo) > 2*time.Second {
		s.busyInfo = true
		s.lastInfo = now
		go func() {
			inf, err := A2SInfo(s.Addr, 1500*time.Millisecond)
			var pl []A2SPlayer
			if err == nil {
				pl, _ = A2SPlayers(s.Addr, 1500*time.Millisecond)
			}
			var ps ProcStat
			if s.Dir != "" {
				ps = findServerProc(s.Dir)
			}
			s.mu.Lock()
			s.info, s.ierr, s.busyInfo = inf, err, false
			if err == nil {
				s.players = pl
				s.push("players", float64(inf.Players))
				s.push("ping", float64(inf.Ping.Milliseconds()))
			}
			s.proc = ps
			if ps.Found {
				s.push("cpu", ps.CPU)
				s.push("ram", float64(ps.RSS)/1048576)
			}
			s.mu.Unlock()
		}()
	}
	every := 1000 * time.Millisecond
	if s.Dir != "" {
		every = 250 * time.Millisecond
	} else if s.fast {
		every = 500 * time.Millisecond
	}
	if !s.busyLive && now.Sub(s.lastLive) > every && (s.Dir != "" || s.Rcon != "") {
		s.busyLive = true
		s.lastLive = now
		go func() {
			var l *Live
			var err error
			src := ""
			if s.Dir != "" {
				l, err = readLiveFile(s.Dir)
				src = "file"
			}
			if l == nil && s.Rcon != "" {
				l, err = fetchLiveRcon(s.Addr, s.Rcon)
				src = "rcon"
			}
			s.mu.Lock()
			if l != nil {
				s.live, s.lerr, s.lsrc = l, nil, src
				if l.FPS > 0 {
					s.push("fps", l.FPS)
				}
			} else {
				s.lerr = err
				if s.live != nil && time.Since(s.live.At) > 6*time.Second {
					s.live = nil
				}
			}
			s.busyLive = false
			s.mu.Unlock()
		}()
	}
	s.mu.Unlock()
}

// ---------------------------------------------------------------- pool shared by Monitor and Map

type pool struct {
	list []*srv
}

func newPool(cfg *Config) *pool {
	p := &pool{}
	seenLocal := false
	for _, sv := range cfg.Servers {
		s := &srv{SavedServer: sv, hist: map[string][]float64{}}
		if sv.Dir != "" {
			seenLocal = true
		}
		p.list = append(p.list, s)
	}
	if !seenLocal && hldsInstalled(cfg.DefaultDir) {
		p.list = append([]*srv{{SavedServer: SavedServer{Name: "", Addr: "127.0.0.1:27015", Dir: cfg.DefaultDir}, local: true, hist: map[string][]float64{}}}, p.list...)
	}
	return p
}

var sharedPool *pool

// ---------------------------------------------------------------- Monitor tab

type monitor struct {
	a      *App
	sel    int
	input  textinput.Model
	mode   string // "", "add", "rcon"
	status string
}

func newMonitor(a *App) *monitor {
	if sharedPool == nil {
		sharedPool = newPool(a.cfg)
	}
	ti := textinput.New()
	ti.Prompt = "› "
	ti.CharLimit = 120
	return &monitor{a: a, input: ti}
}

func (m *monitor) Capturing() bool { return m.mode != "" }
func (m *monitor) Help() string {
	if m.mode != "" {
		return keyHelp("enter", "ok", "esc", "cancel")
	}
	return keyHelp("↑↓", "select", "enter", "tactical map", "a", "add server", "p", "rcon password", "d", "delete")
}

func (m *monitor) cur() *srv {
	if len(sharedPool.list) == 0 {
		return nil
	}
	if m.sel >= len(sharedPool.list) {
		m.sel = len(sharedPool.list) - 1
	}
	return sharedPool.list[m.sel]
}

func (m *monitor) Update(msg tea.Msg, a *App) tea.Cmd {
	switch k := msg.(type) {
	case frameMsg:
		for _, s := range sharedPool.list {
			s.poll()
		}
		return nil
	case tea.KeyMsg:
		if m.mode != "" {
			switch k.String() {
			case "esc":
				m.mode = ""
				return nil
			case "enter":
				v := strings.TrimSpace(m.input.Value())
				switch m.mode {
				case "add":
					f := strings.Fields(v)
					if len(f) > 0 {
						sv := SavedServer{Addr: normAddr(f[0])}
						if len(f) > 1 {
							sv.Name = strings.Join(f[1:], " ")
						}
						a.cfg.AddServer(sv)
						a.cfg.Save()
						sharedPool.list = append(sharedPool.list, &srv{SavedServer: sv, hist: map[string][]float64{}})
						m.sel = len(sharedPool.list) - 1
						m.status = "added " + sv.Addr + " - press p to set its rcon password for live data"
					}
				case "rcon":
					if s := m.cur(); s != nil {
						s.mu.Lock()
						s.Rcon = v
						s.mu.Unlock()
						a.cfg.AddServer(s.SavedServer)
						a.cfg.Save()
						m.status = "rcon password saved for " + s.Addr
					}
				}
				m.mode = ""
				return nil
			}
			var c tea.Cmd
			m.input, c = m.input.Update(msg)
			return c
		}
		switch k.String() {
		case "up", "k":
			if m.sel > 0 {
				m.sel--
			}
		case "down", "j":
			if m.sel < len(sharedPool.list)-1 {
				m.sel++
			}
		case "a":
			m.mode = "add"
			m.input.SetValue("")
			m.input.Placeholder = "ip:port  optional name"
			m.input.EchoMode = textinput.EchoNormal
			return m.input.Focus()
		case "p":
			if m.cur() != nil {
				m.mode = "rcon"
				m.input.SetValue("")
				m.input.Placeholder = "rcon_password of " + m.cur().Addr
				m.input.EchoMode = textinput.EchoPassword
				return m.input.Focus()
			}
		case "d", "delete":
			if s := m.cur(); s != nil && !s.local {
				var keep []SavedServer
				for _, x := range a.cfg.Servers {
					if x.Addr != s.Addr {
						keep = append(keep, x)
					}
				}
				a.cfg.Servers = keep
				a.cfg.Save()
				sharedPool.list = append(sharedPool.list[:m.sel], sharedPool.list[m.sel+1:]...)
			}
		case "enter":
			if s := m.cur(); s != nil {
				mv := a.models["Map"].(*mapView)
				mv.target = s
				a.switchTo("Map")
			}
		}
	}
	return nil
}

func statusDot(s *srv) string {
	switch {
	case s.info != nil && s.ierr == nil:
		return sOK.Render("●")
	case s.lastInfo.IsZero() || s.busyInfo && s.info == nil:
		return sWarn.Render("●")
	}
	return sErr.Render("●")
}

func (m *monitor) View(w, h int) string {
	list := sharedPool.list
	// summary meters
	totP, totM, online := 0, 0, 0
	for _, s := range list {
		s.mu.Lock()
		if s.info != nil && s.ierr == nil {
			online++
			totP += s.info.Players
			totM += s.info.MaxPlayers
		}
		s.mu.Unlock()
	}
	frac := 0.0
	if totM > 0 {
		frac = float64(totP) / float64(totM)
	}
	head := " " + sSub.Render("Servers") + " " + sText.Render(fmt.Sprintf("%d", len(list))) + sMuted.Render(fmt.Sprintf(" (%d online)", online)) +
		"    " + sSub.Render("Players") + " " + bar(frac, 24, cAccent) + " " + sText.Render(fmt.Sprintf("%d/%d", totP, totM))
	if m.status != "" {
		head += "    " + sMuted.Render(m.status)
	}
	// table
	cols := []struct {
		t string
		w int
	}{{"", 2}, {"SERVER", 30}, {"ADDRESS", 22}, {"MAP", 14}, {"PLAYERS", 20}, {"PING", 6}, {"FPS", 6}, {"CPU", 7}, {"RAM", 9}, {"XMOD", 8}, {"LIVE", 6}}
	var hb strings.Builder
	for _, c := range cols {
		hb.WriteString(pad(sHeader.Render(c.t), c.w) + " ")
	}
	rows := []string{hb.String()}
	for i, s := range list {
		s.mu.Lock()
		name := s.title()
		if s.local {
			name = "⌂ " + name
		}
		cells := []string{statusDot(s), trunc(name, 30), s.Addr, "-", "", "-", "-", "-", "-", "-", sDim.Render("-")}
		if s.info != nil {
			cells[3] = trunc(s.info.Map, 14)
			f := 0.0
			if s.info.MaxPlayers > 0 {
				f = float64(s.info.Players) / float64(s.info.MaxPlayers)
			}
			cells[4] = bar(f, 10, cAccent) + " " + fmt.Sprintf("%2d/%-2d", s.info.Players, s.info.MaxPlayers)
			pc := sOK
			if s.info.Ping > 80*time.Millisecond {
				pc = sWarn
			}
			if s.info.Ping > 160*time.Millisecond {
				pc = sErr
			}
			cells[5] = pc.Render(fmt.Sprintf("%dms", s.info.Ping.Milliseconds()))
		} else if s.ierr != nil {
			cells[3] = sErr.Render("offline")
		}
		if s.live != nil {
			if s.live.FPS > 0 {
				cells[6] = fmt.Sprintf("%.0f", s.live.FPS)
			}
			cells[9] = sSub.Render(s.live.Xmod)
			cells[10] = sOK.Render(s.lsrc)
		} else if s.lerr != nil && (s.Rcon != "" || s.Dir != "") {
			cells[10] = sWarn.Render("err")
		}
		if s.proc.Found {
			cpuS := sOK
			if s.proc.CPU > 50 {
				cpuS = sWarn
			}
			if s.proc.CPU > 90 {
				cpuS = sErr
			}
			cells[7] = cpuS.Render(fmt.Sprintf("%.1f%%", s.proc.CPU))
			cells[8] = humanBytes(s.proc.RSS)
		}
		s.mu.Unlock()
		var rb strings.Builder
		for j, c := range cols {
			rb.WriteString(pad(cells[j], c.w) + " ")
		}
		line := rb.String()
		if i == m.sel {
			line = sRowSel.Width(w - 2).Render(stripStyleKeepWidth(line, w-2))
		}
		rows = append(rows, line)
	}
	if len(list) == 0 {
		rows = append(rows, "", sMuted.Render("  No servers yet. Press ")+sKey.Render("a")+sMuted.Render(" to add one (ip:port), or install a local server with ")+sText.Render("xcmd setup"))
	}
	tableH := len(rows) + 2
	table := sPanel.Width(w - 2).Render(strings.Join(rows, "\n"))
	out := head + "\n" + table
	if m.mode != "" {
		label := "Add server"
		if m.mode == "rcon" {
			label = "Rcon password"
		}
		out += "\n" + sPanelHi.Width(w-2).Render(sSub.Render(label)+"  "+m.input.View())
		tableH += 3
	}
	// detail
	if s := m.cur(); s != nil {
		dh := h - tableH - 2
		if dh > 6 {
			out += "\n" + m.detail(s, w, dh)
		}
	}
	return out
}

// highlight a row: keep the text, drop inner colours
func stripStyleKeepWidth(s string, w int) string { return pad(stripANSI(s), w) }

func stripANSI(s string) string {
	var b strings.Builder
	in := false
	for _, r := range s {
		if r == 0x1b {
			in = true
			continue
		}
		if in {
			if (r >= 'a' && r <= 'z') || (r >= 'A' && r <= 'Z') {
				in = false
			}
			continue
		}
		b.WriteRune(r)
	}
	return b.String()
}

func (m *monitor) detail(s *srv, w, h int) string {
	s.mu.Lock()
	defer s.mu.Unlock()
	leftW := w * 62 / 100
	rightW := w - leftW - 4
	// players
	var lines []string
	lines = append(lines, sHeader.Render(pad("PLAYER", 22)+pad("K/D", 8)+pad("MONEY", 9)+pad("HP", 5)+pad("AP", 5)+pad("WEAPON", 10)+"PING"))
	if s.live != nil {
		ps := append([]LivePlayer(nil), s.live.Players...)
		sort.Slice(ps, func(i, j int) bool {
			if ps[i].Team != ps[j].Team {
				return ps[i].Team > ps[j].Team
			}
			return ps[i].Frags > ps[j].Frags
		})
		for _, p := range ps {
			name := teamStyle(p.Team).Render(trunc(p.Name, 20))
			if !p.Alive {
				name = sDim.Render(gDead + " " + trunc(p.Name, 18))
			}
			wpn := weaponName(p.Weapon)
			hp := sHP.Render(fmt.Sprint(p.HP))
			if !p.Alive {
				hp = sDim.Render("-")
			}
			ping := fmt.Sprint(p.Ping)
			if p.Bot {
				ping = sDim.Render("BOT")
			}
			lines = append(lines, pad(name, 22)+pad(fmt.Sprintf("%d/%d", p.Frags, p.Deaths), 8)+pad(sMoney.Render(fmt.Sprintf("$%d", p.Money)), 9)+pad(hp, 5)+pad(sAP.Render(fmt.Sprint(p.AP)), 5)+pad(trunc(wpn, 9), 10)+ping)
		}
		if len(ps) == 0 {
			lines = append(lines, sMuted.Render("server is empty"))
		}
	} else if len(s.players) > 0 {
		for _, p := range s.players {
			lines = append(lines, pad(trunc(p.Name, 20), 22)+pad(fmt.Sprint(p.Score), 8)+sDim.Render(fmt.Sprintf("connected %s", (time.Duration(p.Duration)*time.Second).Round(time.Second))))
		}
		lines = append(lines, "", sMuted.Render("money / HP / weapons need the xcmd agent + rcon (press p)"))
	} else if s.ierr != nil {
		lines = append(lines, sErr.Render("no answer: "+trunc(s.ierr.Error(), leftW-6)))
	} else {
		lines = append(lines, sMuted.Render("waiting for data…"))
	}
	left := sPanel.Width(leftW).Height(h - 2).Render(sSub.Render("Players") + "\n" + strings.Join(clip(lines, h-3), "\n"))
	// right: info + graphs
	var r []string
	if s.info != nil {
		r = append(r, sText.Bold(true).Render(trunc(s.info.Name, rightW-2)))
		r = append(r, sMuted.Render("map ")+sText.Render(s.info.Map)+sMuted.Render("  bots ")+sText.Render(fmt.Sprint(s.info.Bots))+sMuted.Render("  os ")+sText.Render(osName(s.info.OS)))
	}
	if s.live != nil {
		l := s.live
		score := sT.Render(fmt.Sprintf("T %d", l.T)) + sMuted.Render(" x ") + sCT.Render(fmt.Sprintf("%d CT", l.CT))
		left := "-"
		if l.Left >= 0 {
			left = fmt.Sprintf("%d:%02d", l.Left/60, l.Left%60)
		}
		r = append(r, score+sMuted.Render("   round ")+sText.Render(left))
		if l.FPS > 0 {
			r = append(r, sMuted.Render("server fps ")+sText.Render(fmt.Sprintf("%.0f", l.FPS))+sMuted.Render("  frame ")+sText.Render(fmt.Sprintf("%.2f ms (max %.1f)", l.FMS, l.FMax))+sMuted.Render("  lua ")+sText.Render(fmt.Sprintf("%.0f KB", l.LuaKB)))
		}
		r = append(r, sMuted.Render("xmod ")+sSub.Render(l.Xmod)+sMuted.Render("  agent ")+sText.Render(l.Agent)+sMuted.Render("  via ")+sOK.Render(s.lsrc))
	} else if s.lerr != nil && (s.Rcon != "" || s.Dir != "") {
		r = append(r, sWarn.Render("live: "+trunc(s.lerr.Error(), rightW-8)))
	} else if s.Rcon == "" && s.Dir == "" {
		r = append(r, sMuted.Render("press ")+sKey.Render("p")+sMuted.Render(" for live stats (rcon)"))
	}
	if s.proc.Found {
		up := ""
		if !s.proc.Started.IsZero() {
			up = time.Since(s.proc.Started).Round(time.Second).String()
		}
		r = append(r, sMuted.Render("pid ")+sText.Render(fmt.Sprint(s.proc.PID))+sMuted.Render("  up ")+sText.Render(up))
	}
	r = append(r, "")
	gw := rightW - 20
	if gw < 10 {
		gw = 10
	}
	graph := func(label, key string, col lipgloss.Color, unit string) {
		h := s.hist[key]
		if len(h) == 0 {
			return
		}
		r = append(r, pad(sMuted.Render(label), 9)+sparkline(h, gw, col)+" "+sText.Render(fmt.Sprintf("%.0f%s", h[len(h)-1], unit)))
	}
	graph("players", "players", cAccent, "")
	graph("ping", "ping", cAccent2, "ms")
	graph("fps", "fps", cOK, "")
	graph("cpu", "cpu", cWarn, "%")
	graph("ram", "ram", cCT, "MB")
	right := sPanel.Width(rightW).Height(h - 2).Render(sSub.Render("Server") + "\n" + strings.Join(clip(r, h-3), "\n"))
	return lipgloss.JoinHorizontal(lipgloss.Top, left, right)
}

func clip(l []string, n int) []string {
	if n < 0 {
		n = 0
	}
	if len(l) > n {
		return l[:n]
	}
	return l
}

func osName(b byte) string {
	switch b {
	case 'l':
		return "linux"
	case 'w':
		return "windows"
	}
	return "?"
}
