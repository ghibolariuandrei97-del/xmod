// main.cpp - Metamod entry points, engine/game hooks, the per-frame bot loop, fake client commands,
// creating and kicking bots.
#include "xbots.h"
#include <cstdio>

// game DLL calls that skip the other Metamod plugins (like every bot mod does)
#define MDLL_ClientCommand     (*gpGamedllFuncs->dllapi_table->pfnClientCommand)
#define MDLL_ClientConnect     (*gpGamedllFuncs->dllapi_table->pfnClientConnect)
#define MDLL_ClientPutInServer (*gpGamedllFuncs->dllapi_table->pfnClientPutInServer)

bool xbots_link_attach();
void xbots_link_detach();

enginefuncs_t    g_engfuncs;
globalvars_t    *gpGlobals = nullptr;
meta_globals_t  *gpMetaGlobals = nullptr;
gamedll_funcs_t *gpGamedllFuncs = nullptr;
mutil_funcs_t   *gpMetaUtilFuncs = nullptr;

Bot g_bots[33];

plugin_info_t Plugin_info = {
    META_INTERFACE_VERSION, "xmod_bots", XMOD_EXT_VERSION, __DATE__, "Andrew & Claude",
    "https://github.com/ghibolariuandrei97", "XBOTS", PT_ANYTIME, PT_ANYTIME,
};

// ---------------------------------------------------------------------------------------------
void Bot::reset_life()
{
    vPitch = vYaw = 0.f;
    moveMode = MOVE_NONE; path.clear(); pathPos = 0; linkFlags = 0;
    strafeDir = 0; strafeUntil = 0.f;
    for (float &t : holdT) t = 0.f;
    pressOnce = 0; autoButtons = 0; duckUntil = 0.f;
    fireMode = FIRE_OFF; fireDown = false;
    lookMode = LOOK_PATH; lookEnt = 0;
    stuckN = 0; arrivedSent = true;
}

Bot *bot_get(int id)
{
    if (id < 1 || id > 32 || !g_bots[id].used) return nullptr;
    Bot &b = g_bots[id];
    if (!b.ed || b.ed->free || !b.ed->pvPrivateData) return nullptr;
    return &b;
}

void bot_emit(const char *ev, int id)
{
    if (!g_xmod || !g_xext) return;
    XmodValue *a = g_xmod->val_array();
    g_xmod->val_push(a, g_xmod->val_int(id));
    g_xmod->emit_event(ev, a);
}

// ---------------------------------------------------------------------------------------------
// Fake client commands: we fill our own argv and call the game's ClientCommand; the game reads the
// arguments back through the engine functions Cmd_Argv/Cmd_Args/Cmd_Argc, which we answer while faking.
static bool s_fake = false;
static std::vector<std::string> s_argv;
static char s_args[512];

static const char *hk_Cmd_Args(void)
{
    if (s_fake) RETURN_META_VALUE(MRES_SUPERCEDE, s_args);
    RETURN_META_VALUE(MRES_IGNORED, nullptr);
}
static const char *hk_Cmd_Argv(int i)
{
    if (s_fake) RETURN_META_VALUE(MRES_SUPERCEDE, (i >= 0 && i < (int)s_argv.size()) ? s_argv[i].c_str() : "");
    RETURN_META_VALUE(MRES_IGNORED, nullptr);
}
static int hk_Cmd_Argc(void)
{
    if (s_fake) RETURN_META_VALUE(MRES_SUPERCEDE, (int)s_argv.size());
    RETURN_META_VALUE(MRES_IGNORED, 0);
}

// ---------------------------------------------------------------------------------------------
// Crash guard. ReGameDLL's CBasePlayer::Spawn() does "pev->flags &= FL_PROXY", which wipes FL_FAKECLIENT from our
// bots on every spawn and then sends MSG_ONE messages to them. The engine drops a MSG_ONE to a bot only when the
// target has FL_FAKECLIENT, otherwise it writes into the bot's (non-existent) network channel:
// "SZ_GetSpace: Tried to write to an uninitialized sizebuf_t". So we restore the flag right before every message
// to one of our bots, and we swallow client cvar queries to bots (the engine never guards those).
static inline bool is_our_bot(const edict_t *e)
{
    if (!e) return false;
    int i = g_engfuncs.pfnIndexOfEdict(e);
    return i >= 1 && i <= 32 && g_bots[i].used && g_bots[i].ed == e;
}
static void hk_MessageBegin(int dest, int, const float *, edict_t *ed)
{
    if (ed && (dest == MSG_ONE || dest == MSG_ONE_UNRELIABLE) && is_our_bot(ed)) ed->v.flags |= FL_FAKECLIENT;
    RETURN_META(MRES_IGNORED);
}
static void hk_QueryClientCvarValue(const edict_t *e, const char *)
{
    if (is_our_bot(e) || (e && (e->v.flags & FL_FAKECLIENT))) RETURN_META(MRES_SUPERCEDE);
    RETURN_META(MRES_IGNORED);
}
static void hk_QueryClientCvarValue2(const edict_t *e, const char *, int)
{
    if (is_our_bot(e) || (e && (e->v.flags & FL_FAKECLIENT))) RETURN_META(MRES_SUPERCEDE);
    RETURN_META(MRES_IGNORED);
}

