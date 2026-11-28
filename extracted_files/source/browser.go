package main

// Global server browser: asks the Steam master servers for every Counter-Strike 1.6 server, queries them all
// (A2S_INFO + A2S_RULES) and lists the ones running xmod (they publish xmod_* cvars).

import (
	"fmt"
	"sort"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"github.com/charmbracelet/bubbles/textinput"
	tea "github.com/charmbracelet/bubbletea"
)

type bEntry struct {
	info  *ServerInfo
	xmod  string // version or "yes"
	rules map[string]string
}

type browser struct {
	a        *App
	mu       sync.Mutex
	scanning bool
	phase    string
	total    int64
	done     int64
	answered int64
	list     []*bEntry
	errs     []string
	showAll  bool
	sel      int
	off      int
	filter   textinput.Model
	filtering bool
	started  time.Time
	status   string
}

func newBrowser(a *App) *browser {
	ti := textinput.New()
	ti.Prompt = "/ "
	ti.Placeholder = "filter by name, map or ip"
	return &browser{a: a, filter: ti}
}

func (b *browser) Capturing() bool { return b.filtering }
func (b *browser) Help() string {
	if b.filtering {
		return keyHelp("enter", "apply", "esc", "clear")
	}
	return keyHelp("s", "scan the world", "a", "xmod only / all", "/", "filter", "enter", "add to monitor")
}

func (b *browser) scan() {
	b.mu.Lock()
	if b.scanning {
		b.mu.Unlock()
		return
	}
	b.scanning, b.list, b.errs, b.total, b.done, b.answered = true, nil, nil, 0, 0, 0
	b.phase, b.started = "asking the Steam master servers…", time.Now()
	b.mu.Unlock()
	go func() {
		seen := map[string]bool{}
		var addrs []string
		for _, m := range b.a.cfg.Masters {
			err := MasterQuery(m, `\appid\10\gamedir\cstrike`, 60, func(page []string) {
				for _, a := range page {
					if !seen[a] {
						seen[a] = true
						addrs = append(addrs, a)
					}
				}
				b.mu.Lock()
				b.phase = fmt.Sprintf("master list: %d servers…", len(addrs))
				b.mu.Unlock()
			})
			if err != nil {
				b.mu.Lock()
				b.errs = append(b.errs, err.Error())
				b.mu.Unlock()
				continue
			}
			if len(addrs) > 0 {
				break
			}
		}
		atomic.StoreInt64(&b.total, int64(len(addrs)))
		b.mu.Lock()
		b.phase = fmt.Sprintf("querying %d servers…", len(addrs))
		b.mu.Unlock()
		jobs := make(chan string)
		var wg sync.WaitGroup
		for i := 0; i < 160; i++ {
			wg.Add(1)
			go func() {
				defer wg.Done()
				for a := range jobs {
					inf, err := A2SInfo(a, 1200*time.Millisecond)
					if err == nil {
						atomic.AddInt64(&b.answered, 1)
						e := &bEntry{info: inf}
						if r, err := A2SRules(a, 1500*time.Millisecond); err == nil {
							e.rules = r
							for k, v := range r {
								if strings.HasPrefix(strings.ToLower(k), "xmod_") {
									if e.xmod == "" || k == "xmod_version" {
										e.xmod = "yes"
										if k == "xmod_version" {
											e.xmod = v
										}
									}
								}
							}
						}
						b.mu.Lock()
						b.list = append(b.list, e)
						b.mu.Unlock()
					}
					atomic.AddInt64(&b.done, 1)
				}
			}()
		}
		for _, a := range addrs {
			jobs <- a
		}
		close(jobs)
		wg.Wait()
		b.mu.Lock()
		b.scanning = false
		x := 0
		for _, e := range b.list {
			if e.xmod != "" {
				x++
			}
		}
		b.phase = fmt.Sprintf("done in %s: %d servers answered, %d run xmod", time.Since(b.started).Round(time.Second), b.answered, x)
		b.mu.Unlock()
	}()
}

func (b *browser) visible() []*bEntry {
	q := strings.ToLower(strings.TrimSpace(b.filter.Value()))
	var out []*bEntry
	for _, e := range b.list {
		if !b.showAll && e.xmod == "" {
			continue
		}
		if q != "" && !strings.Contains(strings.ToLower(e.info.Name+" "+e.info.Map+" "+e.info.Addr), q) {
			continue
		}
		out = append(out, e)
	}
	sort.Slice(out, func(i, j int) bool {
		if (out[i].xmod != "") != (out[j].xmod != "") {
			return out[i].xmod != ""
		}
		if out[i].info.Players != out[j].info.Players {
			return out[i].info.Players > out[j].info.Players
		}
		return out[i].info.Name < out[j].info.Name
	})
	return out
}

