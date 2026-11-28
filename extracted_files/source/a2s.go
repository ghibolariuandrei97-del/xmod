package main

// Server queries: A2S_INFO / A2S_PLAYER / A2S_RULES (GoldSrc + Source formats, challenges, split packets),
// GoldSrc UDP rcon, and the Steam master server.

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"math"
	"net"
	"strconv"
	"strings"
	"time"
)

type ServerInfo struct {
	Addr       string
	Name       string
	Map        string
	Folder     string
	Game       string
	Players    int
	MaxPlayers int
	Bots       int
	Dedicated  byte
	OS         byte
	Password   bool
	Version    string
	Ping       time.Duration
}

type A2SPlayer struct {
	Name     string
	Score    int
	Duration float64
}

func normAddr(a string) string {
	if !strings.Contains(a, ":") {
		return a + ":27015"
	}
	return a
}

type rd struct {
	b []byte
	i int
}

func (r *rd) byte() byte {
	if r.i >= len(r.b) {
		r.i++
		return 0
	}
	v := r.b[r.i]
	r.i++
	return v
}
func (r *rd) str() string {
	if r.i >= len(r.b) {
		return ""
	}
	j := bytes.IndexByte(r.b[r.i:], 0)
	if j < 0 {
		s := string(r.b[r.i:])
		r.i = len(r.b)
		return s
	}
	s := string(r.b[r.i : r.i+j])
	r.i += j + 1
	return s
}
func (r *rd) i16() int16 {
	if r.i+2 > len(r.b) {
		r.i += 2
		return 0
	}
	v := int16(binary.LittleEndian.Uint16(r.b[r.i:]))
	r.i += 2
	return v
}
func (r *rd) i32() int32 {
	if r.i+4 > len(r.b) {
		r.i += 4
		return 0
	}
	v := int32(binary.LittleEndian.Uint32(r.b[r.i:]))
	r.i += 4
	return v
}
func (r *rd) f32() float32 {
	if r.i+4 > len(r.b) {
		r.i += 4
		return 0
	}
	v := binary.LittleEndian.Uint32(r.b[r.i:])
	r.i += 4
	return math.Float32frombits(v)
}

// recv one logical response: handles GoldSrc split packets (0xFFFFFFFE)
func recvPacket(c net.Conn, deadline time.Time) ([]byte, error) {
	buf := make([]byte, 4096)
	parts := map[int][]byte{}
	total := -1
	for {
		c.SetReadDeadline(deadline)
		n, err := c.Read(buf)
		if err != nil {
			return nil, err
		}
		p := append([]byte(nil), buf[:n]...)
		if n < 4 {
			continue
		}
		hdr := int32(binary.LittleEndian.Uint32(p))
		if hdr == -1 {
			return p[4:], nil
		}
		if hdr != -2 || n < 9 {
			continue
		}
		// GoldSrc split: int32 id, byte (index<<4 | total), payload
		num := p[8]
		tot, idx, payload := int(num&0x0f), int(num>>4), p[9:]
		total = tot
		parts[idx] = payload
		if len(parts) == total {
			var all []byte
			for i := 0; i < total; i++ {
				all = append(all, parts[i]...)
			}
			if len(all) >= 4 && int32(binary.LittleEndian.Uint32(all)) == -1 {
				all = all[4:]
			}
			return all, nil
		}
	}
}

func udpQuery(addr string, req []byte, timeout time.Duration) ([]byte, time.Duration, error) {
	c, err := net.DialTimeout("udp", normAddr(addr), timeout)
	if err != nil {
		return nil, 0, err
	}
	defer c.Close()
	start := time.Now()
	if _, err := c.Write(req); err != nil {
		return nil, 0, err
	}
	b, err := recvPacket(c, start.Add(timeout))
	return b, time.Since(start), err
}

var hdr = []byte{0xff, 0xff, 0xff, 0xff}

func A2SInfo(addr string, timeout time.Duration) (*ServerInfo, error) {
	req := append(append([]byte{}, hdr...), append([]byte("TSource Engine Query"), 0)...)
	b, rtt, err := udpQuery(addr, req, timeout)
	if err != nil {
		return nil, err
	}
	if len(b) > 0 && b[0] == 'A' && len(b) >= 5 { // challenge (2020+ servers)
		req2 := append(append([]byte{}, req...), b[1:5]...)
		b, rtt, err = udpQuery(addr, req2, timeout)
		if err != nil {
			return nil, err
		}
	}
	if len(b) == 0 {
		return nil, errors.New("empty reply")
	}
	si := &ServerInfo{Addr: normAddr(addr), Ping: rtt}
	r := &rd{b: b, i: 1}
	switch b[0] {
	case 'I':
		r.byte() // protocol
		si.Name, si.Map, si.Folder, si.Game = r.str(), r.str(), r.str(), r.str()
		r.i16()
		si.Players, si.MaxPlayers, si.Bots = int(r.byte()), int(r.byte()), int(r.byte())
		si.Dedicated, si.OS = r.byte(), r.byte()
		si.Password = r.byte() == 1
		r.byte() // vac
		si.Version = r.str()
	case 'm':
		r.str() // address
		si.Name, si.Map, si.Folder, si.Game = r.str(), r.str(), r.str(), r.str()
		si.Players, si.MaxPlayers = int(r.byte()), int(r.byte())
		r.byte()
		si.Dedicated, si.OS = r.byte(), r.byte()
		si.Password = r.byte() == 1
	default:
		return nil, fmt.Errorf("unexpected reply %q", b[0])
	}
	return si, nil
}

