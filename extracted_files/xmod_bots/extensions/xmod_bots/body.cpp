// body.cpp - what a bot does every server frame: human-like aim (spring model + error + recoil
// control), path following with jump/duck/ladder handling, strafing, stuck recovery, the fire
// controller (tap / burst / spray, never through a teammate) and vision checks.
#include "xbots.h"

// ---------------------------------------------------------------------------------------------
// vision
struct Smoke { Vector pos; };
static std::vector<Smoke> s_smokes;
static float s_smokeFrame = -1.f;

void vis_frame_smokes()
{
    if (s_smokeFrame == gpGlobals->time) return;
    s_smokeFrame = gpGlobals->time;
    s_smokes.clear();
    edict_t *e = nullptr;
    while ((e = g_engfuncs.pfnFindEntityByString(e, "classname", "grenade")) && !FNullEnt(e)) {
        const char *m = STRING(e->v.model);
        if (!m || !strstr(m, "smokegrenade")) continue;
        if (e->v.velocity.Length() > 10.f) continue;           // still flying
        if (!(e->v.flags & FL_ONGROUND)) continue;
        s_smokes.push_back({e->v.origin + Vector(0, 0, 40)});
    }
}

static bool smoke_blocks(const Vector &a, const Vector &b)
{
    for (const Smoke &s : s_smokes) {
        Vector ab = b - a, as = s.pos - a;
        float len2 = DotProduct(ab, ab);
        float t = len2 > 0.f ? xb_clamp(DotProduct(as, ab) / len2, 0.f, 1.f) : 0.f;
        Vector p = a + ab * t;
        if ((p - s.pos).Length() < 115.f) return true;
    }
    return false;
}

bool trace_visible(const Vector &a, const Vector &b, edict_t *skip, edict_t *target)
{
    TraceResult tr;
    g_engfuncs.pfnTraceLine(a, b, 0x100 /* ignore glass */, skip, &tr);
    return tr.flFraction >= 1.f || (target && tr.pHit == target);
}

// returns part (PART_*) + 1 when visible, 0 when not
int vis_check(edict_t *viewer, edict_t *t, bool useFov, float fov, float maxDist, Vector *where)
{
    Vector eye = xb_eye(viewer);
    Vector org = t->v.origin;
    float dist = (org - eye).Length();
    if (dist > maxDist) return 0;
    if (t->v.rendermode != kRenderNormal && t->v.renderamt < 25.f && dist > 120.f) return 0;   // invisible
    if (useFov && dist > 100.f) {
        Vector fwd = xb_view_forward(viewer->v.v_angle.x, viewer->v.v_angle.y);
        Vector d = org - eye;
        float l = d.Length();
        if (l > 0.f && DotProduct(fwd, d * (1.f / l)) < cosf(xb_rad(fov * 0.5f))) return 0;
    }
    bool ducking = (t->v.flags & FL_DUCKING) != 0;
    Vector pts[3] = {
        org + t->v.view_ofs,                                        // head
        org + Vector(0, 0, ducking ? 2.f : 6.f),                    // body
        org - Vector(0, 0, ducking ? 12.f : 28.f),                  // feet
    };
    vis_frame_smokes();
    for (int p = 0; p < 3; ++p) {
        if (dist > 150.f && smoke_blocks(eye, pts[p])) continue;
        if (trace_visible(eye, pts[p], viewer, t)) { if (where) *where = pts[p]; return p + 1; }
    }
    return 0;
}

void vis_scan(Bot &b, bool allTeams, std::vector<SeenInfo> &out)
{
    out.clear();
    int myTeam = xb_team(b.ed);
    for (int i = 1; i <= gpGlobals->maxClients; ++i) {
        if (i == b.id) continue;
        edict_t *t = xb_player(i);
        if (!t || !xb_alive(t) || (t->v.flags & FL_NOTARGET)) continue;
        int team = xb_team(t);
        if (team != 1 && team != 2) continue;
        if (!allTeams && team == myTeam) continue;
        int part = vis_check(b.ed, t, true, b.fov, b.vision);
        if (!part) continue;
        out.push_back({i, (t->v.origin - b.ed->v.origin).Length(), part - 1});
    }
    // closest first
    for (size_t i = 1; i < out.size(); ++i)
        for (size_t j = i; j > 0 && out[j].dist < out[j - 1].dist; --j) std::swap(out[j], out[j - 1]);
}