static void fake_one(edict_t *e, const std::string &cmd)
{
    s_argv.clear();
    size_t i = 0, n = cmd.size();
    while (i < n && cmd[i] == ' ') ++i;
    size_t argsStart = std::string::npos;
    while (i < n) {
        std::string tok;
        if (cmd[i] == '"') {
            ++i;
            while (i < n && cmd[i] != '"') tok += cmd[i++];
            if (i < n) ++i;
        } else {
            while (i < n && cmd[i] != ' ') tok += cmd[i++];
        }
        s_argv.push_back(tok);
        while (i < n && cmd[i] == ' ') ++i;
        if (s_argv.size() == 1) argsStart = i;
    }
    if (s_argv.empty()) return;
    std::string rest = (argsStart != std::string::npos && argsStart < n) ? cmd.substr(argsStart) : "";
    snprintf(s_args, sizeof(s_args), "%s", rest.c_str());
    s_fake = true;
    MDLL_ClientCommand(e);
    s_fake = false;
    s_argv.clear();
    s_args[0] = 0;
}

void fake_command(edict_t *e, const char *line)
{
    if (!e || !line || s_fake) return;
    std::string cur;
    bool q = false;
    for (const char *p = line; ; ++p) {
        char c = *p;
        if (c == '"') q = !q;
        if (c == 0 || c == '\n' || (c == ';' && !q)) {
            if (!cur.empty()) fake_one(e, cur);
            cur.clear();
            if (c == 0) break;
            continue;
        }
        cur += c;
    }
}

// ---------------------------------------------------------------------------------------------
int bot_create(const char *name, std::string &err)
{
    edict_t *e = g_engfuncs.pfnCreateFakeClient(name);
    if (!e || e->free) { err = "server is full"; return 0; }
    if (e->pvPrivateData) g_engfuncs.pfnFreeEntPrivateData(e);
    e->pvPrivateData = nullptr;
    e->v.frags = 0;
    // FL_FAKECLIENT must be set BEFORE anything can send this client a network message: the engine only skips
    // MSG_ONE to clients that have it. Without it ClientPutInServer (MOTD, team menu) crashes the server with
    // "SZ_GetSpace: Tried to write to an uninitialized sizebuf_t".
    e->v.flags |= FL_FAKECLIENT | FL_CLIENT;
    gpMetaUtilFuncs->pfnCallGameEntity(PLID, "player", &e->v);   // let the game create its CBasePlayer
    e->v.flags |= FL_FAKECLIENT | FL_CLIENT;

    int idx = xb_index(e);
    char *ib = g_engfuncs.pfnGetInfoKeyBuffer(e);
    static const char *keys[][2] = {
        {"model", "gordon"}, {"rate", "3500.000000"}, {"cl_updaterate", "20"}, {"cl_lw", "1"}, {"cl_lc", "1"},
        {"tracker", "0"}, {"cl_dlmax", "128"}, {"lefthand", "1"}, {"friends", "0"}, {"dm", "0"}, {"_ah", "0"},
        {"_vgui_menus", "0"}, {"*bot", "1"},
    };
    for (auto &kv : keys) g_engfuncs.pfnSetClientKeyValue(idx, ib, const_cast<char *>(kv[0]), const_cast<char *>(kv[1]));

    char reject[128] = {0};
    e->v.flags |= FL_FAKECLIENT;
    if (!MDLL_ClientConnect(e, name, "127.0.0.1", reject)) {
        err = reject[0] ? reject : "connection rejected";
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "kick #%d\n", g_engfuncs.pfnGetPlayerUserId(e));
        g_engfuncs.pfnServerCommand(cmd);
        return 0;
    }
    e->v.flags |= FL_FAKECLIENT;
    MDLL_ClientPutInServer(e);
    e->v.flags |= FL_FAKECLIENT;

    Bot &b = g_bots[idx];
    b = Bot();
    b.used = true;
    b.ed = e;
    b.id = idx;
    b.lastRun = gpGlobals->time;
    b.spawnedAt = gpGlobals->time;
    b.yaw = e->v.angles.y;
    return idx;
}

bool bot_kick(int id)
{
    Bot *b = bot_get(id);
    if (!b) return false;
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "kick #%d\n", g_engfuncs.pfnGetPlayerUserId(b->ed));
    g_engfuncs.pfnServerCommand(cmd);
    return true;
}

