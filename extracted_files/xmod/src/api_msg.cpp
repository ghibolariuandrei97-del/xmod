#include "xmod.h"
#include <cstring>
#include <unordered_set>

// ===========================================================================================================
//  Message safety model (GoldSrc allows exactly ONE open message; MESSAGE_BEGIN while another is open = Sys_Error crash)
//   1. CAPTURE/REPLAY: a game message that has Lua listeners is never left open in the engine while Lua runs. We swallow
//      Begin/Write*/End, record them, run the handlers, then replay the message to the engine ourselves (or drop it if a
//      handler returned true). So handlers may freely send messages / kill players / give items.
//   2. DEFERRAL: if Lua ever runs while a *pass-through* message is open, anything that would send messages is queued and
//      flushed right after that message ends.
//   3. SCRIPT BUILDER: xmod.msg.begin/byte/.../finish only record; the engine sees the whole message atomically at finish().
//      A script that errors between begin() and finish() cannot leave the engine mid-message (the builder is discarded).
// ===========================================================================================================
extern bool g_any_msg_hook;

struct MsgOp { int kind; double num; std::string str; };         // kind: 1 byte 2 char 3 short 4 long 5 angle 6 coord 7 string 8 entity
static struct {
	bool capture = false, block = false;
	int dest = 0, type = 0; bool hasorg = false; float org[3] = {0, 0, 0}; edict_t *ed = nullptr;
	Event *ev = nullptr; std::string name; std::vector<MsgOp> ops;
} M;
static struct { bool active = false; int dest = 0, type = 0; bool hasorg = false; float org[3] = {0, 0, 0}; edict_t *ed = nullptr; std::vector<MsgOp> ops; } S;
static bool g_pass_open = false;
static std::vector<std::function<void()>> g_deferred;
static std::unordered_set<std::string> g_blocked;
static std::string g_names[256]; static bool g_named[256]; static Event *g_msg_ev[256];

static const std::string &msg_name(int type) {
	static std::string none;
	if (type < 0 || type > 255) return none;
	if (!g_named[type]) {
		g_named[type] = true;
		const char *n = (type >= 64) ? GET_USER_MSG_NAME(PLID, type, nullptr) : nullptr;
		g_names[type] = n ? n : ("svc:" + std::to_string(type));
	}
	return g_names[type];
}
void msg_reset() {
	M = decltype(M)(); S = decltype(S)(); g_pass_open = false; g_deferred.clear(); g_blocked.clear();
	for (int i = 0; i < 256; i++) { g_named[i] = false; g_names[i].clear(); g_msg_ev[i] = nullptr; }
}

// ---- direct engine emission (our g_engfuncs is the real engine table: these calls bypass every Metamod hook, including ours)
static void emit_message(int dest, int type, bool hasorg, const float *org, edict_t *ed, const std::vector<MsgOp> &ops) {
	MESSAGE_BEGIN(dest, type, hasorg ? org : nullptr, ed);
	for (const MsgOp &o : ops) {
		switch (o.kind) {
		case 1: WRITE_BYTE((int)o.num); break;   case 2: WRITE_CHAR((int)o.num); break;
		case 3: WRITE_SHORT((int)o.num); break;  case 4: WRITE_LONG((int)o.num); break;
		case 5: WRITE_ANGLE((float)o.num); break; case 6: WRITE_COORD((float)o.num); break;
		case 7: WRITE_STRING(o.str.c_str()); break; case 8: WRITE_ENTITY((int)o.num); break;
		}
	}
	MESSAGE_END();
}

// ---- deferral
bool msg_busy() { return g_pass_open; }
void msg_defer(const std::function<void()> &f) { g_deferred.push_back(f); }
static void flush_deferred() {
	if (g_deferred.empty()) return;
	std::vector<std::function<void()>> q; q.swap(g_deferred);
	for (auto &f : q) f();
}
void msg_end_post() { g_pass_open = false; flush_deferred(); }       // engine post hook of MessageEnd
void msg_frame_reset() { g_pass_open = false; flush_deferred(); }    // messages never span frames: recover from any stuck state

