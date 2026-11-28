package main

import (
	"bufio"
	"fmt"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"sort"
	"strings"
	"time"
)

type args struct {
	pos   []string
	flags map[string]string
}

func parseArgs(a []string) args {
	r := args{flags: map[string]string{}}
	valued := map[string]bool{"os": true, "dir": true, "rcon": true, "map": true, "port": true, "maxplayers": true, "name": true, "repo": true, "bsp": true, "maps": true}
	for i := 0; i < len(a); i++ {
		s := a[i]
		if strings.HasPrefix(s, "--") {
			k := strings.TrimPrefix(s, "--")
			if eq := strings.Index(k, "="); eq >= 0 {
				r.flags[k[:eq]] = k[eq+1:]
			} else if valued[k] && i+1 < len(a) {
				r.flags[k] = a[i+1]
				i++
			} else {
				r.flags[k] = "1"
			}
		} else if s == "-y" {
			r.flags["yes"] = "1"
		} else {
			r.pos = append(r.pos, s)
		}
	}
	return r
}

func (a args) has(k string) bool { _, ok := a.flags[k]; return ok }

func main() {
	cfg := loadConfig()
	if len(os.Args) < 2 {
		interactive(cfg, "Home")
		return
	}
	if os.Args[1] == "__query" {
		debugQuery(os.Args[2:])
		return
	}
	if os.Args[1] == "__browse" {
		debugBrowse()
		return
	}
	if os.Args[1] == "__render" {
		debugRender(os.Args[2:])
		return
	}
	if err := dispatch(cfg, os.Args[1:]); err != nil {
		errMsg(err.Error())
		os.Exit(1)
	}
}

// the full-screen app; actions chosen inside it (install, setup, start …) run in between and come back
func interactive(cfg *Config, tab string) {
	if !isTTY() {
		usage()
		return
	}
	for {
		ex, err := runApp(cfg, tab)
		if err != nil {
			errMsg(err.Error())
			return
		}
		if len(ex) == 0 {
			return
		}
		fmt.Println()
		if err := dispatch(cfg, ex); err != nil {
			errMsg(err.Error())
		}
		if ex[0] == "start" {
			return
		}
		fmt.Print("\n" + sMuted.Render("  press Enter to go back to xcmd ") )
		bufio.NewReader(os.Stdin).ReadString('\n')
		catCached = nil
		cfg = loadConfig()
		if ex[0] == "install" || ex[0] == "update" {
			tab = "Mods"
		} else {
			tab = "Home"
		}
	}
}

func dirOf(cfg *Config, a args) string {
	d := cfg.DefaultDir
	if v := a.flags["dir"]; v != "" {
		d = v
	}
	abs, err := filepath.Abs(d)
	if err == nil {
		d = abs
	}
	return d
}

