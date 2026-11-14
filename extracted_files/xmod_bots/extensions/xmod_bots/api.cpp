// api.cpp - the Lua functions xmod.bot.* (all synchronous, game thread, fast).
// Node indices are 1-based in Lua (0 = none). Teams use xmod numbers: 1 = T, 2 = CT.
#include "xbots.h"
#include <algorithm>

#define X g_xmod

// ---------------------------------------------------------------------------------------------
// argument / return helpers
static inline const XmodValue *arg(XmodCall *c, int i) { return X->call_arg(c, i); }
static inline int argc(XmodCall *c) { return X->call_argc(c); }
static inline double num(XmodCall *c, int i, double d) { return X->val_get_num(arg(c, i), d); }
static inline int inum(XmodCall *c, int i, int d) { return (int)X->val_get_int(arg(c, i), d); }
static inline bool boolean(XmodCall *c, int i, bool d) { return X->val_get_bool(arg(c, i), d) != 0; }
static std::string str(XmodCall *c, int i, const char *d = "")
{
    size_t n = 0;
    const char *s = X->val_get_str(arg(c, i), &n);
    return s ? std::string(s, n) : std::string(d);
}
static inline Vector vec3(XmodCall *c, int i) { return Vector((float)num(c, i, 0), (float)num(c, i + 1, 0), (float)num(c, i + 2, 0)); }
static const XmodValue *opt(XmodCall *c, int i, const char *key)
{
    const XmodValue *o = arg(c, i);
    return X->val_type(o) == XV_MAP ? X->val_get(o, key) : nullptr;
}
static inline void ret(XmodCall *c, XmodValue *v) { X->call_return(c, v); }
static inline void ret_nil(XmodCall *c) { ret(c, X->val_nil()); }
static inline void ret_bool(XmodCall *c, bool b) { ret(c, X->val_bool(b)); }
static inline void ret_int(XmodCall *c, int64_t v) { ret(c, X->val_int(v)); }
static inline void mset(XmodValue *m, const char *k, XmodValue *v) { X->val_set(m, k, v); }

static Bot *need_bot(XmodCall *c, int i = 0)
{
    Bot *b = bot_get(inum(c, i, 0));
    if (!b) X->call_error(c, "xmod.bot: this id is not a bot created by xmod_bots (check xmod.bot.is_bot(id))");
    return b;
}
static int need_node(XmodCall *c, int i)
{
    int n = inum(c, i, 0) - 1;
    if (n < 0 || n >= (int)g_nav.nodes.size()) { X->call_error(c, "xmod.bot: invalid nav node index"); return -1; }
    return n;
}
static const char *part_name(int p) { return p == PART_HEAD ? "head" : (p == PART_BODY ? "body" : "feet"); }
static int part_of(const std::string &s) { return s == "body" ? PART_BODY : (s == "feet" ? PART_FEET : PART_HEAD); }

// ---------------------------------------------------------------------------------------------
// lifecycle
static void f_add(XmodCall *c, void *)
{
    std::string name = str(c, 0, "xbot");
    if (name.empty()) name = "xbot";
    if (name.size() > 31) name.resize(31);
    std::string err;
    int id = bot_create(name.c_str(), err);
    if (id > 0) ret_int(c, id); else ret(c, X->val_cstr(err.c_str()));
}
static void f_remove(XmodCall *c, void *) { ret_bool(c, bot_kick(inum(c, 0, 0))); }
static void f_is_bot(XmodCall *c, void *) { ret_bool(c, bot_get(inum(c, 0, 0)) != nullptr); }
static void f_list(XmodCall *c, void *)
{
    XmodValue *a = X->val_array();
    for (int i = 1; i <= 32; ++i) if (bot_get(i)) X->val_push(a, X->val_int(i));
    ret(c, a);
}
static void f_count(XmodCall *c, void *)
{
    int n = 0;
    for (int i = 1; i <= 32; ++i) if (bot_get(i)) ++n;
    ret_int(c, n);
}
static void f_cmd(XmodCall *c, void *)
{
    Bot *b = need_bot(c); if (!b) return;
    fake_command(b->ed, str(c, 1).c_str());
    ret_nil(c);
}
static void f_say(XmodCall *c, void *)
{
    Bot *b = need_bot(c); if (!b) return;
    std::string t = str(c, 1), clean;
    for (char ch : t) if (ch != '"' && ch != ';' && ch != '\n' && ch != '\r' && ch != '%') clean += ch;
    if (clean.size() > 150) clean.resize(150);
    if (clean.empty()) { ret_nil(c); return; }
    std::string line = std::string(boolean(c, 2, false) ? "say_team" : "say") + " \"" + clean + "\"";
    fake_command(b->ed, line.c_str());
    ret_nil(c);
}

