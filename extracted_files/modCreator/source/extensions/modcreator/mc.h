// ModCreator - the "make a whole new mod" toolkit for xmod (namespace xmod.mc, Lua library lib/modcreator.lua).
// This header is shared by every .cpp of the extension.
//
// Layers:
//   C (this extension, xmod.mc.*)  : things Lua cannot do with the xmod 0.4.4 core - virtual calls (ExecuteHam),
//                                     KeyValue dispatch, engine movement (WalkMove/MoveToOrigin/DropToFloor),
//                                     studio models (sequences, attachments, bones, controllers), named private data
//                                     (859 CS members + 76 gamerules members), game rules / round state, bullets,
//                                     radius damage, weapon events, weaponbox world models.
//   Lua (lib/modcreator.lua)        : the frameworks - entity classes, NPCs + AI, rounds, teams, objectives, weapons, fx.
//
// Works with xmod 0.4.0+ (needs only the ABI-1 extension API; fire_hook_sync is optional).
#pragma once
#include "mm_sdk.h"
#include "xmod_ext.h"
#include <studio.h>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#ifdef _WIN32
#define strcasecmp _stricmp
#endif
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MC_VERSION "1.0.0"
#define MC_NS      "mc"

extern const XmodApi *g_xmod;
extern XmodExt *g_xext;

// ---- calling convention of C++ member functions of the game DLL (same as xmod's Ham)
#if defined(_WIN32) && defined(_MSC_VER)
#define MC_CC __fastcall
#define MC_DUMMY int,
#define MC_DUMMY_ARG 0,
#elif defined(_WIN32)
#define MC_CC __attribute__((thiscall))
#define MC_DUMMY
#define MC_DUMMY_ARG
#else
#define MC_CC
#define MC_DUMMY
#define MC_DUMMY_ARG
#endif

#ifdef _WIN32
static const bool MC_LINUX = false;
#else
static const bool MC_LINUX = true;
#endif

// ---- small helpers over the value API
struct Args {
	XmodCall *c;
	int n;
	explicit Args(XmodCall *call) : c(call), n(g_xmod->call_argc(call)) {}
	const XmodValue *v(int i) const { return i < n ? g_xmod->call_arg(c, i) : nullptr; }
	bool has(int i) const { const XmodValue *x = v(i); return x && g_xmod->val_type(x) != XV_NIL; }
	int64_t i(int k, int64_t d = 0) const { const XmodValue *x = v(k); return x ? g_xmod->val_get_int(x, d) : d; }
	double f(int k, double d = 0) const { const XmodValue *x = v(k); return x ? g_xmod->val_get_num(x, d) : d; }
	bool b(int k, bool d = false) const { const XmodValue *x = v(k); if (!x || g_xmod->val_type(x) == XV_NIL) return d;
		if (g_xmod->val_type(x) == XV_BOOL) return g_xmod->val_get_bool(x, d) != 0; return g_xmod->val_get_num(x, 0) != 0; }
	const char *s(int k, const char *d = nullptr) const { const XmodValue *x = v(k); if (!x) return d; const char *r = g_xmod->val_get_str(x, nullptr); return r ? r : d; }
	int type(int k) const { const XmodValue *x = v(k); return x ? g_xmod->val_type(x) : XV_NIL; }
	void ret(XmodValue *r) const { g_xmod->call_return(c, r); }
	void ret_int(int64_t x) const { ret(g_xmod->val_int(x)); }
	void ret_num(double x) const { ret(g_xmod->val_num(x)); }
	void ret_bool(bool x) const { ret(g_xmod->val_bool(x)); }
	void ret_str(const char *x) const { ret(g_xmod->val_cstr(x)); }
	void ret_nil() const { ret(g_xmod->val_nil()); }
	void ret_vec(const float *x) const { XmodValue *a = g_xmod->val_array(); for (int k = 0; k < 3; k++) g_xmod->val_push(a, g_xmod->val_num(x[k])); ret(a); }
	void err(const char *fmt, ...) const;
};
// array {x,y,z} for a Lua function that returns a vector: Lua side unpacks with table.unpack
XmodValue *mc_vec(const float *x);

// ---- entities
edict_t *mc_edict(int index);                  // nullptr when the index is not a live entity
inline int mc_index(const edict_t *e) { return e ? ENTINDEX(e) : 0; }
void *mc_cbase(edict_t *e);                    // private data (CBaseEntity*) or nullptr
edict_t *mc_edict_of_cbase(void *cbase);       // CBaseEntity* -> edict (pev lives right after the vptr)
void **mc_vtable(edict_t *e);
int mc_vidx(const char *ham_name);             // vtable index of a Ham function on this OS, -1 unknown
const char *mc_ham_names();                    // comma separated

// ---- private data / game rules
bool mc_pdata_find(const char *name, int *offset, int *type, int *count, bool *uns);   // "m_iClip" or "CBasePlayerWeapon.m_iClip"
void *mc_rules();                              // CHalfLifeMultiplay* (current map) or nullptr
const char *mc_rules_source();
bool mc_regamedll();
bool mc_rules_find(const char *name, int *offset, int *type, int *count, bool *uns);
enum { PT_INT, PT_FLOAT, PT_BOOL, PT_SHORT, PT_CHAR, PT_CBASE, PT_EHANDLE, PT_EDICT, PT_ENTVARS, PT_VECTOR, PT_STRING, PT_STRINGINT };
XmodValue *mc_read_member(char *base, int type, int count, bool uns, int index);
bool mc_write_member(char *base, int type, int count, int index, const Args &a, int arg, std::string &err);

// ---- module registration (each .cpp registers its own functions)
bool mc_reg(const char *name, xmod_sync_fn fn);
bool reg_ent();
bool reg_studio();
bool reg_pdata();
bool reg_rules();
bool reg_combat();
bool reg_weapon();
void weapon_on_map_change();

// ---- engine hooks implemented in weapon.cpp (installed by main.cpp)
void mc_hook_setmodel(edict_t *e, const char *m);
void mc_hook_playbackevent(int flags, const edict_t *invoker, unsigned short ev);
void mc_hook_ucd(const edict_t *ent, int sendweapons, struct clientdata_s *cd);

// ---- frequently used pdata (byte offsets on this OS, resolved once from offsets.inc)
struct PdOff { int active_item, player_items, ammo, next_attack, last_hitgroup, anim_ext, wb_items, wb_ammo_names, wb_ammo_count,
	w_player, w_next, w_id, w_next_primary, w_next_secondary, w_idle, w_ammo_type, w_clip, w_in_reload; };
extern PdOff g_pd;
void mc_pd_init();
template <class T> inline T &pd(void *cbase, int byte_off) { return *reinterpret_cast<T *>(reinterpret_cast<char *>(cbase) + byte_off); }
void mc_log(const char *fmt, ...);
