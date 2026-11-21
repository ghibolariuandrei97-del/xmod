-- Codex self test (runs inside xmod's mock engine with libcxtarget.so loaded)
xmod.register("codex_selftest", "1", "test")
local C = xmod.codex
local pass, fail = 0, 0
local function ok(c, what) if c then pass = pass + 1 else fail = fail + 1; xmod.log("CXTEST FAIL: " .. what .. "  (last_error: " .. tostring(C.last_error()) .. ")") end end
local function near(a, b) return type(a) == "number" and math.abs(a - b) < 1e-4 end
local M = "libcxtarget"
local function def(t) t.module = t.module or M; local r = C.func(t); ok(r == true, "define " .. t.name .. " -> " .. tostring(C.last_error())); return r end

local hook_log = {}

xmod.timer.after(0.5, function()
  C.cache_clear()          -- every run tests the real resolution paths (the cache has its own phase-2 test)
  local st = C.status()
  ok(st.sync_hooks == true, "xmod 0.4.1 sync hooks available")
  ok(C.module(M) ~= nil, "module found")
  ok(C.module("definitely_not_loaded") == nil, "missing module -> nil")

  -- definitions in every convention
  def{ name = "add", symbol = "cx_add", args = {"int", "int"}, ret = "int" }
  def{ name = "add_calls", symbol = "cx_add_calls", ret = "int" }
  def{ name = "std", symbol = "cx_std", conv = "stdcall", args = {"int", "int", "int"}, ret = "int" }
  def{ name = "fast", symbol = "cx_fast", conv = "fastcall", args = {"int", "int", "int"}, ret = "int" }
  def{ name = "fmul", symbol = "cx_fmul", args = {"float", "float"}, ret = "float" }
  def{ name = "dadd", symbol = "cx_dadd", args = {"double", "double"}, ret = "double" }
  def{ name = "i64", symbol = "cx_i64", args = {"i64"}, ret = "i64" }
  def{ name = "greet", symbol = "cx_greet", args = {"string"}, ret = "string" }
  def{ name = "vlen", symbol = "cx_vlen", args = {"vector"}, ret = "float" }
  def{ name = "crash", symbol = "cx_crash", args = {"ptr"}, ret = "int" }
  def{ name = "divz", symbol = "cx_divz", args = {"int", "int"}, ret = "int" }
  def{ name = "global_inc", symbol = "cx_global_inc", args = {"int"}, ret = "int" }
  def{ name = "ret5", symbol = "cx_ret5", ret = "int" }
  for _, n in ipairs({"call_add:int,int:int", "call_std_many:int:int", "call_fast:int,int,int:int", "call_fmul:float,float:float",
                      "call_dadd:double,double:double", "call_i64:i64:i64", "call_greet:string:string", "call_global_inc:int:int",
                      "call_ret5::int", "new_animal::ptr", "animal_speak:ptr,int:int", "animal_weight:ptr,float:float"}) do
    local name, a, r = n:match("^([%w_]+):([%w,]*):(%w+)$")
    local args = {}
    for t in a:gmatch("[%w]+") do args[#args + 1] = t end
    def{ name = name, symbol = "cx_" .. name, args = args, ret = r }
  end
  def{ name = "speak", conv = "thiscall", this = "ptr", vindex = 0, args = {"int"}, ret = "int" }
  def{ name = "weight", conv = "thiscall", this = "ptr", vindex = 1, args = {"float"}, ret = "float" }
  def{ name = "speak_direct", conv = "thiscall", this = "ptr", symbol = "Animal::speak", args = {"int"}, ret = "int" }

  -- plain calls
  ok(C.call("add", 2, 3) == 5, "cdecl add")
  ok(C.call("std", 1, 2, 3) == 123, "stdcall")
  ok(C.call("fast", 9, 3, 2) == 868, "fastcall (ecx, edx, stack)")
  ok(near(C.call("fmul", 1.5, 4), 6), "float return")
  ok(near(C.call("dadd", 1.25, 2.5), 3.75), "double args/return")
  ok(C.call("i64", 4000000000) == 12000000000, "int64 arg/return")
  ok(C.call("greet", "andrew") == "hello andrew", "string arg/return")
  ok(near(C.call("vlen", {x = 3, y = 4, z = 0}), 5), "vector pointer arg")
  local obj = C.call("new_animal")
  ok(type(obj) == "number" and obj > 0x10000, "object pointer")
  ok(C.call("speak", obj, 2) == 42, "virtual call by vtable index")
  ok(C.call("speak_direct", obj, 3) == 43, "method by demangled symbol (this first)")
  ok(near(C.call("weight", obj, 2.5), 10), "virtual float method")
  ok(not pcall(C.call, "add", 1), "wrong argument count is an error, not a crash")

  -- the fault guard
  local okc, err = pcall(C.call, "crash", 0)
  ok(not okc and tostring(err):find("crashed"), "NULL dereference is caught: " .. tostring(err))
  ok(C.info("crash").faulted == true, "function marked faulted")
  ok(not pcall(C.call, "crash", 0), "faulted function refused")
  C.reset("crash")
  local mem = C.alloc(16); C.write(mem, "i32", 41)
  ok(C.call("crash", mem) == 42, "after reset it works with valid memory")
  okc, err = pcall(C.call, "divz", 1, 0)
  ok(not okc and tostring(err):find("crashed"), "divide by zero is caught: " .. tostring(err))
  ok(C.call("add", 20, 22) == 42, "server state is fine after two caught crashes")

  -- memory
  ok(C.readable(0) == false and C.read(16, "i32") == nil, "reading NULL is refused")
  ok(C.write(mem, "f32", 2.5) and near(C.read(mem, "f32"), 2.5), "write/read float")
  C.write(mem, "str", "codex")
  ok(C.read(mem, "str") == "codex", "write/read string")
  ok(C.read(mem, "bytes", 3) == "63 6F 64", "read bytes")
  ok(C.free(mem) == true, "free")

  -- signatures
  local sm = C.sig_make("add")
  ok(sm.unique and C.find(M, sm.sig) == C.addr("add"), "sig_make + find roundtrip: " .. tostring(sm.sig))
  local sm2 = C.sig_make("global_inc")
  ok(sm2.unique and C.find(M, sm2.sig) == C.addr("global_inc"), "PIC function signature (GOT bytes wildcarded): " .. tostring(sm2.sig))
  local sm_esc = sm.sig:gsub("%?%?", "2A"):gsub("(%x%x)", "\\x%1"):gsub(" ", "")
  ok(C.find(M, sm_esc) == C.addr("add"), "\\x style signature (Orpheu/SourceMod): " .. sm_esc)
  ok(C.find(M, "CC CC CC CC") == nil, "ambiguous signature -> nil")
  def{ name = "add_by_sig", sig = sm.sig, args = {"int", "int"}, ret = "int" }
  ok(C.call("add_by_sig", 4, 5) == 9 and C.info("add_by_sig").how == "signature", "define by signature")
  local xr = C.xref(M, "CODEX_MAGIC_STRING_123")
  ok(#xr == 1 and xr[1].func == C.addr("call_add") or #xr >= 1, "xref found something")
  ok(#xr >= 1 and C.symbol(M, "cx_uses_string") == xr[1].func, "xref gives the function that uses the string")
  def{ name = "uses_string", xref = "CODEX_MAGIC_STRING_123", args = {"int"}, ret = "int" }
  ok(C.call("uses_string", 1) == 23, "define by string reference")

  -- hooks: pre changes an argument (xmod.hook.set_arg), post overrides the return
  ok(C.hook("add"), "hook add")
  xmod.on("codex:add", function(a, b) hook_log[#hook_log + 1] = "pre " .. a .. " " .. b; xmod.hook.set_arg(1, a * 10) end)
  ok(C.call("call_add", 2, 3) == 23, "pre-hook changed arg 1 through the native caller")
  ok(C.call("add", 2, 3) == 5, "codex.call bypasses hooks")
  ok(C.callh("add", 2, 3) == 23, "codex.callh goes through hooks")
  local post_id = xmod.on("codex:add:post", function(a, b, r) if r == 777 then return end; if a == 1000 then return xmod.HC_SUPERCEDE, 99 end end)
  ok(C.call("call_add", 100, 0) == 99, "post-hook overrides the return (a was changed to 1000 by the pre-hook)")

  -- supercede: original must not run
  local before = C.call("add_calls")
  xmod.on("codex:add", function(a) if a == 7 then return xmod.HC_SUPERCEDE, 7 end end)   -- every pre-handler sees the ORIGINAL args (like Ham)
  ok(C.call("call_add", 7, 0) == 7, "supercede returns the value")
  ok(C.call("add_calls") == before, "supercede skipped the original")

  -- re-entrancy: a handler calling its own function
  def{ name = "ret5x", symbol = "cx_ret5", ret = "int", replace = true }
  ok(C.hook("ret5"), "hook ret5")
  xmod.on("codex:ret5", function() local v = C.call("ret5"); return xmod.HC_SUPERCEDE, v + 1 end)
  ok(C.call("call_ret5") == 6, "handler calling its own function does not recurse")

  -- stdcall hook: callee pops arguments; 1000 calls keep the stack balanced
  ok(C.hook("std"), "hook stdcall")
  xmod.on("codex:std", function(a, b, c) xmod.hook.set_arg(3, 9) end)
  ok(C.call("call_std_many", 1000) == 129000, "stdcall hook, 1000 calls, stack intact")

  -- fastcall hook (ecx/edx)
  ok(C.hook("fast"), "hook fastcall")
  xmod.on("codex:fast", function(a, b, c) ok(a == 9 and b == 3 and c == 2, "fastcall args seen in pre-hook"); xmod.hook.set_arg(2, 0) end)
  ok(C.call("call_fast", 9, 3, 2) == 898, "fastcall hook changes the ecx/edx argument")

  -- float / double / int64 / string hooks
  ok(C.hook("fmul") and C.hook("dadd") and C.hook("i64") and C.hook("greet"), "hook float/double/i64/string functions")
  xmod.on("codex:fmul:post", function(a, b, r) return xmod.HC_SUPERCEDE, r + 0.5 end)
  ok(near(C.call("call_fmul", 2, 3), 6.5), "float return through st(0) after a post-hook")
  xmod.on("codex:dadd", function(a, b) xmod.hook.set_arg(2, 10.25) end)
  ok(near(C.call("call_dadd", 1, 2), 11.25), "double arg changed in pre-hook")
  xmod.on("codex:i64:post", function(a, r) codex_i64_seen = r; C.set_ret(r + 1) end)
  ok(C.call("call_i64", 5000000000) == 15000000001, "int64 return changed with codex.set_ret")
  xmod.on("codex:greet", function(n) C.set_arg(1, "world") end)
  xmod.on("codex:greet:post", function(n, r) if r == "hello world" then C.set_ret("bye world") end end)
  ok(C.call("call_greet", "x") == "bye world", "string arg + string return replaced")

  -- PIC prologue (get_pc_thunk is relocated)
  ok(C.hook("global_inc"), "hook a function that starts with call __x86.get_pc_thunk")
  local g1 = C.call("call_global_inc", 5)
  local g2 = C.call("call_global_inc", 5)
  ok(g2 - g1 == 5, "PIC function still reads/writes its globals correctly after the detour")

  -- vtable hook (by object pointer)
  ok(C.hook("speak", { object = obj }), "vtable hook")
  xmod.on("codex:speak", function(this, x) ok(this == obj, "this = object pointer"); xmod.hook.set_arg(1, x + 100) end)
  ok(C.call("animal_speak", obj, 1) == 141, "virtual call from native code goes through the vtable hook")
  ok(C.call("speak", obj, 1) == 41, "codex.call on a hooked virtual bypasses the hook")
  ok(C.hook("weight", { object = obj }), "vtable hook float method")
  xmod.on("codex:weight:post", function(this, k, r) return xmod.HC_SUPERCEDE, r * 2 end)
  ok(near(C.call("animal_weight", obj, 1), 8), "virtual float return doubled")

  -- unhook restores the bytes
  local orig = C.read(C.addr("ret5"), "bytes", 5)
  ok(C.unhook("ret5") and C.call("call_ret5") == 5, "unhook ret5")
  ok(C.unhook("add") and C.call("call_add", 2, 3) == 5, "unhook add -> original behaviour")

  -- patches
  local a5 = C.addr("ret5")
  local bytes = C.read(a5, "bytes", 6)
  ok(C.patch(a5, "B8 07 00 00 00 C3", "FF FF") == nil, "patch with wrong expected bytes is refused")
  local pid = C.patch(a5, "B8 07 00 00 00 C3")
  ok(pid and C.call("ret5") == 7, "patch code")
  ok(C.patch(a5 + 1, "90") == nil, "overlapping patch refused")
  ok(C.unpatch(pid) and C.call("ret5") == 5 and C.read(a5, "bytes", 6) == bytes, "unpatch restores the exact bytes")


  -- hard-coded limits: find every use of a constant in a function and change it (all branches, reversible)
  def{ name = "add_money", symbol = "cx_add_money", args = {"int", "int"}, ret = "int" }
  def{ name = "call_add_money", symbol = "cx_call_add_money", args = {"int", "int"}, ret = "int" }
  ok(C.call("call_add_money", 15000, 5000) == 16000 and C.call("call_add_money", 20000, 1) == 16000, "money capped at 16000 before")
  local exact = C.consts("add_money", 16000)
  local near = C.consts("add_money", 16000, { near = true })
  ok(#exact == 2 and #near == 3, "consts finds the limit on every path (exact " .. #exact .. ", near " .. #near .. ")")
  ok(C.set_const({ addr = exact[1].addr, size = 1 }, 1000) == nil and C.last_error():find("does not fit"), "a value that does not fit the field is refused")
  local ids = C.set_const(near, 999999)
  ok(ids and #ids == 3, "set_const patches all sites")
  ok(C.call("call_add_money", 15000, 5000) == 20000 and C.call("call_add_money", 20000, 1) == 20001 and C.call("call_add_money", 999999, 5) == 999999, "limit raised to 999999 on every path")
  ok(#C.consts("add_money", 16000) == 2, "consts sees the shipped bytes even while patched")
  for _, id in ipairs(ids) do C.unpatch(id) end
  ok(C.call("call_add_money", 15000, 5000) == 16000, "unpatch -> original limit")
  local ids2 = C.set_const("add_money", 16000, 50000)
  ok(ids2 and #ids2 == 2, "set_const(func, old, new) short form")
  for _, id in ipairs(ids2 or {}) do C.unpatch(id) end

  -- a hooked function is still found by signature (another plugin) and gets its real signature
  ok(C.hook("add_money"), "hook add_money")
  local hs = C.sig_make("add_money")
  ok(hs.unique and not hs.sig:find("^E9") and C.find(M, hs.sig) == C.addr("add_money"), "sig_make / find see the original bytes of a hooked function: " .. tostring(hs.sig))
  def{ name = "add_money2", sig = hs.sig, args = {"int", "int"}, ret = "int" }
  ok(C.addr("add_money2") == C.addr("add_money") and C.info("add_money2").how == "signature", "second definition by signature while hooked")
  ok(C.unhook("add_money") and C.call("call_add_money", 15000, 5000) == 16000, "unhook add_money")

  -- stack overflow (endless recursion) is caught too, twice in a row (the guard re-arms)
  def{ name = "recurse", symbol = "cx_recurse", args = {"int"}, ret = "int" }
  local ok1, e1 = pcall(C.call, "recurse", 1)
  ok(not ok1 and tostring(e1):find("crashed"), "stack overflow caught: " .. tostring(e1))
  C.reset("recurse")
  local ok2 = pcall(C.call, "recurse", 1)
  ok(not ok2, "second stack overflow caught as well")
  ok(C.call("add", 2, 3) == 5, "fine after two stack overflows")

  -- lib/codex.lua
  do
  local cx = require("codex")
  ok(cx.available() and (cx.platform() == "linux" or cx.platform() == "windows"), "lib: available / platform")
  local ran = false
  ok(cx.ready(function(c) ran = (c == C) end) == true and ran, "lib: ready() runs at once when attached")
  local n, failed = cx.define({
    lib_add = { module = M, symbol = "cx_add", args = {"int", "int"}, ret = "int" },
    lib_missing = { module = M, symbol = "no_such_function_here", args = {}, ret = "int" },
    lib_crash = { module = M, symbol = "cx_crash", args = {"ptr"}, ret = "int" },
  }, { quiet = true })
  ok(n == 2 and failed.lib_missing and failed.lib_missing:find("not found"), "lib: define() counts and reports failures: " .. tostring(failed.lib_missing))
  ok(cx.has("lib_add") and not cx.has("lib_missing") and not cx.has("never_defined"), "lib: has()")
  local all, miss = cx.need("lib_add", "lib_missing")
  ok(not all and miss[1] == "lib_missing", "lib: need()")
  local tok, tr = cx.try("lib_add", 20, 22)
  ok(tok and tr == 42 and cx.call("lib_add", 1, 2) == 3, "lib: try / call")
  local cr, ce = cx.call("lib_crash", 0)
  ok(cr == nil and ce:find("crashed"), "lib: call() of a crashing function returns nil, err")
  local hk, he = cx.hook("lib_missing")
  ok(hk == false and he, "lib: hook() of an unresolved function fails cleanly")
  local h, herr = cx.limit("add_money", 16000, 70000, { near = true })
  ok(h and #h.ids == 3 and C.call("call_add_money", 60000, 5000) == 65000, "lib: limit() " .. tostring(herr))
  h:restore()
  ok(not h.active and C.call("call_add_money", 15000, 5000) == 16000, "lib: limit handle restore()")
  local none, nerr = cx.limit("add_money", 12345, 1)
  ok(none == nil and nerr:find("no instruction"), "lib: limit() with no site -> nil, err")
  local obj = C.alloc(256)
  C.write(obj + 7 * 4, "i32", 4242)
  ok(cx.pdata(obj, 2, "i32", 5) == 4242, "lib: pdata() uses the linux diff on Linux")
  cx.set_pdata(obj, 2, 777)
  local F = cx.fields{ money = { index = 2, linux_diff = 5 }, hp = { windows = 8, linux = 12, type = "f32" } }
  F.set(obj, "hp", 1.5)
  ok(F.get(obj, "money") == 777 and C.read(obj + 12, "f32") == 1.5 and F.offset("hp") == 12, "lib: fields()")
  C.free(obj)
  local txt = cx.dump({ "lib_add", "lib_missing" })
  ok(txt:find('lib_add = { module = "libcxtarget", sig = { linux = "', 1, true) and txt:find("lib_missing: not resolved"), "lib: dump() gamedata")
  ok(cx.describe("lib_add"):find("OK") and cx.where(C.addr("lib_add")):find("^libcxtarget.so%+"), "lib: describe / where")
  end

  -- strings the game keeps: astring lives in the engine string pool, string_t is a pool offset
  def{ name = "keep", symbol = "cx_keep", args = {"astring"} }
  def{ name = "kept", symbol = "cx_kept", ret = "string" }
  C.call("keep", "weapon_" .. "ak47")
  C.call("add", 1, 2)                                   -- reuse the stack / temporaries
  ok(C.call("kept") == "weapon_ak47", "astring stays valid after the call (engine string pool)")
  def{ name = "ident_st", symbol = "cx_ident", args = {"string_t"}, ret = "string_t" }
  ok(C.call("ident_st", "info_target") == "info_target", "string_t argument and return")

  -- misc
  ok(C.insn(C.addr("add"), 3)[1].len > 0, "insn decoder")
  ok(C.vindex(obj, C.vfunc(obj, 1)) == 1, "vfunc / vindex")
  ok(C.engfunc("pfnPrecacheModel") ~= nil, "engfunc table lookup")
  local inf = C.info("add")
  ok(inf.resolved and inf.how:find("symbol") and inf.proto == "int add(int, int)", "info: " .. tostring(inf.proto))

  -- call graph (1.1): callees / callers / calltree / analyze / treemaps / resolvers / symbol_at / find_value
  local root = C.symbol(M, "cx_tree_root")
  local names = {}
  for _, c in ipairs(C.callees("cx_tree_root") or {}) do if c.name then names[#names + 1] = c.name end end
  local nm = table.concat(names, ",")
  ok(nm:find("cx_leaf_a") and nm:find("cx_leaf_b") and select(2, nm:gsub("cx_leaf_a", "")) == 2, "callees through the PLT: " .. nm)
  local u = C.callees("cx_tree_root", { unique = true, direct = true })
  local un = {}; for _, c in ipairs(u) do un[#un + 1] = c.name or "?" end
  ok(#u == 3 and table.concat(un, ","):find("strlen"), "callees unique/direct (cx_leaf_a, cx_leaf_b, strlen): " .. table.concat(un, ","))
  local who = {}
  for _, r in ipairs(C.callers("cx_leaf_b") or {}) do who[#who + 1] = r.name or "?" end
  local w = table.concat(who, ",")
  ok(w:find("cx_tree_root") and w:find("cx_tree_other"), "callers through the PLT: " .. w)
  ok(#C.callers("cx_leaf_a", { unique = true }) == 1, "callers unique: one function calls cx_leaf_a twice")
  local tr = C.calltree(root, { depth = 2 })
  ok(tr and tr.name == "cx_tree_root" and #tr.children == 2 and tr.children[1].name == "cx_leaf_a", "calltree down")
  local up = C.calltree("cx_leaf_b", { dir = "up", depth = 1 }); ok(up and #up.children == 2, "calltree up")
  local an = C.analyze("cx_tree_root")
  ok(an and an.strings[1] == "CODEX_TREE_STR" and an.bounded and #an.callees == 2 and an.insns > 5, "analyze: strings / callees / size")
  local found_const = false; for _, k in ipairs(an.consts) do if k == 0x1234 then found_const = true end end; ok(found_const, "analyze: constants")
  local tm = C.treemap("cx_tree_root")
  ok(type(tm) == "string" and tm:sub(1, 4) == "cx1:", "treemap text: " .. tostring(tm))
  local hits = C.treemap_find(M, tm)
  ok(hits and hits[1].addr == root and hits[1].score >= 0.99, "treemap_find finds it again: " .. tostring(hits and hits[1].name))
  ok(C.treemap_find(M, "cx1:2(damaged") == nil and C.last_error():find("damaged"), "treemap_find: bad text -> nil")
  def{ name = "leaf_b_g", callee = { of = "cx_tree_root", index = 2 }, args = {"int"}, ret = "int" }
  ok(C.call("leaf_b_g", 1) == (1 ~ 0x5A5A) and C.info("leaf_b_g").how:find("callee #2"), "resolver: callee #2 of a function")
  def{ name = "root_c", caller = "cx_leaf_a", args = {"int"}, ret = "int" }
  ok(C.addr("root_c") == root, "resolver: the only caller of a function")
  ok(C.func{ name = "amb", module = M, caller = "cx_leaf_b", ret = "int" } == nil and C.last_error():find("2 functions call"), "resolver: ambiguous caller -> reason")
  def{ name = "root_t", treemap = tm, args = {"int"}, ret = "int" }
  ok(C.addr("root_t") == root and C.info("root_t").how:find("treemap"), "resolver: treemap")
  local sa = C.symbol_at(root + 3); ok(sa and sa.name == "cx_tree_root" and sa.offset == 3, "symbol_at")
  local fv = C.find_value(M, "str", "CODEX_TREE_STR"); ok(fv and #fv == 1, "find_value str")
  local before = C.call("root_t", 1)
  local rv = C.replace_value(M, "str", "CODEX_TREE_STR", "CODEX")
  ok(rv and rv.count == 1 and C.call("root_t", 1) == before - 9 * 0x1234, "replace_value str (shorter, NUL padded)")
  for _, id in ipairs(rv and rv.patches or {}) do C.unpatch(id) end
  ok(C.call("root_t", 1) == before, "replace_value undone with unpatch")
  def{ name = "magic", symbol = "cx_read_magic", ret = "int" }
  local mv = C.replace_value(M, "i32", 0x13572468, 777)
  ok(mv and mv.count == 1 and C.call("magic") == 777, "replace_value i32 in data")
  for _, id in ipairs(mv and mv.patches or {}) do C.unpatch(id) end
  ok(C.call("magic") == 0x13572468 and #C.find_value(M, "i32", 0x13572468) == 1, "find_value i32 / restored")
  ok(C.replace_value(M, "i32", 0x7EADBEEF, 1) == nil, "replace_value: missing value -> nil")
  ok(not pcall(C.find_value, M, "nonsense", 1), "find_value: bad type is an error")
  local cx = require("codex")
  local ff = cx.find_func{ module = M, strings = { "CODEX_TREE_STR" } }
  ok(ff and #ff == 1 and ff[1].addr == root, "lib find_func by string")
  local ff2 = cx.find_func{ module = M, calls = { "libcxtarget:cx_leaf_b" } }
  ok(ff2 and #ff2 == 2, "lib find_func: 2 functions call cx_leaf_b")
  local ff3 = cx.find_func{ module = M, calls = { "libcxtarget:cx_leaf_b", "libcxtarget:cx_leaf_a" } }
  ok(ff3 and #ff3 == 1 and ff3[1].name == "cx_tree_root", "lib find_func: calls both")
  local ff4 = cx.find_func{ module = M, called_by = { "libcxtarget:cx_tree_other" } }
  ok(ff4 and #ff4 == 1 and ff4[1].name == "cx_leaf_b", "lib find_func: called_by")
  ok(cx.find_func{ module = M, calls = { "libcxtarget:cx_leaf_b" }, consts = { 0x1234 } }[1].addr == root, "lib find_func: consts filter")
  local n, failed = cx.define({ found_root = { module = M, find = { strings = { "CODEX_TREE_STR" } }, args = { "int" }, ret = "int" } })
  ok(n == 1 and C.addr("found_root") == root and C.call("found_root", 1) == C.call("root_t", 1), "lib define with find = {...}")
  local lines = cx.tree("libcxtarget:cx_tree_root", { depth = 1 })
  ok(#lines == 3 and lines[1]:find("cx_tree_root"), "lib tree lines")
  ok(cx.wl(1, 2) == 2, "lib wl")
  ok(cx.check(nil, { "root_t", "lib_missing" }) == 1, "lib check counts problems")

  xmod.log(string.format("CXTEST DONE: %d passed, %d failed", pass, fail))
end)

-- second phase: the resolution cache (written one frame after the first resolutions)
xmod.timer.after(1.5, function()
  ok(C.func{ name = "add", module = M, symbol = "nope_wrong_symbol", args = {"int", "int"}, ret = "int", replace = true }, "redefine with a wrong symbol: cache still finds it")
  ok(C.info("add").how == "cache" and C.call("add", 1, 1) == 2, "resolved from the cache: " .. tostring(C.info("add").how))
  ok(C.func{ name = "nothing", module = M, sig = "DE AD BE EF 13 37 DE AD", ret = "int" } == nil and C.info("nothing").why ~= nil, "unresolvable function: nil + reason, no crash")
  ok(not pcall(C.call, "nothing"), "calling an unresolved function is a Lua error")
  xmod.log(string.format("CXTEST PHASE2: %d passed, %d failed", pass, fail))
end)