// ---------------------------------------------------------------------------------------------
// game DLL hooks
static void dll_StartFrame(void)
{
    float now = gpGlobals->time;
    for (int i = 1; i <= 32; ++i) if (g_bots[i].used && g_bots[i].ed) g_bots[i].ed->v.flags |= FL_FAKECLIENT;
    for (int i = 1; i <= gpGlobals->maxClients && i <= 32; ++i) {
        Bot &b = g_bots[i];
        if (!b.used) continue;
        edict_t *e = g_engfuncs.pfnPEntityOfEntIndex(i);
        if (!e || e != b.ed || e->free || !e->pvPrivateData) { b.used = false; continue; }
        bot_run(b, now);
    }
    g_nav.frame(now);
    RETURN_META(MRES_IGNORED);
}

static void dll_ClientDisconnect(edict_t *e)
{
    int idx = e ? xb_index(e) : 0;
    if (idx >= 1 && idx <= 32) g_bots[idx].used = false;
    RETURN_META(MRES_IGNORED);
}

static void dll_ServerDeactivate(void)
{
    if (g_nav.dirty && gpGlobals) g_nav.save(STRING(gpGlobals->mapname));
    for (Bot &b : g_bots) b.used = false;     // the engine drops fake clients on level change
    RETURN_META(MRES_IGNORED);
}

static void dll_ServerActivate_Post(edict_t *, int, int)
{
    if (!g_xext) xbots_link_attach();          // retry if xmod was not ready at attach time
    g_nav.load(STRING(gpGlobals->mapname));
    RETURN_META(MRES_IGNORED);
}

static DLL_FUNCTIONS s_dll, s_dllPost;
static enginefuncs_t s_eng;

extern "C" {
C_DLLEXPORT int GetEntityAPI2(DLL_FUNCTIONS *t, int *)
{
    memset(&s_dll, 0, sizeof(s_dll));
    s_dll.pfnStartFrame = dll_StartFrame;
    s_dll.pfnClientDisconnect = dll_ClientDisconnect;
    s_dll.pfnServerDeactivate = dll_ServerDeactivate;
    memcpy(t, &s_dll, sizeof(DLL_FUNCTIONS));
    return TRUE;
}

C_DLLEXPORT int GetEntityAPI2_Post(DLL_FUNCTIONS *t, int *)
{
    memset(&s_dllPost, 0, sizeof(s_dllPost));
    s_dllPost.pfnServerActivate = dll_ServerActivate_Post;
    memcpy(t, &s_dllPost, sizeof(DLL_FUNCTIONS));
    return TRUE;
}

C_DLLEXPORT int GetEngineFunctions(enginefuncs_t *t, int *)
{
    memset(&s_eng, 0, sizeof(s_eng));
    s_eng.pfnCmd_Args = hk_Cmd_Args;
    s_eng.pfnCmd_Argv = hk_Cmd_Argv;
    s_eng.pfnCmd_Argc = hk_Cmd_Argc;
    s_eng.pfnMessageBegin = hk_MessageBegin;
    s_eng.pfnQueryClientCvarValue = hk_QueryClientCvarValue;
    s_eng.pfnQueryClientCvarValue2 = hk_QueryClientCvarValue2;
    memcpy(t, &s_eng, sizeof(enginefuncs_t));
    return TRUE;
}


// ---------------------------------------------------------------------------------------------
// Metamod entry points
C_DLLEXPORT void WINAPI GiveFnptrsToDll(enginefuncs_t *eng, globalvars_t *globals)
{
    memcpy(&g_engfuncs, eng, sizeof(enginefuncs_t));
    gpGlobals = globals;
}

C_DLLEXPORT int Meta_Query(char *, plugin_info_t **pinfo, mutil_funcs_t *mutil)
{
    *pinfo = &Plugin_info;
    gpMetaUtilFuncs = mutil;
    return TRUE;
}

C_DLLEXPORT int Meta_Attach(PLUG_LOADTIME, META_FUNCTIONS *t, meta_globals_t *mg, gamedll_funcs_t *gd)
{
    gpMetaGlobals = mg;
    gpGamedllFuncs = gd;
    memset(t, 0, sizeof(META_FUNCTIONS));
    t->pfnGetEntityAPI2 = GetEntityAPI2;
    t->pfnGetEntityAPI2_Post = GetEntityAPI2_Post;
    t->pfnGetEngineFunctions = GetEngineFunctions;
    if (!xbots_link_attach())
        g_engfuncs.pfnServerPrint("[xmod_bots] xmod not found yet (is xmod listed BEFORE xmod_bots in metamod/plugins.ini?) - will retry on map start\n");
    if (gpGlobals && gpGlobals->mapname) g_nav.load(STRING(gpGlobals->mapname));
    return TRUE;
}

C_DLLEXPORT int Meta_Detach(PLUG_LOADTIME, PL_UNLOAD_REASON)
{
    if (g_nav.dirty && gpGlobals && gpGlobals->mapname) g_nav.save(STRING(gpGlobals->mapname));
    for (int i = 1; i <= 32; ++i) if (g_bots[i].used) bot_kick(i);
    xbots_link_detach();
    return TRUE;
}
} // extern "C"
