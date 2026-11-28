package main

import (
	"path/filepath"
	"strings"
	"sync"
	"time"
)

type ProcStat struct {
	PID     int
	CPU     float64 // % of one core
	RSS     int64
	Started time.Time
	Found   bool
}

type cpuSample struct {
	cpu  float64 // seconds
	wall time.Time
}

var (
	procMu      sync.Mutex
	procSamples = map[int]cpuSample{}
)

// cpuPercent turns cumulative cpu seconds into a % using the previous sample of that pid
func cpuPercent(pid int, cpuSec float64) float64 {
	procMu.Lock()
	defer procMu.Unlock()
	now := time.Now()
	prev, ok := procSamples[pid]
	procSamples[pid] = cpuSample{cpuSec, now}
	if !ok {
		return 0
	}
	dt := now.Sub(prev.wall).Seconds()
	if dt <= 0 {
		return 0
	}
	return (cpuSec - prev.cpu) / dt * 100
}

func samePathPrefix(exe, dir string) bool {
	if dir == "" {
		return true
	}
	a, _ := filepath.Abs(dir)
	return strings.HasPrefix(strings.ToLower(filepath.Clean(exe)), strings.ToLower(filepath.Clean(a)))
}
