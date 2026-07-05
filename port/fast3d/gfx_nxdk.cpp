// Native Original Xbox (NV2A) rendering + window-manager backend -- SKELETON.
//
// The fast3d OpenGL backend needs GLSL 1.30 + runtime shader compilation, which
// pbgl/NV2A can't do. Once Milestone 2 confirms the GL path is dead, this backend
// draws via pbkit instead. Right now it's a fillable skeleton: every vtable member
// is present with sane stub behaviour, the combiner decode (gfx_cc) is wired so the
// engine builds correct vertex buffers, and the NV2A/pbkit work is marked
// TODO(nv2a) / TODO(pbkit). Nothing rasterises yet. See docs/PORT_XBOX_NXDK.md.
//
// The whole file is #ifdef PLATFORM_NXDK, so it is an empty translation unit on
// every other platform (the recursive port/*.cpp glob compiles it everywhere).
//
// Deliberately depends only on the fast3d API headers + gfx_cc + the C++ stdlib --
// NOT pbkit yet -- so the skeleton compiles on NXDK before any NV2A code is written.
// Pull in <pbkit/pbkit.h> (and friends) when you start filling the TODO(pbkit) bodies.

#include "../../src/include/platform.h"

#ifdef PLATFORM_NXDK

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h> // malloc/free for the texture swizzle staging buffer
#include <string.h>
#include <time.h>

#include <pbkit/pbkit.h>
#include <hal/video.h>
#include <hal/debug.h>
#include <xboxkrnl/xboxkrnl.h> // MmAllocateContiguousMemory for GPU-visible vertex data
#include <xgu/xgu.h>           // NV2A helper: transform/combiner/state pushes
#include <xgu/xgux.h>          // NV2A helper: vertex attribute arrays + draw
#include "../include/xboxtrace.h"

#include "gfx_rendering_api.h"
#include "gfx_window_manager_api.h"
#include "gfx_cc.h"
#include "gfx_nxdk.h"

// Capped render-path trace (-> E:\pdboot.log) to localise a first-frame crash. Remove
// once the renderer is solid.
#define NXDK_RTRACE(...) do { static int _n = 0; if (_n < 16) { _n++; xboxTracef(__VA_ARGS__); } } while (0)

// Each backend defines its own file-local ShaderProgram (same pattern as
// gfx_opengl.cpp / gfx_sdlgpu.cpp). For NV2A this carries the decoded N64 combiner,
// which a filled-in load_shader/draw_triangles maps to NV2A register combiners.
struct ShaderProgram {
    uint64_t shader_id0;
    uint32_t shader_id1;
    struct CCFeatures cc; // decoded combiner -> drives the future NV2A combiner setup
    uint8_t num_inputs;
    bool used_textures[2];
    bool used;
};

#define NXDK_MAX_SHADERS 128

static struct {
    ShaderProgram shaders[NXDK_MAX_SHADERS];
    int shader_count;
    ShaderProgram *cur_shader;

    uint32_t next_texture_id; // 1-based; 0 is "none"
    int cur_tile;             // bound tile for select/upload

    int next_framebuffer_id;  // 0 is the default (on-screen) target

    enum FilteringMode tex_filter;

    double time0;             // get_time() origin
    int target_fps;
    uint32_t width, height;

    // --- Phase 1 geometry state ---
    int vp_x, vp_y, vp_w, vp_h;       // current viewport (from set_viewport)
    bool depth_test, depth_mask;      // from set_depth_mode
    bool use_alpha;                   // from set_use_alpha (blend enable)

    // GPU-visible (physically contiguous) vertex ARENA, DOUBLE-BUFFERED. xgux_draw_arrays
    // QUEUES a draw referencing this buffer's physical address; the GPU executes it later.
    // Within a frame, each draw bump-allocates its own region so later draws don't clobber
    // earlier queued ones. Across frames we ALTERNATE buffers: the GPU may still be reading
    // frame N's vertices when the CPU starts writing frame N+1, so frame N+1 must use the
    // OTHER buffer (single-buffered, that race showed as flickering/corrupt verts even with
    // the sfence). Layout [x,y,z,w, r,g,b,a, u,v] = NXDK_VTX_FLOATS/vertex.
    float *vtx[2];
    size_t vtx_caps;                  // per-buffer capacity in vertices
    size_t vtx_off;                   // current bump offset in vertices (reset per frame)
    int vtx_frame;                    // which buffer this frame writes to (0/1, toggled per frame)

    uint32_t tex_bound[2];            // bound texture id per tile (0 = none)
    int combiner_textured;            // current combiner mode (-1 unset / 0 unlit / 1 textured / 2 const-lerp)
    uint32_t combiner_f0, combiner_f1; // constant colours pushed for mode 2 (valid only while mode == 2)
    uint32_t tex_applied;             // texture id last programmed into stage 0 (per-frame cache)
    bool tex_is_fb;                   // tile 0 is bound to a framebuffer (effect) we don't have
} g;

#define NXDK_VTX_FLOATS 10 // x,y,z,w, r,g,b,a, u,v

// ---------------------------------------------------------------------------------
// Texture pool. fast3d hands RGBA8 (R,G,B,A bytes); we convert to NV2A A8R8G8B8
// (0xAARRGGBB) in a GPU-visible contiguous buffer per texture, keyed by 1-based id.
// ---------------------------------------------------------------------------------
struct NxdkTexture {
    uint8_t *argb;        // swizzled A8R8G8B8 (POT container), NULL = unallocated
    uint32_t w, h;        // actual texel dimensions
    uint32_t dw, dh;      // POT container dimensions (swizzle needs power-of-two)
    uint32_t pitch;       // dw * 4
    float us, vs;         // UV scale = w/dw, h/dh (maps fast3d's [0,1] into the POT content)
    bool linear_filter;
    uint32_t cms, cmt;    // wrap modes (G_TX_*); mapped to NV2A in nxdk_apply_texture
};

#define NXDK_MAX_TEXTURES 8192
static struct NxdkTexture g_NxdkTex[NXDK_MAX_TEXTURES]; // [0] unused

// GPU-visible memory must be WRITE-COMBINED (uncached) or the NV2A reads stale/zero
// data through the CPU cache -- plain MmAllocateContiguousMemory (cached) made every
// vertex degenerate to the origin (invisible geometry). Matches nxdk-sdl3's
// MmAllocateContiguousMemoryEx(..., PAGE_WRITECOMBINE | PAGE_READWRITE).
static void *nxdk_gpu_alloc(size_t bytes) {
    // Cap the highest acceptable physical address at 0x03FFFFFF (64 MB). xgux_set_attrib
    // _pointer feeds the NV2A a physical address via ((uint32_t)ptr & 0x03ffffff) -- if
    // the allocation lands above 64 MB that mask truncates to the WRONG address and the
    // GPU reads garbage vertices/texels (invisible geometry + GPU fault). 64 MB covers
    // the whole stock-console RAM, so this is always valid.
    return MmAllocateContiguousMemoryEx((ULONG)bytes, 0, 0x03FFFFFF, 0,
                                        PAGE_WRITECOMBINE | PAGE_READWRITE);
}

// Smallest power of two >= num (>=1).
static uint32_t nxdk_npot2pot(uint32_t num) {
    uint32_t p = 1;
    while (p < num) { p <<= 1; }
    return p;
}

// log2 of a power-of-two value.
static uint32_t nxdk_ulog2(uint32_t v) {
    uint32_t r = 0;
    while (v > 1) { v >>= 1; r++; }
    return r;
}

// NV2A swizzle (Z-order). Width/height must be powers of two. Adapted from the QEMU/
// Nouveau routine carried in nxdk-sdl3 (swizzle.c, LGPL) -- inlined for bpp=4 (A8R8G8B8)
// so the texture upload doesn't pull in another translation unit. src is linear (row
// pitch = width*4), dst is the swizzled GPU buffer.
static void nxdk_swizzle_rgba8(const uint8_t *src, uint32_t width, uint32_t height, uint8_t *dst) {
    uint32_t mask_x = 0, mask_y = 0, bit = 1, mask_bit = 1;
    bool done;
    do {
        done = true;
        if (bit < width)  { mask_x |= mask_bit; mask_bit <<= 1; done = false; }
        if (bit < height) { mask_y |= mask_bit; mask_bit <<= 1; done = false; }
        bit <<= 1;
    } while (!done);

    const uint32_t row_pitch = width * 4;
    uint32_t off_y = 0;
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t *src_row = src + y * row_pitch;
        uint32_t off_x = 0;
        for (uint32_t x = 0; x < width; x++) {
            *((uint32_t *)(dst + (off_y + off_x) * 4)) = *((const uint32_t *)(src_row + x * 4));
            off_x = (off_x - mask_x) & mask_x;
        }
        off_y = (off_y - mask_y) & mask_y;
    }
}

// ---------------------------------------------------------------------------------
// Identification / capabilities
// ---------------------------------------------------------------------------------

static const char *nxdk_get_name(void) {
    return "nxdk-nv2a";
}

static int nxdk_get_max_texture_size(void) {
    return 4096; // NV2A max
}

static struct GfxClipParameters nxdk_get_clip_parameters(void) {
    // TODO(nv2a): confirm against the pbkit projection/viewport convention. The NV2A
    // is D3D-like: clip-space Z is 0..1, and Y handling depends on how the viewport
    // is set up. These two flags feed the CPU vertex math in gfx_pc, so get them
    // right before trusting any geometry.
    struct GfxClipParameters p;
    p.z_is_from_0_to_1 = true;
    p.invert_y = false;
    return p;
}

// ---------------------------------------------------------------------------------
// Shaders -- on NV2A a "shader" is a register-combiner + texture-stage configuration
// derived from the N64 colour combiner. We decode the combiner here so the engine
// gets the right vertex format; programming the NV2A combiner is the big TODO.
// ---------------------------------------------------------------------------------

static void nxdk_unload_shader(struct ShaderProgram *old_prg) {
    (void)old_prg;
    g.cur_shader = NULL;
}

static void nxdk_load_shader(struct ShaderProgram *new_prg) {
    g.cur_shader = new_prg;
    // TODO(nv2a): program the NV2A register combiners + texture stages from
    // new_prg->cc (see gfx_cc CCFeatures). This is the heart of the port -- map the
    // 1-2 cycle N64 combiner to the NV2A's combiner stages (general + final).
}

static struct ShaderProgram *nxdk_lookup_shader(uint64_t shader_id0, uint32_t shader_id1) {
    for (int i = 0; i < g.shader_count; i++) {
        if (g.shaders[i].used && g.shaders[i].shader_id0 == shader_id0 &&
            g.shaders[i].shader_id1 == shader_id1) {
            return &g.shaders[i];
        }
    }
    return NULL;
}

static struct ShaderProgram *nxdk_create_and_load_new_shader(uint64_t shader_id0, uint32_t shader_id1) {
    if (g.shader_count >= NXDK_MAX_SHADERS) {
        return NULL; // pool full -- bump NXDK_MAX_SHADERS if this ever trips
    }
    ShaderProgram *prg = &g.shaders[g.shader_count++];
    memset(prg, 0, sizeof(*prg));
    prg->shader_id0 = shader_id0;
    prg->shader_id1 = shader_id1;
    gfx_cc_get_features(shader_id0, shader_id1, &prg->cc);
    prg->num_inputs = (uint8_t)prg->cc.num_inputs;
    prg->used_textures[0] = prg->cc.used_textures[0];
    prg->used_textures[1] = prg->cc.used_textures[1];
    prg->used = true;
    nxdk_load_shader(prg);
    return prg;
}

