// Small platform layer for the Nexus client (no SDK types here).
#pragma once
#include <string>
namespace nxp {
void *lib_open(const char *path);
std::string lib_error();                 // why the last lib_open failed (Windows error code + text / dlerror)
std::string list_dir(const std::string &dir, const char *prefix);   // file names in dir starting with prefix (case-insensitive), comma separated
bool file_exists(const std::string &path);
void *lib_sym(void *lib, const char *name);
void *gl_sym(const char *name);          // OpenGL 1.1 entry point of the GL library the engine uses (NULL when the engine runs without OpenGL)
void *gl_ext_sym(const char *name);      // newer GL function; needs a current context
bool  gl_context();                      // is a GL context current on this thread?
std::string self_dir();                  // directory of this module, with a trailing separator
void  fatal_box(const char *text);
double now();                            // monotonic seconds
extern void *(*gl_override)(const char *name);   // tests: take GL from somewhere else (OSMesa)
extern double (*clock_override)();               // tests: controllable time
}
