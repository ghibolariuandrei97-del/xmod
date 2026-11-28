package main

// HTTP downloads with progress + GitHub (releases, repo contents, raw files).

import (
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"time"
)

var httpClient = &http.Client{Timeout: 0}

func ghAPI() string {
	if v := os.Getenv("XCMD_GITHUB_API"); v != "" {
		return strings.TrimRight(v, "/")
	}
	return "https://api.github.com"
}
func ghRaw() string {
	if v := os.Getenv("XCMD_GITHUB_RAW"); v != "" {
		return strings.TrimRight(v, "/")
	}
	return "https://raw.githubusercontent.com"
}

func newReq(url string) (*http.Request, error) {
	req, err := http.NewRequest("GET", url, nil)
	if err != nil {
		return nil, err
	}
	req.Header.Set("User-Agent", "xcmd/"+xcmdVersion)
	if strings.HasPrefix(url, ghAPI()) {
		req.Header.Set("Accept", "application/vnd.github+json")
		tok := os.Getenv("XCMD_GITHUB_TOKEN")
		if tok == "" {
			tok = os.Getenv("GITHUB_TOKEN")
		}
		if tok != "" && tok != "proxy-injected" {
			req.Header.Set("Authorization", "Bearer "+tok)
		}
	}
	return req, nil
}

func getJSON(url string, v interface{}) error {
	req, err := newReq(url)
	if err != nil {
		return err
	}
	c := &http.Client{Timeout: 20 * time.Second}
	resp, err := c.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		b, _ := io.ReadAll(io.LimitReader(resp.Body, 300))
		msg := strings.TrimSpace(string(b))
		if resp.StatusCode == 403 && strings.Contains(msg, "rate limit") {
			return fmt.Errorf("GitHub API rate limit reached (set XCMD_GITHUB_TOKEN to a personal token)")
		}
		return fmt.Errorf("%s: HTTP %d %s", url, resp.StatusCode, trunc(msg, 120))
	}
	return json.NewDecoder(resp.Body).Decode(v)
}

func getBytes(url string) ([]byte, error) {
	req, err := newReq(url)
	if err != nil {
		return nil, err
	}
	c := &http.Client{Timeout: 20 * time.Second}
	resp, err := c.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		return nil, fmt.Errorf("%s: HTTP %d", url, resp.StatusCode)
	}
	return io.ReadAll(io.LimitReader(resp.Body, 64<<20))
}

// download url -> dest, calling prog(done, total) (total -1 when unknown)
func download(url, dest string, prog func(done, total int64)) error {
	os.MkdirAll(filepath.Dir(dest), 0o755)
	req, err := newReq(url)
	if err != nil {
		return err
	}
	resp, err := httpClient.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		return fmt.Errorf("download %s: HTTP %d", url, resp.StatusCode)
	}
	tmp := dest + ".part"
	f, err := os.Create(tmp)
	if err != nil {
		return err
	}
	total := resp.ContentLength
	var done int64
	buf := make([]byte, 64*1024)
	last := time.Time{}
	for {
		n, rerr := resp.Body.Read(buf)
		if n > 0 {
			if _, werr := f.Write(buf[:n]); werr != nil {
				f.Close()
				return werr
			}
			done += int64(n)
			if prog != nil && time.Since(last) > 80*time.Millisecond {
				prog(done, total)
				last = time.Now()
			}
		}
		if rerr == io.EOF {
			break
		}
		if rerr != nil {
			f.Close()
			return rerr
		}
	}
	f.Close()
	if prog != nil {
		prog(done, done)
	}
	os.Remove(dest)
	return os.Rename(tmp, dest)
}

// ---- GitHub

type ghAsset struct {
	Name string `json:"name"`
	URL  string `json:"browser_download_url"`
	Size int64  `json:"size"`
}
type ghRelease struct {
	Tag    string    `json:"tag_name"`
	Name   string    `json:"name"`
	Assets []ghAsset `json:"assets"`
}

func ghLatestRelease(repo string) (*ghRelease, error) {
	var r ghRelease
	if err := getJSON(ghAPI()+"/repos/"+repo+"/releases/latest", &r); err != nil {
		return nil, err
	}
	return &r, nil
}

type ghContent struct {
	Name        string `json:"name"`
	Path        string `json:"path"`
	Sha         string `json:"sha"`
	Size        int64  `json:"size"`
	Type        string `json:"type"`
	DownloadURL string `json:"download_url"`
}

func ghContents(repo, branch, path string) ([]ghContent, error) {
	var list []ghContent
	u := strings.TrimRight(ghAPI()+"/repos/"+repo+"/contents/"+path, "/")
	if branch != "" {
		u += "?ref=" + branch
	}
	err := getJSON(u, &list)
	return list, err
}

func rawURL(repo, branch, path string) string {
	return ghRaw() + "/" + repo + "/" + branch + "/" + path
}