static void nxdk_shader_get_info(struct ShaderProgram *prg, uint8_t *num_inputs, bool used_textures[2]) {
    *num_inputs = prg->num_inputs;
    used_textures[0] = prg->used_textures[0];
    used_textures[1] = prg->used_textures[1];
}

static void nxdk_clear_shaders(void) {
    g.shader_count = 0;
    g.cur_shader = NULL;
}

// ---------------------------------------------------------------------------------
// Textures -- bookkeeping now; pbkit/VRAM upload + swizzle later.
// ---------------------------------------------------------------------------------

static uint32_t nxdk_new_texture(void) {
    uint32_t id = ++g.next_texture_id;
    if (id >= NXDK_MAX_TEXTURES) {
        // Pool full -- wrap is wrong but better than OOB. Bump NXDK_MAX_TEXTURES.
        id = 1;
        g.next_texture_id = 1;
    }
    if (g_NxdkTex[id].argb) {
        MmFreeContiguousMemory(g_NxdkTex[id].argb);
    }
    memset(&g_NxdkTex[id], 0, sizeof(g_NxdkTex[id]));
    return id;
}

static void nxdk_select_texture(int tile, uint32_t texture_id, bool linear_filter) {
    g.cur_tile = tile;
    if (tile >= 0 && tile < 2) {
        g.tex_bound[tile] = texture_id;
    }
    if (texture_id < NXDK_MAX_TEXTURES) {
        g_NxdkTex[texture_id].linear_filter = linear_filter;
    }
    if (tile == 0) { g.tex_is_fb = false; } // a real texture replaced any framebuffer bind
}

static void nxdk_upload_texture(const uint8_t *rgba32_buf, uint32_t width, uint32_t height, bool gen_mipmaps) {
    (void)gen_mipmaps; // TODO(nv2a): mipmaps
    if (g.cur_tile < 0 || g.cur_tile >= 2) { return; }
    uint32_t id = g.tex_bound[g.cur_tile];
    if (id == 0 || id >= NXDK_MAX_TEXTURES) { return; }
    struct NxdkTexture *t = &g_NxdkTex[id];

    if (t->argb) { MmFreeContiguousMemory(t->argb); t->argb = NULL; }
    if (!width || !height) { return; }

    // Swizzled textures need a power-of-two container; the actual texels live in the
    // top-left and us/vs map fast3d's [0,1] UV into that region. (POT textures -- most of
    // them -- have dw==w so us==1 and wrap tiles correctly.)
    t->w = width;  t->h = height;
    t->dw = nxdk_npot2pot(width);
    t->dh = nxdk_npot2pot(height);
    t->pitch = t->dw * 4;
    t->us = (float)width / (float)t->dw;
    t->vs = (float)height / (float)t->dh;

    const size_t containerpx = (size_t)t->dw * t->dh;
    t->argb = (uint8_t *)nxdk_gpu_alloc(containerpx * 4); // swizzled (GPU-visible)
    if (!t->argb) {
        // Contiguous low-64MB pool exhausted (watch for this at 1080i, whose
        // framebuffers eat ~25 MB of it). Capped trace so an OOM isn't silent.
        static int oomlogs = 0;
        if (oomlogs < 8) { oomlogs++; xboxTracef("rdr: TEX ALLOC FAILED %ux%u", width, height); }
        return;
    }

    // Build a LINEAR POT staging buffer: convert RGBA8 (R,G,B,A) -> A8R8G8B8 into the
    // top-left, zero-pad the rest, then swizzle into the GPU buffer.
    uint32_t *lin = (uint32_t *)malloc(containerpx * 4);
    if (!lin) { MmFreeContiguousMemory(t->argb); t->argb = NULL; return; }
    memset(lin, 0, containerpx * 4);
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t *s = rgba32_buf + (size_t)y * width * 4;
        uint32_t *d = lin + (size_t)y * t->dw;
        for (uint32_t x = 0; x < width; x++, s += 4) {
            d[x] = ((uint32_t)s[3] << 24) | ((uint32_t)s[0] << 16) | ((uint32_t)s[1] << 8) | (uint32_t)s[2];
        }
    }
    nxdk_swizzle_rgba8((const uint8_t *)lin, t->dw, t->dh, t->argb);
    free(lin);
    // The swizzled destination is WRITE-COMBINED (nxdk_gpu_alloc): fence the stores
    // before any draw can queue a read of this texture -- same lesson as the vertex
    // arena, where missing sfences showed as stale/garbage data on first use.
    __asm__ __volatile__("sfence" ::: "memory");
}

static void nxdk_set_sampler_parameters(int sampler, bool linear_filter, uint32_t cms, uint32_t cmt, bool mipmaps) {
    (void)mipmaps;
    if (sampler < 0 || sampler >= 2) { return; }
    uint32_t id = g.tex_bound[sampler];
    if (id == 0 || id >= NXDK_MAX_TEXTURES) { return; }
    g_NxdkTex[id].linear_filter = linear_filter;
    g_NxdkTex[id].cms = cms;
    g_NxdkTex[id].cmt = cmt;
}

static void nxdk_delete_texture(uint32_t texID) {
    if (texID == 0 || texID >= NXDK_MAX_TEXTURES) { return; }
    if (g_NxdkTex[texID].argb) {
        MmFreeContiguousMemory(g_NxdkTex[texID].argb);
        g_NxdkTex[texID].argb = NULL;
    }
}

static void nxdk_set_texture_filter(enum FilteringMode mode) { g.tex_filter = mode; }
static enum FilteringMode nxdk_get_texture_filter(void) { return g.tex_filter; }
static void nxdk_set_mipmap_filter(enum MipmapFilteringMode mode) { (void)mode; }
static void nxdk_set_anisotropy_level(int level) { (void)level; }
static int nxdk_get_max_anisotropy_level(void) { return 1; /* TODO(nv2a): NV2A supports up to 4 */ }

// ---------------------------------------------------------------------------------
// Render state
// ---------------------------------------------------------------------------------

// Actual NV2A blend enable = (game asked for alpha) AND (this draw does NOT write depth).
// The N64 sets the alpha-blend blender bits for anti-aliased OPAQUE surfaces too (it blends
// edge pixels by coverage), and fast3d forwards that as use_alpha. Our combiner's
// alpha = tex_alpha * shade_alpha is < 1 for many opaque textures, so blending them turned
// every wall translucent (see-through everything). Opaque geometry writes depth (Z_UPD);
// translucent geometry (glass/smoke/effects) does not -- so gating blend on !depth_mask
// keeps opaque surfaces solid while still blending true translucents and the 2D/HUD (which
// is depth-off, so it blends as before).
static void nxdk_update_blend(void) {
    const bool blend = g.use_alpha && !g.depth_mask;
    uint32_t *p = pb_begin();
    p = xgu_set_blend_enable(p, blend);
    if (blend) {
        p = xgu_set_blend_func_sfactor(p, XGU_FACTOR_SRC_ALPHA);
        p = xgu_set_blend_func_dfactor(p, XGU_FACTOR_ONE_MINUS_SRC_ALPHA);
    }
    pb_end(p);
}

static void nxdk_set_depth_mode(bool depth_test, bool depth_update, bool depth_compare,
                                bool depth_source_prim, uint16_t zmode) {
    (void)depth_source_prim; (void)zmode;
    g.depth_test = depth_test;
    g.depth_mask = depth_update;
    uint32_t *p = pb_begin();
    // Depth-tested draws bind 3-component positions (X,Y screen pixels + Z pre-scaled to
    // the Z24 range [0,0xFFFFFF] in draw_triangles) so the world and models sort
    // front-to-back. Depth-off draws bind 2 components (z defaults to 0).
    //
    // depth_test with depth_compare OFF means "write z, never reject" (N64 Z_UPD without
    // Z_CMP); the GL/SDL_GPU backends map that to ALWAYS. With depth rejection now real,
    // keeping LESS_OR_EQUAL here would wrongly cull those draws. ZMODE_DEC (decals) still
    // shares LESS_OR_EQUAL -- the reference backends add a polygon offset, which the NV2A
    // path doesn't do yet (patchy decals are a known cosmetic follow-up, not a blocker).
    p = xgu_set_depth_test_enable(p, depth_test);
    p = xgu_set_depth_mask(p, depth_update);
    p = xgu_set_depth_func(p, depth_compare ? XGU_FUNC_LESS_OR_EQUAL : XGU_FUNC_ALWAYS);
    pb_end(p);
    // depth_mask feeds the blend gate (opaque depth-writers must not blend).
    nxdk_update_blend();
}

static void nxdk_set_depth_range(float znear, float zfar) {
    (void)znear; (void)zfar;
    // Depth range is folded into the viewport Z scale/offset (see nxdk_set_viewport).
}

static void nxdk_set_viewport(int x, int y, int width, int height) {
    g.vp_x = x; g.vp_y = y; g.vp_w = width; g.vp_h = height;
    // IDENTITY X/Y viewport (like nxdk-sdl3's SDL_render): draw_triangles does the
    // clip->screen transform on the CPU (perspective divide + pixel mapping), so X/Y must
    // pass through untouched. Wrestling the NV2A X/Y viewport scale/offset for clip-space
    // input collapsed the geometry; pre-transforming to screen pixels is deterministic.
    //
    // DEPTH (Z): the NV2A fixed-function pipeline does NOT run SET_VIEWPORT_SCALE/OFFSET
    // as a vertex transform stage -- the composite matrix is expected to output SCREEN
    // coordinates including the Z range (xemu models this by applying an INVERSE viewport
    // to the composite result; the nxdk xgu samples bake a viewport matrix into their
    // composite). With composite == identity, a viewport Z-scale of 0xFFFFFF never
    // multiplied anything: the raster received raw NDC z in [0,1], which collapses to ~1
    // code out of 16.7M in the Z24 buffer -> every depth-tested fragment tied and draws
    // resolved in submission order (rooms-through-walls, props erased by their own room).
    // So: draw_triangles pre-scales z to [0, 0xFFFFFF] on the CPU, the viewport Z-scale
    // stays 1.0 (identity, like X/Y), and the depth clip range is pinned to the same
    // [0, 0xFFFFFF] (leaving it at pbkit's default clamped every fragment to one depth).
    NXDK_RTRACE("rdr: viewport %d %d %d %d", x, y, width, height);
    uint32_t *p = pb_begin();
    p = xgu_set_viewport_offset(p, 0.0f, 0.0f, 0.0f, 0.0f);
    p = xgu_set_viewport_scale(p, 1.0f, 1.0f, 1.0f, 1.0f);
    p = xgu_set_clip_min(p, 0.0f);
    p = xgu_set_clip_max(p, (float)0xFFFFFF);
    pb_end(p);
    NXDK_RTRACE("rdr: viewport ok");
}

static void nxdk_set_scissor(int x, int y, int width, int height) {
    // PD's portal renderer scissors each room/draw-slot to the screen rect where it can be
    // seen (a door/window opening). Without this the adjacent room's geometry spills across
    // the whole screen and paints over the near walls ("rooms rendered through walls").
    // fast3d hands the rect in GL bottom-left window coords (like the SDL_GPU backend); the
    // NV2A scissor is top-left, so flip Y. Clamp to the surface so the rect stays valid.
    const int fbw = (int)pb_back_buffer_width();
    const int fbh = (int)pb_back_buffer_height();
    int rx = x;
    int ry = fbh - y - height; // bottom-left -> top-left
    int rw = width;
    int rh = height;
    if (rx < 0) { rw += rx; rx = 0; }
    if (ry < 0) { rh += ry; ry = 0; }
    if (rx + rw > fbw) { rw = fbw - rx; }
    if (ry + rh > fbh) { rh = fbh - ry; }
    if (rw < 0) { rw = 0; }
    if (rh < 0) { rh = 0; }
    uint32_t *p = pb_begin();
    p = xgu_set_scissor_rect(p, false, (uint32_t)rx, (uint32_t)ry, (uint32_t)rw, (uint32_t)rh);
    pb_end(p);
}

