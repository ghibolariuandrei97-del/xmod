package main

// Visual identity of xcmd: dark panels, teal accent, CT blue / T orange, money green, HP red.

import (
	"fmt"
	"strings"

	"github.com/charmbracelet/lipgloss"
	"github.com/charmbracelet/x/ansi"
)

// never ask the terminal for its background colour (old consoles never answer: no startup delay)
func init() { lipgloss.SetHasDarkBackground(true) }

var (
	cAccent  = lipgloss.Color("#2DD4BF")
	cAccent2 = lipgloss.Color("#818CF8")
	cText    = lipgloss.Color("#E5E7EB")
	cMuted   = lipgloss.Color("#6B7280")
	cDim     = lipgloss.Color("#4B5563")
	cBorder  = lipgloss.Color("#374151")
	cPanel   = lipgloss.Color("#111827")
	cOK      = lipgloss.Color("#4ADE80")
	cWarn    = lipgloss.Color("#FBBF24")
	cErr     = lipgloss.Color("#F87171")
	cCT      = lipgloss.Color("#60A5FA")
	cT       = lipgloss.Color("#FB923C")
	cMoney   = lipgloss.Color("#86EFAC")
	cHP      = lipgloss.Color("#F87171")
	cAP      = lipgloss.Color("#93C5FD")
	cSel     = lipgloss.Color("#1F2937")
)

var (
	sTitle   = lipgloss.NewStyle().Foreground(cAccent).Bold(true)
	sSub     = lipgloss.NewStyle().Foreground(cAccent2).Bold(true)
	sText    = lipgloss.NewStyle().Foreground(cText)
	sMuted   = lipgloss.NewStyle().Foreground(cMuted)
	sDim     = lipgloss.NewStyle().Foreground(cDim)
	sOK      = lipgloss.NewStyle().Foreground(cOK)
	sWarn    = lipgloss.NewStyle().Foreground(cWarn)
	sErr     = lipgloss.NewStyle().Foreground(cErr)
	sCT      = lipgloss.NewStyle().Foreground(cCT).Bold(true)
	sT       = lipgloss.NewStyle().Foreground(cT).Bold(true)
	sMoney   = lipgloss.NewStyle().Foreground(cMoney)
	sHP      = lipgloss.NewStyle().Foreground(cHP)
	sAP      = lipgloss.NewStyle().Foreground(cAP)
	sKey     = lipgloss.NewStyle().Foreground(lipgloss.Color("#0B1220")).Background(cAccent).Padding(0, 1).Bold(true)
	sKeyDesc = lipgloss.NewStyle().Foreground(cMuted).Padding(0, 1)
	sBadge   = lipgloss.NewStyle().Foreground(lipgloss.Color("#0B1220")).Background(cAccent2).Padding(0, 1).Bold(true)
	sPanel   = lipgloss.NewStyle().Border(lipgloss.RoundedBorder()).BorderForeground(cBorder).Padding(0, 1)
	sPanelHi = lipgloss.NewStyle().Border(lipgloss.RoundedBorder()).BorderForeground(cAccent).Padding(0, 1)
	sHeader  = lipgloss.NewStyle().Foreground(cMuted).Bold(true)
	sRowSel  = lipgloss.NewStyle().Background(cSel).Foreground(cText).Bold(true)
)

const logoArt = `██╗  ██╗ ██████╗███╗   ███╗██████╗
╚██╗██╔╝██╔════╝████╗ ████║██╔══██╗
 ╚███╔╝ ██║     ██╔████╔██║██║  ██║
 ██╔██╗ ██║     ██║╚██╔╝██║██║  ██║
██╔╝ ██╗╚██████╗██║ ╚═╝ ██║██████╔╝
╚═╝  ╚═╝ ╚═════╝╚═╝     ╚═╝╚═════╝`

// gradient between two hex colours
func lerpHex(a, b string, t float64) lipgloss.Color {
	var r1, g1, b1, r2, g2, b2 int
	fmt.Sscanf(a, "#%02x%02x%02x", &r1, &g1, &b1)
	fmt.Sscanf(b, "#%02x%02x%02x", &r2, &g2, &b2)
	mix := func(x, y int) int { return x + int(float64(y-x)*t) }
	return lipgloss.Color(fmt.Sprintf("#%02X%02X%02X", mix(r1, r2), mix(g1, g2), mix(b1, b2)))
}