// ---------------------------------------------------------------------------------------------
// control
static void f_set(XmodCall *c, void *)
{
    Bot *b = need_bot(c); if (!b) return;
    const XmodValue *v;
    if ((v = opt(c, 1, "aim_speed"))) b->aimK = xb_clamp((float)X->val_get_num(v, b->aimK), 5.f, 2000.f);
    if ((v = opt(c, 1, "max_turn"))) b->maxTurn = xb_clamp((float)X->val_get_num(v, b->maxTurn), 30.f, 3000.f);
    if ((v = opt(c, 1, "aim_error"))) b->aimErr = xb_clamp((float)X->val_get_num(v, b->aimErr), 0.f, 200.f);
    if ((v = opt(c, 1, "recoil"))) b->recoil = xb_clamp((float)X->val_get_num(v, b->recoil), 0.f, 2.f);
    if ((v = opt(c, 1, "lag"))) b->lag = xb_clamp((float)X->val_get_num(v, b->lag), 0.f, 1.f);
    if ((v = opt(c, 1, "fov"))) b->fov = xb_clamp((float)X->val_get_num(v, b->fov), 30.f, 360.f);
    if ((v = opt(c, 1, "vision"))) b->vision = xb_clamp((float)X->val_get_num(v, b->vision), 100.f, 16384.f);
    if ((v = opt(c, 1, "frozen"))) b->frozen = X->val_get_bool(v, b->frozen) != 0;
    if ((v = opt(c, 1, "speed"))) b->speedScale = xb_clamp((float)X->val_get_num(v, 1.0), 0.f, 2.f);
    ret_nil(c);
}

static void f_go(XmodCall *c, void *)
{
    Bot *b = need_bot(c); if (!b) return;
    Vector goal = vec3(c, 1);
    const XmodValue *v;
    b->speed = (v = opt(c, 4, "speed")) ? xb_clamp((float)X->val_get_num(v, 1.0), 0.05f, 1.f) : 1.f;
    b->walk = (v = opt(c, 4, "walk")) ? X->val_get_bool(v, false) != 0 : false;
    bool direct = (v = opt(c, 4, "direct")) ? X->val_get_bool(v, false) != 0 : false;
    bool fallback = (v = opt(c, 4, "fallback")) ? X->val_get_bool(v, true) != 0 : true;
    b->goal = goal;
    b->path.clear();
    b->pathPos = 0;
    b->linkFlags = 0;
    b->stuckN = 0;
    b->stuckPos = b->ed->v.origin;
    b->arrivedSent = false;
    if (!direct && !g_nav.nodes.empty()) {
        int from = g_nav.nearest(b->ed->v.origin, 320.f, true);
        int to = g_nav.nearest(goal, 320.f, true);
        if (from >= 0 && to >= 0 && g_nav.find_path(from, to, xb_team(b->ed), b->path)) {
            b->moveMode = MOVE_PATH;
            if (b->path.size() >= 2) b->linkFlags = 0;
            ret_int(c, (int64_t)b->path.size());
            return;
        }
        if (!fallback) { b->moveMode = MOVE_NONE; b->arrivedSent = true; ret_bool(c, false); return; }
    }
    b->moveMode = MOVE_DIRECT;
    ret_int(c, -1);
}