static void nxdk_set_use_alpha(bool use_alpha, bool modulate) {
    (void)modulate;
    g.use_alpha = use_alpha;
    nxdk_update_blend(); // gated on !depth_mask -- see nxdk_update_blend
}

// ---------------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------------

// Compute, from the decoded combiner, the per-vertex float stride and the byte offset
// of the first combiner input (used as the diffuse colour) and texcoord0. Layout
// mirrors gfx_opengl's vertex builder: pos(4), per-tex uv(2)+clamp(0..2), fog(4),
// grayscale(4), then per-input (3 or 4).
static void nxdk_vertex_layout(const struct CCFeatures *cc, int *stride,
                               int *color_off, int *color_size, int *uv0_off) {
    int n = 4; // position xyzw
    *uv0_off = -1;
    for (int i = 0; i < 2; i++) {
        if (cc->used_textures[i]) {
            if (i == 0) { *uv0_off = n; }
            n += 2;
            for (int j = 0; j < 2; j++) {
                if (cc->clamp[i][j]) { n += 1; }
            }
        }
    }
    if (cc->opt_fog) { n += 4; }
    if (cc->opt_grayscale) { n += 4; }

    if (cc->num_inputs >= 1) {
        *color_off = n;
        *color_size = cc->opt_alpha ? 4 : 3;
    } else {
        *color_off = -1;
        *color_size = 0;
    }

    int total = n;
    for (int i = 0; i < cc->num_inputs; i++) {
        total += cc->opt_alpha ? 4 : 3;
    }
    *stride = total;
}

// Stage-0 colour OCW variants: the product modes (unlit/textured) write A*B to the
// final-combiner input register (AB_DST = 0x4, as set once by nxdk_setup_combiner);
// the const-lerp mode computes A*B + C*D, so the SUM must land there instead
// (SUM_DST = 0x4, AB/CD discarded). Pushed on every mode switch so the modes can't
// inherit each other's routing.
#define NXDK_COLOR_OCW(ab_dst, sum_dst) ( \
    XGU_MASK(NV097_SET_COMBINER_COLOR_OCW_AB_DST, (ab_dst)) \
    | XGU_MASK(NV097_SET_COMBINER_COLOR_OCW_CD_DST, 0x0) \
    | XGU_MASK(NV097_SET_COMBINER_COLOR_OCW_SUM_DST, (sum_dst)) \
    | XGU_MASK(NV097_SET_COMBINER_COLOR_OCW_MUX_ENABLE, 0) \
    | XGU_MASK(NV097_SET_COMBINER_COLOR_OCW_AB_DOT_ENABLE, 0) \
    | XGU_MASK(NV097_SET_COMBINER_COLOR_OCW_CD_DOT_ENABLE, 0) \
    | XGU_MASK(NV097_SET_COMBINER_COLOR_OCW_OP, NV097_SET_COMBINER_COLOR_OCW_OP_NOSHIFT))

// Switch the register combiner between unlit (output = vertex diffuse) and textured
// (output = tex0 * diffuse). Only the shader-stage program + the colour/alpha input
// combiner words change; the control/fog set by nxdk_setup_combiner stay. Tracked so
// a run of same-mode draws doesn't re-push it. ICW masks match SDL_render_xgu's
// unlit_combiner_apply / texture_combiner_apply.
static void nxdk_combiner_mode(bool textured) {
    if (g.combiner_textured == (int)textured) { return; }
    g.combiner_textured = (int)textured;
    uint32_t *p = pb_begin();
    p = pb_push1(p, NV097_SET_SHADER_OTHER_STAGE_INPUT, 0);
    p = pb_push1(p, NV097_SET_COMBINER_COLOR_OCW + 0 * 4, NXDK_COLOR_OCW(0x4, 0x0));
    if (textured) {
        p = pb_push1(p, NV097_SET_SHADER_STAGE_PROGRAM,
            XGU_MASK(NV097_SET_SHADER_STAGE_PROGRAM_STAGE0, NV097_SET_SHADER_STAGE_PROGRAM_STAGE0_2D_PROJECTIVE));
        p = pb_push1(p, NV097_SET_COMBINER_COLOR_ICW + 0 * 4,
            XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_A_SOURCE, 0x8) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_A_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_A_MAP, 0x6)
            | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_B_SOURCE, 0x4) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_B_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_B_MAP, 0x6)
            | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_C_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_C_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_C_MAP, 0x0)
            | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_D_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_D_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_D_MAP, 0x0));
        p = pb_push1(p, NV097_SET_COMBINER_ALPHA_ICW + 0 * 4,
            XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_A_SOURCE, 0x8) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_A_ALPHA, 1) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_A_MAP, 0x6)
            | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_B_SOURCE, 0x4) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_B_ALPHA, 1) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_B_MAP, 0x6)
            | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_C_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_C_ALPHA, 1) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_C_MAP, 0x0)
            | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_D_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_D_ALPHA, 1) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_D_MAP, 0x0));
    } else {
        p = pb_push1(p, NV097_SET_SHADER_STAGE_PROGRAM,
            XGU_MASK(NV097_SET_SHADER_STAGE_PROGRAM_STAGE0, NV097_SET_SHADER_STAGE_PROGRAM_STAGE0_PROGRAM_NONE)
            | XGU_MASK(NV097_SET_SHADER_STAGE_PROGRAM_STAGE1, NV097_SET_SHADER_STAGE_PROGRAM_STAGE1_PROGRAM_NONE)
            | XGU_MASK(NV097_SET_SHADER_STAGE_PROGRAM_STAGE2, NV097_SET_SHADER_STAGE_PROGRAM_STAGE2_PROGRAM_NONE)
            | XGU_MASK(NV097_SET_SHADER_STAGE_PROGRAM_STAGE3, NV097_SET_SHADER_STAGE_PROGRAM_STAGE3_PROGRAM_NONE));
        p = pb_push1(p, NV097_SET_COMBINER_COLOR_ICW + 0 * 4,
            XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_A_SOURCE, 0x4) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_A_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_A_MAP, 0x6)
            | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_B_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_B_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_B_MAP, 0x1)
            | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_C_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_C_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_C_MAP, 0x0)
            | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_D_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_D_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_D_MAP, 0x0));
        p = pb_push1(p, NV097_SET_COMBINER_ALPHA_ICW + 0 * 4,
            XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_A_SOURCE, 0x4) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_A_ALPHA, 1) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_A_MAP, 0x6)
            | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_B_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_B_ALPHA, 1) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_B_MAP, 0x1)
            | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_C_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_C_ALPHA, 1) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_C_MAP, 0x0)
            | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_D_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_D_ALPHA, 1) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_D_MAP, 0x0));
    }
    pb_end(p);
}

// Const-lerp combiner (mode 2) for PD's two-tone glyph/outline shaders:
//
//   rgb = tex0.a * C0.rgb + (1 - tex0.a) * C1.rgb      (one general stage: A*B + C*D)
//   a   = tex0.a * C0.a
//
// PD text (textRender) is a 2-cycle combiner: colour = (ENV - PRIM) * TEX1_ALPHA + PRIM
// with alpha = TEX0_ALPHA * ENV_ALPHA -- the same glyph texture is bound to both tiles,
// tile 0 with the wide-coverage palette, tile 1 with the tight-core palette, giving a
// PRIM-coloured halo around an ENV-coloured core (HUD digits: green glow, dark core).
// The old tex0*diffuse path flattened that to tex0 * input0 = a flat ENV-coloured glyph;
// HUD/hudmsg text passes ENV = 0x000000a0 (black glow), so those strings rendered as
// near-invisible dark smudges -- the "missing font letters". Here C0 = input1 (ENV, with
// ENV alpha -- fast3d numbers inputs by first appearance, and ENV is combiner slot A),
// C1 = input2 (PRIM), and tex0's alpha stands in for tile 1's (same glyph, slightly
// wider coverage ramp -- a marginally thicker core, visually fine). The constants ride
// the NV2A combiner FACTOR registers; COMBINER_CONTROL is already SAME_FACTOR_ALL.
static void nxdk_combiner_lerp2(uint32_t f0, uint32_t f1) {
    if (g.combiner_textured == 2 && g.combiner_f0 == f0 && g.combiner_f1 == f1) { return; }
    const bool mode_changed = (g.combiner_textured != 2);
    g.combiner_textured = 2;
    g.combiner_f0 = f0;
    g.combiner_f1 = f1;
    uint32_t *p = pb_begin();
    p = pb_push1(p, NV097_SET_COMBINER_FACTOR0 + 0 * 4, f0);
    p = pb_push1(p, NV097_SET_COMBINER_FACTOR1 + 0 * 4, f1);
    if (mode_changed) {
        p = pb_push1(p, NV097_SET_SHADER_OTHER_STAGE_INPUT, 0);
        p = pb_push1(p, NV097_SET_SHADER_STAGE_PROGRAM,
            XGU_MASK(NV097_SET_SHADER_STAGE_PROGRAM_STAGE0, NV097_SET_SHADER_STAGE_PROGRAM_STAGE0_2D_PROJECTIVE));
        // Colour: A = tex0 alpha (replicated), B = const0, C = 1 - tex0 alpha, D = const1;
        // route the SUM (not AB) to the final-combiner input.
        p = pb_push1(p, NV097_SET_COMBINER_COLOR_OCW + 0 * 4, NXDK_COLOR_OCW(0x0, 0x4));
        p = pb_push1(p, NV097_SET_COMBINER_COLOR_ICW + 0 * 4,
            XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_A_SOURCE, 0x8) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_A_ALPHA, 1) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_A_MAP, 0x6)
            | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_B_SOURCE, 0x1) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_B_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_B_MAP, 0x6)
            | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_C_SOURCE, 0x8) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_C_ALPHA, 1) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_C_MAP, 0x1)
            | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_D_SOURCE, 0x2) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_D_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_D_MAP, 0x6));
        // Alpha: A = tex0 alpha, B = const0 alpha (AB_DST 0x4 from nxdk_setup_combiner).
        p = pb_push1(p, NV097_SET_COMBINER_ALPHA_ICW + 0 * 4,
            XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_A_SOURCE, 0x8) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_A_ALPHA, 1) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_A_MAP, 0x6)
            | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_B_SOURCE, 0x1) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_B_ALPHA, 1) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_B_MAP, 0x6)
            | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_C_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_C_ALPHA, 1) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_C_MAP, 0x0)
            | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_D_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_D_ALPHA, 1) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_D_MAP, 0x0));
    }
    pb_end(p);
}

// Map fast3d's N64 clamp mode to an NV2A texture address mode. cms/cmt are bit flags:
// G_TX_CLAMP=2, G_TX_MIRROR=1, plain repeat=0. CLAMP wins (fonts/UI need it -- without
// it glyph quads whose UVs touch the atlas edge wrap to a different glyph -> fragmented
// text), then MIRROR, else WRAP/repeat.
static XguTextureAddress nxdk_cm_to_xgu(uint32_t cm) {
    if (cm & 2) { return XGU_CLAMP_TO_EDGE; } // G_TX_CLAMP
    if (cm & 1) { return XGU_MIRROR; }        // G_TX_MIRROR
    return XGU_WRAP;
}

