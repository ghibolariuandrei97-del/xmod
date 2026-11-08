#include "xmod.h"
#include <cstring>
#include <unordered_set>

extern bool g_any_msg_hook;

struct MsgArg { bool isstr; double num; std::string str; };
static struct {
	bool active = false, block = false;
	int dest = 0, type = 0, ent = 0;
	Event *ev = nullptr;
	std::string name;
	std::vector<MsgArg> args;
} M;
static std::unordered_set<std::string> g_blocked;
static std::string g_names[256]; static bool g_named[256];

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
void msg_reset() { M = decltype(M)(); g_blocked.clear(); for (int i = 0; i < 256; i++) { g_named[i] = false; g_names[i].clear(); } }

bool msg_begin(int dest, int type, const float *, edict_t *ed) {
	M.active = false; M.block = false;
	if (!g_any_msg_hook && g_blocked.empty()) return false;
	const std::string &n = msg_name(type);
	if (n.empty()) return false;
	if (g_blocked.count(n)) { M.block = true; return true; }
	Event *e = ev_get(("message:" + n).c_str());
	if (!ev_has(e)) return false;
	M.active = true; M.dest = dest; M.type = type; M.ev = e; M.name = n; M.args.clear();
	M.ent = (ed && !FNullEnt(ed)) ? ENTINDEX(ed) : 0;
	return false;
}
bool msg_write(int, double num, const char *str) {
	if (M.block) return true;
	if (M.active) { MsgArg a; a.isstr = str != nullptr; a.num = num; if (str) a.str = str; M.args.push_back(std::move(a)); }
	return false;
}
bool msg_end() {
	if (M.block) { M.block = false; return true; }
	if (!M.active) return false;
	M.active = false;
	auto args = std::move(M.args); M.args.clear();
	EvResult r = ev_fire(M.ev, 4, [&](lua_State *l) {
		lua_pushstring(l, M.name.c_str()); lua_pushinteger(l, M.dest); lua_pushinteger(l, M.ent);
		lua_createtable(l, (int)args.size(), 0);
		for (size_t i = 0; i < args.size(); i++) { if (args[i].isstr) lua_pushstring(l, args[i].str.c_str()); else lua_pushnumber(l, args[i].num); lua_rawseti(l, -2, (lua_Integer)i + 1); }
		return 4; });
	return r.kind == 1;
}

// ------------------------------------------------------------ script-facing writers (talk to engine directly)
static int resolve_msg(lua_State *l, int idx) {
	if (lua_isinteger(l, idx)) return (int)lua_tointeger(l, idx);
	const char *n = luaL_checkstring(l, idx);
	int id = GET_USER_MSG_ID(PLID, n, nullptr);
	if (!id) luaL_error(l, "unknown user message '%s'", n);
	return id;
}
static int m_begin(lua_State *l) {
	int dest = (int)luaL_checkinteger(l, 1), type = resolve_msg(l, 2);
	float org[3]; bool has = lua_isnumber(l, 3);
	if (has) for (int i = 0; i < 3; i++) org[i] = (float)luaL_checknumber(l, 3 + i);
	int eidx = (int)luaL_optinteger(l, 6, 0);
	edict_t *e = eidx ? ent_edict(eidx) : nullptr;
	if (eidx && !e) return luaL_error(l, "invalid target entity");
	MESSAGE_BEGIN(dest, type, has ? org : nullptr, e); return 0; }
static int m_byte(lua_State *l) { WRITE_BYTE((int)luaL_checkinteger(l, 1)); return 0; }
static int m_char(lua_State *l) { WRITE_CHAR((int)luaL_checkinteger(l, 1)); return 0; }
static int m_short(lua_State *l) { WRITE_SHORT((int)luaL_checkinteger(l, 1)); return 0; }
static int m_long(lua_State *l) { WRITE_LONG((int)luaL_checkinteger(l, 1)); return 0; }
static int m_angle(lua_State *l) { WRITE_ANGLE((float)luaL_checknumber(l, 1)); return 0; }
static int m_coord(lua_State *l) { WRITE_COORD((float)luaL_checknumber(l, 1)); return 0; }
static int m_string(lua_State *l) { WRITE_STRING(luaL_checkstring(l, 1)); return 0; }
static int m_entity(lua_State *l) { WRITE_ENTITY((int)luaL_checkinteger(l, 1)); return 0; }
static int m_finish(lua_State *) { MESSAGE_END(); return 0; }

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
static int m_chat(lua_State *l) {      // chat(id|0, text [, sender_id])   colors: {y}{t}{g}
	int id = (int)luaL_checkinteger(l, 1); std::string t = colorize(luaL_checkstring(l, 2)); int sender = (int)luaL_optinteger(l, 3, 0);
	static int mid = 0; if (!mid) mid = GET_USER_MSG_ID(PLID, "SayText", nullptr); if (!mid) return 0;
	each_target(id, [&](edict_t *e) { MESSAGE_BEGIN(MSG_ONE, mid, nullptr, e); WRITE_BYTE(sender); WRITE_STRING("\x01%s1"); WRITE_STRING(t.c_str()); MESSAGE_END(); });
	return 0; }
