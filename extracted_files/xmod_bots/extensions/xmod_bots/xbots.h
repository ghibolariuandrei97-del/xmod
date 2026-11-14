// xmod_bots - bots for CS 1.6 as an xmod extension (namespace xmod.bot.*)
// Shared declarations of the C++ side ("the body"): bot slots, navigation graph, helpers.
// The Lua plugin plugins/xbots/xbots.lua is "the brain" (tactics, chat, memory, cvars).
#pragma once

#include "mm_sdk.h"        // xmod's minimal Metamod SDK (pulls in the HLSDK's extdll.h)
#include <in_buttons.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "xmod_ext.h"

#define XMOD_EXT_NAME    "xmod_bots"
#define XMOD_EXT_VERSION "1.0.0"
#define XB_NS            "bot"            // Lua namespace: xmod.bot.*

extern const XmodApi *g_xmod;              // set by link.cpp
extern XmodExt       *g_xext;

// ---------------------------------------------------------------------------------------------
// CS private data (same offsets as AMXX / xmod: 4-byte units, + linux diff)
#ifdef _WIN32
#define XB_PDIFF 0
#define XB_WDIFF 0
#else
#define XB_PDIFF 5
#define XB_WDIFF 4
#endif
#define OFS_TEAM        114   // CBasePlayer::m_iTeam
#define OFS_ACTIVEITEM  373   // CBasePlayer::m_pActiveItem
#define OFS_WEAPON_ID   43    // CBasePlayerItem::m_iId
#define OFS_CLIP        51    // CBasePlayerWeapon::m_iClip

inline int pd_int(edict_t *e, int ofs, int diff) {
    if (!e || !e->pvPrivateData) return 0;
    return *(int *)((char *)e->pvPrivateData + (ofs + diff) * 4);
}
inline void *pd_ptr(edict_t *e, int ofs, int diff) {
    if (!e || !e->pvPrivateData) return nullptr;
    return *(void **)((char *)e->pvPrivateData + (ofs + diff) * 4);
}

// ---------------------------------------------------------------------------------------------
// math helpers (kept on plain floats so they work with every HLSDK variant)
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
inline float xb_rad(float d) { return d * (float)(M_PI / 180.0); }
inline float xb_deg(float r) { return r * (float)(180.0 / M_PI); }
inline float xb_angnorm(float a) { while (a > 180.f) a -= 360.f; while (a < -180.f) a += 360.f; return a; }
inline float xb_clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float xb_frand(float lo, float hi) { return lo + (hi - lo) * ((float)rand() / (float)RAND_MAX); }
inline float xb_dist(const Vector &a, const Vector &b) { return (a - b).Length(); }
inline float xb_dist2d(const Vector &a, const Vector &b) { float dx = a.x - b.x, dy = a.y - b.y; return sqrtf(dx * dx + dy * dy); }
// direction -> view angles in v_angle convention (pitch positive = looking down)
inline void xb_dir_to_view(const Vector &d, float &pitch, float &yaw) {
    yaw = xb_deg(atan2f(d.y, d.x));
    pitch = -xb_deg(atan2f(d.z, sqrtf(d.x * d.x + d.y * d.y)));
}
inline Vector xb_view_forward(float pitch, float yaw) {
    float p = xb_rad(pitch), y = xb_rad(yaw);
    return Vector(cosf(p) * cosf(y), cosf(p) * sinf(y), -sinf(p));
}

// ---------------------------------------------------------------------------------------------
// players
inline edict_t *xb_player(int i) {
    if (!gpGlobals || i < 1 || i > gpGlobals->maxClients) return nullptr;
    edict_t *e = g_engfuncs.pfnPEntityOfEntIndex(i);
    if (!e || e->free || !e->pvPrivateData) return nullptr;
    return e;
}
inline bool xb_alive(edict_t *e) {
    return e && e->v.deadflag == DEAD_NO && e->v.health > 0.f && e->v.iuser1 == 0 && !(e->v.flags & FL_SPECTATOR);
}
inline int xb_team(edict_t *e) { return pd_int(e, OFS_TEAM, XB_PDIFF); }   // 1 T, 2 CT
inline Vector xb_eye(edict_t *e) { return e->v.origin + e->v.view_ofs; }
inline int xb_index(edict_t *e) { return g_engfuncs.pfnIndexOfEdict(e); }

// ---------------------------------------------------------------------------------------------
// bot slots
enum { LOOK_PATH = 0, LOOK_POINT = 1, LOOK_ENT = 2, LOOK_NONE = 3 };
enum { PART_HEAD = 0, PART_BODY = 1, PART_FEET = 2 };
enum { MOVE_NONE = 0, MOVE_DIRECT = 1, MOVE_PATH = 2 };
enum { FIRE_OFF = 0, FIRE_TAP = 1, FIRE_BURST = 2, FIRE_SPRAY = 3 };

struct Bot {
    bool    used = false;
    edict_t *ed = nullptr;
    int     id = 0;
    float   lastRun = 0.f, msAcc = 0.f;
    float   spawnedAt = 0.f;
    bool    wasAlive = false;

    // aim model (set from Lua with bot.set)
    float   aimK = 120.f;          // spring stiffness (higher = snappier)
    float   maxTurn = 600.f;       // deg/s
    float   aimErr = 10.f;         // world units of deliberate error at the target
    float   recoil = 0.5f;         // 0..1 recoil compensation
    float   lag = 0.08f;           // s: aims where the target was this long ago
    float   fov = 100.f;           // degrees
    float   vision = 4000.f;       // units
    bool    frozen = false;
    float   speedScale = 1.f;