// ---- game message hooks (called from the engine pre-hooks in main.cpp; true = supersede the real call)
bool msg_begin(int dest, int type, const float *org, edict_t *ed) {
	M.capture = false; M.block = false; g_pass_open = false;
	if (!g_any_msg_hook && g_blocked.empty()) { g_pass_open = true; return false; }
	const std::string &n = msg_name(type);
	if (n.empty()) { g_pass_open = true; return false; }
	if (g_blocked.count(n)) { M.block = true; return true; }           // static block: nothing reaches the engine
	Event *e = (type >= 0 && type < 256) ? g_msg_ev[type] : nullptr;
	if (!e) { e = ev_get(("message:" + n).c_str()); if (type >= 0 && type < 256) g_msg_ev[type] = e; }   // cached: no allocation per message
	if (!ev_has(e)) { g_pass_open = true; return false; }
	M.capture = true; M.dest = dest; M.type = type; M.ev = e; M.name = n; M.ed = ed; M.ops.clear();
	M.hasorg = org != nullptr; if (org) { M.org[0] = org[0]; M.org[1] = org[1]; M.org[2] = org[2]; }
	return true;                                                      // engine never sees this Begin; we replay later
}
bool msg_write(int kind, double num, const char *str) {
	if (M.block) return true;
	if (M.capture) { M.ops.push_back(MsgOp{kind, num, str ? str : ""}); return true; }
	return false;
}
bool msg_end() {
	if (M.block) { M.block = false; return true; }
	if (!M.capture) return false;                                     // pass-through message: the real End runs, msg_end_post() follows
	M.capture = false;
	std::vector<MsgOp> ops; ops.swap(M.ops);                          // take ownership BEFORE running Lua: nested game messages must not disturb us
	int dest = M.dest, type = M.type; bool hasorg = M.hasorg; float org[3] = {M.org[0], M.org[1], M.org[2]}; edict_t *ed = M.ed; Event *ev = M.ev; std::string name = M.name;
	int ent = (ed && !FNullEnt(ed)) ? ENTINDEX(ed) : 0;
	EvResult r = ev_fire(ev, 4, [&](lua_State *l) {
		lua_pushstring(l, name.c_str()); lua_pushinteger(l, dest); lua_pushinteger(l, ent);
		lua_createtable(l, (int)ops.size(), 0);
		for (size_t i = 0; i < ops.size(); i++) { if (ops[i].kind == 7) lua_pushstring(l, ops[i].str.c_str()); else lua_pushnumber(l, ops[i].num); lua_rawseti(l, -2, (lua_Integer)i + 1); }
		return 4; });
	if (r.kind != 1) emit_message(dest, type, hasorg, org, ed, ops);  // not blocked: replay to the engine
	return true;
}

// ---- script-facing low level builder
static int resolve_msg(lua_State *l, int idx) {
	if (lua_isinteger(l, idx)) return (int)lua_tointeger(l, idx);
	const char *n = luaL_checkstring(l, idx);
	int id = GET_USER_MSG_ID(PLID, n, nullptr);
	if (!id) luaL_error(l, "unknown user message '%s'", n);
	return id;
}
void msg_script_cleanup() {                                           // called when the outermost script call ends
	if (S.active) { S.active = false; S.ops.clear(); xerr("script left xmod.msg.begin() without finish(): message discarded"); }
}
static int m_begin(lua_State *l) {
	int dest = (int)luaL_checkinteger(l, 1), type = resolve_msg(l, 2);
	float org[3] = {0, 0, 0}; bool has = lua_isnumber(l, 3);
	if (has) for (int i = 0; i < 3; i++) org[i] = (float)luaL_checknumber(l, 3 + i);
	int eidx = (int)luaL_optinteger(l, 6, 0);
	edict_t *e = eidx ? ent_edict(eidx) : nullptr;
	if (eidx && !e) return luaL_error(l, "invalid target entity");
	if (S.active) xerr("xmod.msg.begin() called twice without finish(): previous message discarded");
	S.active = true; S.dest = dest; S.type = type; S.hasorg = has; memcpy(S.org, org, sizeof org); S.ed = e; S.ops.clear();
	return 0; }