// ---------------------------------------------------------------------------------------------
// aim
static bool look_target(Bot &b, float now, Vector &target, bool &isEnt, float &tSpeed)
{
    edict_t *e = b.ed;
    isEnt = false; tSpeed = 0.f;
    bool onLadder = e->v.movetype == MOVETYPE_FLY;
    int mode = b.lookMode;
    if (onLadder && b.moveMode != MOVE_NONE) mode = LOOK_PATH;   // climbing needs the view on the ladder

    switch (mode) {
    case LOOK_POINT:
        target = b.lookPt;
        return true;
    case LOOK_ENT: {
        edict_t *t = g_engfuncs.pfnPEntityOfEntIndex(b.lookEnt);
        if (!t || t->free) { b.lookMode = LOOK_PATH; return false; }
        bool player = b.lookEnt >= 1 && b.lookEnt <= gpGlobals->maxClients;
        if (player && !xb_alive(t)) { b.lookMode = LOOK_PATH; return false; }
        Vector org = t->v.origin;
        if (!player) org = (t->v.absmin + t->v.absmax) * 0.5f;
        if (player) {
            bool duck = (t->v.flags & FL_DUCKING) != 0;
            if (b.lookPart == PART_HEAD) org = org + t->v.view_ofs - Vector(0, 0, 1.5f);
            else if (b.lookPart == PART_BODY) org = org + Vector(0, 0, duck ? 2.f : 6.f);
            else org = org - Vector(0, 0, duck ? 10.f : 24.f);
        }
        // humans aim where the target *was*: lag behind a moving target
        org = org - t->v.velocity * b.lag;
        tSpeed = t->v.velocity.Length2D();
        target = org;
        isEnt = true;
        return true;
    }
    case LOOK_PATH: {
        if (b.moveMode == MOVE_NONE) return false;
        Vector p = b.goal;
        if (b.moveMode == MOVE_PATH && b.pathPos < b.path.size()) {
            size_t ahead = b.pathPos;
            // look one node further when it is close: smoother turns
            if (!onLadder && ahead + 1 < b.path.size() && xb_dist2d(e->v.origin, g_nav.nodes[b.path[ahead]].pos) < 120.f) ++ahead;
            int n = b.path[ahead];
            if (n >= 0 && n < (int)g_nav.nodes.size()) p = g_nav.nodes[n].pos;
        }
        if (onLadder) { target = p; return true; }
        target = Vector(p.x, p.y, p.z + 17.f);
        if (xb_dist2d(e->v.origin, target) < 24.f) return false;   // too close to give a stable direction
        return true;
    }
    default:
        return false;
    }
}