func dispatch(cfg *Config, raw []string) error {
	a := parseArgs(raw)
	if v := a.flags["repo"]; v != "" {
		cfg.Repo = v
	}
	if a.has("plain") {
		os.Setenv("XCMD_PLAIN", "1")
	}
	if a.has("force") {
		os.Setenv("XCMD_FORCE", "1")
	}
	if v := a.flags["os"]; v != "" {
		targetOS = strings.ToLower(v)
	}
	makeZipAfter = a.has("zip")
	if len(a.pos) == 0 {
		interactive(cfg, "Home")
		return nil
	}
	cmd, rest := strings.ToLower(a.pos[0]), a.pos[1:]
	dir := dirOf(cfg, a)
	switch cmd {
	case "help", "-h", "--help":
		usage()
	case "version", "-v", "--version":
		fmt.Println(sTitle.Render("xcmd") + " " + xcmdVersion + sMuted.Render(" ("+runtime.GOOS+"/"+runtime.GOARCH+")"))
	case "setup":
		prof := ""
		if len(rest) > 0 {
			prof = strings.ToLower(rest[0])
		} else if a.has("all") {
			prof = "all"
		}
		pr := findProfile(prof)
		if pr == nil {
			fmt.Println(sSub.Render("Choose what to put in the folder:") + sMuted.Render("  xcmd setup <profile> --dir <folder> [--os linux|windows|both] [--zip]"))
			for _, p := range profiles {
				fmt.Println("  " + pad(sText.Bold(true).Render(p.Key), 12) + pad(sTitle.Render(p.Title), 44) + sMuted.Render(p.Desc))
			}
			if prof != "" {
				return fmt.Errorf("unknown profile %q", prof)
			}
			return nil
		}
		names := append([]string{}, pr.Pkgs...)
		if !a.has("no-agent") {
			names = append(names, "agent")
		}
		return doInstall(cfg, dir, names, false, pr.Title+" → "+dir)
	case "install", "i", "add":
		if len(rest) == 0 {
			return fmt.Errorf("what? e.g. xcmd install modcreator  (xcmd search lists everything)")
		}
		var names []string
		for _, n := range rest {
			switch strings.ToLower(n) {
			case "all":
				names = append(names, "rehlds", "regamedll", "metamod", "xmod", "agent", "query", "codex", "xbot", "modcreator")
			case "server", "cs", "cs16":
				names = append(names, "hlds")
			case "xbots", "bots", "bot":
				names = append(names, "xbot")
			case "mc":
				names = append(names, "modcreator")
			case "rehlds", "regamedll", "metamod", "metamod-r":
				names = append(names, strings.TrimSuffix(strings.ToLower(n), "-r"))
			default:
				names = append(names, strings.ToLower(n))
			}
		}
		return doInstall(cfg, dir, names, false, "Installing "+strings.Join(rest, ", "))
	case "update", "upgrade":
		st := loadState(dir)
		names := rest
		if len(names) == 0 {
			for _, n := range st.Names() {
				if n != "hlds" {
					names = append(names, n)
				}
			}
		}
		if len(names) == 0 {
			return fmt.Errorf("nothing installed in %s", dir)
		}
		catCached = nil
		return doInstall(cfg, dir, names, true, "Updating "+dir)
	case "remove", "rm", "uninstall":
		if len(rest) == 0 {
			return fmt.Errorf("remove what?")
		}
		st := loadState(dir)
		for _, n := range rest {
			kept, err := removePkg(dir, st, strings.ToLower(n))
			if err != nil {
				return err
			}
			okMsg("removed " + n)
			for _, k := range kept {
				warnMsg("kept your edited " + k)
			}
		}
	case "enable", "disable":
		st := loadState(dir)
		for _, n := range rest {
			if err := setEnabled(dir, st, strings.ToLower(n), cmd == "enable"); err != nil {
				return err
			}
			okMsg(n + " " + cmd + "d" + sMuted.Render(" — change the map or restart the server"))
		}
	case "list", "ls":
		listInstalled(dir)
	case "search", "find":
		return search(cfg, dir, strings.Join(rest, " "))
	case "info", "show":
		if len(rest) == 0 {
			return fmt.Errorf("info about what?")
		}
		return showInfo(cfg, dir, rest[0])
	case "start", "run":
		return startServer(cfg, dir, a)
	case "monitor", "top", "stats":
		addServersFromArgs(cfg, rest, a)
		interactive(cfg, "Monitor")
	case "map", "radar", "hltv":
		addServersFromArgs(cfg, rest, a)
		if v := a.flags["maps"]; v != "" {
			abs, _ := filepath.Abs(strings.Trim(v, `"`))
			cfg.MapsDir = abs
			cfg.Save()
			okMsg("maps folder: " + abs)
		}
		if v := a.flags["bsp"]; v != "" {
			os.MkdirAll(filepath.Join(homeDir(), "maps"), 0o755)
			copyFile(v, filepath.Join(homeDir(), "maps", filepath.Base(v)))
		}
		interactive(cfg, "Map")
	case "mods", "library":
		interactive(cfg, "Mods")
	case "browser", "servers-world", "browse":
		interactive(cfg, "Browser")
	case "servers":
		return serversCmd(cfg, rest, a)
	case "doctor":
		doctor(cfg, dir)
	case "dir":
		if len(rest) > 0 {
			d, _ := filepath.Abs(rest[0])
			cfg.DefaultDir = d
			cfg.Save()
		}
		fmt.Println(sMuted.Render("default server dir: ") + sText.Render(cfg.DefaultDir))
	default:
		return fmt.Errorf("unknown command %q (xcmd help)", cmd)
	}
	return nil
}

