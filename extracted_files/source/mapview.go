package main

// Live tactical map: top-down map rendered from the server's BSP with half-block pixels, players with view cones,
// team panels with money / HP / armor / weapons, score, round timer and kill feed. "HLTV in the terminal".

import (
	"fmt"
	"math"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"time"

	tea "github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/lipgloss"
)

type rgb struct{ r, g, b uint8 }

func hexRGB(s string) rgb {
	var r, g, b int
	fmt.Sscanf(s, "#%02x%02x%02x", &r, &g, &b)
	return rgb{uint8(r), uint8(g), uint8(b)}
}
func mix(a, b rgb, t float64) rgb {
	if t < 0 {
		t = 0
	}
	if t > 1 {
		t = 1
	}
	f := func(x, y uint8) uint8 { return uint8(float64(x) + (float64(y)-float64(x))*t) }
	return rgb{f(a.r, b.r), f(a.g, b.g), f(a.b, b.b)}
}
func fg(c rgb) string { return fmt.Sprintf("\x1b[38;2;%d;%d;%dm", c.r, c.g, c.b) }
func bg(c rgb) string { return fmt.Sprintf("\x1b[48;2;%d;%d;%dm", c.r, c.g, c.b) }

var (
	colVoid    = hexRGB("#14161C")
	colLow     = hexRGB("#5C3F22")
	colHigh    = hexRGB("#E2AE6C")
	colEdge    = hexRGB("#F1E4CC")
	colStep    = hexRGB("#3B2816")
	colCT      = hexRGB("#3B82F6")
	colCTLight = hexRGB("#93C5FD")
	colT       = hexRGB("#F97316")
	colTLight  = hexRGB("#FDBA74")
	colDead    = hexRGB("#9CA3AF")
	colGrid    = hexRGB("#1E222B")
)

type mapView struct {
	a      *App
	target *srv
	mu     sync.Mutex
	hm     *HeightMap
	hmFor  string
	hmErr  string
	busy   bool
	names  bool
	cones  bool
	// auto bounds when there is no bsp
	bx0, by0, bx1, by1 float64
}

func newMapView(a *App) *mapView { return &mapView{a: a, names: true, cones: true} }

func (m *mapView) Capturing() bool { return false }
func (m *mapView) Help() string {
	return keyHelp("n", "names", "c", "view cones", "←→", "server", "esc", "monitor")
}

func (m *mapView) pickTarget() *srv {
	if m.target == nil && sharedPool != nil {
		for _, s := range sharedPool.list { // first server with live data
			s.mu.Lock()
			ok := s.live != nil
			s.mu.Unlock()
			if ok {
				m.target = s
				break
			}
		}
		if m.target == nil && len(sharedPool.list) > 0 {
			m.target = sharedPool.list[0]
		}
	}
	return m.target
}

func (m *mapView) Update(msg tea.Msg, a *App) tea.Cmd {
	switch k := msg.(type) {
	case frameMsg:
		if sharedPool != nil {
			for _, s := range sharedPool.list {
				s.fast = s == m.target && a.cur == "Map"
			}
		}
		if s := m.pickTarget(); s != nil {
			s.mu.Lock()
			var mapName, dl string
			if s.live != nil {
				mapName, dl = s.live.Map, s.live.DL
			} else if s.info != nil {
				mapName = s.info.Map
			}
			dir := s.Dir
			s.mu.Unlock()
			m.ensureMap(mapName, dir, dl)
		}
	case tea.KeyMsg:
		switch k.String() {
		case "n":
			m.names = !m.names
		case "c":
			m.cones = !m.cones
		case "esc", "backspace":
			a.switchTo("Monitor")
		case "left", "right":
			if sharedPool != nil && len(sharedPool.list) > 0 {
				i := 0
				for j, s := range sharedPool.list {
					if s == m.target {
						i = j
					}
				}
				if k.String() == "right" {
					i = (i + 1) % len(sharedPool.list)
				} else {
					i = (i + len(sharedPool.list) - 1) % len(sharedPool.list)
				}
				m.target = sharedPool.list[i]
			}
		}
	}
	return nil
}