func (b *browser) Update(msg tea.Msg, a *App) tea.Cmd {
	switch k := msg.(type) {
	case tea.KeyMsg:
		if b.filtering {
			switch k.String() {
			case "esc":
				b.filter.SetValue("")
				b.filtering = false
				return nil
			case "enter":
				b.filtering = false
				return nil
			}
			var c tea.Cmd
			b.filter, c = b.filter.Update(msg)
			return c
		}
		b.mu.Lock()
		n := len(b.visible())
		b.mu.Unlock()
		switch k.String() {
		case "s", "r":
			b.scan()
		case "a":
			b.showAll = !b.showAll
			b.sel = 0
		case "/":
			b.filtering = true
			return b.filter.Focus()
		case "up", "k":
			if b.sel > 0 {
				b.sel--
			}
		case "down", "j":
			if b.sel < n-1 {
				b.sel++
			}
		case "pgdown":
			b.sel += 10
			if b.sel >= n {
				b.sel = n - 1
			}
		case "pgup":
			b.sel -= 10
			if b.sel < 0 {
				b.sel = 0
			}
		case "enter":
			b.mu.Lock()
			v := b.visible()
			b.mu.Unlock()
			if b.sel < len(v) {
				e := v[b.sel]
				sv := SavedServer{Addr: e.info.Addr, Name: e.info.Name}
				a.cfg.AddServer(sv)
				a.cfg.Save()
				if sharedPool != nil {
					sharedPool.list = append(sharedPool.list, &srv{SavedServer: sv, hist: map[string][]float64{}})
				}
				b.status = "added " + e.info.Name + " to the monitor"
			}
		}
	}
	return nil
}

func (b *browser) View(w, h int) string {
	b.mu.Lock()
	defer b.mu.Unlock()
	var out []string
	title := sSub.Render("Global server browser") + sMuted.Render("  — Counter-Strike 1.6 servers that run xmod")
	out = append(out, " "+title)
	if b.scanning || b.total > 0 || b.phase != "" {
		frac := 0.0
		if b.total > 0 {
			frac = float64(atomic.LoadInt64(&b.done)) / float64(b.total)
		}
		x := 0
		for _, e := range b.list {
			if e.xmod != "" {
				x++
			}
		}
		out = append(out, " "+gradBar(frac, 40)+" "+sText.Render(fmt.Sprintf("%3.0f%%", frac*100))+"  "+sMuted.Render(b.phase)+
			"  "+sOK.Render(fmt.Sprintf("● %d xmod", x))+sMuted.Render(fmt.Sprintf(" / %d answered", atomic.LoadInt64(&b.answered))))
	} else {
		out = append(out, " "+sMuted.Render("Press ")+sKey.Render("s")+sMuted.Render(" to scan every CS 1.6 server in the world (takes ~1 minute)."))
	}
	for _, e := range b.errs {
		out = append(out, " "+sWarn.Render("! "+trunc(e, w-6)))
	}
	if b.status != "" {
		out = append(out, " "+sOK.Render(b.status))
	}
	if b.filtering || b.filter.Value() != "" {
		out = append(out, " "+b.filter.View())
	}
	v := b.visible()
	cols := []struct {
		t string
		w int
	}{{"", 2}, {"NAME", 38}, {"ADDRESS", 22}, {"MAP", 16}, {"PLAYERS", 16}, {"PING", 7}, {"XMOD", 9}, {"OS", 8}}
	var hb strings.Builder
	for _, c := range cols {
		hb.WriteString(pad(sHeader.Render(c.t), c.w) + " ")
	}
	rows := []string{hb.String()}
	room := h - len(out) - 4
	if room < 3 {
		room = 3
	}
	if b.sel < b.off {
		b.off = b.sel
	}
	if b.sel >= b.off+room {
		b.off = b.sel - room + 1
	}
	for i := b.off; i < len(v) && i < b.off+room; i++ {
		e := v[i]
		dot := sDim.Render("○")
		xm := sDim.Render("-")
		if e.xmod != "" {
			dot, xm = sOK.Render("●"), sSub.Render(e.xmod)
		}
		f := 0.0
		if e.info.MaxPlayers > 0 {
			f = float64(e.info.Players) / float64(e.info.MaxPlayers)
		}
		cells := []string{dot, trunc(e.info.Name, 38), e.info.Addr, trunc(e.info.Map, 16), bar(f, 8, cAccent) + fmt.Sprintf(" %2d/%-2d", e.info.Players, e.info.MaxPlayers),
			fmt.Sprintf("%dms", e.info.Ping.Milliseconds()), xm, osName(e.info.OS)}
		var rb strings.Builder
		for j, c := range cols {
			rb.WriteString(pad(cells[j], c.w) + " ")
		}
		line := rb.String()
		if i == b.sel {
			line = sRowSel.Width(w - 4).Render(stripStyleKeepWidth(line, w-4))
		}
		rows = append(rows, line)
	}
	if len(v) == 0 && !b.scanning && b.total > 0 {
		rows = append(rows, sMuted.Render("no xmod servers found yet - press a to see all servers"))
	}
	out = append(out, sPanel.Width(w-2).Render(strings.Join(rows, "\n")))
	return strings.Join(out, "\n")
}