static void add_op(lua_State *l, int kind, double num, const char *str = nullptr) {
	if (!S.active) luaL_error(l, "xmod.msg write function called without xmod.msg.begin()");
	if (S.ops.size() > 512) luaL_error(l, "message too large");
	S.ops.push_back(MsgOp{kind, num, str ? str : ""});
}
static int m_byte(lua_State *l) { add_op(l, 1, (double)luaL_checkinteger(l, 1)); return 0; }
static int m_char(lua_State *l) { add_op(l, 2, (double)luaL_checkinteger(l, 1)); return 0; }
static int m_short(lua_State *l) { add_op(l, 3, (double)luaL_checkinteger(l, 1)); return 0; }
static int m_long(lua_State *l) { add_op(l, 4, (double)luaL_checkinteger(l, 1)); return 0; }
static int m_angle(lua_State *l) { add_op(l, 5, luaL_checknumber(l, 1)); return 0; }
static int m_coord(lua_State *l) { add_op(l, 6, luaL_checknumber(l, 1)); return 0; }
static int m_string(lua_State *l) { add_op(l, 7, 0, luaL_checkstring(l, 1)); return 0; }
static int m_entity(lua_State *l) { add_op(l, 8, (double)luaL_checkinteger(l, 1)); return 0; }
static int m_finish(lua_State *l) {
	if (!S.active) return luaL_error(l, "xmod.msg.finish() without xmod.msg.begin()");
	S.active = false;
	int dest = S.dest, type = S.type; bool hasorg = S.hasorg; float org[3] = {S.org[0], S.org[1], S.org[2]}; edict_t *ed = S.ed; std::vector<MsgOp> ops; ops.swap(S.ops);
	if (g_pass_open) msg_defer([=]() { emit_message(dest, type, hasorg, org, ed, ops); });
	else emit_message(dest, type, hasorg, org, ed, ops);
	return 0; }

// ---- high level helpers (each is one atomic engine sequence; deferred if a pass-through message is open)
static std::string colorize(const char *in) {
	std::string o;
	for (const char *p = in; *p; ++p) {
		if (p[0] == '{' && p[1] && p[2] == '}') {
			if (p[1] == 'y') { o += '\x01'; p += 2; continue; } if (p[1] == 't') { o += '\x03'; p += 2; continue; }
			if (p[1] == 'g') { o += '\x04'; p += 2; continue; }
		}
		o += *p;
	}
	if (o.size() > 188) o.resize(188);
	return o;
}
template <class F> static void each_target(int id, F f) {
	if (id == 0) { for (int i = 1; i <= g_maxclients; i++) if (g_players[i].connected && g_players[i].putin && !g_players[i].bot) f(INDEXENT(i)); }
	else if (is_valid_player(id) && g_players[id].putin) f(INDEXENT(id));
}
static void run_or_defer(const std::function<void()> &f) { if (g_pass_open) msg_defer(f); else f(); }

static void do_chat(int id, const std::string &t, int sender) {
	static int mid = 0; if (!mid) mid = GET_USER_MSG_ID(PLID, "SayText", nullptr); if (!mid) return;
	each_target(id, [&](edict_t *e) { MESSAGE_BEGIN(MSG_ONE, mid, nullptr, e); WRITE_BYTE(sender); WRITE_STRING("\x01%s1"); WRITE_STRING(t.c_str()); MESSAGE_END(); });
}
void xmod_color_chat(int id, const std::string &text) { std::string t = colorize(text.c_str()); run_or_defer([=]() { do_chat(id, t, 0); }); }
void xmod_client_print(int id, const std::string &kind, const std::string &text) {
	int k = kind == "center" ? 1 : kind == "chat" ? 2 : 0; std::string t = text; if (k != 1) t += "\n";
	each_target(id, [&](edict_t *e) { CLIENT_PRINTF(e, (PRINT_TYPE)k, t.c_str()); });
}
static int m_chat(lua_State *l) {      // chat(id|0, text [, sender_id])   colors: {y}{t}{g}
	int id = (int)luaL_checkinteger(l, 1); std::string t = colorize(luaL_checkstring(l, 2)); int sender = (int)luaL_optinteger(l, 3, 0);
	run_or_defer([=]() { do_chat(id, t, sender); }); return 0; }
