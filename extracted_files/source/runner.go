package main

// Live installer view: a list of steps with spinner / progress bar / speed / ETA and a log tail.
// Falls back to plain text when the output is not a terminal (or XCMD_PLAIN=1).

import (
	"fmt"
	"os"
	"strings"
	"sync"
	"time"

	tea "github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/lipgloss"
	"github.com/mattn/go-isatty"
)

type Step struct {
	Title    string
	Run      func(t *Task) error
	Optional bool // a failure is reported but does not stop the rest
}

const (
	stPending = iota
	stRunning
	stDone
	stFailed
	stSkipped
)

type stepView struct {
	title   string
	status  int
	frac    float64
	label   string
	note    string
	started time.Time
	took    time.Duration
	err     error
}

type Task struct {
	r   *runner
	idx int
	// download speed tracking
	lastBytes int64
	lastT     time.Time
	speed     float64
}

type runner struct {
	mu    sync.Mutex
	title string
	steps []*stepView
	logs  []string
	prog  *tea.Program
	plain bool
	done  bool
	spin  int
	start time.Time
}

type tickMsg struct{}
type doneMsg struct{}

func isTTY() bool {
	if os.Getenv("XCMD_PLAIN") == "1" {
		return false
	}
	return isatty.IsTerminal(os.Stdout.Fd()) || isatty.IsCygwinTerminal(os.Stdout.Fd())
}

func (t *Task) Log(format string, a ...interface{}) {
	line := fmt.Sprintf(format, a...)
	r := t.r
	r.mu.Lock()
	for _, l := range strings.Split(strings.TrimRight(line, "\n"), "\n") {
		r.logs = append(r.logs, l)
	}
	if len(r.logs) > 400 {
		r.logs = r.logs[len(r.logs)-400:]
	}
	r.mu.Unlock()
	if r.plain {
		fmt.Println(sDim.Render("    │ ") + line)
	}
}

// Progress: frac in 0..1 (negative = indeterminate), label shown next to the bar
func (t *Task) Progress(frac float64, label string) {
	r := t.r
	r.mu.Lock()
	s := r.steps[t.idx]
	s.frac, s.label = frac, label
	r.mu.Unlock()
}

func (t *Task) Note(n string) { t.r.mu.Lock(); t.r.steps[t.idx].note = n; t.r.mu.Unlock() }

// DownloadProgress adapts download() callbacks
func (t *Task) DownloadProgress(done, total int64) {
	now := time.Now()
	if !t.lastT.IsZero() {
		dt := now.Sub(t.lastT).Seconds()
		if dt > 0.2 {
			inst := float64(done-t.lastBytes) / dt
			if t.speed == 0 {
				t.speed = inst
			} else {
				t.speed = t.speed*0.7 + inst*0.3
			}
			t.lastBytes, t.lastT = done, now
		}
	} else {
		t.lastBytes, t.lastT = done, now
	}
	label := humanBytes(done)
	frac := -1.0
	if total > 0 {
		frac = float64(done) / float64(total)
		label += " / " + humanBytes(total)
		if t.speed > 0 && done < total {
			eta := time.Duration(float64(total-done)/t.speed) * time.Second
			label += fmt.Sprintf(" · %s/s · %s left", humanBytes(int64(t.speed)), eta.Round(time.Second))
		}
	} else if t.speed > 0 {
		label += fmt.Sprintf(" · %s/s", humanBytes(int64(t.speed)))
	}
	t.Progress(frac, label)
}

func (r *runner) Init() tea.Cmd { return tick() }
func tick() tea.Cmd       { return tea.Tick(90*time.Millisecond, func(time.Time) tea.Msg { return tickMsg{} }) }

func (r *runner) Update(msg tea.Msg) (tea.Model, tea.Cmd) {
	switch m := msg.(type) {
	case tickMsg:
		r.spin++
		return r, tick()
	case doneMsg:
		r.done = true
		return r, tea.Quit
	case tea.KeyMsg:
		if m.String() == "ctrl+c" {
			r.done = true
			return r, tea.Quit
		}
	}
	return r, nil
}

var spinFrames = spinnerFrames()