static void update_aim(Bot &b, float now, float dt)
{
    Vector target;
    bool isEnt;
    float tSpeed;
    edict_t *e = b.ed;
    if (!look_target(b, now, target, isEnt, tSpeed)) {
        // nothing to look at: let the view settle
        b.vPitch *= 0.8f; b.vYaw *= 0.8f;
        b.pitch += b.vPitch * dt; b.yaw = xb_angnorm(b.yaw + b.vYaw * dt);
        b.aimErrDeg = 180.f;
        return;
    }
    if (isEnt) {
        if (now >= b.errNext) {
            float r = b.aimErr * (1.f + xb_clamp(tSpeed / 250.f, 0.f, 1.f));
            b.errOfs = Vector(xb_frand(-1, 1), xb_frand(-1, 1), xb_frand(-0.6f, 0.6f)) * r;
            b.errNext = now + xb_frand(0.35f, 0.8f);
        }
        float settle = xb_clamp(1.f - (now - b.trackStart) * 1.2f, 0.25f, 1.f);   // aim settles while tracking
        target = target + b.errOfs * settle;
    }
    Vector eye = xb_eye(e);
    float dp, dy;
    xb_dir_to_view(target - eye, dp, dy);
    if (isEnt) {                                   // recoil control: v_angle + punchangle is where bullets go
        dp -= e->v.punchangle.x * b.recoil;
        dy -= e->v.punchangle.y * b.recoil;
    }
    // critically damped spring, sub-stepped for stability
    float k = b.aimK, c = 2.f * sqrtf(k);
    float left = dt;
    while (left > 0.f) {
        float h = left > 0.01f ? 0.01f : left;
        left -= h;
        float ep = xb_angnorm(dp - b.pitch), ey = xb_angnorm(dy - b.yaw);
        b.vPitch += (ep * k - b.vPitch * c) * h;
        b.vYaw += (ey * k - b.vYaw * c) * h;
        b.vPitch = xb_clamp(b.vPitch, -b.maxTurn, b.maxTurn);
        b.vYaw = xb_clamp(b.vYaw, -b.maxTurn, b.maxTurn);
        b.pitch = xb_clamp(b.pitch + b.vPitch * h, -89.f, 89.f);
        b.yaw = xb_angnorm(b.yaw + b.vYaw * h);
    }
    float ep = xb_angnorm(dp - b.pitch), ey = xb_angnorm(dy - b.yaw);
    b.aimErrDeg = sqrtf(ep * ep + ey * ey);
}

// ---------------------------------------------------------------------------------------------
// movement
static inline void hold_auto(Bot &b, int bit) { b.autoButtons |= bit; }

static void arrive(Bot &b)
{
    b.moveMode = MOVE_NONE;
    b.path.clear();
    b.pathPos = 0;
    if (!b.arrivedSent) { b.arrivedSent = true; bot_emit("bot_arrived", b.id); }
}