    // view / look
    float   pitch = 0.f, yaw = 0.f, vPitch = 0.f, vYaw = 0.f;   // vPitch/vYaw = angular velocity
    int     lookMode = LOOK_PATH, lookEnt = 0, lookPart = PART_HEAD;
    Vector  lookPt;
    float   trackStart = 0.f, errNext = 0.f;
    Vector  errOfs;
    float   aimErrDeg = 180.f;

    // movement
    int     moveMode = MOVE_NONE;
    Vector  goal;
    std::vector<int> path;
    size_t  pathPos = 0;
    int     linkFlags = 0;         // flags of the link we are walking now
    float   speed = 1.f;
    bool    walk = false;
    int     strafeDir = 0;
    float   strafeUntil = 0.f;
    bool    arrivedSent = true;

    // buttons
    float   holdT[16] = {0};       // per IN_ bit: held until this time
    int     pressOnce = 0;
    float   duckUntil = 0.f, lastJump = 0.f;

    // fire
    int     fireMode = FIRE_OFF;
    float   fireHold = 0.2f, firePause = 0.3f, fireMaxAng = 5.f;
    float   fireT = 0.f;
    bool    fireDown = false;
    float   ffCheckT = 0.f;
    bool    ffBlocked = false;

    // stuck
    Vector  stuckPos;
    float   stuckCheck = 0.f, unstuckUntil = 0.f;
    int     stuckN = 0;
    int     autoButtons = 0;

    void reset_life();
};

extern Bot g_bots[33];
Bot *bot_get(int id);                 // nullptr if id is not one of our bots
void bot_run(Bot &b, float now);      // body.cpp - once per server frame
void bot_emit(const char *ev, int id);// emit a Lua event with the bot id

// fake client commands (main.cpp)
void fake_command(edict_t *e, const char *line);
int  bot_create(const char *name, std::string &err);
bool bot_kick(int id);

// vision (body.cpp)
struct SeenInfo { int id; float dist; int part; };
int  vis_check(edict_t *viewer, edict_t *target, bool useFov, float fov, float maxDist, Vector *where = nullptr);
void vis_scan(Bot &b, bool allTeams, std::vector<SeenInfo> &out);
void vis_frame_smokes();
bool trace_visible(const Vector &a, const Vector &b, edict_t *skip, edict_t *target);

// ---------------------------------------------------------------------------------------------
// navigation graph (nav.cpp)
enum NodeFlags : uint32_t {
    NF_CROUCH = 1, NF_LADDER = 2, NF_CAMP = 4, NF_GOAL = 8, NF_RESCUE = 16, NF_TSPAWN = 32,
    NF_CTSPAWN = 64, NF_SNIPER = 128, NF_HOSTAGE = 256, NF_AVOID = 512, NF_DOOR = 1024, NF_LEARNED = 2048,
    NF_JUMP = 4096, NF_USER1 = 65536, NF_USER2 = 131072, NF_USER3 = 262144, NF_USER4 = 524288
};
enum LinkFlags : uint8_t { LF_JUMP = 1, LF_CROUCH = 2, LF_LADDER = 4, LF_DROP = 8 };

struct NavLink { int to; uint8_t flags; };
struct NavNode {
    Vector   pos;             // standing player origin (feet + 36)
    uint32_t flags = 0;
    float    radius = 32.f;
    float    danger[2] = {0, 0};   // learned: deaths of T / CT around here (0..1)
    float    value[2] = {0, 0};    // learned: kills made by T / CT from here (0..1)
    std::vector<NavLink> links;
};

struct NavConfig {
    bool  autolearn = true;      // learn nodes from human movement
    int   maxNodes = 6000;
    float dangerWeight = 2.0f;   // A* cost multiplier for learned danger
    float pathRandom = 0.25f;    // random cost noise -> bots take different routes
};

class Nav {
public:
    std::vector<NavNode> nodes;
    NavConfig cfg;
    bool dirty = false;

    void   clear();
    int    add(const Vector &pos, uint32_t flags);
    void   remove(int i);
    bool   link(int a, int b, uint8_t flags);
    bool   unlink(int a, int b);
    bool   linked(int a, int b) const;
    int    link_flags(int a, int b) const;
    int    nearest(const Vector &pos, float maxDist, bool needVisible) const;
    void   near_list(const Vector &pos, float radius, std::vector<int> &out) const;
    bool   find_path(int from, int to, int team, std::vector<int> &out);
    void   learn(const Vector &pos, int team, bool death, float amount);
    bool   load(const char *map);
    bool   save(const char *map);

    // analyzer (flood fill from spawns, incremental over frames)
    void   analyze_start(bool clearFirst, int maxNodes);
    bool   analyzing() const { return an_active; }
    float  analyze_progress() const;
    void   frame(float now);          // analyzer step + human learning

    void   rebuild_grid();
private:
    void   grid_add(int i);
    static int64_t cell_key(int cx, int cy) { return ((int64_t)cx << 32) ^ (uint32_t)cy; }
    std::unordered_map<int64_t, std::vector<int>> grid;   // 2D cells of 128 units

    bool   an_active = false;
    std::vector<int> an_queue;
    size_t an_head = 0;
    int    an_max = 6000;
    void   an_expand(int node);
    void   an_mark_goals();

    // human learning
    int    learn_last[33];
    bool   learn_air[33];
    float  learn_next = 0.f;
    void   learn_humans(float now);
public:
    Nav() { for (int i = 0; i < 33; ++i) { learn_last[i] = -1; learn_air[i] = false; } }
};
extern Nav g_nav;
std::string xb_data_path(const char *file);   // <gamedir>/addons/xmod/data/bots/<file>

// Lua API registration (api.cpp)
bool xbots_register(const XmodApi *api, XmodExt *ext);