// Program NV2A texture stage 0 from the bound swizzled texture (A8R8G8B8) and switch the
// combiner to the textured / const-lerp path, or disable both. Matches SDL_render_xgu's
// bind. lerp2 (with f0/f1) selects the two-constant glyph combiner -- see
// nxdk_combiner_lerp2.
static void nxdk_apply_texture(const struct CCFeatures *cc, bool lerp2, uint32_t f0, uint32_t f1) {
    const bool use = cc->used_textures[0] && g.tex_bound[0] && g.tex_bound[0] < NXDK_MAX_TEXTURES
                     && g_NxdkTex[g.tex_bound[0]].argb;
    if (use && lerp2) {
        nxdk_combiner_lerp2(f0, f1);
    } else {
        nxdk_combiner_mode(use);
    }
    if (use) {
        // Skip re-pushing the texture registers for a run of same-texture draws (fast3d
        // batches by texture) -- 546 re-binds/frame would flood the push buffer.
        if (g.tex_bound[0] == g.tex_applied) { return; }
        g.tex_applied = g.tex_bound[0];
        struct NxdkTexture *t = &g_NxdkTex[g.tex_bound[0]];
        const uint32_t phys = (uint32_t)(uintptr_t)t->argb & 0x03ffffff;
        const XguTexFilter filt = t->linear_filter ? XGU_TEXTURE_FILTER_LINEAR : XGU_TEXTURE_FILTER_NEAREST;
        uint32_t *p = pb_begin();
        p = xgu_set_texture_offset(p, 0, (const void *)(uintptr_t)phys);
        p = xgu_set_texture_format(p, 0, 2, false, XGU_SOURCE_COLOR, 2,
                                   XGU_TEXTURE_FORMAT_A8R8G8B8_SWIZZLED, 1,
                                   nxdk_ulog2(t->dw), nxdk_ulog2(t->dh), 0);
        p = xgu_set_texture_control0(p, 0, true, 0, 0);
        p = xgu_set_texture_control1(p, 0, t->pitch);
        p = xgu_set_texture_image_rect(p, 0, t->w, t->h);
        p = xgu_set_texture_filter(p, 0, 0, XGU_TEXTURE_CONVOLUTION_GAUSSIAN, filt, filt, false, false, false, false);
        // Address mode per the N64 tile clamp flags (fonts/UI use CLAMP; tiled world
        // textures use WRAP). The bool after each mode is "wrap enable" (true == WRAP).
        const XguTextureAddress au = nxdk_cm_to_xgu(t->cms);
        const XguTextureAddress av = nxdk_cm_to_xgu(t->cmt);
        p = xgu_set_texture_address(p, 0, au, (au == XGU_WRAP), av, (av == XGU_WRAP), XGU_CLAMP_TO_EDGE, false, false);
        pb_end(p);
    } else {
        // Texture stage off for colour-only draws. Clear the cache so the next textured
        // draw re-binds (the stage we just disabled must be re-enabled + re-pointed).
        g.tex_applied = 0;
        uint32_t *p = pb_begin();
        p = xgu_set_texture_control0(p, 0, false, 0, 0);
        pb_end(p);
    }
}

// Per-frame draw counter (reset + logged in nxdk_start_frame). Diagnostic only.
int g_NxdkFrameDraws = 0;
// Per-frame NDC-z span across depth-tested draws (x1000, ints for the broken-%f log).
// Diagnoses the "objects only against sky" symptom: if the span is crammed near 1000 the
// depth precision is collapsed; if it spans ~0..1000 the values are fine and the cause is
// elsewhere. Reset + logged in nxdk_start_frame.
int g_NxdkZMin = 100000, g_NxdkZMax = -100000;
// Arena telemetry (trap T2): draws dropped by overflow this frame + the all-time
// vertex high-water mark. The HUD/text draw LAST, so silent overflow eats the UI on
// heavy frames -- the drop trace makes that diagnosable instead of "text flickers".
int g_NxdkVtxDrops = 0;
int g_NxdkVtxHighWater = 0;

// A clip-space vertex normalised to our fixed attribute set, used for CPU near-plane
// clipping (nxdk_draw_triangles). fast3d's z is D3D-style (z_is_from_0_to_1): the near
// plane is z == 0, far is z == w. We clip triangles against z >= 0 so no surviving vertex
// has w <= 0 -- such a vertex sign-flips through the perspective divide and stretches
// across the screen (the "geometry moving faster than the camera" artifact).
struct NxdkClipV { float x, y, z, w, r, g, b, a, s, t; };

static inline void nxdkClipLerp(struct NxdkClipV *o, const struct NxdkClipV *a,
                                const struct NxdkClipV *b, float f) {
    o->x = a->x + (b->x - a->x) * f;
    o->y = a->y + (b->y - a->y) * f;
    o->z = a->z + (b->z - a->z) * f;
    o->w = a->w + (b->w - a->w) * f;
    o->r = a->r + (b->r - a->r) * f;
    o->g = a->g + (b->g - a->g) * f;
    o->b = a->b + (b->b - a->b) * f;
    o->a = a->a + (b->a - a->a) * f;
    o->s = a->s + (b->s - a->s) * f;
    o->t = a->t + (b->t - a->t) * f;
}

static void nxdk_draw_triangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) {
    (void)buf_vbo_len;
    if (!g.cur_shader) { return; }

    const struct CCFeatures *cc = &g.cur_shader->cc;
    int stride, color_off, color_size, uv0_off;
    nxdk_vertex_layout(cc, &stride, &color_off, &color_size, &uv0_off);

    // Skip draws that sample an (unimplemented) framebuffer-effect texture -- otherwise
    // they'd stretch a stale font glyph across the screen. The effect is simply absent.
    if (g.tex_is_fb && cc->used_textures[0]) { return; }

    const size_t nverts = buf_vbo_num_tris * 3;
    if (nverts == 0) { return; }
    g_NxdkFrameDraws++;

    NXDK_RTRACE("rdr: draw nv=%d stride=%d coff=%d uv=%d vlen=%d",
                (int)nverts, stride, color_off, uv0_off, (int)buf_vbo_len);

    // Allocate the two arena buffers once. Each is modest (2 MB / ~52k verts) -- a single
    // big block can't be allocated contiguously in the low 64 MB after the 33 MB ROM, but
    // two smaller blocks fit. Trace a failure so an OOM is obvious instead of silent.
    if (!g.vtx[0]) {
        g.vtx_caps = 52 * 1024; // per buffer; 52k * 10 floats * 4B ~= 2 MB
        g.vtx[0] = (float *)nxdk_gpu_alloc(g.vtx_caps * NXDK_VTX_FLOATS * sizeof(float));
        g.vtx[1] = (float *)nxdk_gpu_alloc(g.vtx_caps * NXDK_VTX_FLOATS * sizeof(float));
        if (!g.vtx[0] || !g.vtx[1]) { g.vtx_caps = 0; g.vtx[0] = NULL; xboxTracef("rdr: VERTEX ARENA ALLOC FAILED"); return; }
    }
    // Bump-allocate this draw's own region in THIS frame's buffer. On overflow DROP the
    // draw (return) rather than wrap (which would alias earlier queued draws this frame).
    // Near-plane clipping can split each triangle into two (4-vertex polygon), so reserve
    // up to 2x the source vertex count. Dropped draws are counted (g_NxdkVtxDrops) and
    // reported from nxdk_start_frame -- HUD/text draw last, so overflow eats the UI.
    if (g.vtx_off + nverts * 2 > g.vtx_caps) { g_NxdkVtxDrops++; return; }
    float *const base = g.vtx[g.vtx_frame] + g.vtx_off * NXDK_VTX_FLOATS;

    // De-interleave fast3d's variable layout into a fixed [pos4, colour4, uv2], doing
    // the clip->screen transform on the CPU (perspective divide + pixel mapping), since
    // the NV2A viewport is identity. NDC y is up; screen y is down -> flip.
    const bool textured = (uv0_off >= 0);
    const float vpx = (float)g.vp_x, vpy = (float)g.vp_y;
    const float vpw = (float)g.vp_w, vph = (float)g.vp_h;
    // Swizzled textures live in a POT container; scale fast3d's normalised [0,1] UV by
    // us/vs = texw/dw so it maps into the real texel region (==1 for POT textures).
    float us = 1.0f, vs = 1.0f;
    if (textured) {
        const uint32_t tid = g.tex_bound[0];
        if (tid && tid < NXDK_MAX_TEXTURES && g_NxdkTex[tid].argb) {
            us = g_NxdkTex[tid].us; vs = g_NxdkTex[tid].vs;
        }
    }
    // PD's two-tone glyph/outline shaders (both texture tiles + >= 2 constant combiner
    // inputs, e.g. text: colour = lerp(PRIM -> ENV, tile1 alpha), alpha = tex0a * ENVa)
    // get the const-lerp combiner. The inputs are flat per draw (prim/env constants
    // written per-vertex by gfx_pc), so read them once from vertex 0: input1 (ENV +
    // its alpha) and input2 (PRIM) sit consecutively after color_off. Everything else
    // keeps the plain tex0 * input0 path. See nxdk_combiner_lerp2 for why (this was
    // the "missing font letters": HUD text ENV is 0x000000a0, a near-invisible flat
    // dark glyph under the old path).
    const bool lerp2 = textured && cc->used_textures[1] && cc->num_inputs >= 2 && color_off >= 0;
    uint32_t lerp_f0 = 0, lerp_f1 = 0;
    if (lerp2) {
        const float *i1 = buf_vbo + color_off;               // input1 rgb(+a): ENV for text
        const float *i2 = buf_vbo + color_off + color_size;  // input2 rgb: PRIM for text
        const float a1 = (color_size == 4) ? i1[3] : 1.0f;
        #define NXDK_F2B(f) ((uint32_t)(((f) < 0.0f ? 0.0f : ((f) > 1.0f ? 1.0f : (f))) * 255.0f + 0.5f))
        lerp_f0 = (NXDK_F2B(a1) << 24) | (NXDK_F2B(i1[0]) << 16) | (NXDK_F2B(i1[1]) << 8) | NXDK_F2B(i1[2]);
        lerp_f1 = 0xFF000000u | (NXDK_F2B(i2[0]) << 16) | (NXDK_F2B(i2[1]) << 8) | NXDK_F2B(i2[2]);
        #undef NXDK_F2B
    }

    // fast3d hands CLIP-space verts and expects the GPU to do BOTH the perspective divide
    // and the near-plane clip. We divide on the CPU (the NV2A rasterises pretransformed
    // screen pixels with no homogeneous clip), so we must clip the near plane ourselves: a
    // vertex at/behind the eye (clip w <= 0) sign-flips through the divide and stretches
    // across the screen as the camera moves. fast3d's z is D3D-style (near plane at z == 0),
    // so we Sutherland-Hodgman each 3D triangle against z >= 0 -- the front part is kept and
    // re-split at the near plane (no explosion AND no holes, unlike a whole-triangle cull).
    // Depth-off 2D/HUD (w == 1) is passed through unclipped to keep the proven menu path
    // byte-identical (doclip == false -> every vertex counts as inside).
    const bool doclip = g.depth_test;
    size_t out = 0;
    for (size_t t = 0; t + 3 <= nverts; t += 3) {
        struct NxdkClipV in[3];
        for (int k = 0; k < 3; k++) {
            const float *src = buf_vbo + (t + (size_t)k) * (size_t)stride;
            struct NxdkClipV *v = &in[k];
            v->x = src[0]; v->y = src[1]; v->z = src[2]; v->w = src[3];
            if (color_off >= 0) {
                const float *c = src + color_off;
                v->r = c[0]; v->g = c[1]; v->b = c[2];
                v->a = (color_size == 4) ? c[3] : 1.0f;
            } else {
                v->r = v->g = v->b = v->a = 1.0f;
            }
            if (textured) { v->s = src[uv0_off] * us; v->t = src[uv0_off + 1] * vs; }
            else { v->s = v->t = 0.0f; }
        }

        // Clip against the near plane z >= 0 -> a polygon of up to 4 vertices.
        struct NxdkClipV poly[4];
        int np = 0;
        for (int i = 0; i < 3; i++) {
            const struct NxdkClipV *A = &in[i];
            const struct NxdkClipV *B = &in[(i + 1) % 3];
            const bool Ain = !doclip || A->z >= 0.0f;
            const bool Bin = !doclip || B->z >= 0.0f;
            if (Ain) { poly[np++] = *A; }
            if (Ain != Bin) {
                const float denom = B->z - A->z;
                const float f = (denom != 0.0f) ? (-A->z / denom) : 0.0f;
                nxdkClipLerp(&poly[np++], A, B, f);
            }
        }
        if (np < 3) { continue; } // wholly behind the near plane

        // Emit the polygon as a triangle fan (np is 3 or 4).
        for (int fi = 2; fi < np; fi++) {
            const int idx[3] = { 0, fi - 1, fi };
            for (int j = 0; j < 3; j++) {
                const struct NxdkClipV *v = &poly[idx[j]];
                float *dst = base + (out + (size_t)j) * NXDK_VTX_FLOATS;
                const float iw = (v->w != 0.0f) ? 1.0f / v->w : 0.0f;
                const float ndcx = v->x * iw, ndcy = v->y * iw;
                float ndcz = v->z * iw;
                // Clamp NDC depth to [0,1] before the Z24 pre-scale below. Clamping keeps
                // far-plane overshoot outside the pinned depth clip range from firing the
                // NV2A's Z clip on these pretransformed verts (which would mangle their XY).
                ndcz = ndcz < 0.0f ? 0.0f : (ndcz > 1.0f ? 1.0f : ndcz);
                if (doclip) { // depth-tested draws only -- track the NDC-z span (x1000)
                    const int zi = (int)(ndcz * 1000.0f);
                    if (zi < g_NxdkZMin) { g_NxdkZMin = zi; }
                    if (zi > g_NxdkZMax) { g_NxdkZMax = zi; }
                }
                dst[0] = (ndcx * 0.5f + 0.5f) * vpw + vpx;          // screen x (pixels)
                dst[1] = (1.0f - (ndcy * 0.5f + 0.5f)) * vph + vpy; // screen y (pixels, flipped)
                // Pre-scale depth to the Z24 range ON THE CPU. The fixed-function path
                // treats the composite output (== our vertex, identity matrices) as
                // already screen-space, so no viewport Z-scale is ever applied to it --
                // raw NDC z in [0,1] quantised to a single Z24 code and every
                // depth-tested draw tied (submission-order rendering: props erased by
                // their own room's BG, rooms painted through walls). 16777215 = 0xFFFFFF
                // matches the depth clip range pinned in nxdk_set_viewport.
                dst[2] = ndcz * 16777215.0f;                        // Z24 depth [0, 0xFFFFFF]
                dst[3] = 1.0f;
                dst[4] = v->r; dst[5] = v->g; dst[6] = v->b; dst[7] = v->a;
                dst[8] = v->s; dst[9] = v->t;
            }
            out += 3;
        }
    }
    if (out == 0) { return; } // every triangle clipped away -- nothing to draw

    // The arena is WRITE-COMBINED: the CPU stores above sit in the write-combine buffer
    // and may not have reached RAM yet. pbkit can kick the push buffer mid-frame, so the
    // NV2A could DMA-read this draw's vertices before the writes land -> individual verts
    // snap to stale/garbage positions (flickering points on movement). Fence the WC
    // writes so they're globally visible before the draw is queued.
    __asm__ __volatile__("sfence" ::: "memory");

    NXDK_RTRACE("rdr: deinterleaved");
    nxdk_apply_texture(cc, lerp2, lerp_f0, lerp_f1);
    NXDK_RTRACE("rdr: applied tex");

    const uint32_t bstride = NXDK_VTX_FLOATS * sizeof(float);
    // Bind THIS draw's arena region (base), not the arena start -- each queued draw must
    // point at its own vertices. Position is CPU-pretransformed screen pixels (X,Y) like
    // SDL_render_xgu's float pos[2]: the NV2A rasterises in screen space with no homogeneous
    // clip. (Binding 4 components ran every vertex through the clip pipeline where z>>w
    // clipped all geometry away.) For depth-tested draws bind 3 components so the per-vertex
    // screen-space Z (dst[2], pre-scaled to [0,0xFFFFFF]) reaches the Z24 buffer and the
    // world/models sort front-to-back; depth-off 2D/HUD keeps 2 components (z=0). Gated, so
    // a Z mishap only touches depth-tested geometry, never the proven menu/HUD path.
    xgux_set_attrib_pointer(XGU_VERTEX_ARRAY, XGU_FLOAT, g.depth_test ? 3 : 2, bstride, base);
    xgux_set_attrib_pointer(XGU_COLOR_ARRAY,  XGU_FLOAT, 4, bstride, base + 4);
    if (textured) {
        xgux_set_attrib_pointer(XGU_TEXCOORD0_ARRAY, XGU_FLOAT, 2, bstride, base + 8);
    } else {
        xgux_set_attrib_pointer(XGU_TEXCOORD0_ARRAY, XGU_FLOAT, 0, 0, NULL);
    }
    // Disable arrays we never supply so a previous draw's binding can't dangle.
    xgux_set_attrib_pointer(XGU_TEXCOORD1_ARRAY, XGU_FLOAT, 0, 0, NULL);
    xgux_set_attrib_pointer(XGU_NORMAL_ARRAY,    XGU_FLOAT, 0, 0, NULL);
    NXDK_RTRACE("rdr: bound arrays");

    xgux_draw_arrays(XGU_TRIANGLES, 0, (uint32_t)out);
    // Advance the bump allocator by the EMITTED vertex count (out, after near-plane
    // clipping), ROUNDED UP to a multiple of 4 vertices. Each vertex is 40 bytes
    // (NXDK_VTX_FLOATS*4), and 4 verts = 160 bytes = a multiple of 32, so every draw's base
    // stays 32-byte aligned -- the NV2A vertex DMA requires 32-byte alignment
    // (SDL_render_xgu's SDL_XGU_VERTEX_ALIGNMENT). Unaligned bases made the GPU misread
    // vertices -> the flickering streaks/lines (alignment shifted frame to frame).
    g.vtx_off = (g.vtx_off + out + 3u) & ~(size_t)3u;
    if ((int)g.vtx_off > g_NxdkVtxHighWater) { g_NxdkVtxHighWater = (int)g.vtx_off; }
    NXDK_RTRACE("rdr: drawn");
}

