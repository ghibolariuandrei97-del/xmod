//go:build windows

package main

import (
	"strings"
	"time"
	"unsafe"

	"golang.org/x/sys/windows"
)

var (
	k32             = windows.NewLazySystemDLL("kernel32.dll")
	procMemInfo     = k32.NewProc("K32GetProcessMemoryInfo")
	procGlobalMemEx = k32.NewProc("GlobalMemoryStatusEx")
)

type pmc struct {
	cb                                                                    uint32
	PageFaultCount                                                        uint32
	PeakWorkingSetSize, WorkingSetSize                                    uintptr
	QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage                          uintptr
	QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage                    uintptr
	PagefileUsage, PeakPagefileUsage                                      uintptr
}

func findServerProc(dir string) ProcStat {
	snap, err := windows.CreateToolhelp32Snapshot(windows.TH32CS_SNAPPROCESS, 0)
	if err != nil {
		return ProcStat{}
	}
	defer windows.CloseHandle(snap)
	var pe windows.ProcessEntry32
	pe.Size = uint32(unsafe.Sizeof(pe))
	for err = windows.Process32First(snap, &pe); err == nil; err = windows.Process32Next(snap, &pe) {
		name := strings.ToLower(windows.UTF16ToString(pe.ExeFile[:]))
		if name != "hlds.exe" {
			continue
		}
		ps := procStat(int(pe.ProcessID), dir)
		if ps.Found {
			return ps
		}
	}
	return ProcStat{}
}

func procStat(pid int, dir string) ProcStat {
	h, err := windows.OpenProcess(windows.PROCESS_QUERY_LIMITED_INFORMATION|windows.PROCESS_VM_READ, false, uint32(pid))
	if err != nil {
		h, err = windows.OpenProcess(windows.PROCESS_QUERY_LIMITED_INFORMATION, false, uint32(pid))
		if err != nil {
			return ProcStat{}
		}
	}
	defer windows.CloseHandle(h)
	buf := make([]uint16, 1024)
	n := uint32(len(buf))
	if windows.QueryFullProcessImageName(h, 0, &buf[0], &n) == nil {
		if !samePathPrefix(windows.UTF16ToString(buf[:n]), dir) {
			return ProcStat{}
		}
	}
	ps := ProcStat{PID: pid, Found: true}
	var c, e, k, u windows.Filetime
	if windows.GetProcessTimes(h, &c, &e, &k, &u) == nil {
		tot := float64(k.Nanoseconds()+u.Nanoseconds()) / 1e9
		ps.CPU = cpuPercent(pid, tot)
		ps.Started = time.Unix(0, c.Nanoseconds())
	}
	var m pmc
	m.cb = uint32(unsafe.Sizeof(m))
	if r, _, _ := procMemInfo.Call(uintptr(h), uintptr(unsafe.Pointer(&m)), uintptr(m.cb)); r != 0 {
		ps.RSS = int64(m.WorkingSetSize)
	}
	return ps
}

func hostMemTotal() int64 {
	var ms struct {
		Length, MemoryLoad                                    uint32
		TotalPhys, AvailPhys, TotalPageFile, AvailPageFile    uint64
		TotalVirtual, AvailVirtual, AvailExtendedVirtual      uint64
	}
	ms.Length = uint32(unsafe.Sizeof(ms))
	if r, _, _ := procGlobalMemEx.Call(uintptr(unsafe.Pointer(&ms))); r == 0 {
		return 0
	}
	return int64(ms.TotalPhys)
}
