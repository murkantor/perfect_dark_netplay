#ifndef GFX_OPENGL_H
#define GFX_OPENGL_H

#include "gfx_rendering_api.h"

// extern "C" so the vtable keeps an unmangled symbol under the MSVC C++ ABI
// (clang i386-pc-win32 / NXDK), where C++ global variables are otherwise mangled
// and unreachable from the C callers (video.c). No-op on the Itanium ABI.
#ifdef __cplusplus
extern "C" {
#endif

extern struct GfxRenderingAPI gfx_opengl_api;

#ifdef __cplusplus
}
#endif

#endif