func a2sChallenged(addr string, kind byte, timeout time.Duration) ([]byte, error) {
	req := append(append([]byte{}, hdr...), kind, 0xff, 0xff, 0xff, 0xff)
	b, _, err := udpQuery(addr, req, timeout)
	if err != nil {
		return nil, err
	}
	if len(b) >= 5 && b[0] == 'A' {
		req = append(append([]byte{}, hdr...), kind)
		req = append(req, b[1:5]...)
		b, _, err = udpQuery(addr, req, timeout)
		if err != nil {
			return nil, err
		}
	}
	return b, nil
}

func A2SPlayers(addr string, timeout time.Duration) ([]A2SPlayer, error) {
	b, err := a2sChallenged(addr, 'U', timeout)
	if err != nil {
		return nil, err
	}
	if len(b) < 2 || b[0] != 'D' {
		return nil, errors.New("bad player reply")
	}
	r := &rd{b: b, i: 1}
	n := int(r.byte())
	var out []A2SPlayer
	for i := 0; i < n && r.i < len(b); i++ {
		r.byte()
		p := A2SPlayer{Name: r.str(), Score: int(r.i32()), Duration: float64(r.f32())}
		out = append(out, p)
	}
	return out, nil
}

func A2SRules(addr string, timeout time.Duration) (map[string]string, error) {
	b, err := a2sChallenged(addr, 'V', timeout)
	if err != nil {
		return nil, err
	}
	if len(b) < 3 || b[0] != 'E' {
		return nil, errors.New("bad rules reply")
	}
	r := &rd{b: b, i: 1}
	n := int(r.i16())
	m := map[string]string{}
	for i := 0; i < n && r.i < len(b); i++ {
		k := r.str()
		m[k] = r.str()
	}
	return m, nil
}

// ---------------------------------------------------------------- rcon (GoldSrc, UDP)

var rconChallenges = map[string]string{}

func rconExec(addr, pass, cmd string, timeout time.Duration) (string, error) {
	addr = normAddr(addr)
	c, err := net.DialTimeout("udp", addr, timeout)
	if err != nil {
		return "", err
	}
	defer c.Close()
	deadline := time.Now().Add(timeout)
	ch := rconChallenges[addr]
	if ch == "" {
		c.Write(append(append([]byte{}, hdr...), []byte("challenge rcon\n")...))
		buf := make([]byte, 1400)
		c.SetReadDeadline(deadline)
		n, err := c.Read(buf)
		if err != nil {
			return "", fmt.Errorf("no rcon challenge: %v", err)
		}
		f := strings.Fields(string(buf[4:n]))
		if len(f) < 3 {
			return "", fmt.Errorf("bad rcon challenge %q", string(buf[4:n]))
		}
		ch = f[2]
		rconChallenges[addr] = ch
	}
	c.Write(append(append([]byte{}, hdr...), []byte(fmt.Sprintf("rcon %s \"%s\" %s\n", ch, pass, cmd))...))
	var out bytes.Buffer
	buf := make([]byte, 4096)
	quiet := 120 * time.Millisecond
	for {
		d := deadline
		if out.Len() > 0 {
			d = time.Now().Add(quiet)
		}
		c.SetReadDeadline(d)
		n, err := c.Read(buf)
		if err != nil {
			break
		}
		p := buf[:n]
		if len(p) >= 4 && int32(binary.LittleEndian.Uint32(p)) == -1 {
			p = p[4:]
		}
		if len(p) > 0 && p[0] == 'l' {
			p = p[1:]
		}
		out.Write(bytes.TrimRight(p, "\x00"))
		if strings.Contains(out.String(), "Bad challenge") {
			delete(rconChallenges, addr)
			return "", fmt.Errorf("bad rcon challenge")
		}
	}
	if out.Len() == 0 {
		delete(rconChallenges, addr)
		return "", fmt.Errorf("no rcon answer (wrong password or rcon blocked)")
	}
	return out.String(), nil
}

// ---------------------------------------------------------------- master server

// MasterQuery lists server addresses (filter e.g. `\appid\10\gamedir\cstrike`); fn gets each page.
func MasterQuery(master, filter string, maxPages int, fn func(page []string)) error {
	c, err := net.DialTimeout("udp", master, 4*time.Second)
	if err != nil {
		return err
	}
	defer c.Close()
	seed := "0.0.0.0:0"
	buf := make([]byte, 8192)
	for page := 0; page < maxPages; page++ {
		req := append([]byte{0x31, 0xff}, []byte(seed)...)
		req = append(req, 0)
		req = append(req, []byte(filter)...)
		req = append(req, 0)
		var n int
		var rerr error
		for try := 0; try < 3; try++ {
			c.Write(req)
			c.SetReadDeadline(time.Now().Add(3 * time.Second))
			n, rerr = c.Read(buf)
			if rerr == nil {
				break
			}
		}
		if rerr != nil {
			if page == 0 {
				return fmt.Errorf("master %s: %v", master, rerr)
			}
			return nil
		}
		if n < 6 || buf[4] != 0x66 {
			return fmt.Errorf("master %s: bad reply", master)
		}
		var list []string
		last := ""
		for i := 6; i+6 <= n; i += 6 {
			ip := net.IPv4(buf[i], buf[i+1], buf[i+2], buf[i+3]).String()
			port := int(binary.BigEndian.Uint16(buf[i+4:]))
			a := ip + ":" + strconv.Itoa(port)
			last = a
			if a == "0.0.0.0:0" {
				fn(list)
				return nil
			}
			list = append(list, a)
		}
		fn(list)
		if last == "" || last == seed {
			return nil
		}
		seed = last
		time.Sleep(1100 * time.Millisecond) // masters rate-limit
	}
	return nil
}
