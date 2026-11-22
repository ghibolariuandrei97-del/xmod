// Pulls in the Half-Life SDK client interface (cl_enginefunc_t, cldll_func_t, cl_entity_t, ref_params_t ...) in one place.
// Only public SDK interfaces are used by Nexus: no engine memory patching, no signature scanning.
#pragma once
#include <cstring>
#include <cstdio>
#include <cstdlib>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif
typedef unsigned char byte;
typedef int qboolean;
typedef float vec_t;
typedef float vec3_t[3];
typedef int (*pfnUserMsgHook)(const char *pszName, int iSize, void *pbuf);
#include "wrect.h"
#include "enums.h"
#include "netadr.h"
#include "cdll_int.h"
#include "cl_entity.h"
#include "triangleapi.h"
#include "event_api.h"
#include "pmtrace.h"
#include "keydefs.h"
#include "usercmd.h"
#include "cvardef.h"
#include "in_buttons.h"
#undef min
#undef max
#ifndef PM_WORLD_ONLY
#define PM_WORLD_ONLY 0x00000008
#endif