// ---------------------------------------------------------------------------------
// Frame lifecycle
// ---------------------------------------------------------------------------------

static void nxdk_init(void) {
    // NOTE: do NOT memset(&g) here -- the WM (wm_init) and this RAPI share the single
    // file-static `g`, and gfx_init runs wm_init (which sets g.width/g.height from
    // pb_back_buffer_*) BEFORE this. Zeroing g wiped the dimensions back to 0, which
    // made gfx_pc compute a degenerate (0,1) viewport -> nothing visible. `g` is
    // already zero-initialized at program start, so just set the RAPI fields.
    g.tex_filter = FILTER_LINEAR;
    if (g.target_fps == 0) { g.target_fps = 60; }
    g.next_framebuffer_id = 1;
    g.combiner_textured = -1; // unset -> first nxdk_combiner_mode() pushes the input combiner
}

static void nxdk_on_resize(void) { /* Xbox modes are fixed; nothing to do */ }

// NV2A pipeline state, replicated from nxdk-sdl3's SDL_render_xgu device init. The
// critical one is the SCISSOR rect -- without it the NV2A scissors away every fragment.
// Also disables texgen, texture matrices, normalization, and sets all weight model-view
// + inverse matrices to identity.
//
// Run ONCE (proven sufficient by both nxdk-sdl3's SDL_render_xgu and the canonical
// pbkit demo loop -- pb_target_back_buffer / pb_fill / pb_erase_* do NOT clobber the
// combiner/scissor/transform state). Running this every frame (×546 draws on the intro
// frames) overflowed the push buffer and hung the GPU deterministically.
static bool g_NxdkOneshotDone; // reset by the runtime video-mode switch (wm_start_frame)

