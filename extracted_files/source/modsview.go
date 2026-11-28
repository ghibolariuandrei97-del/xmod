package main

// Mods tab: the package library (search, install, update, enable/disable, remove) for the default server.

import (
	"fmt"
	"strings"
	"sync"

	"github.com/charmbracelet/bubbles/textinput"
	tea "github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/lipgloss"
)

type modsView struct {
	a         *App
	mu        sync.Mutex
	cat       *Catalog
	loading   bool
	sel       int
	search    textinput.Model
	searching bool
	status    string
}

func newModsView(a *App) *modsView {
	ti := textinput.New()
	ti.Prompt = "/ "
	ti.Placeholder = "search mods, tags…"
	return &modsView{a: a, search: ti}
}

func (m *modsView) Capturing() bool { return m.searching }
func (m *modsView) Help() string {
	if m.searching {
		return keyHelp("enter", "ok", "esc", "clear")
	}
	return keyHelp("i", "install", "u", "update", "e", "enable/disable", "x", "remove", "/", "search")
}

func (m *modsView) list() []*Pkg {
	if m.cat == nil {
		return nil
	}
	q := strings.ToLower(strings.TrimSpace(m.search.Value()))
	var out []*Pkg
	for _, p := range m.cat.Sorted() {
		if q != "" && !strings.Contains(strings.ToLower(p.Name+" "+p.Title+" "+p.Description+" "+strings.Join(p.Tags, " ")), q) {
			continue
		}
		out = append(out, p)
	}
	return out
}

func (m *modsView) Update(msg tea.Msg, a *App) tea.Cmd {
	switch k := msg.(type) {
	case frameMsg:
		m.mu.Lock()
		if m.cat == nil && !m.loading && a.cur == "Mods" {
			m.loading = true
			go func() {
				c := loadCatalog(a.cfg)
				m.mu.Lock()
				m.cat, m.loading = c, false
				m.mu.Unlock()
			}()
		}
		m.mu.Unlock()
	case tea.KeyMsg:
		if m.searching {
			switch k.String() {
			case "esc":
				m.search.SetValue("")
				m.searching = false
				return nil
			case "enter":
				m.searching = false
				return nil
			}
			var c tea.Cmd
			m.search, c = m.search.Update(msg)
			m.sel = 0
			return c
		}
		m.mu.Lock()
		l := m.list()
		m.mu.Unlock()
		var p *Pkg
		if m.sel < len(l) {
			p = l[m.sel]
		}
		dir := a.cfg.DefaultDir
		switch k.String() {
		case "up", "k":
			if m.sel > 0 {
				m.sel--
			}
		case "down", "j":
			if m.sel < len(l)-1 {
				m.sel++
			}
		case "/":
			m.searching = true
			return m.search.Focus()
		case "i", "enter":
			if p != nil {
				a.exec = []string{"install", p.Name, "--dir", dir}
				a.quit = true
			}
		case "u":
			if p != nil {
				a.exec = []string{"update", p.Name, "--dir", dir}
				a.quit = true
			}
		case "e":
			if p != nil {
				st := loadState(dir)
				if ip := st.Packages[p.Name]; ip != nil {
					if err := setEnabled(dir, st, p.Name, !ip.Enabled); err != nil {
						m.status = sErr.Render(err.Error())
					} else if ip.Enabled {
						m.status = sOK.Render(p.Name+" enabled") + sMuted.Render(" (restart the map / server)")
					} else {
						m.status = sWarn.Render(p.Name+" disabled") + sMuted.Render(" (restart the map / server)")
					}
				} else {
					m.status = sMuted.Render(p.Name + " is not installed")
				}
			}
		case "x", "delete":
			if p != nil {
				st := loadState(dir)
				kept, err := removePkg(dir, st, p.Name)
				if err != nil {
					m.status = sErr.Render(err.Error())
				} else {
					m.status = sOK.Render(p.Name + " removed")
					if len(kept) > 0 {
						m.status += sMuted.Render(fmt.Sprintf(" (kept %d edited config file(s))", len(kept)))
					}
				}
			}
		}
	}
	return nil
}

func pkgStatus(p *Pkg, st *State) (string, string) {
	ip := st.Packages[p.Name]
	switch {
	case ip != nil && !ip.Enabled:
		return sWarn.Render(gOff), "disabled"
	case ip != nil && p.Upstream == "" && p.Version != "" && ip.Version != p.Version && p.Builtin == "" && p.Name != "agent":
		return sSub.Render("↑"), "update"
	case ip != nil:
		return sOK.Render(gOK), "installed"
	case p.Builtin != "" && len(p.Metamod) == 0 && st.Packages[p.Builtin] != nil:
		return sOK.Render(gOK), "included in " + p.Builtin
	case p.Name == "hlds" && hldsInstalled(st.Dir):
		return sOK.Render(gOK), "installed"
	case p.Available:
		return sText.Render("○"), "available"
	}
	return sDim.Render("·"), "not published"
}

