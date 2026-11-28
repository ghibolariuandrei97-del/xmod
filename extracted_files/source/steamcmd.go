package main

// HLDS (Counter-Strike 1.6, app 90) through Valve's SteamCMD - xcmd downloads SteamCMD itself, runs it with the right
// arguments (force_install_dir before login, app_set_config 90 mod cstrike, retries for the famous 0x202/0x402 errors)
// and turns its output into a progress bar.

import (
	"archive/tar"
	"bufio"
	"compress/gzip"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"runtime"
	"strconv"
	"strings"
)

func steamcmdURL() string {
	if v := os.Getenv("XCMD_STEAMCMD_URL"); v != "" {
		return v
	}
	if runtime.GOOS == "windows" {
		return "https://steamcdn-a.akamaihd.net/client/installer/steamcmd.zip"
	}
	return "https://steamcdn-a.akamaihd.net/client/installer/steamcmd_linux.tar.gz"
}

func steamcmdDir(dir string) string { return filepath.Join(dir, ".xcmd", "steamcmd") }
func steamcmdBin(dir string) string {
	if v := os.Getenv("XCMD_STEAMCMD"); v != "" {
		return v
	}
	if runtime.GOOS == "windows" {
		return filepath.Join(steamcmdDir(dir), "steamcmd.exe")
	}
	return filepath.Join(steamcmdDir(dir), "steamcmd.sh")
}

func ensureSteamCmd(dir string, t *Task) error {
	bin := steamcmdBin(dir)
	if fileExists(bin) {
		return nil
	}
	sd := steamcmdDir(dir)
	os.MkdirAll(sd, 0o755)
	arc := filepath.Join(sd, filepath.Base(steamcmdURL()))
	t.Log("downloading %s", steamcmdURL())
	if err := download(steamcmdURL(), arc, t.DownloadProgress); err != nil {
		return err
	}
	if strings.HasSuffix(arc, ".zip") {
		if _, err := unzipAll(arc, sd); err != nil {
			return err
		}
	} else {
		if err := untgz(arc, sd); err != nil {
			return err
		}
	}
	os.Remove(arc)
	if !fileExists(bin) {
		return fmt.Errorf("steamcmd not found after extraction")
	}
	return nil
}

func untgz(path, dest string) error {
	f, err := os.Open(path)
	if err != nil {
		return err
	}
	defer f.Close()
	gz, err := gzip.NewReader(f)
	if err != nil {
		return err
	}
	tr := tar.NewReader(gz)
	for {
		h, err := tr.Next()
		if err == io.EOF {
			return nil
		}
		if err != nil {
			return err
		}
		p := filepath.Join(dest, filepath.FromSlash(h.Name))
		if !strings.HasPrefix(p, filepath.Clean(dest)) {
			continue
		}
		switch h.Typeflag {
		case tar.TypeDir:
			os.MkdirAll(p, 0o755)
		case tar.TypeReg:
			os.MkdirAll(filepath.Dir(p), 0o755)
			o, err := os.OpenFile(p, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, os.FileMode(h.Mode)|0o600)
			if err != nil {
				return err
			}
			io.Copy(o, tr)
			o.Close()
		case tar.TypeSymlink:
			os.Remove(p)
			os.Symlink(h.Linkname, p)
		}
	}
}

var (
	reAppProgress = regexp.MustCompile(`progress:\s*([\d.]+)\s*\((\d+)\s*/\s*(\d+)\)`)
	reSelfUpdate  = regexp.MustCompile(`\[\s*(\d+)%\]\s*(.*)`)
	reState       = regexp.MustCompile(`Update state \(0x[0-9a-fA-F]+\)\s*([a-zA-Z ]+)`)
	reSuccess     = regexp.MustCompile(`Success! App '90'`)
)

func hldsInstalled(dir string) bool {
	return fileExists(hldsBinary(dir)) && (fileExists(filepath.Join(dir, "cstrike", "dlls", "mp.dll")) || fileExists(filepath.Join(dir, "cstrike", "dlls", "cs.so")))
}