static int m_print(lua_State *l) {     // print(id|0, "console"|"center"|"chat", text)   (client print: not a user message, always safe)
	int id = (int)luaL_checkinteger(l, 1); const char *k = luaL_checkstring(l, 2); std::string t = luaL_checkstring(l, 3);
	int kind = !strcmp(k, "center") ? 1 : !strcmp(k, "chat") ? 2 : 0; if (kind != 1) t += "\n";
	each_target(id, [&](edict_t *e) { CLIENT_PRINTF(e, (PRINT_TYPE)kind, t.c_str()); });
	return 0; }
static short fx(double v) { double s = v * 8192.0; return (short)(s < -32768 ? -32768 : s > 32767 ? 32767 : s); }
struct HudOpts { int r, g, b, ch, ef; double x, y, hold, fin, fout; };
static void do_hud(int id, const std::string &t, const HudOpts &o) {
	each_target(id, [&](edict_t *e) {
		MESSAGE_BEGIN(MSG_ONE, SVC_TEMPENTITY, nullptr, e);
		WRITE_BYTE(TE_TEXTMESSAGE); WRITE_BYTE(o.ch & 0xFF); WRITE_SHORT(o.x == -1 ? -8192 : fx(o.x)); WRITE_SHORT(o.y == -1 ? -8192 : fx(o.y));
		WRITE_BYTE(o.ef); WRITE_BYTE(o.r); WRITE_BYTE(o.g); WRITE_BYTE(o.b); WRITE_BYTE(255); WRITE_BYTE(255); WRITE_BYTE(255); WRITE_BYTE(250); WRITE_BYTE(0);
		WRITE_SHORT((int)(o.fin * 256)); WRITE_SHORT((int)(o.fout * 256)); WRITE_SHORT((int)(o.hold * 256));
		if (o.ef == 2) WRITE_SHORT(256);
		WRITE_STRING(t.c_str()); MESSAGE_END(); });
}
static int m_hud(lua_State *l) {       // hud(id|0, text, {r,g,b,x,y,hold,fadein,fadeout,channel,effect})
	int id = (int)luaL_checkinteger(l, 1); std::string t = luaL_checkstring(l, 2); if (t.size() > 480) t.resize(480);
	auto num = [&](const char *k, double d) { double v = d; if (lua_istable(l, 3)) { lua_getfield(l, 3, k); if (lua_isnumber(l, -1)) v = lua_tonumber(l, -1); lua_pop(l, 1); } return v; };
	HudOpts o{(int)num("r", 0), (int)num("g", 255), (int)num("b", 0), (int)num("channel", 1), (int)num("effect", 0), num("x", -1), num("y", 0.35), num("hold", 4), num("fadein", 0.2), num("fadeout", 0.5)};
	run_or_defer([=]() { do_hud(id, t, o); }); return 0; }
static void do_fade(int id, int dur, int hold, int fl, int r, int g, int b, int a) {
	static int mid = 0; if (!mid) mid = GET_USER_MSG_ID(PLID, "ScreenFade", nullptr); if (!mid) return;
	each_target(id, [&](edict_t *e) { MESSAGE_BEGIN(MSG_ONE, mid, nullptr, e); WRITE_SHORT(dur); WRITE_SHORT(hold); WRITE_SHORT(fl);
		WRITE_BYTE(r); WRITE_BYTE(g); WRITE_BYTE(b); WRITE_BYTE(a); MESSAGE_END(); });
}
static int m_fade(lua_State *l) {      // fade(id|0, seconds, hold, flags, r,g,b,a)
	int id = (int)luaL_checkinteger(l, 1);
	int dur = (int)(luaL_checknumber(l, 2) * 4096), hold = (int)(luaL_checknumber(l, 3) * 4096), fl = (int)luaL_optinteger(l, 4, 0);
	int r = (int)luaL_optinteger(l, 5, 0), g = (int)luaL_optinteger(l, 6, 0), b = (int)luaL_optinteger(l, 7, 0), a = (int)luaL_optinteger(l, 8, 255);
	run_or_defer([=]() { do_fade(id, dur, hold, fl, r, g, b, a); }); return 0; }