// find / download / parse the BSP in the background
func (m *mapView) ensureMap(name, dir, dl string) {
	m.mu.Lock()
	defer m.mu.Unlock()
	if name == "" || m.hmFor == name || m.busy {
		return
	}
	m.busy = true
	m.hm, m.hmErr = nil, "loading "+name+".bsp …"
	go func() {
		var hm *HeightMap
		var err error
		var cands []string
		if dir != "" {
			cands = append(cands, filepath.Join(dir, "cstrike", "maps", name+".bsp"))
		}
		for _, d := range mapDirs(m.a.cfg) {
			cands = append(cands, filepath.Join(d, name+".bsp"))
		}
		for _, c := range cands {
			if fileExists(c) {
				hm, err = loadBSP(c, 640)
				break
			}
		}
		if hm == nil && err == nil && dl != "" { // FastDL
			base := strings.TrimRight(dl, "/")
			dst := filepath.Join(homeDir(), "maps", name+".bsp")
			for _, ext := range []string{".bsp", ".bsp.bz2"} {
				tmp := dst + ext[4:]
				if download(base+"/maps/"+name+ext, tmp, nil) == nil {
					if data, e := readMaybeBz2(tmp); e == nil {
						os.WriteFile(dst, data, 0o644)
						if ext != ".bsp" {
							os.Remove(tmp)
						}
						hm, err = loadBSP(dst, 640)
						break
					}
				}
			}
		}
		m.mu.Lock()
		m.hm, m.hmFor, m.busy = hm, name, false
		m.hmErr = ""
		if hm == nil {
			if err != nil {
				m.hmErr = name + ".bsp: " + err.Error()
			} else {
				m.hmErr = "no " + name + ".bsp found - tell xcmd where your CS 1.6 maps are: xcmd map --maps \"C:\\...\\cstrike\\maps\""
			}
		}
		m.mu.Unlock()
	}()
}

func (m *mapView) View(w, h int) string {
	s := m.pickTarget()
	if s == nil {
		return "\n  " + sMuted.Render("No servers. Add one in the Monitor tab (a), or run ") + sText.Render("xcmd setup")
	}
	s.mu.Lock()
	live := s.live
	title := s.title()
	lerr := s.lerr
	s.mu.Unlock()
	if live == nil {
		msg := "waiting for live data from " + s.Addr
		if lerr != nil {
			msg = "no live data: " + lerr.Error()
		}
		hint := "The map needs the xcmd agent on the server (xcmd install agent) and the rcon password (Monitor → p), or a local server."
		return "\n  " + sSub.Render(title) + "\n\n  " + sWarn.Render(msg) + "\n  " + sMuted.Render(hint)
	}
	sideW := 30
	if w < 110 {
		sideW = 24
	}
	mapW := w - 2*sideW - 6
	feedH := 6
	mapH := h - 4 - feedH - 2
	if mapW < 20 || mapH < 6 {
		return sWarn.Render("window too small for the map")
	}
	var ct, t []LivePlayer
	for _, p := range live.Players {
		switch p.Team {
		case 2:
			ct = append(ct, p)
		case 1:
			t = append(t, p)
		}
	}
	sort.Slice(ct, func(i, j int) bool { return ct[i].Frags > ct[j].Frags })
	sort.Slice(t, func(i, j int) bool { return t[i].Frags > t[j].Frags })
	// score line
	left := "--:--"
	if live.Left >= 0 {
		left = fmt.Sprintf("%d:%02d", live.Left/60, live.Left%60)
	}
	score := sT.Render(fmt.Sprintf("T %d", live.T)) + sMuted.Render("  x  ") + sCT.Render(fmt.Sprintf("%d CT", live.CT))
	head := lipgloss.PlaceHorizontal(mapW, lipgloss.Center, score+sMuted.Render("   "+gTime)+sText.Bold(true).Render(left)+sMuted.Render("   "+live.Map))
	m.mu.Lock()
	hm, hmErr := m.hm, m.hmErr
	m.mu.Unlock()
	note := ""
	if hmErr != "" {
		head = lipgloss.PlaceHorizontal(mapW, lipgloss.Center, score+sMuted.Render("   "+gTime)+sText.Bold(true).Render(left)+sMuted.Render("   "+live.Map))
		note = sWarn.Render(trunc(hmErr, mapW))
		mapH--
	}
	mapStr := m.render(hm, live.Players, mapW, mapH)
	body := head + "\n" + mapStr
	if note != "" {
		body = head + "\n" + lipgloss.PlaceHorizontal(mapW, lipgloss.Center, note) + "\n" + mapStr
	}
	center := sPanel.Width(mapW + 2).Render(body)
	ctPanel := teamPanel("CT", ct, sCT, sideW, mapH+2)
	tPanel := teamPanel("TR", t, sT, sideW, mapH+2)
	top := lipgloss.JoinHorizontal(lipgloss.Top, ctPanel, center, tPanel)
	// kill feed
	var kl []string
	ks := live.Kills
	if len(ks) > feedH-1 {
		ks = ks[len(ks)-(feedH-1):]
	}
	for _, k := range ks {
		tm := time.Unix(k.Time, 0).Format("15:04:05")
		if k.Time < 1000000000 { // older agents: seconds of map time
			tm = fmt.Sprintf("%02d:%02d", k.Time/60, k.Time%60)
		}
		killer := teamStyle(k.KTeam).Render(k.Killer)
		if k.Killer == "" || k.Killer == k.Victim {
			killer = sMuted.Render("world")
		}
		hs := ""
		if k.Headshot {
			hs = sWarn.Render(" "+gHS+"headshot")
		}
		kl = append(kl, sDim.Render("["+tm+"] ")+killer+sMuted.Render(" killed ")+teamStyle(k.VTeam).Render(k.Victim)+sMuted.Render(" with ")+sText.Render(k.Weapon)+hs)
	}
	if len(kl) == 0 {
		kl = append(kl, sDim.Render("no kills yet"))
	}
	feed := sPanel.Width(lipgloss.Width(top) - 2).Render(strings.Join(kl, "\n"))
	return top + "\n" + feed
}

