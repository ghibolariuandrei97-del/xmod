package main

// GoldSrc BSP (v30) -> top-down "overview": every walkable (upward facing) face of the world is rasterised into a
// height map; the renderer colours it by height and outlines walls. No overview images needed - the server's own
// maps/<map>.bsp is enough (or FastDL).

import (
	"compress/bzip2"
	"encoding/binary"
	"errors"
	"io"
	"math"
	"os"
	"path/filepath"
	"strings"
)

type HeightMap struct {
	W, H       int
	MinX, MinY float64
	Cell       float64 // world units per pixel
	Z          []float32
	ZLo, ZHi   float32
	Name       string
}

func (hm *HeightMap) at(x, y int) float32 {
	if x < 0 || y < 0 || x >= hm.W || y >= hm.H {
		return float32(math.NaN())
	}
	return hm.Z[y*hm.W+x]
}

type bspLump struct{ off, len int32 }

func loadBSP(path string, res int) (*HeightMap, error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	return parseBSP(b, res, strings.TrimSuffix(filepath.Base(path), ".bsp"))
}

func parseBSP(b []byte, res int, name string) (*HeightMap, error) {
	if len(b) < 4+15*8 {
		return nil, errors.New("not a bsp")
	}
	ver := binary.LittleEndian.Uint32(b)
	if ver != 30 {
		return nil, errors.New("unsupported bsp version (GoldSrc v30 expected)")
	}
	l := make([]bspLump, 15)
	for i := range l {
		l[i].off = int32(binary.LittleEndian.Uint32(b[4+i*8:]))
		l[i].len = int32(binary.LittleEndian.Uint32(b[8+i*8:]))
		if l[i].off < 0 || l[i].len < 0 || int(l[i].off+l[i].len) > len(b) {
			return nil, errors.New("corrupt bsp lump table")
		}
	}
	f32 := func(o int) float64 { return float64(math.Float32frombits(binary.LittleEndian.Uint32(b[o:]))) }
	u16 := func(o int) int { return int(binary.LittleEndian.Uint16(b[o:])) }
	i32 := func(o int) int { return int(int32(binary.LittleEndian.Uint32(b[o:]))) }
	// planes (20 bytes), vertices (12), edges (4), surfedges (4), faces (20), texinfo (40), models (64)
	planes, verts, edges, surf, faces, tinfo, tex, models := l[1], l[3], l[12], l[13], l[7], l[6], l[2], l[14]
	nPlanes, nVerts, nEdges, nSurf, nFaces, nTinfo := int(planes.len)/20, int(verts.len)/12, int(edges.len)/4, int(surf.len)/4, int(faces.len)/20, int(tinfo.len)/40
	if models.len < 64 || nFaces == 0 {
		return nil, errors.New("bsp without world model")
	}
	// texture names (to skip sky / tool textures)
	texName := func(miptex int) string {
		if tex.len < 4 {
			return ""
		}
		n := i32(int(tex.off))
		if miptex < 0 || miptex >= n {
			return ""
		}
		o := i32(int(tex.off) + 4 + miptex*4)
		if o < 0 {
			return ""
		}
		p := int(tex.off) + o
		if p+16 > len(b) {
			return ""
		}
		s := b[p : p+16]
		if i := strings.IndexByte(string(s), 0); i >= 0 {
			s = s[:i]
		}
		return strings.ToLower(string(s))
	}
	firstFace, numFaces := i32(int(models.off)+56), i32(int(models.off)+60)
	type poly struct {
		pts    [][3]float64
		nx, ny float64
		nz, d  float64
	}
	var polys []poly
	minX, minY, maxX, maxY := math.Inf(1), math.Inf(1), math.Inf(-1), math.Inf(-1)
	for fi := firstFace; fi < firstFace+numFaces && fi < nFaces; fi++ {
		fo := int(faces.off) + fi*20
		pl, side, fe, ne, ti := u16(fo), u16(fo+2), i32(fo+4), u16(fo+8), u16(fo+10)
		if pl >= nPlanes || ne < 3 {
			continue
		}
		if ti < nTinfo {
			tn := texName(i32(int(tinfo.off) + ti*40 + 32))
			if strings.HasPrefix(tn, "sky") || tn == "aaatrigger" || tn == "clip" || tn == "null" || tn == "origin" || tn == "hint" || tn == "skip" || tn == "black" {
				continue
			}
		}
		po := int(planes.off) + pl*20
		nx, ny, nz, d := f32(po), f32(po+4), f32(po+8), f32(po+12)
		if side != 0 {
			nx, ny, nz, d = -nx, -ny, -nz, -d
		}
		if nz < 0.55 { // walls and ceilings
			continue
		}
		var pts [][3]float64
		ok := true
		for k := 0; k < ne; k++ {
			si := fe + k
			if si < 0 || si >= nSurf {
				ok = false
				break
			}
			e := i32(int(surf.off) + si*4)
			var vi int
			if e >= 0 {
				if e >= nEdges {
					ok = false
					break
				}
				vi = u16(int(edges.off) + e*4)
			} else {
				if -e >= nEdges {
					ok = false
					break
				}
				vi = u16(int(edges.off) + (-e)*4 + 2)
			}
			if vi >= nVerts {
				ok = false
				break
			}
			vo := int(verts.off) + vi*12
			p := [3]float64{f32(vo), f32(vo + 4), f32(vo + 8)}
			pts = append(pts, p)
			minX, maxX = math.Min(minX, p[0]), math.Max(maxX, p[0])
			minY, maxY = math.Min(minY, p[1]), math.Max(maxY, p[1])
		}
		if ok {
			polys = append(polys, poly{pts: pts, nx: nx, ny: ny, nz: nz, d: d})
		}
	}
	if len(polys) == 0 {
		return nil, errors.New("no floors found in the bsp")
	}
	dx, dy := maxX-minX, maxY-minY
	cell := math.Max(dx, dy) / float64(res)
	if cell <= 0 {
		cell = 1
	}
	hm := &HeightMap{W: int(dx/cell) + 2, H: int(dy/cell) + 2, MinX: minX, MinY: minY, Cell: cell, Name: name}
	hm.Z = make([]float32, hm.W*hm.H)
	nan := float32(math.NaN())
	for i := range hm.Z {
		hm.Z[i] = nan
	}
	// rasterise each polygon (convex, as all BSP faces) - image y grows downward = world y decreasing
	for _, p := range polys {
		x0, y0, x1, y1 := math.Inf(1), math.Inf(1), math.Inf(-1), math.Inf(-1)
		for _, v := range p.pts {
			x0, x1 = math.Min(x0, v[0]), math.Max(x1, v[0])
			y0, y1 = math.Min(y0, v[1]), math.Max(y1, v[1])
		}
		ix0, ix1 := int((x0-minX)/cell), int((x1-minX)/cell)+1
		iy0, iy1 := int((y0-minY)/cell), int((y1-minY)/cell)+1
		for iy := iy0; iy <= iy1; iy++ {
			wy := minY + (float64(iy)+0.5)*cell
			for ix := ix0; ix <= ix1; ix++ {
				wx := minX + (float64(ix)+0.5)*cell
				if !inConvex(p.pts, wx, wy) {
					continue
				}
				z := float32((p.d - p.nx*wx - p.ny*wy) / p.nz)
				py := hm.H - 1 - iy
				if ix < 0 || ix >= hm.W || py < 0 || py >= hm.H {
					continue
				}
				o := py*hm.W + ix
				if math.IsNaN(float64(hm.Z[o])) || z > hm.Z[o] {
					hm.Z[o] = z
				}
			}
		}
	}
	// height range (2..98 percentile) for the colours
	var zs []float64
	for _, z := range hm.Z {
		if !math.IsNaN(float64(z)) {
			zs = append(zs, float64(z))
		}
	}
	if len(zs) == 0 {
		return nil, errors.New("empty raster")
	}
	sortFloats(zs)
	hm.ZLo, hm.ZHi = float32(zs[len(zs)*2/100]), float32(zs[len(zs)*98/100])
	if hm.ZHi-hm.ZLo < 1 {
		hm.ZHi = hm.ZLo + 1
	}
	return hm, nil
}

