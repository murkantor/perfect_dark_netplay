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
#include <string.h>
#include <time.h>

#include "gfx_rendering_api.h"
#include "gfx_window_manager_api.h"
#include "gfx_cc.h"
#include "gfx_nxdk.h"

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
} g;

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
    return ++g.next_texture_id;
}

static void nxdk_select_texture(int tile, uint32_t texture_id, bool linear_filter) {
    g.cur_tile = tile;
    (void)texture_id; (void)linear_filter;
    // TODO(pbkit): bind texture_id to NV2A texture stage `tile`.
}

static void nxdk_upload_texture(const uint8_t *rgba32_buf, uint32_t width, uint32_t height, bool gen_mipmaps) {
    (void)rgba32_buf; (void)width; (void)height; (void)gen_mipmaps;
    // TODO(pbkit): allocate VRAM, swizzle RGBA8 (NV2A wants swizzled or linear-pitch
    // textures), upload, build mips if requested, and key it to the current id.
}

static void nxdk_set_sampler_parameters(int sampler, bool linear_filter, uint32_t cms, uint32_t cmt, bool mipmaps) {
    (void)sampler; (void)linear_filter; (void)cms; (void)cmt; (void)mipmaps;
    // TODO(nv2a): set the texture-stage filter + wrap (clamp/wrap/mirror) modes.
}

static void nxdk_delete_texture(uint32_t texID) {
    (void)texID;
    // TODO(pbkit): free the VRAM backing texID.
}

static void nxdk_set_texture_filter(enum FilteringMode mode) { g.tex_filter = mode; }
static enum FilteringMode nxdk_get_texture_filter(void) { return g.tex_filter; }
static void nxdk_set_mipmap_filter(enum MipmapFilteringMode mode) { (void)mode; }
static void nxdk_set_anisotropy_level(int level) { (void)level; }
static int nxdk_get_max_anisotropy_level(void) { return 1; /* TODO(nv2a): NV2A supports up to 4 */ }

// ---------------------------------------------------------------------------------
// Render state
// ---------------------------------------------------------------------------------

static void nxdk_set_depth_mode(bool depth_test, bool depth_update, bool depth_compare,
                                bool depth_source_prim, uint16_t zmode) {
    (void)depth_test; (void)depth_update; (void)depth_compare; (void)depth_source_prim; (void)zmode;
    // TODO(nv2a): NV_PGRAPH depth-test enable / write-mask / func + the zmode bias.
}

static void nxdk_set_depth_range(float znear, float zfar) {
    (void)znear; (void)zfar;
    // TODO(nv2a): depth range / viewport Z scale-bias.
}

static void nxdk_set_viewport(int x, int y, int width, int height) {
    (void)x; (void)y; (void)width; (void)height;
    // TODO(nv2a): NV2A viewport (offset + scale). Note get_clip_parameters depends
    // on the Y convention chosen here.
}

static void nxdk_set_scissor(int x, int y, int width, int height) {
    (void)x; (void)y; (void)width; (void)height;
    // TODO(nv2a): NV2A clip-rectangle.
}

static void nxdk_set_use_alpha(bool use_alpha, bool modulate) {
    (void)use_alpha; (void)modulate;
    // TODO(nv2a): alpha-blend enable + the blend func (modulate vs straight alpha).
}

// ---------------------------------------------------------------------------------
// Drawing -- THE central TODO.
// ---------------------------------------------------------------------------------

static void nxdk_draw_triangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) {
    (void)buf_vbo; (void)buf_vbo_len; (void)buf_vbo_num_tris;
    // TODO(nv2a): submit buf_vbo as a triangle list through pbkit. The per-vertex
    // float layout matches g.cur_shader (4 pos + per-texture UVs + per-input combiner
    // colours + optional fog/grayscale) -- mirror gfx_opengl's draw_triangles
    // attribute walk. Positions arrive in CLIP space already (the immediate path),
    // so program an identity transform (or fold uMVP/the fixups via set_mvp). Push
    // the inline vertex data + a DRAW_ARRAYS to the NV2A.
}

// ---------------------------------------------------------------------------------
// Frame lifecycle
// ---------------------------------------------------------------------------------

static void nxdk_init(void) {
    memset(&g, 0, sizeof(g));
    g.tex_filter = FILTER_LINEAR;
    g.target_fps = 60;
    g.next_framebuffer_id = 1;
    // TODO(pbkit): pb_init() if this backend owns the device (vs. nxdk-sdl3 owning
    // the window). Allocate the lo-res render target here (render-low + upscale per
    // the M4 HD-mode plan).
}

static void nxdk_on_resize(void) { /* Xbox modes are fixed; nothing to do */ }
static void nxdk_start_frame(void) { /* TODO(pbkit): pb_wait_for_vbl / begin push buffer */ }
static void nxdk_end_frame(void) { /* TODO(pbkit): flush the push buffer */ }
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
    // TODO(pbkit): bind fb_id's colour surface as the active texture.
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
    // TODO(nv2a): replace with KeQueryPerformanceCounter/Frequency for precision.
    // clock() (pdclib) is enough to get the frame loop / fps + CPU% HUD running.
    return (double)clock() / (double)CLOCKS_PER_SEC;
}

static void wm_init(const struct GfxWindowInitSettings *settings) {
    g.width = settings ? settings->width : 640;
    g.height = settings ? settings->height : 480;
    g.time0 = nxdk_now();
    // TODO(pbkit): bring up the NV2A display (pb_init + set the initial video mode;
    // see the M4 HD-mode table / xboxVideoModeAvailable in video.c).
}

static void wm_close(void) { /* TODO(pbkit): pb_kill */ }

static int wm_get_display_mode(int modenum, int *out_w, int *out_h) {
    // TODO(M4): enumerate the Xbox-allowed modes (480i/480p/720p/1080i) filtered by
    // xboxVideoModeAvailable (video.c). Until wired, advertise just the current mode.
    if (modenum != 0) {
        return 0;
    }
    if (out_w) *out_w = (int)g.width;
    if (out_h) *out_h = (int)g.height;
    return 1;
}

static int wm_get_current_display_mode(int *out_w, int *out_h) {
    if (out_w) *out_w = (int)g.width;
    if (out_h) *out_h = (int)g.height;
    return 1;
}

static int wm_get_num_display_modes(void) { return 1; /* TODO(M4): the filtered HD list */ }

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
    (void)should_center;
    g.width = (uint32_t)width;
    g.height = (uint32_t)height;
    // TODO(pbkit): switch the NV2A video mode to the closest allowed mode.
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

static bool wm_start_frame(void) { return true; }
static void wm_swap_buffers_begin(void) { /* TODO(pbkit): present the back buffer */ }
static void wm_swap_buffers_end(void) { /* TODO(pbkit): pb_wait_for_vbl */ }
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