static void f_stop(XmodCall *c, void *)
{
    Bot *b = need_bot(c); if (!b) return;
    b->moveMode = MOVE_NONE; b->path.clear(); b->arrivedSent = true; b->strafeUntil = 0.f;
    ret_nil(c);
}
static void f_strafe(XmodCall *c, void *)
{
    Bot *b = need_bot(c); if (!b) return;
    int d = inum(c, 1, 0);
    b->strafeDir = d > 0 ? 1 : (d < 0 ? -1 : 0);
    b->strafeUntil = gpGlobals->time + (float)num(c, 2, 0.4);
    ret_nil(c);
}
static void f_look(XmodCall *c, void *)
{
    Bot *b = need_bot(c); if (!b) return;
    b->lookMode = LOOK_POINT; b->lookPt = vec3(c, 1); b->lookEnt = 0;
    ret_nil(c);
}
static void f_look_ent(XmodCall *c, void *)
{
    Bot *b = need_bot(c); if (!b) return;
    int ent = inum(c, 1, 0);
    int part = part_of(str(c, 2, "head"));
    if (b->lookMode != LOOK_ENT || b->lookEnt != ent) { b->trackStart = gpGlobals->time; b->errNext = 0.f; }
    b->lookMode = LOOK_ENT; b->lookEnt = ent; b->lookPart = part;
    ret_nil(c);
}
static void f_look_path(XmodCall *c, void *)
{
    Bot *b = need_bot(c); if (!b) return;
    b->lookMode = LOOK_PATH; b->lookEnt = 0;
    ret_nil(c);
}
static void f_look_none(XmodCall *c, void *)
{
    Bot *b = need_bot(c); if (!b) return;
    b->lookMode = LOOK_NONE; b->lookEnt = 0;
    ret_nil(c);
}
static void f_press(XmodCall *c, void *)
{
    Bot *b = need_bot(c); if (!b) return;
    b->pressOnce |= inum(c, 1, 0);
    ret_nil(c);
}
static void f_hold(XmodCall *c, void *)
{
    Bot *b = need_bot(c); if (!b) return;
    int mask = inum(c, 1, 0);
    double secs = num(c, 2, 0);
    float until = secs > 0 ? gpGlobals->time + (float)secs : 1e9f;
    for (int bit = 0; bit < 16; ++bit) if (mask & (1 << bit)) b->holdT[bit] = until;
    ret_nil(c);
}
static void f_release(XmodCall *c, void *)
{
    Bot *b = need_bot(c); if (!b) return;
    int mask = argc(c) >= 2 ? inum(c, 1, 0) : 0xFFFF;
    for (int bit = 0; bit < 16; ++bit) if (mask & (1 << bit)) b->holdT[bit] = 0.f;
    ret_nil(c);
}
static void f_fire(XmodCall *c, void *)
{
    Bot *b = need_bot(c); if (!b) return;
    int mode = inum(c, 1, 0);
    if (mode < FIRE_OFF || mode > FIRE_SPRAY) mode = FIRE_OFF;
    if (mode != b->fireMode) { b->fireDown = false; }
    b->fireMode = mode;
    b->fireHold = (float)num(c, 2, b->fireHold);
    b->firePause = (float)num(c, 3, b->firePause);
    b->fireMaxAng = (float)num(c, 4, b->fireMaxAng);
    ret_nil(c);
}

static void f_state(XmodCall *c, void *)
{
    Bot *b = need_bot(c); if (!b) return;
    XmodValue *m = X->val_map();
    static const char *modes[] = {"none", "direct", "path"};
    mset(m, "move", X->val_cstr(modes[b->moveMode]));
    mset(m, "moving", X->val_bool(b->moveMode != MOVE_NONE));
    mset(m, "path_left", X->val_int(b->moveMode == MOVE_PATH ? (int64_t)(b->path.size() - std::min(b->pathPos, b->path.size())) : 0));
    mset(m, "gx", X->val_num(b->goal.x)); mset(m, "gy", X->val_num(b->goal.y)); mset(m, "gz", X->val_num(b->goal.z));
    mset(m, "aim_error", X->val_num(b->aimErrDeg));
    mset(m, "look", X->val_int(b->lookMode));
    mset(m, "look_ent", X->val_int(b->lookEnt));
    mset(m, "fire", X->val_int(b->fireMode));
    mset(m, "stuck", X->val_int(b->stuckN));
    mset(m, "pitch", X->val_num(b->pitch)); mset(m, "yaw", X->val_num(b->yaw));
    mset(m, "frozen", X->val_bool(b->frozen));
    ret(c, m);
}

