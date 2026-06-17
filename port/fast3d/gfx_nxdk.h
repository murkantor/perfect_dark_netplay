#ifndef GFX_NXDK_H
#define GFX_NXDK_H

// Native Original Xbox (NV2A) rendering + window-manager backend skeleton.
//
// This is the FALLBACK renderer for the OG Xbox: the fast3d OpenGL backend needs
// GLSL 1.30 + runtime shader compilation, which pbgl/NV2A can't do, so once
// Milestone 2 confirms the GL path is dead this backend draws via pbkit instead.
// It is currently a SKELETON -- every GfxRenderingAPI / GfxWindowManagerAPI member
// is present with sane stub behaviour and TODO(nv2a)/TODO(pbkit) markers; nothing
// actually rasterises yet. See docs/PORT_XBOX_NXDK.md.
//
// NOT wired into video.c by default (the pbgl-GL path is tried first per M2). To
// switch to it once the GL wall is confirmed: in videoInit, under PLATFORM_NXDK,
// set renderingAPI = &gfx_nxdk_api and wmAPI = &gfx_nxdk_wm (or keep gfx_sdl as the
// WM if nxdk-sdl3 handles windowing/input and only the renderer is native).

#include "gfx_rendering_api.h"
#include "gfx_window_manager_api.h"

#ifdef __cplusplus
extern "C" {
#endif

extern struct GfxRenderingAPI gfx_nxdk_api;
extern struct GfxWindowManagerAPI gfx_nxdk_wm;

#ifdef __cplusplus
}
#endif

#endif
