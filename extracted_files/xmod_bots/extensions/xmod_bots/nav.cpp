// nav.cpp - the navigation graph: nodes + one-way links, A* with learned danger, an automatic map
// analyzer (flood fill from the spawn points with hull traces, a few ms per frame), learning new nodes
// from how human players move (ladders, jumps, crouch spots included), and the .nav file format.
#include "xbots.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <queue>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

Nav g_nav;

static const float STEP = 64.f;      // analyzer grid
static const float STEPH = 18.f;     // stair step height
static const float JUMPH = 44.f;     // jump height (crouch-jump reaches a bit more)
static const float MAXDROP = 190.f;  // highest drop without much damage
static const float MERGE = 40.f;

// ---------------------------------------------------------------------------------------------
static void mkdir_one(const std::string &p)
{
#ifdef _WIN32
    _mkdir(p.c_str());
#else
    mkdir(p.c_str(), 0755);
#endif
}

std::string xb_data_path(const char *file)
{
    char gd[256] = {0};
    g_engfuncs.pfnGetGameDir(gd);
    std::string base = gd;
    for (const char *part : {"/addons", "/xmod", "/data", "/bots"}) { base += part; mkdir_one(base); }
    return base + "/" + file;
}

static std::string safe_map(const char *map)
{
    std::string s;
    for (const char *p = map; p && *p && s.size() < 60; ++p) {
        char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.') s += c;
    }
    return s;
}

// ---------------------------------------------------------------------------------------------
// basic graph operations
void Nav::clear()
{
    nodes.clear();
    grid.clear();
    an_active = false;
    an_queue.clear();
    an_head = 0;
    for (int i = 0; i < 33; ++i) learn_last[i] = -1;
    for (Bot &b : g_bots) if (b.used && b.moveMode == MOVE_PATH) { b.path.clear(); b.moveMode = MOVE_NONE; }
    dirty = true;
}

void Nav::grid_add(int i)
{
    const Vector &p = nodes[i].pos;
    grid[cell_key((int)floorf(p.x / 128.f), (int)floorf(p.y / 128.f))].push_back(i);
}

void Nav::rebuild_grid()
{
    grid.clear();
    for (int i = 0; i < (int)nodes.size(); ++i) grid_add(i);
}

int Nav::add(const Vector &pos, uint32_t flags)
{
    if ((int)nodes.size() >= 16000) return -1;
    NavNode n;
    n.pos = pos;
    n.flags = flags;
    nodes.push_back(n);
    int i = (int)nodes.size() - 1;
    grid_add(i);
    dirty = true;
    return i;
}

void Nav::remove(int i)
{
    if (i < 0 || i >= (int)nodes.size()) return;
    int last = (int)nodes.size() - 1;
    for (NavNode &n : nodes) {
        n.links.erase(std::remove_if(n.links.begin(), n.links.end(), [i](const NavLink &l) { return l.to == i; }), n.links.end());
        for (NavLink &l : n.links) if (l.to == last) l.to = i;
    }
    if (i != last) nodes[i] = nodes[last];
    nodes.pop_back();
    rebuild_grid();
    for (int k = 0; k < 33; ++k) learn_last[k] = -1;
    for (Bot &b : g_bots) if (b.used && b.moveMode == MOVE_PATH) { b.path.clear(); b.moveMode = MOVE_NONE; }
    an_active = false;
    dirty = true;
}

bool Nav::link(int a, int b, uint8_t flags)
{
    if (a == b || a < 0 || b < 0 || a >= (int)nodes.size() || b >= (int)nodes.size()) return false;
    for (NavLink &l : nodes[a].links) if (l.to == b) { l.flags = flags; dirty = true; return true; }
    if (nodes[a].links.size() >= 16) return false;
    nodes[a].links.push_back({b, flags});
    dirty = true;
    return true;
}

bool Nav::unlink(int a, int b)
{
    if (a < 0 || a >= (int)nodes.size()) return false;
    auto &v = nodes[a].links;
    size_t before = v.size();
    v.erase(std::remove_if(v.begin(), v.end(), [b](const NavLink &l) { return l.to == b; }), v.end());
    if (v.size() != before) { dirty = true; return true; }
    return false;
}