static void do_shake(int id, int a, int d, int f) {
	static int mid = 0; if (!mid) mid = GET_USER_MSG_ID(PLID, "ScreenShake", nullptr); if (!mid) return;
	each_target(id, [&](edict_t *e) { MESSAGE_BEGIN(MSG_ONE, mid, nullptr, e); WRITE_SHORT(a); WRITE_SHORT(d); WRITE_SHORT(f); MESSAGE_END(); });
}
static int m_shake(lua_State *l) {
	int id = (int)luaL_checkinteger(l, 1);
	int a = (int)(luaL_checknumber(l, 2) * 4096), d = (int)(luaL_checknumber(l, 3) * 4096), f = (int)(luaL_checknumber(l, 4) * 4096);
	run_or_defer([=]() { do_shake(id, a, d, f); }); return 0; }
static void do_show_menu(int id, int keys, int secs, const std::string &t) {
	static int mid = 0; if (!mid) mid = GET_USER_MSG_ID(PLID, "ShowMenu", nullptr); if (!mid) return;
	if (!is_valid_player(id)) return; edict_t *e = INDEXENT(id);
	for (size_t pos = 0; pos < t.size() || pos == 0; pos += 175) {
		std::string chunk = t.substr(pos, 175); bool more = pos + 175 < t.size();
		MESSAGE_BEGIN(MSG_ONE, mid, nullptr, e); WRITE_SHORT(keys); WRITE_CHAR(secs); WRITE_BYTE(more ? 1 : 0); WRITE_STRING(chunk.c_str()); MESSAGE_END();
		if (t.empty()) break;
	}
}
static int m_show_menu(lua_State *l) {  // show_menu(id, keys_bitmask, seconds(-1 = forever), text)
	int id = (int)luaL_checkinteger(l, 1), keys = (int)luaL_checkinteger(l, 2), secs = (int)luaL_checkinteger(l, 3);
	std::string t = luaL_checkstring(l, 4);
	run_or_defer([=]() { do_show_menu(id, keys, secs, t); }); return 0; }
static int m_block(lua_State *l) {
	const char *n = luaL_checkstring(l, 1);
	if (lua_toboolean(l, 2)) g_blocked.insert(n); else g_blocked.erase(n);
	return 0; }

void api_register_msg(lua_State *l) {
	lua_getglobal(l, "xmod"); lua_newtable(l);
	static const luaL_Reg fns[] = { {"begin", m_begin}, {"byte", m_byte}, {"char", m_char}, {"short", m_short}, {"long", m_long},
		{"angle", m_angle}, {"coord", m_coord}, {"string", m_string}, {"entity", m_entity}, {"finish", m_finish},
		{"chat", m_chat}, {"print", m_print}, {"hud", m_hud}, {"fade", m_fade}, {"shake", m_shake}, {"show_menu", m_show_menu},
		{"block", m_block}, {nullptr, nullptr} };
	luaL_setfuncs(l, fns, 0);
	struct K { const char *n; int v; };
	static const K ks[] = { {"BROADCAST", MSG_BROADCAST}, {"ONE", MSG_ONE}, {"ALL", MSG_ALL}, {"INIT", MSG_INIT}, {"PVS", MSG_PVS}, {"PAS", MSG_PAS},
		{"ONE_UNRELIABLE", MSG_ONE_UNRELIABLE}, {"TEMPENTITY", SVC_TEMPENTITY}, {nullptr, 0} };
	for (const K *k = ks; k->n; ++k) { lua_pushinteger(l, k->v); lua_setfield(l, -2, k->n); }
	lua_setfield(l, -2, "msg"); lua_pop(l, 1);
}