func teamPanel(label string, ps []LivePlayer, st lipgloss.Style, w, h int) string {
	alive := 0
	for _, p := range ps {
		if p.Alive {
			alive++
		}
	}
	var lines []string
	lines = append(lines, lipgloss.PlaceHorizontal(w-2, lipgloss.Center, st.Render(fmt.Sprintf("%s (%d)", label, len(ps)))+sMuted.Render(fmt.Sprintf("  %d alive", alive))))
	inner := w - 2
	for _, p := range ps {
		if len(lines)+3 > h-2 {
			lines = append(lines, sMuted.Render(fmt.Sprintf("+ %d more", len(ps)-(len(lines)-1)/3)))
			break
		}
		primary, pistol, nades := loadout(p.Weapons)
		cur := weaponName(p.Weapon)
		kd := fmt.Sprintf("%d/%d", p.Frags, p.Deaths)
		name := trunc(p.Name, inner-len(kd)-1)
		if !p.Alive {
			lines = append(lines, sDim.Render(pad(gDead+" "+trunc(p.Name, inner-len(kd)-3), inner-len(kd))+kd))
			lines = append(lines, sDim.Render(fmt.Sprintf("$%d", p.Money)))
			lines = append(lines, sDim.Render(strings.Repeat("─", inner)))
			continue
		}
		lines = append(lines, pad(st.Render(name), inner-len(kd))+sMuted.Render(kd))
		gun := primary
		if gun == "" {
			gun = pistol
		}
		gunS := sText.Render(trunc(gun, inner-10))
		if cur != "" && cur != gun {
			gunS = sText.Render(cur)
			if gun != "" {
				gunS += sDim.Render(" (" + trunc(gun, inner-12-len(cur)) + ")")
			}
		}
		n := ""
		if nades > 0 {
			n = sDim.Render(fmt.Sprintf(" ●%d", nades))
		}
		lines = append(lines, pad(sMoney.Render(fmt.Sprintf("$%d", p.Money)), 8)+gunS+n)
		hpw := inner - 12
		lines = append(lines, sHP.Render(gHP)+fmt.Sprintf("%3d ", p.HP)+bar(float64(p.HP)/100, hpw, cHP)+" "+sAP.Render(fmt.Sprintf("%s%d", gAP, p.AP)))
	}
	if len(ps) == 0 {
		lines = append(lines, sDim.Render("nobody"))
	}
	return sPanel.Width(w).Height(h).Render(strings.Join(lines, "\n"))
}

