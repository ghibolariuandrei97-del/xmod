// XmodValue: the value tree exchanged between Lua and extensions.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <utility>
#include "../include/xmod_ext.h"

struct XmodValue {
	int type = XV_NIL; int64_t i = 0; double n = 0; bool b = false; std::string s;
	std::vector<XmodValue *> arr; std::vector<std::pair<std::string, XmodValue *>> map;
	~XmodValue() { for (auto *c : arr) delete c; for (auto &p : map) delete p.second; }
};
namespace xv {
inline XmodValue *nil() { return new XmodValue(); }
inline XmodValue *boolean(int b) { auto *v = new XmodValue(); v->type = XV_BOOL; v->b = b != 0; return v; }
inline XmodValue *integer(int64_t i) { auto *v = new XmodValue(); v->type = XV_INT; v->i = i; return v; }
inline XmodValue *number(double n) { auto *v = new XmodValue(); v->type = XV_NUM; v->n = n; return v; }
inline XmodValue *str(const char *s, size_t len) { auto *v = new XmodValue(); v->type = XV_STR; if (s) v->s.assign(s, len); return v; }
inline XmodValue *cstr(const char *s) { return s ? str(s, strlen(s)) : nil(); }
inline XmodValue *array() { auto *v = new XmodValue(); v->type = XV_ARRAY; return v; }
inline XmodValue *map() { auto *v = new XmodValue(); v->type = XV_MAP; return v; }
inline void push(XmodValue *a, XmodValue *v) { if (!a || a->type != XV_ARRAY) { delete v; return; } a->arr.push_back(v); }
inline void set(XmodValue *m, const char *key, XmodValue *v) {
	if (!m || m->type != XV_MAP || !key) { delete v; return; }
	for (auto &p : m->map) if (p.first == key) { delete p.second; p.second = v; return; }
	m->map.emplace_back(key, v);
}
inline void free_(XmodValue *v) { delete v; }
inline int type(const XmodValue *v) { return v ? v->type : XV_NIL; }
inline int64_t get_int(const XmodValue *v, int64_t d) { if (!v) return d; if (v->type == XV_INT) return v->i; if (v->type == XV_NUM && v->n == (double)(int64_t)v->n) return (int64_t)v->n; return d; }
inline double get_num(const XmodValue *v, double d) { if (!v) return d; if (v->type == XV_NUM) return v->n; if (v->type == XV_INT) return (double)v->i; return d; }
inline int get_bool(const XmodValue *v, int d) { if (!v) return d; if (v->type == XV_BOOL) return v->b; return d; }
inline const char *get_str(const XmodValue *v, size_t *len) { if (!v || v->type != XV_STR) { if (len) *len = 0; return nullptr; } if (len) *len = v->s.size(); return v->s.c_str(); }
inline size_t len(const XmodValue *v) { return !v ? 0 : v->type == XV_ARRAY ? v->arr.size() : v->type == XV_MAP ? v->map.size() : 0; }
inline const XmodValue *at(const XmodValue *a, size_t i) { return (a && a->type == XV_ARRAY && i < a->arr.size()) ? a->arr[i] : nullptr; }
inline const XmodValue *get(const XmodValue *m, const char *key) { if (!m || m->type != XV_MAP || !key) return nullptr; for (auto &p : m->map) if (p.first == key) return p.second; return nullptr; }
inline const char *key(const XmodValue *m, size_t i) { return (m && m->type == XV_MAP && i < m->map.size()) ? m->map[i].first.c_str() : nullptr; }
inline const XmodValue *val(const XmodValue *m, size_t i) { return (m && m->type == XV_MAP && i < m->map.size()) ? m->map[i].second : nullptr; }
}
inline void xmod_fill_value_api(XmodApi *a) {
	a->val_nil = xv::nil; a->val_bool = xv::boolean; a->val_int = xv::integer; a->val_num = xv::number; a->val_str = xv::str; a->val_cstr = xv::cstr;
	a->val_array = xv::array; a->val_map = xv::map; a->val_push = xv::push; a->val_set = xv::set; a->val_free = xv::free_;
	a->val_type = xv::type; a->val_get_int = xv::get_int; a->val_get_num = xv::get_num; a->val_get_bool = xv::get_bool; a->val_get_str = xv::get_str;
	a->val_len = xv::len; a->val_at = xv::at; a->val_get = xv::get; a->val_key = xv::key; a->val_val = xv::val;
}