bool Nav::linked(int a, int b) const { return link_flags(a, b) >= 0; }

int Nav::link_flags(int a, int b) const
{
    if (a < 0 || a >= (int)nodes.size()) return -1;
    for (const NavLink &l : nodes[a].links) if (l.to == b) return l.flags;
    return -1;
}

void Nav::near_list(const Vector &pos, float radius, std::vector<int> &out) const
{
    out.clear();
    int r = (int)ceilf(radius / 128.f);
    int cx = (int)floorf(pos.x / 128.f), cy = (int)floorf(pos.y / 128.f);
    for (int x = cx - r; x <= cx + r; ++x)
        for (int y = cy - r; y <= cy + r; ++y) {
            auto it = grid.find(cell_key(x, y));
            if (it == grid.end()) continue;
            for (int i : it->second) if (xb_dist(nodes[i].pos, pos) <= radius) out.push_back(i);
        }
}

int Nav::nearest(const Vector &pos, float maxDist, bool needVisible) const
{
    if (nodes.empty()) return -1;
    std::vector<std::pair<float, int>> cand;
    if (maxDist <= 1024.f) {
        std::vector<int> l;
        near_list(pos, maxDist, l);
        for (int i : l) cand.push_back({xb_dist(nodes[i].pos, pos), i});
    } else {
        for (int i = 0; i < (int)nodes.size(); ++i) {
            float d = xb_dist(nodes[i].pos, pos);
            if (d <= maxDist) cand.push_back({d, i});
        }
    }
    if (cand.empty()) return -1;
    std::sort(cand.begin(), cand.end());
    if (!needVisible) return cand[0].second;
    int checks = 0;
    for (auto &c : cand) {
        TraceResult tr;
        g_engfuncs.pfnTraceLine(pos, nodes[c.second].pos, 1, nullptr, &tr);
        if (tr.flFraction >= 1.f) return c.second;
        if (++checks >= 10) break;
    }
    return cand[0].second;
}

// ---------------------------------------------------------------------------------------------
// A*: cost = distance x (1 + danger) + small extra for jumps / crouching / ladders, plus a bit of
// randomness so that bots do not all walk the same corridor.
bool Nav::find_path(int from, int to, int team, std::vector<int> &out)
{
    out.clear();
    int N = (int)nodes.size();
    if (from < 0 || to < 0 || from >= N || to >= N) return false;
    if (from == to) { out.push_back(to); return true; }
    static std::vector<float> g;
    static std::vector<int> par;
    static std::vector<uint32_t> open, closed;
    static uint32_t stamp = 0;
    if ((int)g.size() < N) { g.resize(N); par.resize(N); open.resize(N, 0); closed.resize(N, 0); }
    if (++stamp == 0) { std::fill(open.begin(), open.end(), 0); std::fill(closed.begin(), closed.end(), 0); stamp = 1; }
    int ti = team == 1 ? 0 : (team == 2 ? 1 : -1);
    typedef std::pair<float, int> QE;
    std::priority_queue<QE, std::vector<QE>, std::greater<QE>> pq;
    const Vector &goal = nodes[to].pos;
    g[from] = 0.f; par[from] = -1; open[from] = stamp;
    pq.push({xb_dist(nodes[from].pos, goal), from});
    while (!pq.empty()) {
        int n = pq.top().second;
        pq.pop();
        if (closed[n] == stamp) continue;
        closed[n] = stamp;
        if (n == to) break;
        for (const NavLink &l : nodes[n].links) {
            int m = l.to;
            if (m < 0 || m >= N || closed[m] == stamp) continue;
            const NavNode &nm = nodes[m];
            float cost = xb_dist(nodes[n].pos, nm.pos);
            float mult = 1.f;
            if (ti >= 0) mult += cfg.dangerWeight * nm.danger[ti];
            if (nm.flags & NF_AVOID) mult += 4.f;
            if (l.flags & LF_JUMP) cost += 40.f;
            if (l.flags & LF_CROUCH) cost += 30.f;
            if (l.flags & LF_LADDER) cost += 40.f;
            if (cfg.pathRandom > 0.f) mult *= 1.f + xb_frand(0.f, cfg.pathRandom);
            float ng = g[n] + cost * mult;
            if (open[m] != stamp || ng < g[m]) {
                g[m] = ng; par[m] = n; open[m] = stamp;
                pq.push({ng + xb_dist(nm.pos, goal), m});
            }
        }
    }
    if (closed[to] != stamp) return false;
    for (int n = to; n != -1; n = par[n]) {
        out.push_back(n);
        if ((int)out.size() > N) { out.clear(); return false; }
    }
    std::reverse(out.begin(), out.end());
    return true;
}

