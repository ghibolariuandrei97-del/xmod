package main

import (
	"path/filepath"

	"github.com/charmbracelet/bubbles/textinput"
	"fmt"
	"runtime"
	"strings"

	tea "github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/lipgloss"
)

type homeItem struct {
	key, title, desc string
	exec             []string
	tab              string
}

type home struct {
	a       *App
	sel     int
	items   []homeItem
	editing bool
	input   textinput.Model
	status  string
}

func newHome(a *App) *home {
	ti := textinput.New()
	ti.Prompt = "folder › "
	ti.CharLimit = 260
	h := &home{a: a, input: ti}
	h.build()
	return h
}

func (h *home) build() {
	d := h.a.cfg.DefaultDir
	h.items = nil
	for i, p := range profiles {
		h.items = append(h.items, homeItem{string(rune('a' + i)), p.Title, p.Desc, []string{"setup", p.Key, "--dir", d, "--zip"}, ""})
	}
	h.items = append(h.items,
		homeItem{"u", "Update the folder", "newest releases + a zip with only the changed files", []string{"update", "--dir", d, "--zip"}, ""},
		homeItem{"f", "Change folder", "where xcmd builds the files (upload it to the server by FTP)", nil, "folder"},
		homeItem{"m", "Live monitor", "servers, players, money, weapons, CPU, RAM", nil, "Monitor"},
		homeItem{"t", "Tactical map", "live top-down match view", nil, "Map"},
		homeItem{"l", "Mod library", "search, install, enable / disable", nil, "Mods"},
		homeItem{"w", "Server browser", "every xmod server in the world", nil, "Browser"},
		homeItem{"d", "Doctor", "check the folder and this PC", []string{"doctor", "--dir", d}, ""})
}

func (h *home) Capturing() bool { return h.editing }
func (h *home) Help() string {
	if h.editing {
		return keyHelp("enter", "save", "esc", "cancel")
	}
	return keyHelp("↑↓", "select", "enter", "run", "a-e", "build a profile")
}

func (h *home) Update(msg tea.Msg, a *App) tea.Cmd {
	k, ok := msg.(tea.KeyMsg)
	if !ok {
		return nil
	}
	if h.editing {
		switch k.String() {
		case "esc":
			h.editing = false
			return nil
		case "enter":
			v := strings.Trim(strings.TrimSpace(h.input.Value()), `"`)
			if v != "" {
				if abs, err := filepath.Abs(v); err == nil {
					v = abs
				}
				a.cfg.DefaultDir = v
				a.cfg.Save()
				h.build()
				h.status = "folder: " + v
			}
			h.editing = false
			return nil
		}
		var c tea.Cmd
		h.input, c = h.input.Update(msg)
		return c
	}
	run := func(it homeItem) tea.Cmd {
		if it.tab == "folder" {
			h.editing = true
			h.input.SetValue(a.cfg.DefaultDir)
			h.input.CursorEnd()
			return h.input.Focus()
		}
		if it.tab != "" {
			a.switchTo(it.tab)
			return nil
		}
		a.exec = it.exec
		a.quit = true
		return nil
	}
	switch k.String() {
	case "up", "k":
		if h.sel > 0 {
			h.sel--
		}
	case "down", "j":
		if h.sel < len(h.items)-1 {
			h.sel++
		}
	case "enter":
		return run(h.items[h.sel])
	default:
		for i, it := range h.items {
			if k.String() == it.key {
				h.sel = i
				return run(it)
			}
		}
	}
	return nil
}

func (h *home) View(w, hh int) string {
	dir := h.a.cfg.DefaultDir
	st := loadState(dir)
	left := lipgloss.JoinVertical(lipgloss.Center, logo(), "", sMuted.Render("SteamCMD for xmod · installer · monitor · tactical map · mods · browser"), "")
	// server status
	var s []string
	s = append(s, sSub.Render("Folder")+"  "+sText.Render(dir))
	comp := func(name, label string) {
		icon, ver := sDim.Render("○"), sDim.Render("not installed")
		if name == "hlds" && hldsInstalled(dir) {
			icon, ver = sOK.Render(gOK), sMuted.Render("app 90")
		} else if ip := st.Packages[name]; ip != nil {
			icon, ver = sOK.Render(gOK), sMuted.Render(ip.Version)
			if !ip.Enabled {
				icon, ver = sWarn.Render(gOff), sWarn.Render(ip.Version+" (disabled)")
			}
		}
		s = append(s, "  "+icon+" "+pad(sText.Render(label), 16)+ver)
	}
	if hldsInstalled(dir) {
		comp("hlds", "HLDS / CS 1.6")
	}
	comp("rehlds", "ReHLDS")
	comp("regamedll", "ReGameDLL")
	comp("metamod", "Metamod-R")
	comp("xmod", "xmod")
	comp("agent", "xcmd agent")
	var extra []string
	for _, n := range st.Names() {
		switch n {
		case "hlds", "rehlds", "regamedll", "metamod", "xmod", "agent":
		default:
			extra = append(extra, n)
		}
	}
	if len(extra) > 0 {
		s = append(s, "  "+sMuted.Render("+ "+strings.Join(extra, ", ")))
	}
	s = append(s, "", sMuted.Render(fmt.Sprintf("%s/%s · %d saved servers · repo %s", runtime.GOOS, runtime.GOARCH, len(h.a.cfg.Servers), h.a.cfg.Repo)))
	status := sPanel.Render(strings.Join(s, "\n"))
	// actions (descriptions shortened to the window)
	dw := w - lipgloss.Width(status) - 2 - 4 - 2 - 38 - 2
	if dw < 10 {
		dw = 10
	}
	var acts []string
	for i, it := range h.items {
		line := sKey.Render(it.key) + " " + pad(sText.Bold(true).Render(it.title), 38) + sMuted.Render(trunc(it.desc, dw))
		if i == h.sel {
			line = sRowSel.Render(stripANSI(" " + it.key + "  " + pad(it.title, 38) + trunc(it.desc, dw) + " "))
		}
		if i == len(profiles) {
			acts = append(acts, "")
		}
		acts = append(acts, line)
	}
	if h.editing {
		acts = append(acts, "", h.input.View())
	} else if h.status != "" {
		acts = append(acts, "", sOK.Render(h.status))
	}
	actions := sPanelHi.Render(sSub.Render("Build · update · watch") + "\n" + strings.Join(acts, "\n"))
	body := lipgloss.JoinVertical(lipgloss.Center, left, lipgloss.JoinHorizontal(lipgloss.Top, status, "  ", actions))
	return lipgloss.Place(w, hh, lipgloss.Center, lipgloss.Center, body)
}