// ---------------------------------------------------------------------------------------------
// perception
static void f_scan(XmodCall *c, void *)
{
    Bot *b = need_bot(c); if (!b) return;
    std::vector<SeenInfo> seen;
    vis_scan(*b, boolean(c, 1, false), seen);
    XmodValue *a = X->val_array();
    for (const SeenInfo &s : seen) {
        XmodValue *m = X->val_map();
        mset(m, "id", X->val_int(s.id));
        mset(m, "dist", X->val_num(s.dist));
        mset(m, "part", X->val_cstr(part_name(s.part)));
        X->val_push(a, m);
    }
    ret(c, a);
}
static void f_visible(XmodCall *c, void *)
{
    int vid = inum(c, 0, 0);
    edict_t *v = xb_player(vid);
    edict_t *t = g_engfuncs.pfnPEntityOfEntIndex(inum(c, 1, 0));
    if (!v || !t || t->free) { ret_bool(c, false); return; }
    Bot *b = bot_get(vid);
    bool useFov = !boolean(c, 2, false);
    int p = vis_check(v, t, useFov, b ? b->fov : 100.f, b ? b->vision : 8192.f);
    if (p) ret(c, X->val_cstr(part_name(p - 1))); else ret_bool(c, false);
}
static void f_see_point(XmodCall *c, void *)
{
    edict_t *v = xb_player(inum(c, 0, 0));
    if (!v) { ret_bool(c, false); return; }
    ret_bool(c, trace_visible(xb_eye(v), vec3(c, 1), v, nullptr));
}
static void f_weapon(XmodCall *c, void *)
{
    edict_t *e = xb_player(inum(c, 0, 0));
    void *item = e ? pd_ptr(e, OFS_ACTIVEITEM, XB_PDIFF) : nullptr;
    if (!item) { ret_nil(c); return; }
    entvars_t *pev = *(entvars_t **)((char *)item + 4);                // CBaseEntity: [vtable][pev]
    edict_t *we = pev ? pev->pContainingEntity : nullptr;
    if (!we || we->free || &we->v != pev) { ret_nil(c); return; }
    const char *cls = STRING(pev->classname);
    XmodValue *m = X->val_map();
    mset(m, "name", X->val_cstr(strncmp(cls, "weapon_", 7) == 0 ? cls + 7 : cls));
    mset(m, "clip", X->val_int(pd_int(we, OFS_CLIP, XB_WDIFF)));
    mset(m, "wid", X->val_int(pd_int(we, OFS_WEAPON_ID, XB_WDIFF)));
    mset(m, "ent", X->val_int(xb_index(we)));
    ret(c, m);
}

static void f_config(XmodCall *c, void *)
{
    const XmodValue *v;
    if ((v = opt(c, 0, "autolearn"))) g_nav.cfg.autolearn = X->val_get_bool(v, true) != 0;
    if ((v = opt(c, 0, "max_nodes"))) g_nav.cfg.maxNodes = std::max(100, std::min(16000, (int)X->val_get_int(v, 6000)));
    if ((v = opt(c, 0, "danger_weight"))) g_nav.cfg.dangerWeight = xb_clamp((float)X->val_get_num(v, 2.0), 0.f, 20.f);
    if ((v = opt(c, 0, "path_random"))) g_nav.cfg.pathRandom = xb_clamp((float)X->val_get_num(v, 0.25), 0.f, 2.f);
    ret_nil(c);
}

