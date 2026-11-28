package main

// Upload helpers: xcmd can prepare a folder for a server you only reach by FTP. It remembers every file it wrote
// during a run, and --zip packs the whole folder (setup) or only the changed files (update) for uploading.

import (
	"archive/zip"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"time"
)

var (
	writtenMu sync.Mutex
	written   = map[string]bool{}
)

func noteWritten(rel string) { writtenMu.Lock(); written[filepath.ToSlash(rel)] = true; writtenMu.Unlock() }

func writtenList() []string {
	writtenMu.Lock()
	defer writtenMu.Unlock()
	var l []string
	for k := range written {
		l = append(l, k)
	}
	sort.Strings(l)
	return l
}

// makeZip packs files (relative to dir) into out; nil files = the whole folder except .xcmd
func makeZip(dir, out string, files []string) (int, error) {
	if files == nil {
		filepath.Walk(dir, func(p string, fi os.FileInfo, err error) error {
			if err != nil {
				return nil
			}
			rel, _ := filepath.Rel(dir, p)
			rel = filepath.ToSlash(rel)
			if fi.IsDir() {
				if rel == ".xcmd" {
					return filepath.SkipDir
				}
				return nil
			}
			if strings.HasSuffix(rel, ".xcmd-new") || strings.HasSuffix(rel, ".xcmd-bak") || strings.HasSuffix(rel, ".part") {
				return nil
			}
			files = append(files, rel)
			return nil
		})
	}
	f, err := os.Create(out)
	if err != nil {
		return 0, err
	}
	defer f.Close()
	zw := zip.NewWriter(f)
	n := 0
	for _, rel := range files {
		p := filepath.Join(dir, filepath.FromSlash(rel))
		in, err := os.Open(p)
		if err != nil {
			continue
		}
		st, _ := in.Stat()
		h, _ := zip.FileInfoHeader(st)
		h.Name, h.Method = rel, zip.Deflate
		w, err := zw.CreateHeader(h)
		if err == nil {
			io.Copy(w, in)
			n++
		}
		in.Close()
	}
	return n, zw.Close()
}

// writeUploadNotes leaves XCMD_UPLOAD.txt in the folder: what to upload and the lines the server needs
func writeUploadNotes(dir string, st *State) {
	var b strings.Builder
	b.WriteString("Prepared by xcmd " + xcmdVersion + " on " + time.Now().Format("2006-01-02 15:04") + "\r\n\r\n")
	b.WriteString("Upload EVERYTHING in this folder to the server ROOT (the folder that contains cstrike/), overwrite when asked.\r\n")
	b.WriteString("(the .xcmd folder is xcmd's own bookkeeping - you can skip it)\r\n\r\n")
	var names []string
	for _, n := range st.Names() {
		names = append(names, n+" "+st.Packages[n].Version)
	}
	b.WriteString("Packages: " + strings.Join(names, ", ") + "\r\n\r\n")
	if st.Packages["metamod"] == nil {
		b.WriteString("Metamod is NOT in this folder (your server already has it). Add these lines to cstrike/addons/metamod/plugins.ini:\r\n")
		for _, ip := range sortedByOrder(st) {
			if !ip.Enabled {
				continue
			}
			for _, d := range ip.Metamod {
				b.WriteString(fmt.Sprintf("   win32 addons/xmod/dlls/%s.dll\r\n   linux addons/xmod/dlls/%s_i386.so\r\n", d, d))
			}
		}
		b.WriteString("(xmod_mm must be the first of them)\r\n\r\n")
	} else {
		b.WriteString("cstrike/liblist.gam in this folder loads Metamod (a standard CS 1.6 one if the folder had none). If you keep your own, it needs:\r\n")
		b.WriteString("   gamedll \"addons\\metamod\\metamod.dll\"\r\n   gamedll_linux \"addons/metamod/metamod_i386.so\"\r\n\r\n")
	}
	if st.Packages["agent"] != nil {
		b.WriteString("Live monitor / map: set rcon_password in server.cfg, then on your PC: xcmd monitor IP:PORT --rcon PASSWORD\r\n")
		b.WriteString("(ReHLDS: sv_rcon_condebug 0 keeps the rcon lines out of the console)\r\n")
	}
	os.WriteFile(filepath.Join(dir, "XCMD_UPLOAD.txt"), []byte(b.String()), 0o644)
}

func sortedByOrder(st *State) []*Installed {
	var l []*Installed
	for _, ip := range st.Packages {
		l = append(l, ip)
	}
	sort.Slice(l, func(i, j int) bool { return l[i].Order < l[j].Order })
	return l
}