// learned experience: where a team dies (danger) and where it kills from (value)
void Nav::learn(const Vector &pos, int team, bool death, float amount)
{
    int ti = team == 1 ? 0 : (team == 2 ? 1 : -1);
    if (ti < 0) return;
    int n = nearest(pos, 250.f, false);
    if (n < 0) return;
    float add = 0.25f * amount;
    float *arr = death ? nodes[n].danger : nodes[n].value;
    arr[ti] = std::min(1.f, arr[ti] + add);
    for (const NavLink &l : nodes[n].links) {
        float *a2 = death ? nodes[l.to].danger : nodes[l.to].value;
        a2[ti] = std::min(1.f, a2[ti] + add * 0.5f);
    }
    dirty = true;
}

// ---------------------------------------------------------------------------------------------
// file format (little endian, x86 on both platforms):
//   "XBNAV1\0\0" int32 count; per node: float x y z, uint32 flags, float radius, float danger[2], float value[2],
//   uint8 nlinks, nlinks x (int32 to, uint8 flags)
static const char NAV_MAGIC[8] = {'X', 'B', 'N', 'A', 'V', '1', 0, 0};

bool Nav::save(const char *map)
{
    std::string m = safe_map(map);
    if (m.empty()) return false;
    std::string path = xb_data_path((m + ".nav").c_str()), tmp = path + ".tmp";
    FILE *f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    int32_t count = (int32_t)nodes.size();
    fwrite(NAV_MAGIC, 1, 8, f);
    fwrite(&count, 4, 1, f);
    for (const NavNode &n : nodes) {
        float p[3] = {n.pos.x, n.pos.y, n.pos.z};
        fwrite(p, 4, 3, f);
        fwrite(&n.flags, 4, 1, f);
        fwrite(&n.radius, 4, 1, f);
        fwrite(n.danger, 4, 2, f);
        fwrite(n.value, 4, 2, f);
        uint8_t nl = (uint8_t)n.links.size();
        fwrite(&nl, 1, 1, f);
        for (const NavLink &l : n.links) { int32_t to = l.to; fwrite(&to, 4, 1, f); fwrite(&l.flags, 1, 1, f); }
    }
    bool ok = !ferror(f);
    fclose(f);
    if (!ok) { ::remove(tmp.c_str()); return false; }
    ::remove(path.c_str());
    if (rename(tmp.c_str(), path.c_str()) != 0) return false;
    dirty = false;
    return true;
}