// ---------------------------------------------------------------------------------------------
// navigation
static void f_nav_info(XmodCall *c, void *)
{
    int links = 0;
    for (const NavNode &n : g_nav.nodes) links += (int)n.links.size();
    XmodValue *m = X->val_map();
    mset(m, "nodes", X->val_int((int64_t)g_nav.nodes.size()));
    mset(m, "links", X->val_int(links));
    mset(m, "analyzing", X->val_bool(g_nav.analyzing()));
    mset(m, "progress", X->val_num(g_nav.analyze_progress()));
    mset(m, "dirty", X->val_bool(g_nav.dirty));
    mset(m, "autolearn", X->val_bool(g_nav.cfg.autolearn));
    mset(m, "max_nodes", X->val_int(g_nav.cfg.maxNodes));
    ret(c, m);
}
static void f_nav_load(XmodCall *c, void *) { ret_bool(c, g_nav.load(STRING(gpGlobals->mapname))); }
static void f_nav_save(XmodCall *c, void *) { ret_bool(c, g_nav.save(STRING(gpGlobals->mapname))); }
static void f_nav_clear(XmodCall *c, void *) { g_nav.clear(); ret_nil(c); }
static void f_nav_analyze(XmodCall *c, void *)
{
    g_nav.analyze_start(boolean(c, 0, false), inum(c, 1, 0));
    ret_bool(c, g_nav.analyzing());
}
static void f_nav_add(XmodCall *c, void *)
{
    int i = g_nav.add(vec3(c, 0), (uint32_t)inum(c, 3, 0));
    ret_int(c, i + 1);
}
static void f_nav_remove(XmodCall *c, void *)
{
    int n = need_node(c, 0); if (n < 0) return;
    g_nav.remove(n);
    ret_nil(c);
}
static void f_nav_link(XmodCall *c, void *)
{
    int a = need_node(c, 0); if (a < 0) return;
    int b = need_node(c, 1); if (b < 0) return;
    uint8_t fl = (uint8_t)inum(c, 2, 0);
    bool ok = g_nav.link(a, b, fl);
    if (boolean(c, 3, false)) ok = g_nav.link(b, a, fl) && ok;
    ret_bool(c, ok);
}
static void f_nav_unlink(XmodCall *c, void *)
{
    int a = need_node(c, 0); if (a < 0) return;
    int b = need_node(c, 1); if (b < 0) return;
    bool ok = g_nav.unlink(a, b);
    if (boolean(c, 2, true)) ok = g_nav.unlink(b, a) || ok;
    ret_bool(c, ok);
}
static void f_nav_flags(XmodCall *c, void *)
{
    int n = need_node(c, 0); if (n < 0) return;
    if (argc(c) >= 2) { g_nav.nodes[n].flags = (uint32_t)inum(c, 1, 0); g_nav.dirty = true; }
    ret_int(c, g_nav.nodes[n].flags);
}
static void f_nav_radius(XmodCall *c, void *)
{
    int n = need_node(c, 0); if (n < 0) return;
    if (argc(c) >= 2) { g_nav.nodes[n].radius = xb_clamp((float)num(c, 1, 32), 8.f, 256.f); g_nav.dirty = true; }
    ret(c, X->val_num(g_nav.nodes[n].radius));
}
static void f_nav_nearest(XmodCall *c, void *)
{
    ret_int(c, g_nav.nearest(vec3(c, 0), (float)num(c, 3, 400), boolean(c, 4, false)) + 1);
}
static void f_nav_node(XmodCall *c, void *)
{
    int n = inum(c, 0, 0) - 1;
    if (n < 0 || n >= (int)g_nav.nodes.size()) { ret_nil(c); return; }
    const NavNode &nd = g_nav.nodes[n];
    XmodValue *m = X->val_map();
    mset(m, "x", X->val_num(nd.pos.x)); mset(m, "y", X->val_num(nd.pos.y)); mset(m, "z", X->val_num(nd.pos.z));
    mset(m, "flags", X->val_int(nd.flags));
    mset(m, "radius", X->val_num(nd.radius));
    mset(m, "danger_t", X->val_num(nd.danger[0])); mset(m, "danger_ct", X->val_num(nd.danger[1]));
    mset(m, "value_t", X->val_num(nd.value[0])); mset(m, "value_ct", X->val_num(nd.value[1]));
    XmodValue *links = X->val_array();
    for (const NavLink &l : nd.links) {
        XmodValue *lm = X->val_map();
        mset(lm, "to", X->val_int(l.to + 1));
        mset(lm, "flags", X->val_int(l.flags));
        X->val_push(links, lm);
    }
    mset(m, "links", links);
    ret(c, m);
}
static void f_nav_find(XmodCall *c, void *)
{
    uint32_t f = (uint32_t)inum(c, 0, 0);
    XmodValue *a = X->val_array();
    for (int i = 0; i < (int)g_nav.nodes.size(); ++i)
        if ((g_nav.nodes[i].flags & f) == f) X->val_push(a, X->val_int(i + 1));
    ret(c, a);
}
static void f_nav_near(XmodCall *c, void *)
{
    std::vector<int> l;
    g_nav.near_list(vec3(c, 0), (float)xb_clamp((float)num(c, 3, 512), 1.f, 4096.f), l);
    XmodValue *a = X->val_array();
    for (int i : l) X->val_push(a, X->val_int(i + 1));
    ret(c, a);
}
static void f_nav_path(XmodCall *c, void *)
{
    int a = need_node(c, 0); if (a < 0) return;
    int b = need_node(c, 1); if (b < 0) return;
    std::vector<int> p;
    if (!g_nav.find_path(a, b, inum(c, 2, 0), p)) { ret_nil(c); return; }
    XmodValue *arr = X->val_array();
    for (int n : p) X->val_push(arr, X->val_int(n + 1));
    ret(c, arr);
}
static void f_nav_learn(XmodCall *c, void *)
{
    g_nav.learn(vec3(c, 0), inum(c, 3, 0), str(c, 4, "death") != "kill", (float)num(c, 5, 1.0));
    ret_nil(c);
}
// best node around a point by learned value ("kill") or lowest danger ("safe") for a team
static void f_nav_best(XmodCall *c, void *)
{
    std::vector<int> l;
    g_nav.near_list(vec3(c, 0), (float)xb_clamp((float)num(c, 3, 800), 1.f, 4096.f), l);
    int team = inum(c, 4, 1), ti = team == 2 ? 1 : 0;
    bool safe = str(c, 5, "kill") == "safe";
    uint32_t need = (uint32_t)inum(c, 6, 0);
    int best = -1;
    float bs = -1e9f;
    for (int i : l) {
        const NavNode &n = g_nav.nodes[i];
        if ((n.flags & need) != need || (n.flags & NF_AVOID)) continue;
        float s = safe ? -n.danger[ti] : n.value[ti] - n.danger[ti] * 0.5f + ((n.flags & NF_CAMP) ? 0.3f : 0.f);
        s += xb_frand(0.f, 0.05f);
        if (s > bs) { bs = s; best = i; }
    }
    ret_int(c, best + 1);
}
static void f_nav_random(XmodCall *c, void *)
{
    uint32_t f = (uint32_t)inum(c, 0, 0);
    std::vector<int> l;
    for (int i = 0; i < (int)g_nav.nodes.size(); ++i)
        if ((g_nav.nodes[i].flags & f) == f && !(g_nav.nodes[i].flags & NF_AVOID)) l.push_back(i);
    ret_int(c, l.empty() ? 0 : l[rand() % l.size()] + 1);
}