static void nxdk_oneshot_state(void) {
    if (g_NxdkOneshotDone) { return; }
    g_NxdkOneshotDone = true;
    static const float ident[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    const int w = pb_back_buffer_width();
    const int h = pb_back_buffer_height();

    uint32_t *p = pb_begin();
    p = xgu_set_skin_mode(p, XGU_SKIN_MODE_OFF);
    p = xgu_set_normalization_enable(p, false);
    p = xgu_set_lighting_enable(p, false);
    p = xgu_set_cull_face_enable(p, false);
    p = xgu_set_clear_rect_vertical(p, 0, h);
    p = xgu_set_clear_rect_horizontal(p, 0, w);
    pb_end(p);

    for (int i = 0; i < XGU_TEXTURE_COUNT; i++) {
        p = pb_begin();
        p = xgu_set_texgen_s(p, i, XGU_TEXGEN_DISABLE);
        p = xgu_set_texgen_t(p, i, XGU_TEXGEN_DISABLE);
        p = xgu_set_texgen_r(p, i, XGU_TEXGEN_DISABLE);
        p = xgu_set_texgen_q(p, i, XGU_TEXGEN_DISABLE);
        p = xgu_set_texture_matrix_enable(p, i, false);
        p = xgu_set_texture_matrix(p, i, ident);
        pb_end(p);
    }

    for (int i = 0; i < XGU_WEIGHT_COUNT; i++) {
        p = pb_begin();
        p = xgu_set_model_view_matrix(p, i, ident);
        p = xgu_set_inverse_model_view_matrix(p, i, ident);
        pb_end(p);
    }

    p = pb_begin();
    p = xgu_set_transform_execution_mode(p, XGU_FIXED, XGU_RANGE_MODE_PRIVATE);
    p = xgu_set_projection_matrix(p, ident);
    p = xgu_set_composite_matrix(p, ident);
    p = xgu_set_scissor_rect(p, false, 0, 0, w, h);
    pb_end(p);
}

// Colour-only register combiner: all texture stages off, output = the vertex diffuse
// colour (combiner source 0x4). Adapted verbatim from nxdk-sdl3's SDL_render_xgu.c
// (unlit path) -- without this, pbkit's default combiner writes nothing, so geometry
// rasterises invisibly. TODO(nv2a): the textured path needs tex0 routed in here too.
static void nxdk_setup_combiner(void) {
    uint32_t *p = pb_begin();
    // No texture shader stages.
    p = pb_push1(p, NV097_SET_SHADER_STAGE_PROGRAM,
        XGU_MASK(NV097_SET_SHADER_STAGE_PROGRAM_STAGE0, NV097_SET_SHADER_STAGE_PROGRAM_STAGE0_PROGRAM_NONE)
        | XGU_MASK(NV097_SET_SHADER_STAGE_PROGRAM_STAGE1, NV097_SET_SHADER_STAGE_PROGRAM_STAGE1_PROGRAM_NONE)
        | XGU_MASK(NV097_SET_SHADER_STAGE_PROGRAM_STAGE2, NV097_SET_SHADER_STAGE_PROGRAM_STAGE2_PROGRAM_NONE)
        | XGU_MASK(NV097_SET_SHADER_STAGE_PROGRAM_STAGE3, NV097_SET_SHADER_STAGE_PROGRAM_STAGE3_PROGRAM_NONE));
    p = pb_push1(p, NV097_SET_COMBINER_COLOR_ICW + 0 * 4,
        XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_A_SOURCE, 0x4) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_A_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_A_MAP, 0x6)
        | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_B_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_B_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_B_MAP, 0x1)
        | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_C_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_C_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_C_MAP, 0x0)
        | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_D_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_D_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_D_MAP, 0x0));
    p = pb_push1(p, NV097_SET_COMBINER_COLOR_OCW + 0 * 4,
        XGU_MASK(NV097_SET_COMBINER_COLOR_OCW_AB_DST, 0x4)
        | XGU_MASK(NV097_SET_COMBINER_COLOR_OCW_CD_DST, 0x0)
        | XGU_MASK(NV097_SET_COMBINER_COLOR_OCW_SUM_DST, 0x0)
        | XGU_MASK(NV097_SET_COMBINER_COLOR_OCW_MUX_ENABLE, 0)
        | XGU_MASK(NV097_SET_COMBINER_COLOR_OCW_AB_DOT_ENABLE, 0)
        | XGU_MASK(NV097_SET_COMBINER_COLOR_OCW_CD_DOT_ENABLE, 0)
        | XGU_MASK(NV097_SET_COMBINER_COLOR_OCW_OP, NV097_SET_COMBINER_COLOR_OCW_OP_NOSHIFT));
    p = pb_push1(p, NV097_SET_COMBINER_ALPHA_ICW + 0 * 4,
        XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_A_SOURCE, 0x4) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_A_ALPHA, 1) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_A_MAP, 0x6)
        | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_B_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_B_ALPHA, 1) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_B_MAP, 0x1)
        | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_C_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_C_ALPHA, 1) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_C_MAP, 0x0)
        | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_D_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_D_ALPHA, 1) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_D_MAP, 0x0));
    p = pb_push1(p, NV097_SET_COMBINER_ALPHA_OCW + 0 * 4,
        XGU_MASK(NV097_SET_COMBINER_ALPHA_OCW_AB_DST, 0x4)
        | XGU_MASK(NV097_SET_COMBINER_ALPHA_OCW_CD_DST, 0x0)
        | XGU_MASK(NV097_SET_COMBINER_ALPHA_OCW_SUM_DST, 0x0)
        | XGU_MASK(NV097_SET_COMBINER_ALPHA_OCW_MUX_ENABLE, 0)
        | XGU_MASK(NV097_SET_COMBINER_ALPHA_OCW_OP, NV097_SET_COMBINER_ALPHA_OCW_OP_NOSHIFT));
    p = pb_push1(p, NV097_SET_COMBINER_CONTROL,
        XGU_MASK(NV097_SET_COMBINER_CONTROL_FACTOR0, NV097_SET_COMBINER_CONTROL_FACTOR0_SAME_FACTOR_ALL)
        | XGU_MASK(NV097_SET_COMBINER_CONTROL_FACTOR1, NV097_SET_COMBINER_CONTROL_FACTOR1_SAME_FACTOR_ALL)
        | XGU_MASK(NV097_SET_COMBINER_CONTROL_ITERATION_COUNT, 1));
    p = pb_push1(p, NV097_SET_COMBINER_SPECULAR_FOG_CW0,
        XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_A_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_A_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_A_INVERSE, 0)
        | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_B_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_B_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_B_INVERSE, 0)
        | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_C_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_C_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_C_INVERSE, 0)
        | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_D_SOURCE, 0x4) | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_D_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_D_INVERSE, 0));
    p = pb_push1(p, NV097_SET_COMBINER_SPECULAR_FOG_CW1,
        XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_E_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_E_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_E_INVERSE, 0)
        | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_F_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_F_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_F_INVERSE, 0)
        | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_G_SOURCE, 0x4) | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_G_ALPHA, 1) | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_G_INVERSE, 0)
        | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_SPECULAR_CLAMP, 0));
    pb_end(p);
}

static void nxdk_start_frame(void) {
    // Per-frame global transform state. fast3d hands us CLIP-space vertices (it does
    // the CPU transform), so program the NV2A fixed-function transform as a pass-
    // through: identity composite matrix -> the GPU only does the perspective divide
    // + viewport (set in nxdk_set_viewport). Lighting/culling off (fast3d culls on
    // the CPU and bakes lighting into the vertex colours).
    static const float ident[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
    };
    // Per-frame draw-count trace (first ~50 frames): disambiguates "geometry submitted
    // every frame but black" (state/present bug) from "game stopped submitting" (logic).
    {
        extern int g_NxdkFrameDraws;
        extern int g_NxdkZMin, g_NxdkZMax;
        extern int g_NxdkVtxDrops, g_NxdkVtxHighWater;
        static unsigned s_fr = 0;
        if (s_fr < 50) {
            xboxTracef("rdr: FRAME %u draws=%d zmin=%d zmax=%d (x1000)",
                       s_fr, g_NxdkFrameDraws, g_NxdkZMin, g_NxdkZMax);
            s_fr++;
        }
        // Arena-overflow report (trap T2), rate-limited to ~1/s: without it, dropped
        // draws (the HUD draws last!) look like random UI flicker on heavy frames.
        if (g_NxdkVtxDrops) {
            static unsigned s_dropwarn = 0;
            if ((s_dropwarn++ % 60) == 0) {
                xboxTracef("rdr: ARENA OVERFLOW dropped=%d hiwater=%d/%d verts",
                           g_NxdkVtxDrops, g_NxdkVtxHighWater, (int)g.vtx_caps);
            }
            g_NxdkVtxDrops = 0;
        }
        g_NxdkFrameDraws = 0;
        g_NxdkZMin = 100000; g_NxdkZMax = -100000;
    }
    NXDK_RTRACE("rdr: start_frame");
    g.vtx_frame ^= 1;   // alternate vertex buffers so the GPU isn't still reading the one we overwrite
    g.vtx_off = 0;      // reset the per-frame vertex arena bump allocator
    g.tex_applied = 0;  // reset the per-frame texture-bind cache
    nxdk_oneshot_state();
    uint32_t *p = pb_begin();
    p = xgu_set_transform_execution_mode(p, XGU_FIXED, XGU_RANGE_MODE_PRIVATE);
    p = xgu_set_skin_mode(p, XGU_SKIN_MODE_OFF);
    p = xgu_set_lighting_enable(p, false);
    p = xgu_set_cull_face_enable(p, false);
    // The NV2A fixed-function transform needs modelview + projection set, not just the
    // composite (matching nxdk-sdl3's SDL_render_xgu). All identity -> the GPU does only
    // the perspective divide + viewport on fast3d's clip-space verts.
    p = xgu_set_model_view_matrix(p, 0, ident);
    p = xgu_set_projection_matrix(p, ident);
    p = xgu_set_composite_matrix(p, ident);
    pb_end(p);
    nxdk_setup_combiner();
    g.combiner_textured = 0; // setup_combiner programmed the unlit input combiner
    NXDK_RTRACE("rdr: start_frame ok");
}

static void nxdk_end_frame(void) { /* push buffer is flushed by the WM swap */ }
static void nxdk_finish_render(void) { /* TODO(pbkit): pb_finished / wait for GPU idle */ }

// ---------------------------------------------------------------------------------
// Framebuffers -- effects (mirror, Z-prepass) use these. Minimal bookkeeping for now.
// ---------------------------------------------------------------------------------

static int nxdk_create_framebuffer(void) {
    return g.next_framebuffer_id++;
}

static void nxdk_update_framebuffer_parameters(int fb_id, uint32_t width, uint32_t height, uint32_t msaa_level,
                                               bool opengl_invert_y, bool render_target, bool has_depth_buffer,
                                               bool can_extract_depth) {
    (void)fb_id; (void)width; (void)height; (void)msaa_level; (void)opengl_invert_y;
    (void)render_target; (void)has_depth_buffer; (void)can_extract_depth;
    // TODO(pbkit): (re)allocate the offscreen surface(s) for fb_id. NV2A has no MSAA
    // in the GL sense -- treat msaa_level as 1. Mind the 64 MB / 32 MB GPU budget.
}

static bool nxdk_start_draw_to_framebuffer(int fb_id, float noise_scale) {
    (void)fb_id; (void)noise_scale;
    // TODO(pbkit): point the NV2A render target at fb_id (0 = on-screen). Return true
    // when ready to draw.
    return true;
}

static void nxdk_copy_framebuffer(int fb_dst, int fb_src, int left, int top, bool flip_y, bool use_back) {
    (void)fb_dst; (void)fb_src; (void)left; (void)top; (void)flip_y; (void)use_back;
    // TODO(nv2a): blit fb_src -> fb_dst. This is also where the lo-res render target
    // gets UPSCALE-blitted to the HD scanout buffer (M4).
}

static void nxdk_clear_framebuffer(bool clear_color, bool clear_depth) {
    (void)clear_color; (void)clear_depth;
    // TODO(pbkit): pb_erase / NV097_CLEAR_SURFACE for colour and/or depth.
}

static void nxdk_resolve_msaa_color_buffer(int fb_id_target, int fb_id_source) {
    (void)fb_id_target; (void)fb_id_source;
    // No MSAA on NV2A; a same-size copy at most.
}

static void *nxdk_get_framebuffer_texture_id(int fb_id) {
    (void)fb_id;
    // TODO(pbkit): return the texture handle for fb_id's colour surface (used when an
    // fb is sampled as a texture, e.g. the mirror/security-cam path).
    return NULL;
}

static void nxdk_select_texture_fb(int fb_id) {
    (void)fb_id;
    // Render-to-texture framebuffer effects (blurred menu backdrop, in-game fullscreen
    // effects) aren't implemented -- there's no colour surface to sample. Flag it so
    // draw_triangles SKIPS the fullscreen quad instead of sampling whatever real texture
    // was last bound (a font glyph), which otherwise stretches across the screen as a
    // giant letter / "huge texture in the way".
    g.tex_is_fb = true;
}

// ---------------------------------------------------------------------------------
// Model-view-projection (cache path) -- immediate path uses identity, so a no-op
// store is fine until the display-list cache is supported (it is OFF on Xbox).
// ---------------------------------------------------------------------------------

static void nxdk_set_mvp(const float m[16]) {
    (void)m;
    // TODO(nv2a): only needed if the display-list cache is ever enabled here (it is
    // default-off on Xbox -- needs vertex-shader features the NV2A lacks). Identity
    // for the immediate path.
}

// ---------------------------------------------------------------------------------
// Display-list cache + shader-side palette -- UNSUPPORTED on NV2A (no persistent
// GPU vertex buffers + vertex-shader texture sampling the cache relies on). Returning
// 0 from the create_* hooks makes gfx_pc fall back to the immediate path cleanly,
// which is exactly what we want (the cache is default-off on Xbox anyway).
// ---------------------------------------------------------------------------------

static uint32_t nxdk_cache_create_buffer(const float *data, size_t num_floats) { (void)data; (void)num_floats; return 0; }
static void nxdk_cache_delete_buffer(uint32_t id) { (void)id; }
static void nxdk_cache_replay_begin(uint32_t id) { (void)id; }
static void nxdk_cache_draw(struct ShaderProgram *prg, size_t base_float, size_t num_tris) { (void)prg; (void)base_float; (void)num_tris; }
static void nxdk_cache_set_cull(int mode, bool front_ccw) { (void)mode; (void)front_ccw; }
static void nxdk_cache_replay_end(void) {}
static void nxdk_set_fog_params(int use_vertex_fog, float fog_mul, float fog_off) { (void)use_vertex_fog; (void)fog_mul; (void)fog_off; }
static uint32_t nxdk_cache_create_palette(void) { return 0; }
static void nxdk_cache_delete_palette(uint32_t id) { (void)id; }
static void nxdk_cache_upload_palette(uint32_t id, const void *rgba, int count) { (void)id; (void)rgba; (void)count; }
static void nxdk_cache_bind_palette(uint32_t id, int count) { (void)id; (void)count; }
static void nxdk_set_palette_enable(int enable) { (void)enable; }
static void nxdk_set_shade_routing(int packed) { (void)packed; }

