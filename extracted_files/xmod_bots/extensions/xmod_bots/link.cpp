// link.cpp - connects this Metamod plugin to xmod's extension API.
//
// xmod_bots needs its own Metamod hooks (StartFrame to drive the bots every frame, Cmd_Argv/Args/Argc to
// send fake client commands such as "jointeam", "buy", "say"), so - as xmod_skills.md Part III E7 says -
// it does NOT include xmod_ext_glue.h but writes the Metamod entry points itself (main.cpp) and connects
// to xmod by hand here.
//
// Same calls as xmod_ext_glue.h's Meta_Attach / Meta_Detach (xmod_ext_connect -> ext_register -> ... -> ext_unregister).
#include "xbots.h"

const XmodApi *g_xmod = nullptr;
XmodExt       *g_xext = nullptr;

bool xbots_link_attach()
{
    if (g_xext) return true;
    g_xmod = xmod_ext_connect();                                      // xmod_ext.h
    if (!g_xmod) return false;
    g_xext = g_xmod->ext_register(XMOD_EXT_NAME, XMOD_EXT_VERSION);   // same call the glue makes
    if (!g_xext) { g_xmod = nullptr; return false; }
    if (!xbots_register(g_xmod, g_xext)) {
        g_xmod->log_error("[xmod_bots] registering the xmod.bot functions failed");
        return false;
    }
    g_xmod->log("[xmod_bots] " XMOD_EXT_VERSION " attached: xmod.bot.* is available");
    return true;
}

void xbots_link_detach()
{
    if (g_xmod && g_xext) g_xmod->ext_unregister(g_xext);
    g_xext = nullptr;
    g_xmod = nullptr;
}