// ---------------------------------------------------------------------------------------------
bool xbots_register(const XmodApi *api, XmodExt *ext)
{
    struct F { const char *name; void (*fn)(XmodCall *, void *); };
    static const F funcs[] = {
        {"add", f_add}, {"remove", f_remove}, {"is_bot", f_is_bot}, {"list", f_list}, {"count", f_count},
        {"cmd", f_cmd}, {"say", f_say},
        {"set", f_set}, {"go", f_go}, {"stop", f_stop}, {"strafe", f_strafe},
        {"look", f_look}, {"look_ent", f_look_ent}, {"look_path", f_look_path}, {"look_none", f_look_none},
        {"press", f_press}, {"hold", f_hold}, {"release", f_release}, {"fire", f_fire}, {"state", f_state},
        {"scan", f_scan}, {"visible", f_visible}, {"see_point", f_see_point}, {"weapon", f_weapon},
        {"config", f_config},
        {"nav_info", f_nav_info}, {"nav_load", f_nav_load}, {"nav_save", f_nav_save}, {"nav_clear", f_nav_clear},
        {"nav_analyze", f_nav_analyze}, {"nav_add", f_nav_add}, {"nav_remove", f_nav_remove},
        {"nav_link", f_nav_link}, {"nav_unlink", f_nav_unlink}, {"nav_flags", f_nav_flags}, {"nav_radius", f_nav_radius},
        {"nav_nearest", f_nav_nearest}, {"nav_node", f_nav_node}, {"nav_find", f_nav_find}, {"nav_near", f_nav_near},
        {"nav_path", f_nav_path}, {"nav_learn", f_nav_learn}, {"nav_best", f_nav_best}, {"nav_random", f_nav_random},
    };
    for (const F &f : funcs)
        if (!api->register_func(ext, XB_NS, f.name, f.fn, nullptr)) return false;
    return true;
}