// installHLDS runs SteamCMD (up to 5 attempts: app 90 is known to need several)
func installHLDS(dir string, validate bool, t *Task) error {
	abs, _ := filepath.Abs(dir)
	os.MkdirAll(abs, 0o755)
	var last error
	for attempt := 1; attempt <= 5; attempt++ {
		args := []string{"+force_install_dir", abs, "+login", "anonymous", "+app_set_config", "90", "mod", "cstrike", "+app_update", "90"}
		if validate || attempt > 1 {
			args = append(args, "validate")
		}
		args = append(args, "+quit")
		t.Log("attempt %d: steamcmd %s", attempt, strings.Join(args, " "))
		cmd := exec.Command(steamcmdBin(dir), args...)
		os.MkdirAll(steamcmdDir(dir), 0o755)
		cmd.Dir = steamcmdDir(dir)
		out, _ := cmd.StdoutPipe()
		cmd.Stderr = cmd.Stdout
		if err := cmd.Start(); err != nil {
			return fmt.Errorf("cannot start steamcmd: %v", err)
		}
		success := false
		sc := bufio.NewScanner(out)
		sc.Split(scanCRLF)
		for sc.Scan() {
			line := strings.TrimSpace(sc.Text())
			if line == "" {
				continue
			}
			if m := reAppProgress.FindStringSubmatch(line); m != nil {
				pct, _ := strconv.ParseFloat(m[1], 64)
				done, _ := strconv.ParseInt(m[2], 10, 64)
				total, _ := strconv.ParseInt(m[3], 10, 64)
				state := "downloading"
				if s := reState.FindStringSubmatch(line); s != nil {
					state = strings.TrimSpace(s[1])
				}
				t.Progress(pct/100, fmt.Sprintf("%s · %s / %s · try %d", state, humanBytes(done), humanBytes(total), attempt))
				continue
			}
			if m := reSelfUpdate.FindStringSubmatch(line); m != nil {
				pct, _ := strconv.ParseFloat(m[1], 64)
				t.Progress(pct/100, "SteamCMD self-update: "+trunc(m[2], 40))
			}
			if reSuccess.MatchString(line) {
				success = true
			}
			t.Log("%s", line)
		}
		err := cmd.Wait()
		if success || hldsInstalled(dir) && err == nil {
			fixSteamClient(dir, t)
			if !hldsInstalled(dir) {
				last = fmt.Errorf("steamcmd reported success but %s is missing", hldsBinary(dir))
				continue
			}
			return nil
		}
		last = fmt.Errorf("steamcmd attempt %d failed (%v)", attempt, err)
		t.Log("%v - retrying", last)
	}
	return last
}

// Linux HLDS wants ~/.steam/sdk32/steamclient.so (else "dlopen failed trying to load steamclient.so")
func fixSteamClient(dir string, t *Task) {
	if runtime.GOOS != "linux" {
		return
	}
	src := filepath.Join(steamcmdDir(dir), "linux32", "steamclient.so")
	h, err := os.UserHomeDir()
	if err != nil || !fileExists(src) {
		return
	}
	dst := filepath.Join(h, ".steam", "sdk32", "steamclient.so")
	if fileExists(dst) {
		return
	}
	os.MkdirAll(filepath.Dir(dst), 0o755)
	if err := copyFile(src, dst); err == nil {
		t.Log("installed %s", dst)
	}
	// and next to hlds too (works for every user that runs it)
	copyFile(src, filepath.Join(dir, "steamclient.so"))
}

func copyFile(src, dst string) error {
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()
	st, _ := in.Stat()
	os.MkdirAll(filepath.Dir(dst), 0o755)
	out, err := os.OpenFile(dst, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, st.Mode()|0o600)
	if err != nil {
		return err
	}
	_, err = io.Copy(out, in)
	out.Close()
	return err
}

// steamcmd redraws progress with \r: split on \r and \n
func scanCRLF(data []byte, atEOF bool) (advance int, token []byte, err error) {
	for i, b := range data {
		if b == '\n' || b == '\r' {
			return i + 1, data[:i], nil
		}
	}
	if atEOF && len(data) > 0 {
		return len(data), data, nil
	}
	return 0, nil, nil
}