func logo() string {
	lines := strings.Split(logoArt, "\n")
	var out []string
	for _, l := range lines {
		rs := []rune(l)
		var b strings.Builder
		for i, r := range rs {
			t := float64(i) / float64(len(rs))
			b.WriteString(lipgloss.NewStyle().Foreground(lerpHex("#2DD4BF", "#818CF8", t)).Render(string(r)))
		}
		out = append(out, b.String())
	}
	return strings.Join(out, "\n")
}

// a coloured horizontal bar: [██████░░░░]
func bar(frac float64, width int, col lipgloss.Color) string {
	if frac < 0 {
		frac = 0
	}
	if frac > 1 {
		frac = 1
	}
	n := int(frac*float64(width) + 0.5)
	return lipgloss.NewStyle().Foreground(col).Render(strings.Repeat("█", n)) + sDim.Render(strings.Repeat("░", width-n))
}

// gradient progress bar (teal -> indigo)
func gradBar(frac float64, width int) string {
	if frac < 0 {
		frac = 0
	}
	if frac > 1 {
		frac = 1
	}
	n := int(frac*float64(width) + 0.5)
	var b strings.Builder
	for i := 0; i < width; i++ {
		if i < n {
			b.WriteString(lipgloss.NewStyle().Foreground(lerpHex("#2DD4BF", "#818CF8", float64(i)/float64(width))).Render("━"))
		} else {
			b.WriteString(sDim.Render("━"))
		}
	}
	return b.String()
}

var sparkRunes = []rune("▁▂▃▄▅▆▇█")

func sparkline(vals []float64, width int, col lipgloss.Color) string {
	if len(vals) > width {
		vals = vals[len(vals)-width:]
	}
	maxv, minv := 0.0, 0.0
	for i, v := range vals {
		if i == 0 || v > maxv {
			maxv = v
		}
		if i == 0 || v < minv {
			minv = v
		}
	}
	// show the variation, but keep some baseline so a flat line stays low
	lo := minv - (maxv-minv)*0.6
	if lo < 0 {
		lo = 0
	}
	var b strings.Builder
	for i := 0; i < width-len(vals); i++ {
		b.WriteRune(' ')
	}
	for _, v := range vals {
		i := 0
		if maxv > lo {
			i = int((v - lo) / (maxv - lo) * float64(len(sparkRunes)-1))
		}
		b.WriteRune(sparkRunes[i])
	}
	return lipgloss.NewStyle().Foreground(col).Render(b.String())
}

func keyHelp(pairs ...string) string {
	var parts []string
	for i := 0; i+1 < len(pairs); i += 2 {
		parts = append(parts, sKey.Render(pairs[i])+sKeyDesc.Render(pairs[i+1]))
	}
	return strings.Join(parts, " ")
}

func humanBytes(n int64) string {
	f := float64(n)
	for _, u := range []string{"B", "KB", "MB", "GB"} {
		if f < 1024 || u == "GB" {
			if u == "B" {
				return fmt.Sprintf("%d %s", n, u)
			}
			return fmt.Sprintf("%.1f %s", f, u)
		}
		f /= 1024
	}
	return ""
}

func trunc(s string, n int) string {
	if strings.ContainsRune(s, 0x1b) {
		return ansi.Truncate(s, n, "…")
	}
	r := []rune(s)
	if len(r) <= n {
		return s
	}
	if n <= 1 {
		return string(r[:n])
	}
	return string(r[:n-1]) + "…"
}

func pad(s string, n int) string {
	w := lipgloss.Width(s)
	if w >= n {
		return s
	}
	return s + strings.Repeat(" ", n-w)
}

func padLeft(s string, n int) string {
	w := lipgloss.Width(s)
	if w >= n {
		return s
	}
	return strings.Repeat(" ", n-w) + s
}

func teamStyle(team int) lipgloss.Style {
	switch team {
	case 1:
		return sT
	case 2:
		return sCT
	}
	return sMuted
}

// plain console output helpers (for non-TUI commands)
func say(icon lipgloss.Style, sym, msg string) { fmt.Println(icon.Render(sym) + " " + msg) }
func info(msg string)                          { say(sTitle, "›", msg) }
func okMsg(msg string)                         { say(sOK, gOK, msg) }
func warnMsg(msg string)                       { say(sWarn, "!", msg) }
func errMsg(msg string)                        { say(sErr, gErr, msg) }
