//go:build !windows

package main

import (
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"
)

// findServerProc looks for hlds_linux running from dir
func findServerProc(dir string) ProcStat {
	ents, _ := os.ReadDir("/proc")
	for _, e := range ents {
		pid, err := strconv.Atoi(e.Name())
		if err != nil {
			continue
		}
		exe, err := os.Readlink(filepath.Join("/proc", e.Name(), "exe"))
		if err != nil || !strings.HasSuffix(exe, "hlds_linux") {
			continue
		}
		if !samePathPrefix(exe, dir) {
			continue
		}
		return procStat(pid)
	}
	return ProcStat{}
}

func procStat(pid int) ProcStat {
	ps := ProcStat{PID: pid, Found: true}
	b, err := os.ReadFile(filepath.Join("/proc", strconv.Itoa(pid), "stat"))
	if err == nil {
		s := string(b)
		if i := strings.LastIndex(s, ")"); i > 0 {
			f := strings.Fields(s[i+2:])
			if len(f) > 20 {
				ut, _ := strconv.ParseFloat(f[11], 64)
				st, _ := strconv.ParseFloat(f[12], 64)
				ps.CPU = cpuPercent(pid, (ut+st)/100)
				startTicks, _ := strconv.ParseFloat(f[19], 64)
				if up, err := os.ReadFile("/proc/uptime"); err == nil {
					u, _ := strconv.ParseFloat(strings.Fields(string(up))[0], 64)
					ps.Started = time.Now().Add(-time.Duration((u - startTicks/100) * float64(time.Second)))
				}
			}
		}
	}
	if b, err := os.ReadFile(filepath.Join("/proc", strconv.Itoa(pid), "status")); err == nil {
		for _, l := range strings.Split(string(b), "\n") {
			if strings.HasPrefix(l, "VmRSS:") {
				f := strings.Fields(l)
				if len(f) >= 2 {
					kb, _ := strconv.ParseInt(f[1], 10, 64)
					ps.RSS = kb * 1024
				}
			}
		}
	}
	return ps
}

func hostMemTotal() int64 {
	b, err := os.ReadFile("/proc/meminfo")
	if err != nil {
		return 0
	}
	for _, l := range strings.Split(string(b), "\n") {
		if strings.HasPrefix(l, "MemTotal:") {
			f := strings.Fields(l)
			kb, _ := strconv.ParseInt(f[1], 10, 64)
			return kb * 1024
		}
	}
	return 0
}