func (r *runner) View() string {
	r.mu.Lock()
	defer r.mu.Unlock()
	var b strings.Builder
	b.WriteString("\n " + sTitle.Render("xcmd") + sDim.Render(" › ") + sText.Bold(true).Render(r.title) + "\n\n")
	w := 0
	for _, s := range r.steps {
		if l := lipgloss.Width(s.title); l > w {
			w = l
		}
	}
	for _, s := range r.steps {
		var icon string
		switch s.status {
		case stPending:
			icon = sDim.Render("•")
		case stRunning:
			icon = sTitle.Render(spinFrames[r.spin%len(spinFrames)])
		case stDone:
			icon = sOK.Render(gOK)
		case stFailed:
			icon = sErr.Render(gErr)
		case stSkipped:
			icon = sMuted.Render("–")
		}
		title := pad(s.title, w)
		switch s.status {
		case stRunning:
			title = sText.Bold(true).Render(title)
		case stPending:
			title = sMuted.Render(title)
		default:
			title = sText.Render(title)
		}
		line := " " + icon + " " + title + "  "
		switch s.status {
		case stRunning:
			if s.frac >= 0 {
				line += gradBar(s.frac, 30) + " " + sTitle.Render(fmt.Sprintf("%3.0f%%", s.frac*100))
			} else {
				// indeterminate: moving block
				pos := r.spin % 30
				bb := []rune(strings.Repeat("━", 30))
				var sb strings.Builder
				for i := range bb {
					if i >= pos && i < pos+6 {
						sb.WriteString(sTitle.Render("━"))
					} else {
						sb.WriteString(sDim.Render("━"))
					}
				}
				line += sb.String() + "     "
			}
			if s.label != "" {
				line += "  " + sMuted.Render(s.label)
			}
			line += "  " + sDim.Render(time.Since(s.started).Round(time.Second).String())
		case stDone:
			if s.note != "" {
				line += sMuted.Render(s.note) + "  "
			}
			line += sDim.Render(s.took.Round(100 * time.Millisecond).String())
		case stFailed:
			line += sErr.Render(trunc(fmt.Sprint(s.err), 90))
		case stSkipped:
			line += sMuted.Render(s.note)
		}
		b.WriteString(line + "\n")
	}
	// log tail
	n := 6
	if r.done {
		n = 0
	}
	if n > 0 && len(r.logs) > 0 {
		start := len(r.logs) - n
		if start < 0 {
			start = 0
		}
		b.WriteString("\n")
		for _, l := range r.logs[start:] {
			b.WriteString(sDim.Render("   │ ") + sMuted.Render(trunc(l, 110)) + "\n")
		}
	}
	if r.done {
		failed := 0
		for _, s := range r.steps {
			if s.status == stFailed {
				failed++
			}
		}
		el := time.Since(r.start).Round(time.Second)
		if failed == 0 {
			b.WriteString("\n " + sOK.Render("✔ All done") + sMuted.Render(" in "+el.String()) + "\n")
		} else {
			b.WriteString("\n " + sErr.Render(fmt.Sprintf("✖ %d step(s) failed", failed)) + sMuted.Render(" in "+el.String()+" — details: "+logPath()) + "\n")
		}
	}
	return b.String()
}

func logPath() string { return homeDir() + string(os.PathSeparator) + "last.log" }

// runSteps runs the steps in order; stops at the first non-optional failure.
func runSteps(title string, steps []Step) error {
	r := &runner{title: title, plain: !isTTY(), start: time.Now()}
	for _, s := range steps {
		r.steps = append(r.steps, &stepView{title: s.Title})
	}
	var firstErr error
	work := func() {
		for i, s := range steps {
			t := &Task{r: r, idx: i}
			r.mu.Lock()
			r.steps[i].status, r.steps[i].started, r.steps[i].frac = stRunning, time.Now(), -1
			r.mu.Unlock()
			if r.plain {
				fmt.Println(sTitle.Render("›") + " " + s.Title)
			}
			err := s.Run(t)
			r.mu.Lock()
			sv := r.steps[i]
			sv.took = time.Since(sv.started)
			if err == errSkip {
				sv.status = stSkipped
				if sv.note == "" {
					sv.note = "skipped"
				}
			} else if err != nil {
				sv.status, sv.err = stFailed, err
			} else {
				sv.status = stDone
			}
			r.mu.Unlock()
			if r.plain {
				if err != nil && err != errSkip {
					errMsg(s.Title + ": " + err.Error())
				} else if err == errSkip {
					fmt.Println(sMuted.Render("– " + s.Title + ": " + sv.note))
				} else {
					okMsg(s.Title + sMuted.Render(" "+sv.note))
				}
			}
			if err != nil && err != errSkip {
				t.Log("ERROR: %v", err)
				if firstErr == nil {
					firstErr = fmt.Errorf("%s: %w", s.Title, err)
				}
				if !s.Optional {
					// mark the rest as skipped
					r.mu.Lock()
					for j := i + 1; j < len(r.steps); j++ {
						r.steps[j].status, r.steps[j].note = stSkipped, "not run"
					}
					r.mu.Unlock()
					break
				}
			}
		}
		os.MkdirAll(homeDir(), 0o755)
		r.mu.Lock()
		os.WriteFile(logPath(), []byte(strings.Join(r.logs, "\n")+"\n"), 0o644)
		r.mu.Unlock()
	}
	if r.plain {
		work()
		return firstErr
	}
	p := tea.NewProgram(r)
	r.prog = p
	go func() { work(); time.Sleep(150 * time.Millisecond); p.Send(doneMsg{}) }()
	if _, err := p.Run(); err != nil {
		return err
	}
	return firstErr
}

type skipErr struct{}

func (skipErr) Error() string { return "skipped" }

var errSkip error = skipErr{}

func (t *Task) Skip(note string) error { t.Note(note); return errSkip }