// ---------------------------------------------------------------------------------
// Rendering API vtable (designated initializers -- C++20, robust to reordering).
// ---------------------------------------------------------------------------------

struct GfxRenderingAPI gfx_nxdk_api = {
    .get_name = nxdk_get_name,
    .get_max_texture_size = nxdk_get_max_texture_size,
    .get_clip_parameters = nxdk_get_clip_parameters,
    .unload_shader = nxdk_unload_shader,
    .load_shader = nxdk_load_shader,
    .create_and_load_new_shader = nxdk_create_and_load_new_shader,
    .lookup_shader = nxdk_lookup_shader,
    .shader_get_info = nxdk_shader_get_info,
    .clear_shaders = nxdk_clear_shaders,
    .new_texture = nxdk_new_texture,
    .select_texture = nxdk_select_texture,
    .upload_texture = nxdk_upload_texture,
    .set_sampler_parameters = nxdk_set_sampler_parameters,
    .set_depth_mode = nxdk_set_depth_mode,
    .set_depth_range = nxdk_set_depth_range,
    .set_viewport = nxdk_set_viewport,
    .set_scissor = nxdk_set_scissor,
    .set_use_alpha = nxdk_set_use_alpha,
    .draw_triangles = nxdk_draw_triangles,
    .init = nxdk_init,
    .on_resize = nxdk_on_resize,
    .start_frame = nxdk_start_frame,
    .end_frame = nxdk_end_frame,
    .finish_render = nxdk_finish_render,
    .create_framebuffer = nxdk_create_framebuffer,
    .update_framebuffer_parameters = nxdk_update_framebuffer_parameters,
    .start_draw_to_framebuffer = nxdk_start_draw_to_framebuffer,
    .copy_framebuffer = nxdk_copy_framebuffer,
    .clear_framebuffer = nxdk_clear_framebuffer,
    .resolve_msaa_color_buffer = nxdk_resolve_msaa_color_buffer,
    .get_framebuffer_texture_id = nxdk_get_framebuffer_texture_id,
    .select_texture_fb = nxdk_select_texture_fb,
    .delete_texture = nxdk_delete_texture,
    .set_texture_filter = nxdk_set_texture_filter,
    .get_texture_filter = nxdk_get_texture_filter,
    .set_mipmap_filter = nxdk_set_mipmap_filter,
    .set_anisotropy_level = nxdk_set_anisotropy_level,
    .get_max_anisotropy_level = nxdk_get_max_anisotropy_level,
    .set_mvp = nxdk_set_mvp,
    .cache_create_buffer = nxdk_cache_create_buffer,
    .cache_delete_buffer = nxdk_cache_delete_buffer,
    .cache_replay_begin = nxdk_cache_replay_begin,
    .cache_draw = nxdk_cache_draw,
    .cache_set_cull = nxdk_cache_set_cull,
    .cache_replay_end = nxdk_cache_replay_end,
    .set_fog_params = nxdk_set_fog_params,
    .cache_create_palette = nxdk_cache_create_palette,
    .cache_delete_palette = nxdk_cache_delete_palette,
    .cache_upload_palette = nxdk_cache_upload_palette,
    .cache_bind_palette = nxdk_cache_bind_palette,
    .set_palette_enable = nxdk_set_palette_enable,
    .set_shade_routing = nxdk_set_shade_routing,
};

// =================================================================================
// Window manager. Use this only if a NATIVE pbkit window is wanted; if nxdk-sdl3's
// SDL3 video layer handles windowing/input, keep gfx_sdl as the WM and use only
// gfx_nxdk_api above for rendering. (Many NXDK ports do windowing+input via SDL and
// rendering via pbkit, which is the lower-risk split.)
// =================================================================================

static double nxdk_now(void) {
    // KeQueryPerformanceCounter (ACPI timer, ~3.375 MHz) -- pdclib's clock() was
    // too coarse for frame pacing and useless for the GPU-wait telemetry below.
    static double inv;
    if (inv == 0.0) {
        inv = 1.0 / (double)(int64_t)KeQueryPerformanceFrequency();
    }
    return (double)(int64_t)KeQueryPerformanceCounter() * inv;
}

// ---------------------------------------------------------------------------------
// HD video modes (Milestone 4). The real availability gate is XVideoListModes --
// nxdk's encoder/AV-pack enumeration (NXDK has no XGetVideoFlags dashboard query;
// the inert XC_VIDEO_FLAGS scaffold in port/src/video.c is superseded by this).
// 640x480 is always offered as the guaranteed floor; 1080i can additionally be
// pinned behind a RAM gate (NXDK_1080I_MIN_MIB, 0 = attempt on a stock 64 MB box --
// its framebuffers + Z eat ~25 MB of the low-64 MB contiguous pool, so if hardware
// testing shows texture-alloc failures at 1080i, raise this to ~96 to demand a
// 128 MB console). Boot default stays 640x480; HD engages only when the user picks
// it (Extended > Video resolution list / Video.DefaultWidth/Height in pd.ini).
// ---------------------------------------------------------------------------------

#define NXDK_1080I_MIN_MIB 0

static const struct { int w, h; } nxdk_mode_ladder[] = {
    { 1920, 1080 }, // 1080i (encoder interlaces on scanout)
    { 1280,  720 }, // 720p
    {  640,  480 }, // 480i/480p floor -- always available
};
#define NXDK_NUM_LADDER (int)(sizeof(nxdk_mode_ladder) / sizeof(nxdk_mode_ladder[0]))

static int nxdk_avail_modes[NXDK_NUM_LADDER][2];
static int nxdk_num_avail;
static int nxdk_pending_w, nxdk_pending_h; // latched mode switch, applied at frame start

static uint32_t nxdk_total_ram_mib(void) {
    MM_STATISTICS ms;
    ms.Length = sizeof(ms);
    if (NT_SUCCESS(MmQueryStatistics(&ms))) {
        return (uint32_t)(((uint64_t)ms.TotalPhysicalPages * 4096ull) >> 20);
    }
    return 64;
}

// True iff the video encoder + AV pack + dashboard settings can output w x h at
// 32 bpp (a composite-cable box lists no HD modes, so HD is filtered out there).
static bool nxdk_encoder_allows(int w, int h) {
    VIDEO_MODE vm;
    void *iter = NULL;
    while (XVideoListModes(&vm, 32, 0, &iter)) {
        if (vm.width == w && vm.height == h) {
            return true;
        }
    }
    return false;
}

static void nxdk_build_mode_list(void) {
    nxdk_num_avail = 0;
    for (int i = 0; i < NXDK_NUM_LADDER; i++) {
        const int w = nxdk_mode_ladder[i].w, h = nxdk_mode_ladder[i].h;
        if (w == 1920 && NXDK_1080I_MIN_MIB && nxdk_total_ram_mib() < NXDK_1080I_MIN_MIB) {
            continue; // 1080i pinned behind a RAM gate (see above)
        }
        if (w != 640 && !nxdk_encoder_allows(w, h)) {
            continue; // 640x480 is the unconditional floor
        }
        nxdk_avail_modes[nxdk_num_avail][0] = w;
        nxdk_avail_modes[nxdk_num_avail][1] = h;
        nxdk_num_avail++;
    }
    xboxTracef("PDBOOT: video modes available: %d (ram %u MiB)", nxdk_num_avail, nxdk_total_ram_mib());
}

// Exact-match snap to an available mode; anything unknown falls to 640x480.
static void nxdk_snap_mode(int w, int h, int *out_w, int *out_h) {
    for (int i = 0; i < nxdk_num_avail; i++) {
        if (nxdk_avail_modes[i][0] == w && nxdk_avail_modes[i][1] == h) {
            *out_w = w; *out_h = h;
            return;
        }
    }
    *out_w = 640; *out_h = 480;
}

// Set the encoder mode + bring pbkit up on it. Returns false if pb_init fails
// (e.g. its framebuffer allocation OOMs at 1080i) -- callers step down the ladder.
static bool nxdk_video_mode_up(int w, int h) {
    XVideoSetMode(w, h, 32, REFRESH_DEFAULT);
    int err = pb_init();
    if (err) {
        xboxTracef("PDBOOT: pb_init FAILED %d at %dx%d", err, w, h);
        return false;
    }
    // pbkit can leave video output disabled after pb_init; re-enable it (matches
    // SDL_render_xgu). If this was off, the scanout would hold stale VRAM and never
    // show our rendered frames -- the exact "held previous framebuffer" symptom.
    XVideoSetVideoEnable(true);
    pb_show_front_screen();
    g.width = (uint32_t)pb_back_buffer_width();
    g.height = (uint32_t)pb_back_buffer_height();
    xboxTracef("PDBOOT: pb_init ok %dx%d", (int)g.width, (int)g.height);
    return true;
}

static void wm_init(const struct GfxWindowInitSettings *settings) {
    g.time0 = nxdk_now();
    nxdk_build_mode_list();
    // Requested mode = Video.DefaultWidth/Height via video.c (640x480 by default);
    // snap to an available mode, then walk DOWN the ladder on pb_init failure so an
    // over-ambitious config can't black-screen the console.
    int w = 640, h = 480;
    nxdk_snap_mode(settings ? (int)settings->width : 640,
                   settings ? (int)settings->height : 480, &w, &h);
    while (!nxdk_video_mode_up(w, h)) {
        if (w == 640) {
            return; // even 480 failed -- boot trace has the pb_init error
        }
        pb_kill(); // release the partial init before retrying (best effort)
        w = (w == 1920) ? 1280 : 640;
        h = (w == 1280) ? 720 : 480;
    }
}

static void wm_close(void) { pb_kill(); }

static int wm_get_display_mode(int modenum, int *out_w, int *out_h) {
    // The filtered HD list, high -> low (videoInitDisplayModes expects that order).
    if (modenum < 0 || modenum >= nxdk_num_avail) {
        return 0;
    }
    if (out_w) *out_w = nxdk_avail_modes[modenum][0];
    if (out_h) *out_h = nxdk_avail_modes[modenum][1];
    return 1;
}

static int wm_get_current_display_mode(int *out_w, int *out_h) {
    if (out_w) *out_w = (int)g.width;
    if (out_h) *out_h = (int)g.height;
    return 1;
}

static int wm_get_num_display_modes(void) { return nxdk_num_avail; }

static int32_t wm_get_fullscreen_state(void) { return 1; /* Xbox is always fullscreen */ }
static void wm_set_fullscreen_changed_callback(void (*cb)(bool)) { (void)cb; }
static void wm_set_fullscreen(bool enable) { (void)enable; }
static void wm_set_fullscreen_exclusive(bool exc) { (void)exc; }
static void wm_set_fullscreen_flag(int32_t mode) { (void)mode; }
static int32_t wm_get_fullscreen_flag_mode(void) { return 0; }
static int32_t wm_get_maximized_state(void) { return 0; }
static void wm_set_maximize(bool enable) { (void)enable; }
static void wm_get_active_window_refresh_rate(uint32_t *rr) { if (rr) *rr = 60; }
static void wm_set_cursor_visibility(bool visible) { (void)visible; /* no cursor */ }

static void wm_set_closest_resolution(int32_t width, int32_t height, bool should_center) {
    // In-game resolution pick (videoSetDisplayMode -- the Xbox always reports
    // fullscreen, so this is the path the menu takes). Snap to an available mode
    // and LATCH it; the actual pbkit teardown/re-init happens at the top of the
    // next wm_start_frame, when the previous frame is fully presented and the GPU
    // drained -- pb_kill mid-frame would strand queued draws. vidWidth/Height are
    // persisted by video.c, so the choice also sticks for the next boot.
    (void)should_center;
    int w = 640, h = 480;
    nxdk_snap_mode((int)width, (int)height, &w, &h);
    if ((uint32_t)w != g.width || (uint32_t)h != g.height) {
        nxdk_pending_w = w;
        nxdk_pending_h = h;
    }
}