func doInstall(cfg *Config, dir string, names []string, update bool, title string) error {
	os.MkdirAll(dir, 0o755)
	if !isTTY() {
		info(title)
	}
	steps, err := installSteps(names, dir, cfg, update)
	if err != nil {
		return err
	}
	err = runSteps(title, steps)
	if cfg.DefaultDir != dir {
		cfg.DefaultDir = dir
		cfg.Save()
	}
	st := loadState(dir)
	if err == nil && !hldsInstalled(dir) {
		writeUploadNotes(dir, st)
		ch := writtenList()
		fmt.Println()
		lines := []string{sSub.Render("Ready to upload"), sMuted.Render("folder   ") + sText.Render(dir)}
		if makeZipAfter {
			stamp := time.Now().Format("20060102-1504")
			var zp string
			var n int
			var zerr error
			if update && len(ch) == 0 {
				zp = ""
			} else if update {
				zp = strings.TrimRight(dir, `/\`) + "_update_" + stamp + ".zip"
				n, zerr = makeZip(dir, zp, ch)
			} else {
				zp = strings.TrimRight(dir, `/\`) + "_upload.zip"
				n, zerr = makeZip(dir, zp, nil)
			}
			if zp == "" {
				lines = append(lines, sMuted.Render("zip      ")+sOK.Render("nothing changed - nothing to upload"))
			} else if zerr != nil {
				warnMsg("zip: " + zerr.Error())
			} else {
				lines = append(lines, sMuted.Render("zip      ")+sText.Render(zp)+sMuted.Render(fmt.Sprintf("  (%d files)", n)))
			}
		}
		lines = append(lines, sMuted.Render("changed  ")+sText.Render(fmt.Sprintf("%d files this run", len(ch))))
		lines = append(lines, sMuted.Render("how-to   ")+sText.Render(filepath.Join(dir, "XCMD_UPLOAD.txt")))
		lines = append(lines, "", sMuted.Render("upload it to the server root by FTP · later: ")+sText.Render("xcmd update --zip")+sMuted.Render(" makes a zip with only the changed files"))
		fmt.Println(sPanel.Render(strings.Join(lines, "\n")))
		return nil
	}
	if err == nil && hldsInstalled(dir) {
		fmt.Println()
		next := []string{sSub.Render("Next")}
		next = append(next, sMuted.Render("start the server  ")+sText.Render("xcmd start --dir \""+dir+"\""))
		if st.Packages["agent"] != nil {
			next = append(next, sMuted.Render("watch it live     ")+sText.Render("xcmd monitor")+sMuted.Render("   ·   ")+sText.Render("xcmd map"))
		}
		next = append(next, sMuted.Render("add mods          ")+sText.Render("xcmd search")+sMuted.Render("   ·   ")+sText.Render("xcmd install <mod>"))
		fmt.Println(sPanel.Render(strings.Join(next, "\n")))
	}
	return err
}

func listInstalled(dir string) {
	st := loadState(dir)
	fmt.Println(sSub.Render("Installed in ") + sText.Render(dir))
	if hldsInstalled(dir) && st.Packages["hlds"] == nil {
		fmt.Println("  " + sOK.Render(gOK) + " " + pad(sText.Render("hlds"), 14) + sMuted.Render("(found)"))
	}
	if len(st.Packages) == 0 {
		fmt.Println(sMuted.Render("  nothing yet — xcmd setup"))
		return
	}
	for _, n := range st.Names() {
		ip := st.Packages[n]
		icon := sOK.Render(gOK)
		state := ""
		if !ip.Enabled {
			icon, state = sWarn.Render(gOff), sWarn.Render(" disabled")
		}
		fmt.Println("  " + icon + " " + pad(sText.Bold(true).Render(n), 14) + pad(sMuted.Render(ip.Version), 22) + sDim.Render(fmt.Sprintf("%d files · %s", len(ip.Files), ip.When.Format("2006-01-02"))) + state)
	}
}

func search(cfg *Config, dir, q string) error {
	cat := loadCatalog(cfg)
	st := loadState(dir)
	q = strings.ToLower(q)
	fmt.Println(sSub.Render("Library") + sMuted.Render("  ("+cat.Source+")"))
	n := 0
	for _, p := range cat.Sorted() {
		if q != "" && !strings.Contains(strings.ToLower(p.Name+" "+p.Title+" "+p.Description+" "+strings.Join(p.Tags, " ")), q) {
			continue
		}
		icon, stat := pkgStatus(p, st)
		ver := p.Version
		if ver == "" && p.Upstream != "" {
			ver = "latest release"
		}
		fmt.Println("  " + icon + " " + pad(sText.Bold(true).Render(p.Name), 13) + pad(sMuted.Render(p.Kind), 11) + pad(sSub.Render(trunc(ver, 16)), 17) + pad(sDim.Render(stat), 14) + sMuted.Render(trunc(p.Description, 70)))
		n++
	}
	for _, note := range cat.Notes {
		warnMsg(note)
	}
	if n == 0 {
		warnMsg("nothing matches " + q)
	}
	return nil
}

func showInfo(cfg *Config, dir, name string) error {
	cat := loadCatalog(cfg)
	p := cat.Get(name)
	if p == nil {
		return fmt.Errorf("unknown package %s", name)
	}
	st := loadState(dir)
	icon, stat := pkgStatus(p, st)
	lines := []string{sTitle.Render(p.Display()) + "  " + sBadge.Render(p.Kind) + "  " + icon + " " + sMuted.Render(stat), ""}
	lines = append(lines, wrap(p.Description, 70)...)
	lines = append(lines, "")
	kv := func(k, v string) {
		if v != "" {
			lines = append(lines, pad(sMuted.Render(k), 12)+sText.Render(v))
		}
	}
	kv("version", p.Version)
	kv("requires", strings.Join(p.Requires, ", "))
	kv("metamod", strings.Join(p.Metamod, ", "))
	if p.Namespace != "" {
		kv("namespace", "xmod."+p.Namespace)
	}
	kv("file", p.File)
	kv("upstream", p.Upstream)
	if ip := st.Packages[p.Name]; ip != nil {
		kv("installed", ip.Version)
		kv("enabled", fmt.Sprint(ip.Enabled))
	}
	fmt.Println(sPanel.Render(strings.Join(lines, "\n")))
	return nil
}

func addServersFromArgs(cfg *Config, rest []string, a args) {
	for _, r := range rest {
		sv := SavedServer{Addr: normAddr(r), Rcon: a.flags["rcon"], Name: a.flags["name"]}
		cfg.AddServer(sv)
	}
	if len(rest) > 0 {
		cfg.Save()
		sharedPool = nil
	}
}

func serversCmd(cfg *Config, rest []string, a args) error {
	if len(rest) == 0 || rest[0] == "ls" || rest[0] == "list" {
		if len(cfg.Servers) == 0 {
			fmt.Println(sMuted.Render("no saved servers — xcmd servers add <ip:port> [--name N] [--rcon PASS]"))
		}
		for _, s := range cfg.Servers {
			extra := ""
			if s.Rcon != "" {
				extra += " rcon"
			}
			if s.Dir != "" {
				extra += " local:" + s.Dir
			}
			fmt.Println("  " + sOK.Render("●") + " " + pad(sText.Render(s.Addr), 24) + pad(s.Name, 30) + sMuted.Render(extra))
		}
		return nil
	}
	switch rest[0] {
	case "add":
		if len(rest) < 2 {
			return fmt.Errorf("xcmd servers add <ip:port> [--name N] [--rcon PASS] [--dir local_dir]")
		}
		sv := SavedServer{Addr: normAddr(rest[1]), Name: a.flags["name"], Rcon: a.flags["rcon"]}
		if a.has("dir") {
			sv.Dir = dirOf(cfg, a)
		}
		cfg.AddServer(sv)
		okMsg("saved " + sv.Addr)
	case "rm", "remove", "del":
		if len(rest) < 2 {
			return fmt.Errorf("xcmd servers rm <ip:port|name>")
		}
		var keep []SavedServer
		for _, s := range cfg.Servers {
			if s.Addr != normAddr(rest[1]) && s.Name != rest[1] {
				keep = append(keep, s)
			}
		}
		cfg.Servers = keep
		okMsg("removed " + rest[1])
	default:
		return fmt.Errorf("servers ls | add | rm")
	}
	return cfg.Save()
}

func startServer(cfg *Config, dir string, a args) error {
	if !hldsInstalled(dir) {
		return fmt.Errorf("no server in %s — run: xcmd setup --dir \"%s\"", dir, dir)
	}
	port := a.flags["port"]
	if port == "" {
		port = "27015"
	}
	mp := a.flags["map"]
	if mp == "" {
		mp = "de_dust2"
	}
	maxp := a.flags["maxplayers"]
	if maxp == "" {
		maxp = "32"
	}
	argv := []string{"-console", "-game", "cstrike", "-port", port, "+maxplayers", maxp, "+map", mp, "+sv_lan", "0"}
	if a.flags["rcon"] != "" {
		argv = append(argv, "+rcon_password", a.flags["rcon"])
	}
	bin := hldsBinary(dir)
	cmd := exec.Command(bin, argv...)
	cmd.Dir = dir
	cmd.Stdin, cmd.Stdout, cmd.Stderr = os.Stdin, os.Stdout, os.Stderr
	if runtime.GOOS != "windows" {
		cmd.Env = append(os.Environ(), "LD_LIBRARY_PATH="+dir+":"+os.Getenv("LD_LIBRARY_PATH"))
		if runtime.GOOS == "linux" {
			cmd.Args = append([]string{bin}, argv...)
		}
	}
	// remember it for the monitor
	cfg.AddServer(SavedServer{Name: "", Addr: "127.0.0.1:" + port, Dir: dir, Rcon: a.flags["rcon"]})
	cfg.Save()
	info("starting " + bin + " " + strings.Join(argv, " "))
	info("monitor it from another terminal: " + sText.Render("xcmd monitor"))
	return cmd.Run()
}

func doctor(cfg *Config, dir string) {
	fmt.Println(sSub.Render("xcmd doctor") + sMuted.Render("  "+runtime.GOOS+"/"+runtime.GOARCH+"  "+xcmdVersion))
	check := func(ok bool, okText, badText string) {
		if ok {
			okMsg(okText)
		} else {
			warnMsg(badText)
		}
	}
	if runtime.GOOS == "linux" {
		has32 := fileExists("/lib/i386-linux-gnu/libc.so.6") || fileExists("/lib32/libc.so.6") || fileExists("/usr/lib32/libc.so.6") || fileExists("/usr/lib/i386-linux-gnu/libc.so.6")
		check(has32, "32-bit libraries present", "32-bit libraries missing: sudo dpkg --add-architecture i386 && sudo apt update && sudo apt install lib32gcc-s1 lib32stdc++6")
		h, _ := os.UserHomeDir()
		check(fileExists(filepath.Join(h, ".steam", "sdk32", "steamclient.so")), "~/.steam/sdk32/steamclient.so present", "~/.steam/sdk32/steamclient.so missing (xcmd install hlds puts it there)")
	}
	// network
	var r struct {
		Resources struct {
			Core struct {
				Remaining int `json:"remaining"`
				Limit     int `json:"limit"`
			} `json:"core"`
		} `json:"resources"`
	}
	err := getJSON(ghAPI()+"/rate_limit", &r)
	check(err == nil, fmt.Sprintf("GitHub API reachable (%d/%d requests left this hour)", r.Resources.Core.Remaining, r.Resources.Core.Limit), fmt.Sprintf("GitHub API: %v", err))
	_, err = getBytes(rawURL(cfg.Repo, cfg.Branch, "README.md"))
	if err != nil {
		_, err = ghContents(cfg.Repo, cfg.Branch, "")
	}
	check(err == nil, "xmod repository reachable: "+cfg.Repo, fmt.Sprintf("xmod repository %s: %v", cfg.Repo, err))
	if c, err := net.DialTimeout("udp", cfg.Masters[0], 3*time.Second); err == nil {
		c.Close()
		okMsg("master server resolves: " + cfg.Masters[0])
	} else {
		warnMsg("master server: " + err.Error())
	}
	// server
	fmt.Println(sSub.Render("Server ") + sText.Render(dir))
	check(hldsInstalled(dir), "HLDS + cstrike installed", "HLDS not installed — xcmd setup")
	lib, _ := os.ReadFile(filepath.Join(dir, "cstrike", "liblist.gam"))
	check(strings.Contains(string(lib), "metamod"), "liblist.gam loads Metamod", "liblist.gam does not load Metamod — xcmd install metamod")
	pl, _ := os.ReadFile(filepath.Join(dir, "cstrike", "addons", "metamod", "plugins.ini"))
	check(strings.Contains(string(pl), "xmod_mm"), "Metamod loads xmod", "metamod/plugins.ini has no xmod line — xcmd install xmod")
	st := loadState(dir)
	var names []string
	for n := range st.Packages {
		names = append(names, n)
	}
	sort.Strings(names)
	if len(names) > 0 {
		okMsg("packages: " + strings.Join(names, ", "))
	}
	for _, ip := range st.Packages {
		missing := 0
		for _, f := range ip.Files {
			if !fileExists(filepath.Join(dir, f.Path)) && !fileExists(filepath.Join(dir, f.Path+".disabled")) {
				missing++
			}
		}
		if missing > 0 {
			warnMsg(fmt.Sprintf("%s: %d file(s) missing — xcmd update %s --force", ip.Name, missing, ip.Name))
		}
	}
	pending, _ := filepath.Glob(filepath.Join(dir, "cstrike", "addons", "*", "dlls", "*.xcmd-pending"))
	if len(pending) > 0 {
		warnMsg(fmt.Sprintf("%d file(s) waiting (server was running during an update) — stop it and run xcmd update --force", len(pending)))
	}
	ps := findServerProc(dir)
	if ps.Found {
		okMsg(fmt.Sprintf("server running: pid %d, %.1f%% cpu, %s", ps.PID, ps.CPU, humanBytes(ps.RSS)))
	} else {
		fmt.Println(sMuted.Render("  server not running"))
	}
}

func usage() {
	fmt.Println(logo())
	fmt.Println(sMuted.Render("  SteamCMD for xmod — installer · live monitor · tactical map · mod library · server browser  v" + xcmdVersion))
	fmt.Println()
	sec := func(t string) { fmt.Println(sSub.Render(t)) }
	row := func(c, d string) { fmt.Println("  " + pad(sText.Bold(true).Render(c), 44) + sMuted.Render(d)) }
	sec("Start")
	row("xcmd", "the full-screen app (home, monitor, map, mods, browser)")
	sec("Install")
	row("xcmd setup <profile> --dir D [--zip]", "xmod | metamod | regamedll | rehlds | all  (from GitHub, ready for FTP)")
	row("xcmd install hlds --dir D", "optional: a full HLDS through SteamCMD")
	row("xcmd install <pkg...>", "hlds rehlds regamedll metamod xmod codex modcreator xbot query agent <mod> all")
	row("xcmd update [pkg...] [--zip]", "newest releases (--zip: only the changed files, for FTP)")
	row("xcmd start [--map M --port P --rcon PW]", "run the server")
	sec("Mods")
	row("xcmd search [text]", "the library (your GitHub repo + upstream releases)")
	row("xcmd info <pkg>", "details")
	row("xcmd list", "installed packages")
	row("xcmd enable|disable|remove <pkg>", "manage")
	row("xcmd mods", "library UI")
	sec("Live")
	row("xcmd monitor [ip:port] [--rcon PW]", "htop for your servers (players, money, weapons, CPU, RAM)")
	row("xcmd map [ip:port] [--rcon PW] [--maps DIR]", "live top-down tactical map (DIR: your cstrike/maps)")
	row("xcmd browser", "every xmod server in the world")
	row("xcmd servers [add|rm|ls]", "saved servers")
	sec("Other")
	row("xcmd doctor", "check this machine and the server")
	row("xcmd dir [path]", "show / set the default server folder")
	fmt.Println()
	fmt.Println(sMuted.Render("  flags: --dir D (folder, default " + loadConfig().DefaultDir + ") · --os linux|windows|both · --zip · --no-agent · --plain · --force"))
}

var makeZipAfter bool

type profile struct {
	Key, Title, Desc string
	Pkgs             []string
}

// what "xcmd setup <profile>" puts in the folder (nothing from Steam: only GitHub releases + the xmod repository)
var profiles = []profile{
	{"xmod", "xmod", "xmod alone - the server already has Metamod", []string{"xmod"}},
	{"metamod", "xmod + Metamod-R", "for a server without Metamod", []string{"metamod", "xmod"}},
	{"regamedll", "xmod + Metamod-R + ReGameDLL", "+ the ReGameDLL hooks (xmod.reapi)", []string{"metamod", "regamedll", "xmod"}},
	{"rehlds", "xmod + Metamod-R + ReGameDLL + ReHLDS", "the whole Re stack (full xmod.reapi)", []string{"rehlds", "metamod", "regamedll", "xmod"}},
	{"all", "Everything", "Re stack + xmod + Codex, ModCreator, xbots, query", []string{"rehlds", "metamod", "regamedll", "xmod", "query", "codex", "xbot", "modcreator"}},
}

func findProfile(k string) *profile {
	for i := range profiles {
		if profiles[i].Key == k {
			return &profiles[i]
		}
	}
	return nil
}