// render: the map as cw x ch characters (2 pixels per character vertically)
func (m *mapView) render(hm *HeightMap, ps []LivePlayer, cw, ch int) string {
	pw, ph := cw, ch*2
	pix := make([]rgb, pw*ph)
	var toPix func(x, y float64) (float64, float64)
	if hm != nil {
		sc := math.Min(float64(pw)/float64(hm.W), float64(ph)/float64(hm.H))
		ox, oy := (float64(pw)-float64(hm.W)*sc)/2, (float64(ph)-float64(hm.H)*sc)/2
		zs := make([]float32, pw*ph)
		for py := 0; py < ph; py++ {
			for px := 0; px < pw; px++ {
				// max over the source block (keeps thin corridors when zoomed out)
				sx0, sy0 := (float64(px)-ox)/sc, (float64(py)-oy)/sc
				sx1, sy1 := sx0+1/sc, sy0+1/sc
				best := float32(math.NaN())
				for yy := int(sy0); yy < int(math.Ceil(sy1)) && yy < hm.H; yy++ {
					for xx := int(sx0); xx < int(math.Ceil(sx1)) && xx < hm.W; xx++ {
						z := hm.at(xx, yy)
						if !math.IsNaN(float64(z)) && (math.IsNaN(float64(best)) || z > best) {
							best = z
						}
					}
				}
				zs[py*pw+px] = best
			}
		}
		for py := 0; py < ph; py++ {
			for px := 0; px < pw; px++ {
				z := zs[py*pw+px]
				o := py*pw + px
				if math.IsNaN(float64(z)) {
					pix[o] = colVoid
					continue
				}
				c := mix(colLow, colHigh, float64((z-hm.ZLo)/(hm.ZHi-hm.ZLo)))
				edge, step := false, false
				for _, d := range [][2]int{{1, 0}, {-1, 0}, {0, 1}, {0, -1}} {
					nx, ny := px+d[0], py+d[1]
					if nx < 0 || ny < 0 || nx >= pw || ny >= ph {
						edge = true
						continue
					}
					nz := zs[ny*pw+nx]
					if math.IsNaN(float64(nz)) {
						edge = true
					} else if math.Abs(float64(nz-z)) > 40 {
						step = true
					}
				}
				if edge {
					c = mix(c, colEdge, 0.75)
				} else if step {
					c = mix(c, colStep, 0.55)
				}
				pix[o] = c
			}
		}
		toPix = func(x, y float64) (float64, float64) {
			hx, hy := hm.toPix(x, y)
			return ox + hx*sc, oy + hy*sc
		}
	} else {
		// no bsp: fit the players, dotted grid
		first := m.bx0 == 0 && m.bx1 == 0
		for _, p := range ps {
			if first {
				m.bx0, m.bx1, m.by0, m.by1 = p.X-500, p.X+500, p.Y-500, p.Y+500
				first = false
			}
			m.bx0, m.bx1 = math.Min(m.bx0, p.X-200), math.Max(m.bx1, p.X+200)
			m.by0, m.by1 = math.Min(m.by0, p.Y-200), math.Max(m.by1, p.Y+200)
		}
		for i := range pix {
			pix[i] = colVoid
		}
		for py := 0; py < ph; py += 4 {
			for px := 0; px < pw; px += 4 {
				pix[py*pw+px] = colGrid
			}
		}
		dx, dy := math.Max(m.bx1-m.bx0, 1), math.Max(m.by1-m.by0, 1)
		sc := math.Min(float64(pw)/dx, float64(ph)/dy)
		ox, oy := (float64(pw)-dx*sc)/2, (float64(ph)-dy*sc)/2
		toPix = func(x, y float64) (float64, float64) { return ox + (x-m.bx0)*sc, oy + (m.by1-y)*sc }
	}
	type mark struct {
		r      rune
		col    rgb
		label  string
		lcol   rgb
	}
	marks := map[int]mark{}
	// view cones first, markers on top
	for _, p := range ps {
		if (p.Team != 1 && p.Team != 2) || !p.Alive || !m.cones {
			continue
		}
		cx, cy := toPix(p.X, p.Y)
		col := colCTLight
		if p.Team == 1 {
			col = colTLight
		}
		yaw := p.Yaw * math.Pi / 180
		R := 7.0
		for dy := -R; dy <= R; dy++ {
			for dx := -R; dx <= R; dx++ {
				d := math.Hypot(dx, dy)
				if d > R || d < 1 {
					continue
				}
				a := math.Atan2(-dy, dx) - yaw // screen y is flipped
				for a > math.Pi {
					a -= 2 * math.Pi
				}
				for a < -math.Pi {
					a += 2 * math.Pi
				}
				if math.Abs(a) > 0.55 {
					continue
				}
				x, y := int(cx+dx), int(cy+dy)
				if x < 0 || y < 0 || x >= pw || y >= ph {
					continue
				}
				pix[y*pw+x] = mix(pix[y*pw+x], col, 0.5*(1-d/R)+0.15)
			}
		}
	}
	for _, p := range ps {
		if p.Team != 1 && p.Team != 2 {
			continue
		}
		px, py := toPix(p.X, p.Y)
		cx, cy := int(px), int(py)/2
		if cx < 0 || cy < 0 || cx >= cw || cy >= ch {
			continue
		}
		col, lcol := colCT, colCTLight
		if p.Team == 1 {
			col, lcol = colT, colTLight
		}
		r := '●'
		if !p.Alive {
			r, col, lcol = []rune(gDeadM)[0], colDead, colDead
		}
		lab := ""
		if m.names && p.Alive {
			lab = p.Name
		}
		marks[cy*cw+cx] = mark{r: r, col: col, label: trunc(lab, 12), lcol: lcol}
	}
	// labels occupy the cells right of the marker
	labels := map[int]rune{}
	labelCol := map[int]rgb{}
	for o, mk := range marks {
		if mk.label == "" {
			continue
		}
		x, y := o%cw+1, o/cw
		for _, r := range mk.label {
			if x >= cw {
				break
			}
			if _, taken := marks[y*cw+x]; taken {
				break
			}
			labels[y*cw+x] = r
			labelCol[y*cw+x] = mk.lcol
			x++
		}
	}
	var b strings.Builder
	for cy := 0; cy < ch; cy++ {
		for cx := 0; cx < cw; cx++ {
			top, bot := pix[(cy*2)*pw+cx], pix[(cy*2+1)*pw+cx]
			o := cy*cw + cx
			if mk, ok := marks[o]; ok {
				b.WriteString(bg(mix(top, bot, 0.5)) + fg(mk.col) + "\x1b[1m" + string(mk.r) + "\x1b[22m")
				continue
			}
			if r, ok := labels[o]; ok {
				b.WriteString(bg(mix(mix(top, bot, 0.5), colVoid, 0.55)) + fg(labelCol[o]) + string(r))
				continue
			}
			b.WriteString(fg(top) + bg(bot) + "▀")
		}
		b.WriteString("\x1b[0m")
		if cy < ch-1 {
			b.WriteString("\n")
		}
	}
	return b.String()
}