static void wm_set_dimensions(uint32_t width, uint32_t height, int32_t posX, int32_t posY) {
    (void)posX; (void)posY;
    g.width = width;
    g.height = height;
}

static void wm_get_dimensions(uint32_t *width, uint32_t *height, int32_t *posX, int32_t *posY) {
    if (width) *width = g.width;
    if (height) *height = g.height;
    if (posX) *posX = 0;
    if (posY) *posY = 0;
}

static void wm_get_centered_positions(int32_t width, int32_t height, int32_t *posX, int32_t *posY) {
    (void)width; (void)height;
    if (posX) *posX = 0;
    if (posY) *posY = 0;
}

static void wm_handle_events(void) {
    // TODO: poll Xbox input here, OR leave input to SDL (nxdk-sdl3) and keep this a
    // no-op if gfx_sdl is the real WM.
}

// Bind the back buffer as the NV2A render surface, replicating SDL_render_xgu's
// XBOX_SetRenderTarget(NULL). pb_target_back_buffer() points the colour DMA at the
// current back buffer but does NOT set the surface CLIP rectangle / format / pitch --
// and with a zero surface clip the NV2A discards every fragment, so valid draws write
// nothing to any framebuffer (black despite 68 draws/frame). Push the full surface
// state each frame so the back buffer is a valid render target.
extern "C" unsigned int pb_ColorFmt; // from pbkit.c (C symbol -- file scope, no C++ mangling)

static void nxdk_bind_back_surface(void) {
    const uint32_t pitch  = (uint32_t)pb_back_buffer_pitch();
    const uint32_t cw     = (uint32_t)pb_back_buffer_width();
    const uint32_t ch     = (uint32_t)pb_back_buffer_height();
    const uint32_t zpitch = cw * 4; // Z24S8 = 4 bytes/pixel
    const uint32_t format =
        XGU_MASK(NV097_SET_SURFACE_FORMAT_COLOR, pb_ColorFmt) |
        XGU_MASK(NV097_SET_SURFACE_FORMAT_ZETA, NV097_SET_SURFACE_FORMAT_ZETA_Z24S8) |
        XGU_MASK(NV097_SET_SURFACE_FORMAT_TYPE, NV097_SET_SURFACE_FORMAT_TYPE_PITCH);
    uint32_t *p = pb_begin();
    p = pb_push1(p, NV097_WAIT_FOR_IDLE, 0);
    p = pb_push1(p, NV097_SET_CONTEXT_DMA_COLOR, DMA_CHANNEL_PIXEL_RENDERER);
    p = pb_push1(p, NV097_SET_SURFACE_PITCH,
                 XGU_MASK(NV097_SET_SURFACE_PITCH_COLOR, pitch) |
                 XGU_MASK(NV097_SET_SURFACE_PITCH_ZETA, zpitch));
    p = pb_push1(p, NV097_SET_SURFACE_COLOR_OFFSET, 0); // offset from the DMA address
    p = pb_push1(p, NV097_SET_SURFACE_CLIP_HORIZONTAL,
                 XGU_MASK(NV097_SET_SURFACE_CLIP_HORIZONTAL_X, 0) |
                 XGU_MASK(NV097_SET_SURFACE_CLIP_HORIZONTAL_WIDTH, cw));
    p = pb_push1(p, NV097_SET_SURFACE_CLIP_VERTICAL,
                 XGU_MASK(NV097_SET_SURFACE_CLIP_VERTICAL_Y, 0) |
                 XGU_MASK(NV097_SET_SURFACE_CLIP_VERTICAL_HEIGHT, ch));
    p = pb_push1(p, NV097_SET_SURFACE_FORMAT, format);
    pb_end(p);
}

// CLEAR_SURFACE register + bits, guarded in case an older nv2a_regs.h lacks them.
#ifndef NV097_SET_ZSTENCIL_CLEAR_VALUE
#define NV097_SET_ZSTENCIL_CLEAR_VALUE 0x00001D8C
#endif
#ifndef NV097_SET_COLOR_CLEAR_VALUE
#define NV097_SET_COLOR_CLEAR_VALUE 0x00001D90
#endif
#ifndef NV097_CLEAR_SURFACE
#define NV097_CLEAR_SURFACE 0x00001D94
#endif
#ifndef NV097_CLEAR_SURFACE_Z
#define NV097_CLEAR_SURFACE_Z       0x00000001
#define NV097_CLEAR_SURFACE_STENCIL 0x00000002
#define NV097_CLEAR_SURFACE_R       0x00000010
#define NV097_CLEAR_SURFACE_G       0x00000020
#define NV097_CLEAR_SURFACE_B       0x00000040
#define NV097_CLEAR_SURFACE_A       0x00000080
#endif
#define NXDK_CLEAR_ALL (NV097_CLEAR_SURFACE_Z | NV097_CLEAR_SURFACE_STENCIL \
    | NV097_CLEAR_SURFACE_R | NV097_CLEAR_SURFACE_G | NV097_CLEAR_SURFACE_B | NV097_CLEAR_SURFACE_A)

static bool wm_start_frame(void) {
    // Apply a latched video-mode switch here: the previous frame is fully presented
    // and the GPU drained (wm_swap_buffers_end), so this is the one clean boundary
    // for a pbkit teardown/re-init. Our own GPU allocations (textures, vertex
    // arenas) are untouched by pb_kill; the oneshot state (clear rect, texgen,
    // matrices, scissor) is re-pushed via the reset flag, and nxdk_start_frame
    // re-establishes the per-frame state as always.
    if (nxdk_pending_w) {
        const int nw = nxdk_pending_w, nh = nxdk_pending_h;
        nxdk_pending_w = 0;
        xboxTracef("rdr: video mode switch %dx%d -> %dx%d", (int)g.width, (int)g.height, nw, nh);
        pb_kill();
        if (!nxdk_video_mode_up(nw, nh)) {
            pb_kill();
            nxdk_video_mode_up(640, 480); // ladder floor -- never leave the console dark
        }
        g_NxdkOneshotDone = false;
    }

    // Canonical nxdk pbkit double-buffered loop: every frame, wait for vblank, reset the
    // push buffer, target the current back buffer, then explicitly bind it as the render
    // surface (clip/format/pitch -- see nxdk_bind_back_surface).
    pb_wait_for_vbl();
    pb_reset();
    pb_target_back_buffer();
    nxdk_bind_back_surface();
    const int w = pb_back_buffer_width();
    const int h = pb_back_buffer_height();
    // ONE combined colour+Z+stencil CLEAR_SURFACE instead of pbkit's separate
    // pb_erase_depth_stencil_buffer + pb_fill (each is its own full-screen clear
    // pass; the saved pass is real memory bandwidth at 720p/1080i). The clear rect
    // is pushed per frame -- not in the oneshot -- so a runtime mode switch resizes
    // it. Values: Z24S8 packs (z << 8) | stencil, so far+0 = 0xFFFFFF00; colour is
    // opaque black, same as the old pb_fill.
    uint32_t *p = pb_begin();
    p = xgu_set_clear_rect_horizontal(p, 0, w);
    p = xgu_set_clear_rect_vertical(p, 0, h);
    p = pb_push1(p, NV097_SET_ZSTENCIL_CLEAR_VALUE, 0xFFFFFF00);
    p = pb_push1(p, NV097_SET_COLOR_CLEAR_VALUE, 0xFF000000);
    p = pb_push1(p, NV097_CLEAR_SURFACE, NXDK_CLEAR_ALL);
    pb_end(p);
    pb_erase_text_screen();
    return true;
}
static void wm_swap_buffers_begin(void) { /* present happens in swap_buffers_end */ }

// Perf-HUD GPU slot (video.c; <0 = n/a). What we can measure without GPU timestamps
// is the CPU-side wait for the GPU to drain at present: ~0 when CPU-bound, grows when
// GPU-bound. Not true GPU frame time, but it is the number that says "the NV2A is the
// bottleneck" -- exactly what HD-resolution testing needs. Smoothed like cpuFrameMs.
extern "C" { extern double g_VideoGpuFrameMs; }

static void wm_swap_buffers_end(void) {
    // Drain the GPU and flip the completed back buffer to the front.
    const double t0 = nxdk_now();
    while (pb_busy()) { }
    while (pb_finished()) { }
    const double waitms = (nxdk_now() - t0) * 1000.0;
    g_VideoGpuFrameMs = (g_VideoGpuFrameMs < 0.0) ? waitms
                                                  : g_VideoGpuFrameMs * 0.9 + waitms * 0.1;
}
static double wm_get_time(void) { return nxdk_now() - g.time0; }
static int32_t wm_get_target_fps(void) { return g.target_fps; }
static void wm_set_target_fps(int fps) { g.target_fps = fps; }
static bool wm_can_disable_vsync(void) { return false; }
static void *wm_get_window_handle(void) { return NULL; }
static void wm_set_window_title(const char *title) { (void)title; }
static int wm_get_swap_interval(void) { return 1; }
static bool wm_set_swap_interval(int interval) { (void)interval; return false; }
static void wm_set_taskbar_progress(int state, float value) { (void)state; (void)value; }
static int wm_get_refresh_rates(int width, int height, float *out, int max) {
    (void)width; (void)height;
    if (max > 0 && out) { out[0] = 60.0f; return 1; }
    return 0;
}
static void wm_set_refresh_rate(float hz) { (void)hz; }

struct GfxWindowManagerAPI gfx_nxdk_wm = {
    .init = wm_init,
    .close = wm_close,
    .get_display_mode = wm_get_display_mode,
    .get_current_display_mode = wm_get_current_display_mode,
    .get_num_display_modes = wm_get_num_display_modes,
    .get_fullscreen_state = wm_get_fullscreen_state,
    .set_fullscreen_changed_callback = wm_set_fullscreen_changed_callback,
    .set_fullscreen = wm_set_fullscreen,
    .set_fullscreen_exclusive = wm_set_fullscreen_exclusive,
    .set_fullscreen_flag = wm_set_fullscreen_flag,
    .get_fullscreen_flag_mode = wm_get_fullscreen_flag_mode,
    .get_maximized_state = wm_get_maximized_state,
    .set_maximize = wm_set_maximize,
    .get_active_window_refresh_rate = wm_get_active_window_refresh_rate,
    .set_cursor_visibility = wm_set_cursor_visibility,
    .set_closest_resolution = wm_set_closest_resolution,
    .set_dimensions = wm_set_dimensions,
    .get_dimensions = wm_get_dimensions,
    .get_centered_positions = wm_get_centered_positions,
    .handle_events = wm_handle_events,
    .start_frame = wm_start_frame,
    .swap_buffers_begin = wm_swap_buffers_begin,
    .swap_buffers_end = wm_swap_buffers_end,
    .get_time = wm_get_time,
    .get_target_fps = wm_get_target_fps,
    .set_target_fps = wm_set_target_fps,
    .can_disable_vsync = wm_can_disable_vsync,
    .get_window_handle = wm_get_window_handle,
    .set_window_title = wm_set_window_title,
    .get_swap_interval = wm_get_swap_interval,
    .set_swap_interval = wm_set_swap_interval,
    .set_taskbar_progress = wm_set_taskbar_progress,
    .get_refresh_rates = wm_get_refresh_rates,
    .set_refresh_rate = wm_set_refresh_rate,
};

#endif // PLATFORM_NXDK