var kindStyle = map[string]lipgloss.Color{"game": "#A78BFA", "engine": "#60A5FA", "core": "#2DD4BF", "extension": "#818CF8", "mod": "#FB923C", "plugin": "#FBBF24", "tool": "#9CA3AF"}

func (m *modsView) View(w, h int) string {
	m.mu.Lock()
	defer m.mu.Unlock()
	dir := m.a.cfg.DefaultDir
	head := " " + sSub.Render("Library") + sMuted.Render("  server: ") + sText.Render(dir)
	if m.cat == nil {
		return head + "\n\n  " + sMuted.Render("loading the catalog from GitHub "+m.a.cfg.Repo+" …")
	}
	head += sMuted.Render("  source: " + m.cat.Source)
	st := loadState(dir)
	l := m.list()
	if m.sel >= len(l) {
		m.sel = len(l) - 1
	}
	if m.sel < 0 {
		m.sel = 0
	}
	leftW := w * 55 / 100
	var rows []string
	for i, p := range l {
		icon, stat := pkgStatus(p, st)
		ver := p.Version
		if ip := st.Packages[p.Name]; ip != nil && ip.Version != p.Version && p.Version != "" {
			ver = ip.Version + " → " + p.Version
		} else if ip != nil {
			ver = ip.Version
		}
		kc := kindStyle[p.Kind]
		kind := lipgloss.NewStyle().Foreground(kc).Render(pad(p.Kind, 11))
		line := icon + " " + pad(sText.Bold(true).Render(trunc(p.Display(), 22)), 23) + kind + pad(sMuted.Render(trunc(ver, 20)), 21) + sDim.Render(stat)
		if i == m.sel {
			line = sRowSel.Width(leftW - 2).Render(stripStyleKeepWidth(line, leftW-2))
		}
		rows = append(rows, line)
	}
	if m.searching || m.search.Value() != "" {
		rows = append([]string{m.search.View(), ""}, rows...)
	}
	left := sPanel.Width(leftW).Height(h - 4).Render(strings.Join(clip(rows, h-5), "\n"))
	// details
	var d []string
	if m.sel < len(l) {
		p := l[m.sel]
		icon, stat := pkgStatus(p, st)
		d = append(d, sTitle.Render(p.Display())+"  "+sBadge.Render(p.Kind)+"  "+icon+" "+sMuted.Render(stat), "")
		d = append(d, wrap(p.Description, w-leftW-8)...)
		d = append(d, "")
		kv := func(k, v string) {
			if v != "" {
				d = append(d, pad(sMuted.Render(k), 12)+sText.Render(v))
			}
		}
		kv("name", p.Name)
		kv("latest", p.Version)
		if ip := st.Packages[p.Name]; ip != nil {
			kv("installed", ip.Version+"  ("+ip.When.Format("2006-01-02 15:04")+")")
			kv("files", fmt.Sprint(len(ip.Files)))
			if len(ip.Requires) > 0 {
				kv("uses", strings.Join(uniq(ip.Requires), ", "))
			}
		}
		kv("requires", strings.Join(p.Requires, ", "))
		if p.Builtin != "" {
			kv("comes with", p.Builtin)
		}
		if len(p.Metamod) > 0 {
			kv("metamod", strings.Join(p.Metamod, ", "))
		}
		if p.Namespace != "" {
			kv("namespace", "xmod."+p.Namespace)
		}
		if p.Upstream != "" {
			kv("from", "github.com/"+p.Upstream+" (latest release)")
		} else if p.File != "" {
			kv("file", p.File)
		}
		if p.Size > 0 {
			kv("size", humanBytes(p.Size))
		}
		if len(p.Tags) > 0 {
			kv("tags", strings.Join(p.Tags, " · "))
		}
	}
	for _, n := range m.cat.Notes {
		d = append(d, "", sWarn.Render("! "+trunc(n, w-leftW-10)))
	}
	if m.status != "" {
		d = append(d, "", m.status)
	}
	right := sPanel.Width(w - leftW - 4).Height(h - 4).Render(strings.Join(clip(d, h-5), "\n"))
	return head + "\n" + lipgloss.JoinHorizontal(lipgloss.Top, left, right)
}

func wrap(s string, w int) []string {
	if w < 10 {
		w = 10
	}
	var out []string
	line := ""
	for _, word := range strings.Fields(s) {
		if lipgloss.Width(line)+len(word)+1 > w && line != "" {
			out = append(out, sText.Render(line))
			line = ""
		}
		if line != "" {
			line += " "
		}
		line += word
	}
	if line != "" {
		out = append(out, sText.Render(line))
	}
	return out
}

func uniq(l []string) []string {
	seen := map[string]bool{}
	var o []string
	for _, x := range l {
		if !seen[x] {
			seen[x] = true
			o = append(o, x)
		}
	}
	return o
}