bool Nav::load(const char *map)
{
    an_active = false;
    std::string m = safe_map(map);
    nodes.clear();
    grid.clear();
    for (int i = 0; i < 33; ++i) learn_last[i] = -1;
    dirty = false;
    if (m.empty()) return false;
    FILE *f = fopen(xb_data_path((m + ".nav").c_str()).c_str(), "rb");
    if (!f) return false;
    char magic[8];
    int32_t count = 0;
    bool ok = fread(magic, 1, 8, f) == 8 && memcmp(magic, NAV_MAGIC, 8) == 0 && fread(&count, 4, 1, f) == 1 &&
              count >= 0 && count <= 16000;
    for (int i = 0; ok && i < count; ++i) {
        NavNode n;
        float p[3];
        uint8_t nl = 0;
        ok = fread(p, 4, 3, f) == 3 && fread(&n.flags, 4, 1, f) == 1 && fread(&n.radius, 4, 1, f) == 1 &&
             fread(n.danger, 4, 2, f) == 2 && fread(n.value, 4, 2, f) == 2 && fread(&nl, 1, 1, f) == 1;
        n.pos = Vector(p[0], p[1], p[2]);
        for (int k = 0; ok && k < nl; ++k) {
            int32_t to; uint8_t lf;
            ok = fread(&to, 4, 1, f) == 1 && fread(&lf, 1, 1, f) == 1;
            if (ok && to >= 0 && to < count) n.links.push_back({(int)to, lf});
        }
        for (int t = 0; t < 2; ++t) { n.danger[t] *= 0.9f; n.value[t] *= 0.9f; }   // experience fades slowly
        if (!(n.radius > 0.f && n.radius < 512.f)) n.radius = 32.f;
        nodes.push_back(n);
    }
    fclose(f);
    if (!ok) { nodes.clear(); return false; }
    rebuild_grid();
    return true;
}

// ---------------------------------------------------------------------------------------------
// analyzer
static bool hull(const Vector &a, const Vector &b, int h, TraceResult &tr)
{
    g_engfuncs.pfnTraceHull(a, b, 1 /* ignore monsters */, h, nullptr, &tr);
    return !tr.fStartSolid && !tr.fAllSolid && tr.flFraction >= 1.f;
}

// find the floor under `from` with hull h; out = origin standing on it
static bool drop_to_floor(const Vector &from, int h, float maxFall, Vector &out)
{
    TraceResult tr;
    g_engfuncs.pfnTraceHull(from, from - Vector(0, 0, maxFall), 1, h, nullptr, &tr);
    if (tr.fStartSolid || tr.fAllSolid || tr.flFraction >= 1.f) return false;
    if (tr.vecPlaneNormal.z < 0.7f) return false;                        // too steep: players slide
    out = tr.vecEndPos;
    return true;
}

static bool bad_contents(const Vector &feet)
{
    int c = g_engfuncs.pfnPointContents(feet);
    return c == CONTENTS_LAVA || c == CONTENTS_SLIME || c == CONTENTS_SKY;
}