func inConvex(pts [][3]float64, x, y float64) bool {
	sign := 0.0
	n := len(pts)
	for i := 0; i < n; i++ {
		a, c := pts[i], pts[(i+1)%n]
		cr := (c[0]-a[0])*(y-a[1]) - (c[1]-a[1])*(x-a[0])
		if math.Abs(cr) < 1e-9 {
			continue
		}
		if sign == 0 {
			sign = cr
		} else if (cr > 0) != (sign > 0) {
			return false
		}
	}
	return true
}

func sortFloats(a []float64) {
	// small insertion-free sort without importing sort for float64 slices of any size
	quick(a, 0, len(a)-1)
}
func quick(a []float64, lo, hi int) {
	for lo < hi {
		p := a[(lo+hi)/2]
		i, j := lo, hi
		for i <= j {
			for a[i] < p {
				i++
			}
			for a[j] > p {
				j--
			}
			if i <= j {
				a[i], a[j] = a[j], a[i]
				i++
				j--
			}
		}
		if j-lo < hi-i {
			quick(a, lo, j)
			lo = i
		} else {
			quick(a, i, hi)
			hi = j
		}
	}
}

// world -> heightmap pixel
func (hm *HeightMap) toPix(x, y float64) (float64, float64) {
	return (x - hm.MinX) / hm.Cell, float64(hm.H-1) - (y-hm.MinY)/hm.Cell
}

// readMaybeBz2 for FastDL downloads
func readMaybeBz2(path string) ([]byte, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	if strings.HasSuffix(path, ".bz2") {
		return io.ReadAll(bzip2.NewReader(f))
	}
	return io.ReadAll(f)
}
