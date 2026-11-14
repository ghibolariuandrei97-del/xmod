// Functional test of xmod_bots' C++ side against a tiny fake engine:
// a 1600x1600 room, a long wall with a gap, an 16-unit step and a 40-unit crate (needs a jump).
#include "xbots.h"
#include <cstdio>
#include <cassert>

extern enginefuncs_t g_engfuncs;
extern globalvars_t *gpGlobals;
static globalvars_t G;
static edict_t edicts[64];
static int priv[64][700];

struct Box { float x1, y1, z1, x2, y2, z2; };
static std::vector<Box> world = {
    {-2000, -2000, -100, 2000, 2000, 0},     // floor
    {-832, -800, 0, -800, 800, 300}, {800, -800, 0, 832, 800, 300},     // outer walls
    {-800, -832, 0, 800, -800, 300}, {-800, 800, 0, 800, 832, 300},
    {300, -800, 0, 332, 200, 300},           // long wall, gap at y > 200
    {-220, -60, 0, -150, 60, 16},            // a step (walkable)
    {-460, 300, 0, -330, 420, 40},           // a crate (needs a jump)
};

static void hull_ext(int h, float &hx, float &hy, float &hz) {
    if (h == 1) { hx = 16; hy = 16; hz = 36; } else if (h == 3) { hx = 16; hy = 16; hz = 18; } else { hx = hy = hz = 0; }
}
static void trace(const float *a, const float *b, int hull, TraceResult *tr) {
    float hx, hy, hz; hull_ext(hull, hx, hy, hz);
    memset(tr, 0, sizeof(*tr));
    tr->flFraction = 1.f; tr->pHit = &edicts[0];
    float d[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    float best = 1.f; Vector nrm(0, 0, 0); bool hit = false;
    for (const Box &w : world) {
        float mn[3] = {w.x1 - hx, w.y1 - hy, w.z1 - hz}, mx[3] = {w.x2 + hx, w.y2 + hy, w.z2 + hz};
        bool inside = a[0] > mn[0] && a[0] < mx[0] && a[1] > mn[1] && a[1] < mx[1] && a[2] > mn[2] && a[2] < mx[2];
        if (inside) { tr->fStartSolid = 1; tr->fAllSolid = 1; tr->flFraction = 0; tr->vecEndPos = Vector(a[0], a[1], a[2]); return; }
        float t0 = 0, t1 = 1; int axis = -1; float sign = 0;
        bool miss = false;
        for (int i = 0; i < 3 && !miss; ++i) {
            if (fabsf(d[i]) < 1e-6f) { if (a[i] <= mn[i] || a[i] >= mx[i]) miss = true; continue; }
            float u0 = (mn[i] - a[i]) / d[i], u1 = (mx[i] - a[i]) / d[i];
            float s = -1;
            if (u0 > u1) { std::swap(u0, u1); s = 1; }
            if (u0 > t0) { t0 = u0; axis = i; sign = s; }
            if (u1 < t1) t1 = u1;
            if (t0 >= t1) miss = true;
        }
        if (miss || axis < 0 || t0 >= best) continue;
        best = t0; hit = true;
        nrm = Vector(0, 0, 0); (&nrm.x)[axis] = sign;
    }
    if (hit) { best = std::max(0.f, best - 0.001f); tr->flFraction = best; tr->vecPlaneNormal = nrm; }
    tr->vecEndPos = Vector(a[0] + d[0] * best, a[1] + d[1] * best, a[2] + d[2] * best);
}
static void e_TraceHull(const float *a, const float *b, int, int h, edict_t *, TraceResult *tr) { trace(a, b, h, tr); }
static void e_TraceLine(const float *a, const float *b, int, edict_t *, TraceResult *tr) { trace(a, b, 0, tr); }
static int e_PointContents(const float *) { return -1; }
static edict_t *e_PEnt(int i) { return (i >= 0 && i < 64) ? &edicts[i] : nullptr; }
static int e_Index(const edict_t *e) { return (int)(e - edicts); }
static int e_Offset(const edict_t *e) { return e ? (int)(e - edicts) * 1000 : 0; }
static void e_GameDir(char *s) { strcpy(s, "/tmp/xbtest"); }
static edict_t *e_FindByString(edict_t *start, const char *f, const char *v) {
    int s = start ? e_Index(start) + 1 : 0;
    for (int i = s; i < 64; ++i) if (!edicts[i].free && edicts[i].v.classname > 0 && strcmp(STRING(edicts[i].v.classname), v) == 0) return &edicts[i];
    return nullptr;
}
static const char *s_strings[64]; static int s_nstr = 1;
static int alloc_str(const char *s) { s_strings[s_nstr] = s; return s_nstr++; }
static float s_jumpT[64];
static void e_RunPlayerMove(edict_t *e, const float *va, float fwd, float side, float, unsigned short buttons, byte, byte ms) {
    float dt = ms / 1000.f;
    float y = xb_rad(va[1]);
    float vx = cosf(y) * fwd + sinf(y) * side, vy = sinf(y) * fwd - cosf(y) * side;
    float sp = sqrtf(vx * vx + vy * vy), mx = 250;
    if (sp > mx) { vx *= mx / sp; vy *= mx / sp; }
    int i = e_Index(e);
    if (buttons & IN_JUMP) s_jumpT[i] = G.time;
    float stepH = (G.time - s_jumpT[i] < 0.6f) ? 46.f : 18.f;
    Vector o = e->v.origin;
    Vector to(o.x + vx * dt, o.y + vy * dt, o.z);
    TraceResult tr;
    float up[3] = {o.x, o.y, o.z + stepH}, upTo[3] = {to.x, to.y, to.z + stepH};
    trace(up, upTo, 1, &tr);                                 // step up, move, drop
    Vector p = tr.vecEndPos;
    if (tr.flFraction < 1.f) {                               // slide along the axis that is free
        float ax[3] = {o.x + vx * dt, o.y, o.z + stepH}, ay[3] = {o.x, o.y + vy * dt, o.z + stepH};
        trace(up, ax, 1, &tr); if (tr.flFraction >= 1.f) p = tr.vecEndPos;
        else { trace(up, ay, 1, &tr); p = tr.flFraction >= 1.f ? tr.vecEndPos : Vector(o.x, o.y, o.z + stepH); }
    }
    float a[3] = {p.x, p.y, p.z}, b[3] = {p.x, p.y, p.z - stepH - 300};
    trace(a, b, 1, &tr);
    e->v.origin = tr.vecEndPos;
    e->v.velocity = Vector(vx, vy, 0);
    e->v.flags |= FL_ONGROUND;
    e->v.button = buttons;
}
static int g_events = 0;

static edict_t *make_player(int i, float x, float y, int team) {
    edict_t *e = &edicts[i];
    memset(e, 0, sizeof(*e));
    e->pvPrivateData = priv[i];
    priv[i][OFS_TEAM + XB_PDIFF] = team;
    e->v.origin = Vector(x, y, 36);
    e->v.view_ofs = Vector(0, 0, 17);
    e->v.health = 100; e->v.deadflag = DEAD_NO; e->v.maxspeed = 250; e->v.flags = FL_CLIENT | FL_ONGROUND;
    e->v.movetype = MOVETYPE_WALK;
    e->v.pContainingEntity = e;
    return e;
}

int main() { setvbuf(stdout, nullptr, _IONBF, 0);
    memset(&g_engfuncs, 0, sizeof(g_engfuncs));
    g_engfuncs.pfnTraceHull = e_TraceHull; g_engfuncs.pfnTraceLine = e_TraceLine; g_engfuncs.pfnPointContents = e_PointContents;
    g_engfuncs.pfnPEntityOfEntIndex = e_PEnt; g_engfuncs.pfnIndexOfEdict = e_Index; g_engfuncs.pfnEntOffsetOfPEntity = e_Offset; g_engfuncs.pfnGetGameDir = e_GameDir;
    g_engfuncs.pfnFindEntityByString = e_FindByString; g_engfuncs.pfnRunPlayerMove = e_RunPlayerMove;
    static char strbuf[] = "\0info_player_start\0info_player_deathmatch\0";
    memset(&G, 0, sizeof(G)); G.maxClients = 32; G.time = 1.f; G.pStringBase = strbuf;
    gpGlobals = &G;
    for (int i = 0; i < 64; ++i) { edicts[i].free = 1; }
    edicts[0].free = 0;
    // string table hack: STRING(x) = gpGlobals->pStringBase + x; use absolute offsets from an empty base
    edict_t *s1 = &edicts[40]; memset(s1, 0, sizeof(*s1)); s1->v.classname = 1; s1->v.origin = Vector(-600, -600, 37);
    edict_t *s2 = &edicts[41]; memset(s2, 0, sizeof(*s2)); s2->v.classname = 19; s2->v.origin = Vector(600, -600, 37);
    (void)alloc_str; (void)g_events;

    // 1) analyzer
    system("mkdir -p /tmp/xbtest");
    g_nav.cfg.autolearn = false;
    g_nav.analyze_start(true, 6000);
    int frames = 0;
    while (g_nav.analyzing() && frames < 5000) { g_nav.frame(G.time); G.time += 0.01f; ++frames; }
    int links = 0, jumps = 0, onCrate = 0;
    for (auto &n : g_nav.nodes) { links += (int)n.links.size(); for (auto &l : n.links) if (l.flags & LF_JUMP) ++jumps; if (n.pos.z > 70 && n.pos.z < 80) ++onCrate; }
    printf("analyzer: %zu nodes, %d links, %d jump links, %d nodes on the crate, %d frames\n", g_nav.nodes.size(), links, jumps, onCrate, frames);
    assert(g_nav.nodes.size() > 500 && jumps > 0 && onCrate > 0);
    for (auto &n : g_nav.nodes) assert(!(n.pos.x > 300 - 16 && n.pos.x < 332 + 16 && n.pos.y < 200 + 16));   // no node inside the wall

    // 2) A* around the wall (must use the gap at y > 200)
    int a = g_nav.nearest(Vector(100, -500, 36), 100, true), b = g_nav.nearest(Vector(600, -500, 36), 100, true);
    std::vector<int> path;
    bool ok = g_nav.find_path(a, b, 1, path);
    float maxy = -1e9;
    for (int n : path) maxy = std::max(maxy, g_nav.nodes[n].pos.y);
    printf("A*: ok=%d, %zu nodes, max y on the path %.0f (gap starts at 200)\n", ok, path.size(), maxy);
    assert(ok && maxy > 200);
    // danger makes a detour: mark the gap as deadly for team 1, and check cost grows (path still exists)
    for (int i = 0; i < 6; ++i) g_nav.learn(Vector(316, 300, 36), 1, true, 1.0f);
    std::vector<int> p2; assert(g_nav.find_path(a, b, 1, p2));

    // 3) save / load round trip
    size_t before = g_nav.nodes.size();
    assert(g_nav.save("de_test"));
    assert(g_nav.load("de_test"));
    printf("save/load: %zu -> %zu nodes\n", before, g_nav.nodes.size());
    assert(g_nav.nodes.size() == before);

    // 4) a bot walks the path (around the wall), including onto the crate
    edict_t *e = make_player(1, 100, -500, 1);
    Bot &bot = g_bots[1];
    bot = Bot(); bot.used = true; bot.ed = e; bot.id = 1; bot.lastRun = G.time;
    bot.goal = Vector(600, -500, 36);
    g_nav.find_path(g_nav.nearest(e->v.origin, 320, true), g_nav.nearest(bot.goal, 320, true), 1, bot.path);
    bot.moveMode = MOVE_PATH; bot.arrivedSent = false;
    float t0 = G.time;
    while (bot.moveMode != MOVE_NONE && G.time - t0 < 30.f) { bot_run(bot, G.time); G.time += 0.01f; }
    printf("walk around the wall: arrived=%d in %.1fs, at (%.0f %.0f %.0f)\n", bot.moveMode == MOVE_NONE && bot.stuckN == 0, G.time - t0,
           e->v.origin.x, e->v.origin.y, e->v.origin.z);
    assert(xb_dist2d(e->v.origin, bot.goal) < 40.f);

    bot.goal = Vector(-395, 360, 76);                         // top of the crate
    g_nav.find_path(g_nav.nearest(e->v.origin, 320, true), g_nav.nearest(bot.goal, 320, true), 1, bot.path);
    bot.pathPos = 0; bot.moveMode = MOVE_PATH; bot.arrivedSent = false;
    t0 = G.time;
    while (bot.moveMode != MOVE_NONE && G.time - t0 < 30.f) { bot_run(bot, G.time); G.time += 0.01f; }
    printf("climb the crate: z=%.0f after %.1fs (crate top origin = 76)\n", e->v.origin.z, G.time - t0);
    assert(e->v.origin.z > 70.f);

    // 5) aim: target 90 degrees to the side, check the spring converges like a human (not instantly)
    edict_t *t = make_player(2, e->v.origin.x, e->v.origin.y + 280, 2);
    bot.moveMode = MOVE_NONE; bot.yaw = 0; bot.pitch = 0; bot.vYaw = bot.vPitch = 0;
    bot.aimK = 200; bot.maxTurn = 700; bot.aimErr = 0; bot.lag = 0;
    bot.lookMode = LOOK_ENT; bot.lookEnt = 2; bot.lookPart = PART_BODY; bot.trackStart = G.time;
    float tStart = G.time, t10 = -1;
    for (int i = 0; i < 100; ++i) { bot_run(bot, G.time); G.time += 0.01f; if (t10 < 0 && bot.aimErrDeg < 3.f) t10 = G.time - tStart; }
    printf("aim: 90 deg turn reached < 3 deg error after %.2fs, final error %.2f deg, yaw %.1f\n", t10, bot.aimErrDeg, bot.yaw);
    assert(t10 > 0.1f && t10 < 0.9f && bot.aimErrDeg < 1.f);

    // 6) vision: the target behind the long wall is not visible, in the open it is
    std::vector<SeenInfo> seen;
    vis_scan(bot, false, seen);
    printf("vision: sees %zu enemy (open field)\n", seen.size());
    assert(seen.size() == 1);
    t->v.origin = Vector(600, -100, 36);  e->v.origin = Vector(100, -100, 36); bot.yaw = 0; bot.pitch = 0;
    e->v.v_angle = Vector(0, 0, 0);
    vis_scan(bot, false, seen);
    printf("vision: sees %zu enemy (behind the wall)\n", seen.size());
    assert(seen.empty());

    // 7) fire controller: burst gives on/off pattern, and nothing when not aimed
    bot.fireMode = FIRE_BURST; bot.fireHold = 0.1f; bot.firePause = 0.2f; bot.fireMaxAng = 5; bot.aimErrDeg = 1;
    int on = 0, total = 0;
    t->v.origin = Vector(100, 300, 36);
    bot.lookMode = LOOK_ENT; bot.lookEnt = 2;
    for (int i = 0; i < 100; ++i) { bot_run(bot, G.time); G.time += 0.01f; }   // settle the aim first
    for (int i = 0; i < 300; ++i) { bot_run(bot, G.time); if (e->v.button & IN_ATTACK) ++on; ++total; G.time += 0.01f; }
    printf("burst: attack held %d of %d frames (~1/3 expected)\n", on, total);
    assert(on > 60 && on < 160);
    // 8) learning from a human walking across the room
    g_nav.clear(); g_nav.cfg.autolearn = true;
    edict_t *h = make_player(3, -600, 0, 2);
    for (int i = 0; i < 400; ++i) { h->v.origin.x = -600.f + i * 3.f; g_nav.frame(G.time); G.time += 0.01f; }
    int ll = 0; for (auto &n : g_nav.nodes) ll += (int)n.links.size();
    printf("learned from a human: %zu nodes, %d links\n", g_nav.nodes.size(), ll);
    assert(g_nav.nodes.size() >= 10 && ll >= 2 * ((int)g_nav.nodes.size() - 2));
    std::vector<int> lp; assert(g_nav.find_path(1, (int)g_nav.nodes.size() - 1, 2, lp) && lp.size() == g_nav.nodes.size() - 1);
    printf("ALL OK\n");
    return 0;
}