void Nav::an_expand(int ni)
{
    static const int DIRS[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
    if (ni < 0 || ni >= (int)nodes.size()) return;
    const Vector P = nodes[ni].pos;
    const bool pCrouch = (nodes[ni].flags & NF_CROUCH) != 0;
    TraceResult tr;
    for (auto &d : DIRS) {
        Vector C(P.x + d[0] * STEP, P.y + d[1] * STEP, P.z);
        Vector land;
        uint8_t lf = 0;
        bool ok = false, crouch = false;

        if (!pCrouch && hull(P + Vector(0, 0, STEPH), C + Vector(0, 0, STEPH), 1, tr)) {          // walk
            ok = drop_to_floor(C + Vector(0, 0, STEPH), 1, STEPH + JUMPH + MAXDROP, land);
            if (ok) {   // a hole between the two points needs a jump
                Vector mid = (P + C) * 0.5f + Vector(0, 0, STEPH), mland;
                if (!drop_to_floor(mid, 1, STEPH * 2 + 40.f, mland)) lf |= LF_JUMP;
            }
        }
        if (!ok && !pCrouch && hull(P, P + Vector(0, 0, JUMPH), 1, tr) &&                       // jump up
            hull(P + Vector(0, 0, JUMPH), C + Vector(0, 0, JUMPH), 1, tr) &&
            drop_to_floor(C + Vector(0, 0, JUMPH), 1, JUMPH + MAXDROP, land)) {
            float dz = land.z - P.z;
            if (dz > STEPH - 1.f && dz <= JUMPH + 2.f) { ok = true; lf |= LF_JUMP; }
        }
        if (!ok) {                                                                              // crouch
            Vector pd = P - Vector(0, 0, 18), cd = C - Vector(0, 0, 18), dland;
            if (hull(pd + Vector(0, 0, STEPH), cd + Vector(0, 0, STEPH), 3, tr) &&
                drop_to_floor(cd + Vector(0, 0, STEPH), 3, STEPH + JUMPH + MAXDROP, dland)) {
                land = dland + Vector(0, 0, 18);
                TraceResult t2;
                g_engfuncs.pfnTraceHull(land, land, 1, 1, nullptr, &t2);
                crouch = t2.fStartSolid || t2.fAllSolid;      // standing does not fit there
                ok = true;
            }
        }
        if (!ok) continue;
        float dz = land.z - P.z;
        if (dz < -MAXDROP || bad_contents(land - Vector(0, 0, 34))) continue;
        if (crouch) lf |= LF_CROUCH;
        if (dz < -(STEPH + 2.f)) lf |= LF_DROP;

        int m = nearest(land, MERGE, false);
        if (m < 0) {
            if ((int)nodes.size() >= an_max) { an_head = an_queue.size(); return; }
            m = add(land, crouch ? NF_CROUCH : 0);
            if (m < 0) return;
            an_queue.push_back(m);
        }
        if (m == ni) continue;
        link(ni, m, lf);
        // way back
        if (fabsf(dz) <= STEPH + 1.f && !(lf & LF_JUMP)) link(m, ni, crouch || pCrouch ? LF_CROUCH : 0);
        else if (dz > STEPH) link(m, ni, LF_DROP);                                   // we jumped up: back is a drop
        else if (-dz <= JUMPH && hull(land + Vector(0, 0, JUMPH), P + Vector(0, 0, JUMPH), 1, tr)) link(m, ni, LF_JUMP);
        else if (lf & LF_JUMP) link(m, ni, LF_JUMP);                                  // jump over the hole again
    }
}

void Nav::an_mark_goals()
{
    auto mark_box = [this](const char *cls, uint32_t flag, float pad, float pointRadius) {
        edict_t *e = nullptr;
        while ((e = g_engfuncs.pfnFindEntityByString(e, "classname", cls)) && !FNullEnt(e)) {
            Vector mn = e->v.absmin, mx = e->v.absmax;
            bool brush = (mx - mn).Length() > 8.f;
            Vector c = brush ? (mn + mx) * 0.5f : e->v.origin;
            bool any = false;
            if (brush) {
                for (NavNode &n : nodes)
                    if (n.pos.x >= mn.x - pad && n.pos.x <= mx.x + pad && n.pos.y >= mn.y - pad && n.pos.y <= mx.y + pad &&
                        n.pos.z >= mn.z - 80.f && n.pos.z <= mx.z + 80.f) { n.flags |= flag; any = true; }
            } else {
                std::vector<int> l;
                near_list(c, pointRadius, l);
                for (int i : l) { nodes[i].flags |= flag; any = true; }
            }
            if (!any) { int n = nearest(c, 400.f, false); if (n >= 0) nodes[n].flags |= flag; }
        }
    };
    mark_box("func_bomb_target", NF_GOAL, 16.f, 0.f);
    mark_box("info_bomb_target", NF_GOAL, 0.f, 160.f);
    mark_box("func_hostage_rescue", NF_RESCUE, 16.f, 0.f);
    mark_box("info_hostage_rescue", NF_RESCUE, 0.f, 160.f);
    mark_box("hostage_entity", NF_HOSTAGE, 0.f, 90.f);
    mark_box("info_player_deathmatch", NF_TSPAWN, 0.f, 60.f);
    mark_box("info_player_start", NF_CTSPAWN, 0.f, 60.f);
    dirty = true;
}

void Nav::analyze_start(bool clearFirst, int maxNodes)
{
    if (clearFirst) clear();
    an_max = maxNodes > 0 ? maxNodes : cfg.maxNodes;
    an_queue.clear();
    an_head = 0;
    if (nodes.empty()) {
        static const char *seeds[] = {"info_player_start", "info_player_deathmatch", "info_vip_start", "hostage_entity"};
        for (const char *cls : seeds) {
            edict_t *e = nullptr;
            while ((e = g_engfuncs.pfnFindEntityByString(e, "classname", cls)) && !FNullEnt(e)) {
                Vector land;
                if (!drop_to_floor(e->v.origin + Vector(0, 0, 8), 1, 300.f, land)) continue;
                if (nearest(land, MERGE, false) >= 0) continue;
                int n = add(land, 0);
                if (n >= 0) an_queue.push_back(n);
            }
        }
    } else {
        for (int i = 0; i < (int)nodes.size(); ++i) an_queue.push_back(i);
    }
    an_active = !an_queue.empty();
}

float Nav::analyze_progress() const
{
    if (!an_active) return 1.f;
    return an_queue.empty() ? 1.f : (float)an_head / (float)an_queue.size();
}

// ---------------------------------------------------------------------------------------------
// learn from humans: every few frames, put a node where a human stands (unless one is near) and
// link it to the node they came from. Ladders, jumps and crouch spots come from real movement.
void Nav::learn_humans(float now)
{
    if (now < learn_next) return;
    learn_next = now + 0.12f;
    for (int i = 1; i <= gpGlobals->maxClients && i <= 32; ++i) {
        edict_t *e = xb_player(i);
        if (!e || (e->v.flags & FL_FAKECLIENT) || g_bots[i].used || !xb_alive(e)) { learn_last[i] = -1; continue; }
        int mt = e->v.movetype;
        if ((mt != MOVETYPE_WALK && mt != MOVETYPE_FLY) || e->v.velocity.Length() > 900.f) { learn_last[i] = -1; continue; }
        bool ground = (e->v.flags & FL_ONGROUND) != 0, ladder = mt == MOVETYPE_FLY, duck = (e->v.flags & FL_DUCKING) != 0;
        if (!ground && !ladder) { learn_air[i] = true; continue; }
        if (ground && e->v.groundentity && !FNullEnt(e->v.groundentity) &&
            xb_index(e->v.groundentity) >= 1 && xb_index(e->v.groundentity) <= gpGlobals->maxClients) continue;   // standing on a player
        Vector pos = e->v.origin;
        if (duck) pos.z += 18.f;
        int n = nearest(pos, 44.f, false);
        if (n < 0) {
            if ((int)nodes.size() >= cfg.maxNodes) continue;
            n = add(pos, (duck ? NF_CROUCH : 0) | (ladder ? NF_LADDER : 0) | NF_LEARNED);
            if (n < 0) continue;
        }
        int last = learn_last[i];
        if (last >= 0 && last < (int)nodes.size() && last != n) {
            float d = xb_dist(nodes[last].pos, nodes[n].pos);
            if (d < 160.f) {
                float dz = nodes[n].pos.z - nodes[last].pos.z;
                uint8_t f = 0;
                if (learn_air[i] && dz > STEPH) f |= LF_JUMP;
                if (ladder || (nodes[last].flags & NF_LADDER)) f |= LF_LADDER;
                if (duck) f |= LF_CROUCH;
                if (dz < -40.f) f |= LF_DROP;
                if (link_flags(last, n) < 0) link(last, n, f);
                if (link_flags(n, last) < 0) {
                    if (f & LF_LADDER) link(n, last, LF_LADDER);
                    else if (fabsf(dz) <= STEPH && !(f & LF_JUMP)) link(n, last, f & LF_CROUCH);
                }
            }
        }
        learn_last[i] = n;
        learn_air[i] = false;
    }
}

void Nav::frame(float now)
{
    if (cfg.autolearn) learn_humans(now);
    if (!an_active) return;
    auto t0 = std::chrono::steady_clock::now();
    while (an_head < an_queue.size()) {
        an_expand(an_queue[an_head++]);
        if (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() > 4.0) return;
    }
    an_active = false;
    an_mark_goals();
    int links = 0;
    for (const NavNode &n : nodes) links += (int)n.links.size();
    if (g_xmod && g_xext) {
        XmodValue *a = g_xmod->val_array();
        g_xmod->val_push(a, g_xmod->val_int((int64_t)nodes.size()));
        g_xmod->val_push(a, g_xmod->val_int(links));
        g_xmod->emit_event("bot_nav_done", a);
    }
}