static int m_print(lua_State *l) {     // print(id|0, "console"|"center"|"chat", text)
	int id = (int)luaL_checkinteger(l, 1); const char *k = luaL_checkstring(l, 2); std::string t = luaL_checkstring(l, 3);
	int kind = !strcmp(k, "center") ? 1 : !strcmp(k, "chat") ? 2 : 0; if (kind != 1) t += "\n";
	each_target(id, [&](edict_t *e) { CLIENT_PRINTF(e, (PRINT_TYPE)kind, t.c_str()); });
	return 0; }
static short fx(double v) { double s = v * 8192.0; return (short)(s < -32768 ? -32768 : s > 32767 ? 32767 : s); }
static int m_hud(lua_State *l) {       // hud(id|0, text, {r,g,b,x,y,hold,fadein,fadeout,channel,effect})
	int id = (int)luaL_checkinteger(l, 1); std::string t = luaL_checkstring(l, 2); if (t.size() > 480) t.resize(480);
	auto num = [&](const char *k, double d) { double v = d; if (lua_istable(l, 3)) { lua_getfield(l, 3, k); if (lua_isnumber(l, -1)) v = lua_tonumber(l, -1); lua_pop(l, 1); } return v; };
	int r = (int)num("r", 0), g = (int)num("g", 255), b = (int)num("b", 0), ch = (int)num("channel", 1), ef = (int)num("effect", 0);
	double x = num("x", -1), y = num("y", 0.35), hold = num("hold", 4), fin = num("fadein", 0.2), fout = num("fadeout", 0.5);
	each_target(id, [&](edict_t *e) {
		MESSAGE_BEGIN(MSG_ONE, SVC_TEMPENTITY, nullptr, e);
		WRITE_BYTE(TE_TEXTMESSAGE); WRITE_BYTE(ch & 0xFF); WRITE_SHORT(x == -1 ? -8192 : fx(x)); WRITE_SHORT(y == -1 ? -8192 : fx(y));
		WRITE_BYTE(ef); WRITE_BYTE(r); WRITE_BYTE(g); WRITE_BYTE(b); WRITE_BYTE(255); WRITE_BYTE(255); WRITE_BYTE(255); WRITE_BYTE(250); WRITE_BYTE(0);
		WRITE_SHORT((int)(fin * 256)); WRITE_SHORT((int)(fout * 256)); WRITE_SHORT((int)(hold * 256));
		if (ef == 2) WRITE_SHORT(256);
		WRITE_STRING(t.c_str()); MESSAGE_END(); });
	return 0; }
static int m_fade(lua_State *l) {      // fade(id|0, seconds, hold, flags, r,g,b,a)
	int id = (int)luaL_checkinteger(l, 1); static int mid = 0; if (!mid) mid = GET_USER_MSG_ID(PLID, "ScreenFade", nullptr);
	int dur = (int)(luaL_checknumber(l, 2) * 4096), hold = (int)(luaL_checknumber(l, 3) * 4096), fl = (int)luaL_optinteger(l, 4, 0);
	int r = (int)luaL_optinteger(l, 5, 0), g = (int)luaL_optinteger(l, 6, 0), b = (int)luaL_optinteger(l, 7, 0), a = (int)luaL_optinteger(l, 8, 255);
	each_target(id, [&](edict_t *e) { MESSAGE_BEGIN(MSG_ONE, mid, nullptr, e); WRITE_SHORT(dur); WRITE_SHORT(hold); WRITE_SHORT(fl);
		WRITE_BYTE(r); WRITE_BYTE(g); WRITE_BYTE(b); WRITE_BYTE(a); MESSAGE_END(); });
	return 0; }
static int m_shake(lua_State *l) {
	int id = (int)luaL_checkinteger(l, 1); static int mid = 0; if (!mid) mid = GET_USER_MSG_ID(PLID, "ScreenShake", nullptr);
	int a = (int)(luaL_checknumber(l, 2) * 4096), d = (int)(luaL_checknumber(l, 3) * 4096), f = (int)(luaL_checknumber(l, 4) * 4096);
	each_target(id, [&](edict_t *e) { MESSAGE_BEGIN(MSG_ONE, mid, nullptr, e); WRITE_SHORT(a); WRITE_SHORT(d); WRITE_SHORT(f); MESSAGE_END(); });
	return 0; }
static int m_show_menu(lua_State *l) {  // show_menu(id, keys_bitmask, seconds(-1 = forever), text)
	int id = (int)luaL_checkinteger(l, 1), keys = (int)luaL_checkinteger(l, 2), secs = (int)luaL_checkinteger(l, 3);
	std::string t = luaL_checkstring(l, 4); static int mid = 0; if (!mid) mid = GET_USER_MSG_ID(PLID, "ShowMenu", nullptr); if (!mid) return 0;
	if (!is_valid_player(id)) return 0; edict_t *e = INDEXENT(id);
	for (size_t pos = 0; pos < t.size() || pos == 0; pos += 175) {
		std::string chunk = t.substr(pos, 175); bool more = pos + 175 < t.size();
		MESSAGE_BEGIN(MSG_ONE, mid, nullptr, e); WRITE_SHORT(keys); WRITE_CHAR(secs); WRITE_BYTE(more ? 1 : 0); WRITE_STRING(chunk.c_str()); MESSAGE_END();
		if (t.empty()) break;
	}
	return 0; }
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