// folders searched for <map>.bsp: xcmd's own, the configured one, the default server, common CS 1.6 client installs
func mapDirs(cfg *Config) []string {
	d := []string{}
	if cfg.MapsDir != "" {
		d = append(d, cfg.MapsDir, filepath.Join(cfg.MapsDir, "maps"))
	}
	d = append(d, filepath.Join(homeDir(), "maps"), filepath.Join(cfg.DefaultDir, "cstrike", "maps"))
	h, _ := os.UserHomeDir()
	for _, steam := range []string{`C:\Program Files (x86)\Steam`, `C:\Program Files\Steam`, `D:\Steam`, `D:\SteamLibrary`, `E:\SteamLibrary`,
		filepath.Join(h, ".steam", "steam"), filepath.Join(h, ".local", "share", "Steam")} {
		for _, sub := range []string{"cstrike", "cstrike_downloads"} {
			d = append(d, filepath.Join(steam, "steamapps", "common", "Half-Life", sub, "maps"))
		}
	}
	for _, g := range []string{`C:\Games\Counter-Strike 1.6`, `C:\Program Files (x86)\Counter-Strike 1.6`, `C:\Program Files\Counter-Strike 1.6`, `D:\Counter-Strike 1.6`, `D:\Games\Counter-Strike 1.6`} {
		d = append(d, filepath.Join(g, "cstrike", "maps"), filepath.Join(g, "cstrike_downloads", "maps"))
	}
	return d
}
