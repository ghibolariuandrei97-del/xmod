#include "nx_platform.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#endif
namespace nxp {
void *(*gl_override)(const char *name) = nullptr;
double (*clock_override)() = nullptr;
double now() {
	if (clock_override) return clock_override();
	static const auto t0 = std::chrono::steady_clock::now();
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}
#ifdef _WIN32
void *lib_open(const char *path) { return (void *)LoadLibraryA(path); }      // exactly like the engine loads a client library
std::string lib_error() {
	DWORD e = GetLastError(); char msg[512] = {0};
	FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e, 0, msg, sizeof msg - 1, nullptr);
	for (char *p = msg; *p; p++) if (*p == '\r' || *p == '\n') *p = ' ';
	char b[600]; snprintf(b, sizeof b, "Windows error %lu: %s", (unsigned long)e, msg); return b;
}
std::string list_dir(const std::string &dir, const char *prefix) {
	std::string out; WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA((dir + "*").c_str(), &fd); if (h == INVALID_HANDLE_VALUE) return "(cannot list the folder)";
	do { if (!_strnicmp(fd.cFileName, prefix, strlen(prefix))) { if (!out.empty()) out += ", "; out += fd.cFileName; } } while (FindNextFileA(h, &fd));
	FindClose(h); return out.empty() ? "(none)" : out;
}
bool file_exists(const std::string &path) { DWORD a = GetFileAttributesA(path.c_str()); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }
void *lib_sym(void *lib, const char *name) { return lib ? (void *)GetProcAddress((HMODULE)lib, name) : nullptr; }
static HMODULE gl_lib() { return GetModuleHandleA("opengl32.dll"); }      // only if the engine already loaded it: Nexus never loads a GL library itself
void *gl_sym(const char *name) { if (gl_override) return gl_override(name); HMODULE h = gl_lib(); return h ? (void *)GetProcAddress(h, name) : nullptr; }
void *gl_ext_sym(const char *name) {
	if (gl_override) return gl_override(name);
	typedef void *(__stdcall *getproc_t)(const char *); getproc_t gp = (getproc_t)gl_sym("wglGetProcAddress");
	void *p = gp ? gp(name) : nullptr; return ((size_t)p > 3 && p != (void *)-1) ? p : nullptr;
}
bool gl_context() { if (gl_override) return true; typedef void *(__stdcall *cur_t)(void); cur_t c = (cur_t)gl_sym("wglGetCurrentContext"); return c && c() != nullptr; }
std::string self_dir() {
	HMODULE h = nullptr; char buf[1024] = {0};
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&self_dir, &h);
	GetModuleFileNameA(h, buf, sizeof buf - 1); std::string s = buf; size_t p = s.find_last_of("\\/"); return p == std::string::npos ? "" : s.substr(0, p + 1);
}
void fatal_box(const char *text) { MessageBoxA(nullptr, text, "Nexus", MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND); }
#else
void *lib_open(const char *path) { return dlopen(path, RTLD_NOW | RTLD_LOCAL); }
std::string lib_error() { const char *e = dlerror(); return e ? e : "unknown error"; }
std::string list_dir(const std::string &dir, const char *prefix) {
	std::string out; DIR *d = opendir(dir.c_str()); if (!d) return "(cannot list the folder)";
	while (dirent *e = readdir(d)) if (!strncasecmp(e->d_name, prefix, strlen(prefix))) { if (!out.empty()) out += ", "; out += e->d_name; }
	closedir(d); return out.empty() ? "(none)" : out;
}
bool file_exists(const std::string &path) { struct stat st; return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode); }
void *lib_sym(void *lib, const char *name) { return lib ? dlsym(lib, name) : nullptr; }
static void *gl_lib() { static void *h = nullptr; static bool tried = false; if (!tried) { tried = true; h = dlopen("libGL.so.1", RTLD_NOW | RTLD_NOLOAD); } return h; }
void *gl_sym(const char *name) { if (gl_override) return gl_override(name); void *h = gl_lib(); void *p = h ? dlsym(h, name) : nullptr; return p ? p : dlsym(RTLD_DEFAULT, name); }
void *gl_ext_sym(const char *name) { return gl_sym(name); }
bool gl_context() { if (gl_override) return true; typedef void *(*cur_t)(void); cur_t c = (cur_t)gl_sym("glXGetCurrentContext"); return c ? c() != nullptr : gl_sym("glBegin") != nullptr; }
std::string self_dir() { Dl_info di; if (!dladdr((void *)&self_dir, &di) || !di.dli_fname) return ""; std::string s = di.dli_fname; size_t p = s.find_last_of('/'); return p == std::string::npos ? "" : s.substr(0, p + 1); }
void fatal_box(const char *text) { fprintf(stderr, "Nexus: %s\n", text); }
#endif
}
