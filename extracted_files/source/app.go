package main

// The full-screen app: tabs Home · Monitor · Map · Mods · Browser sharing one window.

import (
	"strings"
	"time"

	tea "github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/lipgloss"
)

type tabModel interface {
	Update(msg tea.Msg, a *App) tea.Cmd
	View(w, h int) string
	Help() string
	Capturing() bool // a text input has focus: keys are not global shortcuts
}

type App struct {
	cfg    *Config
	tabs   []string
	models map[string]tabModel
	cur    string
	w, h   int
	quit   bool
	exec   []string // command to run after the TUI exits (setup from Home)
}

type frameMsg time.Time

func frameTick() tea.Cmd {
	return tea.Tick(250*time.Millisecond, func(t time.Time) tea.Msg { return frameMsg(t) })
}

func newApp(cfg *Config, start string) *App {
	a := &App{cfg: cfg, tabs: []string{"Home", "Monitor", "Map", "Mods", "Browser"}, models: map[string]tabModel{}, cur: start, w: 120, h: 40}
	a.models["Home"] = newHome(a)
	a.models["Monitor"] = newMonitor(a)
	a.models["Map"] = newMapView(a)
	a.models["Mods"] = newModsView(a)
	a.models["Browser"] = newBrowser(a)
	return a
}

func (a *App) Init() tea.Cmd { return tea.Batch(frameTick(), tea.SetWindowTitle("xcmd")) }

func (a *App) switchTo(tab string) { a.cur = tab }

func (a *App) Update(msg tea.Msg) (tea.Model, tea.Cmd) {
	switch m := msg.(type) {
	case tea.WindowSizeMsg:
		a.w, a.h = m.Width, m.Height
	case tea.KeyMsg:
		capt := a.models[a.cur].Capturing()
		k := m.String()
		if k == "ctrl+c" {
			return a, tea.Quit
		}
		if !capt {
			switch k {
			case "q":
				return a, tea.Quit
			case "tab":
				a.cur = a.tabs[(a.idx()+1)%len(a.tabs)]
				return a, nil
			case "shift+tab":
				a.cur = a.tabs[(a.idx()+len(a.tabs)-1)%len(a.tabs)]
				return a, nil
			case "f1", "f2", "f3", "f4", "f5", "1", "2", "3", "4", "5":
				i := int(k[len(k)-1] - '1')
				if i < len(a.tabs) {
					a.cur = a.tabs[i]
				}
				return a, nil
			}
		}
	case frameMsg:
		var cmds []tea.Cmd
		for _, t := range a.tabs { // every tab keeps polling in the background
			if c := a.models[t].Update(m, a); c != nil {
				cmds = append(cmds, c)
			}
		}
		cmds = append(cmds, frameTick())
		if a.quit {
			return a, tea.Quit
		}
		return a, tea.Batch(cmds...)
	}
	cmd := a.models[a.cur].Update(msg, a)
	if a.quit {
		return a, tea.Quit
	}
	return a, cmd
}

func (a *App) idx() int {
	for i, t := range a.tabs {
		if t == a.cur {
			return i
		}
	}
	return 0
}

func (a *App) View() string {
	// tab bar
	var tabs []string
	for i, t := range a.tabs {
		label := " " + string(rune('1'+i)) + " " + t + " "
		if t == a.cur {
			tabs = append(tabs, lipgloss.NewStyle().Foreground(lipgloss.Color("#0B1220")).Background(cAccent).Bold(true).Render(label))
		} else {
			tabs = append(tabs, lipgloss.NewStyle().Foreground(cMuted).Background(lipgloss.Color("#1F2937")).Render(label))
		}
	}
	brand := sTitle.Render(" ◆ xcmd ") + sDim.Render(xcmdVersion+" ")
	clock := sMuted.Render(time.Now().Format("15:04:05") + " ")
	bar := brand + strings.Join(tabs, " ")
	gap := a.w - lipgloss.Width(bar) - lipgloss.Width(clock)
	if gap < 0 {
		gap = 0
	}
	top := bar + strings.Repeat(" ", gap) + clock
	m := a.models[a.cur]
	help := keyHelp("tab", "switch", "q", "quit") + "  " + m.Help()
	body := m.View(a.w, a.h-3)
	// clamp body height
	lines := strings.Split(body, "\n")
	maxL := a.h - 3
	if maxL < 1 {
		maxL = 1
	}
	if len(lines) > maxL {
		lines = lines[:maxL]
	}
	for len(lines) < maxL {
		lines = append(lines, "")
	}
	return top + "\n" + strings.Join(lines, "\n") + "\n" + trunc2(help, a.w)
}

func trunc2(s string, w int) string {
	if lipgloss.Width(s) <= w {
		return s
	}
	return lipgloss.NewStyle().MaxWidth(w).Render(s)
}

func runApp(cfg *Config, tab string) ([]string, error) {
	a := newApp(cfg, tab)
	p := tea.NewProgram(a, tea.WithAltScreen(), tea.WithMouseCellMotion())
	_, err := p.Run()
	return a.exec, err
}