static void update_move(Bot &b, float now, float &fwd, float &side)
{
    edict_t *e = b.ed;
    fwd = side = 0.f;
    bool onGround = (e->v.flags & FL_ONGROUND) != 0;
    bool onLadder = e->v.movetype == MOVETYPE_FLY;
    Vector org = e->v.origin;

    if (now < b.duckUntil) hold_auto(b, IN_DUCK);
    if (b.frozen || b.moveMode == MOVE_NONE) {
        if (now < b.strafeUntil && b.strafeDir && !b.frozen) {
            float sp = (e->v.maxspeed > 0.f ? e->v.maxspeed : 250.f) * b.speedScale;
            side = b.strafeDir * sp;
        }
        b.stuckN = 0;
        return;
    }

    Vector dest = b.goal;
    bool crouchHere = false;
    if (b.moveMode == MOVE_PATH) {
        while (b.pathPos < b.path.size()) {
            int n = b.path[b.pathPos];
            if (n < 0 || n >= (int)g_nav.nodes.size()) { b.path.clear(); break; }
            const NavNode &nd = g_nav.nodes[n];
            float r = nd.radius > 0.f ? nd.radius : 32.f;
            if ((nd.flags & (NF_LADDER | NF_CROUCH)) || (b.linkFlags & (LF_JUMP | LF_LADDER))) r = r < 20.f ? r : 20.f;
            else if (b.pathPos + 1 < b.path.size()) r = r > 48.f ? r : 48.f;      // cut corners a little
            float d2 = xb_dist2d(org, nd.pos);
            float dz = fabsf(org.z - nd.pos.z);
            bool reached = d2 < r && dz < (onLadder ? 28.f : 60.f);
            // overshoot: we are already closer to the next node than this one is
            if (!reached && b.pathPos + 1 < b.path.size() && !(b.linkFlags & (LF_JUMP | LF_LADDER))) {
                int nn = b.path[b.pathPos + 1];
                if (nn >= 0 && nn < (int)g_nav.nodes.size()) {
                    const Vector &np = g_nav.nodes[nn].pos;
                    if (xb_dist2d(org, np) < xb_dist2d(nd.pos, np) && dz < 40.f && d2 < 96.f) reached = true;
                }
            }
            if (!reached) break;
            ++b.pathPos;
            b.linkFlags = (b.pathPos < b.path.size()) ? g_nav.link_flags(n, b.path[b.pathPos]) : 0;
            if (b.linkFlags < 0) b.linkFlags = 0;
        }
        if (b.pathPos < b.path.size()) {
            const NavNode &nd = g_nav.nodes[b.path[b.pathPos]];
            dest = nd.pos;
            crouchHere = (nd.flags & NF_CROUCH) || (b.linkFlags & LF_CROUCH);
            if ((b.linkFlags & LF_JUMP) && onGround && now - b.lastJump > 0.8f && xb_dist2d(org, dest) < 72.f &&
                dest.z - org.z > 12.f) {
                b.pressOnce |= IN_JUMP;
                b.lastJump = now;
                b.duckUntil = now + 0.5f;                            // crouch-jump reaches higher
            }
        } else if (xb_dist2d(org, b.goal) < 28.f || b.path.empty()) {
            arrive(b);
            return;
        }
    } else {   // MOVE_DIRECT
        if (xb_dist2d(org, b.goal) < 26.f && fabsf(org.z - b.goal.z) < 72.f) { arrive(b); return; }
    }
    if (crouchHere) hold_auto(b, IN_DUCK);

    float maxs = (e->v.maxspeed > 0.f ? e->v.maxspeed : 250.f) * b.speedScale;
    float speed = b.walk ? (maxs * 0.5f < 120.f ? maxs * 0.5f : 120.f) : maxs * b.speed;

    Vector d = dest - org;
    d.z = 0.f;
    float l = d.Length();
    if (l > 0.001f) d = d * (1.f / l);
    float y = xb_rad(b.yaw);
    float fx = cosf(y), fy = sinf(y);             // forward (2D)
    float rx = sinf(y), ry = -cosf(y);            // right   (2D)
    if (onLadder) {
        fwd = speed;                               // view is on the ladder: forward climbs up or down
    } else {
        fwd = (d.x * fx + d.y * fy) * speed;
        side = (d.x * rx + d.y * ry) * speed;
    }
    if (now < b.strafeUntil && b.strafeDir) {
        side += b.strafeDir * speed;
        float m = sqrtf(fwd * fwd + side * side);
        if (m > speed && m > 0.f) { fwd *= speed / m; side *= speed / m; }
    }
    if (now < b.unstuckUntil) side = b.strafeDir ? b.strafeDir * speed : side;

    // stuck detection and recovery
    if (now >= b.stuckCheck) {
        float moved = xb_dist2d(org, b.stuckPos);
        b.stuckPos = org;
        b.stuckCheck = now + 0.35f;
        if (moved < 10.f && (onGround || onLadder)) ++b.stuckN; else b.stuckN = 0;
        if (b.stuckN >= 2) {
            switch ((b.stuckN - 2) % 5) {
            case 0: b.pressOnce |= IN_JUMP; b.duckUntil = now + 0.5f; break;
            case 1: b.duckUntil = now + 0.6f; break;
            case 2: b.strafeDir = -1; b.unstuckUntil = now + 0.5f; break;
            case 3: b.strafeDir = 1; b.unstuckUntil = now + 0.5f; break;
            case 4: b.pressOnce |= IN_USE; break;                    // doors / buttons
            }
        }
        if (b.stuckN >= 11) {
            b.stuckN = 0;
            b.moveMode = MOVE_NONE;
            b.path.clear();
            b.arrivedSent = true;
            bot_emit("bot_stuck", b.id);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// fire controller
static bool friend_in_line(Bot &b)
{
    edict_t *e = b.ed;
    Vector eye = xb_eye(e);
    Vector fwd = xb_view_forward(b.pitch + e->v.punchangle.x, b.yaw + e->v.punchangle.y);
    TraceResult tr;
    g_engfuncs.pfnTraceLine(eye, eye + fwd * 8192.f, 0, e, &tr);
    edict_t *h = tr.pHit;
    if (!h || FNullEnt(h)) return false;
    int idx = xb_index(h);
    if (idx < 1 || idx > gpGlobals->maxClients) return false;
    return xb_alive(h) && xb_team(h) == xb_team(e);
}

static int update_fire(Bot &b, float now)
{
    if (b.fireMode == FIRE_OFF) { b.fireDown = false; return 0; }
    if (now >= b.ffCheckT) { b.ffBlocked = friend_in_line(b); b.ffCheckT = now + 0.05f; }
    bool aimed = b.aimErrDeg <= b.fireMaxAng && !b.ffBlocked;
    if (!aimed) { if (b.fireMode == FIRE_BURST) b.fireDown = false; return 0; }
    switch (b.fireMode) {
    case FIRE_SPRAY:
        return IN_ATTACK;
    case FIRE_TAP:
        if (now >= b.fireT) { b.fireT = now + b.firePause; return IN_ATTACK; }
        return 0;
    case FIRE_BURST:
        if (b.fireDown) {
            if (now >= b.fireT) { b.fireDown = false; b.fireT = now + b.firePause; return 0; }
            return IN_ATTACK;
        }
        if (now >= b.fireT) { b.fireDown = true; b.fireT = now + b.fireHold; return IN_ATTACK; }
        return 0;
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------
void bot_run(Bot &b, float now)
{
    edict_t *e = b.ed;
    e->v.flags |= FL_FAKECLIENT;          // never let anything clear it (messages to a bot without it crash the server)
    float dt = now - b.lastRun;
    if (dt < 0.f || dt > 0.25f) dt = 0.01f;
    b.msAcc += dt * 1000.f;
    int ms = (int)b.msAcc;
    if (ms < 1) { ms = 1; b.msAcc = 0.f; }
    else if (ms > 100) { ms = 100; b.msAcc = 0.f; }
    else b.msAcc -= (float)ms;
    b.lastRun = now;

    float fwd = 0.f, side = 0.f;
    int buttons = 0;
    bool alive = xb_alive(e);
    if (alive && !b.wasAlive) {            // (re)spawned: start from the spawn spot's direction
        b.yaw = e->v.angles.y; b.pitch = 0.f; b.vPitch = b.vYaw = 0.f;
        b.stuckPos = e->v.origin; b.stuckN = 0;
    }
    b.wasAlive = alive;
    if (alive) {
        b.autoButtons = 0;
        update_aim(b, now, dt);
        update_move(b, now, fwd, side);
        buttons |= update_fire(b, now);
        for (int bit = 0; bit < 16; ++bit) if (b.holdT[bit] > now) buttons |= (1 << bit);
        buttons |= b.pressOnce | b.autoButtons;
        b.pressOnce = 0;
        if (b.frozen) { fwd = side = 0.f; buttons &= ~(IN_JUMP | IN_DUCK); }
    } else if (b.moveMode != MOVE_NONE || b.fireMode != FIRE_OFF) {
        b.reset_life();
    }

    e->v.v_angle.x = b.pitch;
    e->v.v_angle.y = b.yaw;
    e->v.v_angle.z = 0.f;
    e->v.angles.x = -b.pitch * (1.f / 3.f);
    e->v.angles.y = b.yaw;
    e->v.angles.z = 0.f;
    float va[3] = {b.pitch, b.yaw, 0.f};
    g_engfuncs.pfnRunPlayerMove(e, va, fwd, side, 0.f, (unsigned short)buttons, 0, (byte)ms);
}
