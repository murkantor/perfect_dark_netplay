# Original Xbox (NXDK) build — status & notes

Experimental OG Xbox port on branch **`port-net-xbox`** (branched from
`port-net-predict`; merge `port-net-predict` forward over time). Toolchain:
**NXDK** (https://github.com/XboxDev/nxdk) + **SDL3 for NXDK**
(https://github.com/Ryzee119/nxdk-sdl3).

> **Status: BOOTS END-TO-END; NV2A present (Phase 0) verified (2026-06-17).** The
> engine boots all the way through init + ROM load + `lvReset` (stage load) into the
> native `gfx_nxdk` render loop, which clears the back buffer and presents every frame
> — **confirmed by a blue screen + ticking frame counter in xemu**. The renderer still
> draws no geometry (`draw_triangles`/textures/combiner are no-op stubs) — that's the
> next milestone. Verified in xemu (64 MB). This doc is the living checklist.

## Why / target

- The OG Xbox is **x86 / little-endian** (Pentium III) — so, unlike the Switch port,
  there is **no endianness or new-architecture work**; the existing x86 desktop code
  path applies. `src/include/platform.h` adds an `NXDK` branch **before** `_WIN32`
  (NXDK's clang triple is win32-like and may define `_WIN32`).
- **64 MB RAM is the hard target.** Develop and budget against **64 MB**. A 128 MB
  upgraded console is a **fallback only** — if we ever lean on it, downsize back to
  64 MB. The dlcache stays **off** here (`Video.DlCache=0`): it costs GPU-resident
  memory and needs vertex-shader texture sampling the NV2A lacks.

## Build (once NXDK is set up)

```sh
# bootstrap NXDK once per its README (its `make` build), then:
export NXDK_DIR=/path/to/nxdk
# nxdk-sdl3 is a CMake subproject (no install). EITHER clone it and point at it
# (offline), OR omit NXDK_SDL3_DIR to have CMake FetchContent it from master:
git clone --recursive https://github.com/Ryzee119/nxdk-sdl3.git /path/to/nxdk-sdl3
export NXDK_SDL3_DIR=/path/to/nxdk-sdl3
./tools/buildscripts/xbox_nxdk.sh build_xbox -DROMID=ntsc-final
# -> build_xbox/default.xbe  (run in xemu or on hardware)
```

Internally this drives `cmake/toolchain-nxdk.cmake`, which `include()`s NXDK's own
`$NXDK_DIR/share/toolchain-nxdk.cmake` (compilers/archiver/linker/suffixes) and adds
`XBOX_NXDK` + the `cxbe` path, plus the `XBOX_NXDK` branches in `CMakeLists.txt`.
`NXDK_DIR` must be exported (the nxdk-cc/cxx wrappers expand it).

## Milestones

- **M0 — branch + unknowns** (this doc). Branch created; open unknowns below.
- **M1 — build scaffolding** *(DONE — 2026-06-17)*: cross-compiles + links + cxbe
  packages to `default.xbe`, and **boots on the Xbox**. Required ~20 small fixes:
  NXDK toolchain delegation, nxdk-sdl3 as a CMake subproject, ENet→lwIP (IPv6),
  Lua/stb/glad/minimp3 MSVC-path guards, the pdclib libc gaps (force-included
  `nxdk_compat.h` + the declared-but-undefined `nxdk_compat.c` for `atof`),
  filesystem/threads/time stubs, `<sys/types.h>` stub, `-force:multiple` for PD's
  bit-exact math vs pdclib, building NXDK's `libc++.lib`, and **C linkage for the
  MSVC-ABI**: clang's `i386-pc-win32` mangles C++ global *variables*, so the
  renderer vtable (`gfx_opengl_api`) needed `extern "C"`. **Current runtime state:
  boots to a black screen then hard-locks** — i.e. we're at the M1→M2 boundary; the
  next step is getting boot-stage diagnostic output to localise the hang (renderer
  bring-up vs. ROM-data load vs. an early init/exception).
- **M2 — renderer: GL VERDICT REACHED (2026-06-17), native NV2A backend needed**:
  on-screen boot tracing (`PDBOOT:` via `debugPrint`, forced video mode in `main()`)
  walked boot to `videoInit` → `gfx_init` → `gfx_wapi->init` (`gfx_sdl_init`) and
  showed **`SDL_CreateWindow(SDL_WINDOW_OPENGL)` FAILS for every GL version**, then
  `sysFatalError` (whose `SDL_ShowSimpleMessageBox` loops on Xbox = a flashing hang).
  Conclusion: **nxdk-sdl3 provides no OpenGL context at all** — not a GLSL-version
  wall, the GL window itself can't be created (the NV2A has no GL/EGL). So the
  `gfx_sdl`+`gfx_opengl` path is a dead end on the OG Xbox **regardless of SDL2 vs
  SDL3** — no SDL build exposes GL on the NV2A. **Action taken:** `videoInit` now
  selects `gfx_nxdk_wm` + `gfx_nxdk_api` under `PLATFORM_NXDK` (the native pbkit/NV2A
  backend) instead of the GL path. `gfx_nxdk` is still a **skeleton** (every
  GfxRenderingAPI/WM member is a non-crashing stub), so the game now boots *past*
  `videoInit` and runs but **draws nothing** until the backend is implemented — that
  implementation (draw_triangles via pbkit, the N64 colour-combiner → NV2A register
  combiners, textures, framebuffers, depth/blend) is the real M2 work, now starting.

  **M2a — boot bring-up to the render loop (DONE 2026-06-17).** With the native
  backend selected, on-screen + `E:\pdboot.log` `PDBOOT:` tracing (see "Boot trace"
  below) walked the *entire* boot and flushed out a chain of NXDK-specific (and a few
  platform-general) bugs, each fixed:
  - **`pb_init` reconfigures timer state** → `thrd_sleep` *and*
    `KeStallExecutionProcessor` hang afterward, and an `rdtsc` spin doesn't pace under
    xemu. Lesson: don't pace the boot trace with timers; the trace is now a plain
    `debugPrint` append + a per-line `E:\pdboot.log` write (`port/src/xboxtrace.c`).
  - **`debugClearScreen` faults once pbkit owns the framebuffer** (post-`pb_init`);
    plain `debugPrint` is fine. So the on-screen trace must not clear/over-scroll.
  - **NXDK file API needs BACKSLASH paths** — forward slashes hung `fopen` mid-call.
    `fsOsPath()` (`fs.c`) translates `/`→`\` at every fs open/remove boundary.
  - **NXDK `malloc` hangs on large single allocations** (the 32 MB ROM buffer, the
    ~8 MB memp heap, the inflated data segment) despite plenty of free RAM. Route
    allocations ≥1 MB through `MmAllocateContiguousMemory`, tracked so
    `sysMemFree`/`sysMemRealloc` match (`system.c`).
  - **`calloc`'s 32 MB zero-fill of the ROM buffer** was wasteful (overwritten by the
    read); `fsFileLoad` uses `malloc` + a single trailing NUL on NXDK.
  - **`mpconfigfull` stack-buffer overflow** — the port's `u64 mpsetup.options` grew
    `sizeof(mpconfigfull)` past the N64's hardcoded `0x1ca`, so `challengeLoadConfig`
    smashed the return address (crash on return from `challengesInit`). Sized all
    challenge-config buffers to `sizeof(struct mpconfigfull)`. **Bites any 32-bit
    build, not just Xbox.**
  - **`snprintf("%s", NULL)` / `strcpy(dst, NULL)` fault on NXDK's libc** (glibc
    tolerates them). Hit in `romdataFileLoad` (nameless file slots → guard the
    external-override probe) and pervasively via `langGet` (now returns a static `""`
    instead of NULL — `lang.c`). NOTE: lang text currently resolves empty (the lang
    bank data isn't resolving — a separate **content** issue, not a crash; in-game
    text will be blank until investigated).
  The dead GL diagnostics, kept for reference:
  - GL capability strings (version/vendor/renderer/**GLSL**/extensions) log
    unconditionally at init (`gfx_opengl.cpp` `gfx_opengl_log_info`, moved out of the
    `--debug-gl` gate).
  - The GL≥2.1 gate already fatals with the reported version if pbgl gives an old
    context (`gfx_opengl.cpp:1357`).
  - Shader compile/link failures already dump the **full GLSL compiler log** + fatal
    error (`gfx_opengl.cpp:764`/`777`) — the expected GLSL wall, captured precisely.
  Run on hardware/xemu, copy these log lines into the "renderer findings" section
  below; they scope the native NV2A backend (`gfx_nxdk`) as the *next* milestone.
- **M3 — perf/memory HUD** *(done; desktop-tested)*: `pd.perf()`
  (`src/game/luaai_api.c`) + `scripts/perf_overlay.lua` now show FPS, **CPU%**,
  **GPU%** (n/a until a backend times the GPU), and **physical memory used/total**
  (the live 64 MB watchdog) via `videoGetCpuPercent`/`videoGetGpuPercent`/
  `videoGetMemoryUsage` (`port/src/video.c`). Toggle with `/fps` + `/mem`. On Xbox,
  memory also logs once/second (`#ifdef PLATFORM_NXDK` in `videoEndFrame`) so the
  budget is visible even before the renderer is up. **Confirm the NXDK memory call**
  (`MmQueryStatistics` / `MM_STATISTICS`) on bring-up — it's the one line to fix if
  the kernel struct differs; desktop uses `sysconf` + `/proc/self/statm`.

- **M4 — HD video output (480p / 720p / 1080i)** *(design + inert scaffold; gated on
  M2)*: enumerate the Xbox-allowed modes through the existing display-mode system
  (`videoGet/SetDisplayMode`, the menu resolution dropdown, the `GfxWindowManagerAPI`
  `get_display_mode`/`set_closest_resolution` hooks) — so it's mostly "have the Xbox
  WM report the modes," not new UI. See the dedicated section below.

## Boot trace (`E:\pdboot.log`)

`port/src/xboxtrace.c` (`xboxTraceStage`/`xboxTracef`) writes every `PDBOOT:` stage to
the debug overlay **and** appends it to `E:\pdboot.log` on the writable HDD partition
(opened/closed per line, so a hard lock still leaves the trace-to-the-hang on disk).
Pull it over FTP on real hardware; on xemu it's in the HDD image's E: partition. The
file is truncated fresh each boot (first line `main() entered`). The scattered
`PDBOOT:`/`LVBOOT_TRACE`/`NXDK_*_TRACE` call sites (main.c, system.c, input.c, fs.c,
romdata.c, gfx_pc.cpp, lv.c, pdmain.c) are temporary bring-up scaffolding — remove once
the renderer is solid.

## Renderer findings

- **GL path dead** (see M2): no SDL build exposes a GL context on the NV2A.
- **NV2A present works (Phase 0, 2026-06-17):** `gfx_nxdk` `wm_start_frame` does
  `pb_wait_for_vbl`/`pb_reset`/`pb_target_back_buffer`/`pb_fill(0xFF0000FF)` and
  `wm_swap_buffers_end` does `while(pb_busy()); while(pb_finished());` — **blue screen
  on screen confirms clear + present**. Next: `draw_triangles` (push inline tris via
  pbkit/XGU), textures (swizzled VRAM upload), the N64 colour-combiner → NV2A register
  combiners, then depth/blend/scissor/framebuffers.

## gfx_nxdk renderer architecture (in progress)

`port/fast3d/gfx_nxdk.cpp` drives the NV2A via **pbkit** (device/present) + **XGU**
(state/vertex pushes; header path wired in CMake from `nxdk-sdl3/nxdk_glue/render`).

- **Transform = pass-through.** fast3d does the CPU vertex transform and hands us
  CLIP-space verts (`get_clip_parameters`: z 0..1, invert_y=false — the D3D/NV2A
  convention). So `nxdk_start_frame` sets the fixed-function transform to identity
  (identity composite matrix, lighting+cull off); the NV2A only does the perspective
  divide + viewport. `nxdk_set_viewport` programs the clip→screen offset/scale
  (negative Y; 24-bit depth range — **tune if depth/Y is wrong**).
- **Vertices.** fast3d's per-vertex float layout is *variable* (pos4, per-tex uv+clamp,
  fog4, grayscale4, then per-input colours), computed from the decoded combiner by
  `nxdk_vertex_layout`. `draw_triangles` de-interleaves it into a fixed
  `[pos4, colour4, uv2]` buffer in **GPU-visible contiguous memory**
  (`MmAllocateContiguousMemory`; the NV2A DMAs vertex data, so it can't read fast3d's
  plain-malloc buffer), then binds XGU vertex/colour/texcoord arrays and
  `xgux_draw_arrays`. Colour = the first combiner input (the shade). **The `g.vtx`
  pointer may need `& 0x03ffffff` (physical) for XGU — verify.**
- **Textures (`nxdk_upload_texture` etc.).** Management is solid: RGBA8 → A8R8G8B8
  (0xAARRGGBB) into a per-texture contiguous buffer, keyed by 1-based id, freed on
  `delete_texture`. `nxdk_apply_texture` programs texture stage 0 (linear A8R8G8B8,
  dims, filter, wrap) — **written blind; the XGU texture-register signatures need
  correcting against the real headers.** UVs may need texel-scaling vs normalized.
- **Colour combiner — DESIGN (not yet wired; relies on pbkit's default).** Map the
  decoded `CCFeatures` (`gfx_cc.h`: `c[2][2][4]` input slots, `do_single/do_multiply/
  do_mix`, `opt_alpha`, `used_textures`) to NV2A **register combiners**:
  - Common cases first: **shade-only** (output = diffuse) and **modulate** (output =
    tex0 × diffuse), which cover the bulk of PD surfaces. `SHADER_TEXEL0` →
    texture-stage-0 result; `SHADER_INPUT_n` → the per-vertex colour (diffuse);
    `SHADER_1`/`SHADER_0` → const 1/0.
  - Map the N64 combiner's general-stage (a*b + c*d style) to one NV2A general
    combiner; the final combiner emits the result (+ alpha from the alpha lane when
    `opt_alpha`). 2-cycle (`opt_2cyc`) → a second general combiner.
  - `opt_alpha_threshold`/`opt_texture_edge` → alpha test; `opt_fog` → the NV2A fog
    unit or fold into the final combiner.
  Until wired, textured surfaces depend on whatever pbkit's default combiner does —
  expect wrong colours there; shade-only geometry should be correct.
- **Open/verify:** depth-buffer format vs the 24-bit viewport Z assumption; XGU
  physical-address convention for vertex/texture pointers; scissor
  (`nxdk_set_scissor` is a stub); framebuffer effects (`create_framebuffer` etc. still
  stubbed — mirror/security-cam won't work yet). Boot trace is gated behind
  `xboxTraceSetEnabled()` (`port/src/xboxtrace.c`), default on during bring-up.

## Input (controllers) — plan

**Root cause of the early input/audio crashes:** `SDL_Init` is only called by the SDL
window manager (`gfx_sdl.cpp`), which the NXDK build replaces with `gfx_nxdk` (pbkit).
So `SDL_InitSubSystem(SDL_INIT_GAMEPAD)` / `(SDL_INIT_AUDIO)` ran without a healthy SDL
video/event base — that's why both were skipped. (SDL3's `SDL_InitSubSystem` does
auto-init the base, so the real failure is more likely nxdk-sdl3's USB **hidapi**
enumeration hanging — which the NXDK path already disables.)

**The code already exists.** `inputInit` has an `#ifdef NXDK` block that forces
`SDL_HINT_JOYSTICK_HIDAPI`/`RAWINPUT` off and inits `SDL_INIT_GAMEPAD` alone; the rest
of input.c (`inputReadController` = `SDL_GetGamepadAxis` + the keybind system,
`inputUpdate` = `SDL_UpdateGamepads`, the hotplug event watcher) is platform-agnostic.
It was just gated off by an early `return 0`.

**Approach A (preferred — reuse SDL, almost no new code).** Now behind
**`Input.XboxGamepad`** (pd.ini, default 0). Set it to 1:
- Boot reaches `inputInit` → runs the GAMEPAD-only SDL init. `pdboot.log` traces
  (`input: SDL_InitSubSystem` → `subsys ok` → `AllControllers` → `eventwatch`) pinpoint
  any hang.
- If it enumerates the pad: `inputReadController` fills `OSContPad` from the SDL gamepad
  via the existing default joy binds (SDL maps the Duke/S pad to the standard layout, so
  the desktop binds apply — L-stick = move, R-stick = look, triggers = fire/aim, etc.).
  Likely revives **audio** too (same `SDL_INIT_AUDIO` root cause).
- If it hangs at `SDL_InitSubSystem`/`AllControllers`: nxdk-sdl3's gamepad backend isn't
  usable from this setup → Approach B.

**Approach B (fallback — native nxdk USB / XID).** Read the Xbox gamepad directly through
nxdk's USB host stack (the XID gamepad driver nxdk-sdl3 itself wraps), bypassing SDL.
Implement an `#ifdef NXDK` native read in `inputReadController` that maps the XID report
→ `OSContPad.button` (N64 `CONT_*` bits) + `stick_x/y`/`rstick_x/y`, and poll it in
`inputUpdate`. More code, and needs the nxdk gamepad API confirmed, but avoids SDL
entirely. The button map mirrors the SDL default binds (A/B/X/Y, LB/RB, triggers→Z/R,
Start, D-pad, L-stick→N64 stick, R-stick→C-buttons or analog look).

**Hook points:** `inputInit` (init), `inputReadController` (the `OSContPad` boundary the
game reads every frame), `inputUpdate` (per-frame poll), `inputControllerConnected`/
`inputControllerMask` (connection state).

## Milestone 4 — HD video output (480p / 720p / 1080i)

Design + an **inert `PLATFORM_NXDK` mode table** (`port/src/video.c`, see
`g_XboxVideoModes` / `xboxVideoModeAvailable`). Nothing calls it yet — it's ready to
feed the display-mode list the moment a native Xbox WM (or nxdk-sdl3's video layer)
exists and M2 puts something on screen. **Confirm the `XGetVideoFlags()` header/flag
names** (`hal/video.h`) on bring-up.

**Two gates per mode:**
1. **`XGetVideoFlags()`** — the dashboard's HD settings + the cable. A composite-cable
   box is 480i-only; you can't offer 720p there (no signal). The mode list is filtered
   by what the console actually permits.
2. **RAM** — `videoGetMemoryUsage()` total (the gauge). 480p/720p run on 64 MB; 1080i
   is the tight case (see budget).

**Architecture decision — scan out HD, render internally low.** The game already
renders lo-res and upscales, so HD = a low-res 3D render target **upscale-blitted**
to an HD scanout buffer (one cheap blit; no NV2A fillrate hit, no full-res depth
buffer). Do **not** render the world at native 1080i — that tanks fillrate *and* RAM.

**Budget under the 32 MB game / 32 MB GPU split** (unified RAM; "GPU" = scanout +
textures + vertex/scratch). Scanout is double-buffered RGBA; render target is the
shared lo-res color+depth (~2.4 MB):

| Mode | Scanout ×2 | + render+tex+scratch | GPU total | In 32 MB? |
|---|---|---|---|---|
| 480p (640×480) | 2.4 MB | ~11 MB | ~13 MB | ✅ comfortable |
| 720p (1280×720) | 7.0 MB | ~12 MB | ~19 MB | ✅ fits |
| **1080i (1920×1080)** | **16.6 MB** | ~12 MB | **~29 MB** | ⚠️ ~3 MB headroom |

So 480p/720p are safe on a stock 64 MB box; **1080i is the risk** — it nearly fills
the 32 MB GPU half. Plan: **attempt 1080i on 64 MB** (`XBOX_1080I_MIN_MIB 0`); if
real-world testing OOMs, flip that one constant to lock 1080i behind a **128 MB**
console (`mem_total` gate). The gauge HUD makes the headroom visible while testing.

**Game-memory side (the other 32 MB):** PD's N64 heap is small (4–8 MB on real
hardware; the 32-bit Xbox build has 4-byte pointers, so structs are smaller than the
64-bit desktop build), so 32 MB is generous for game logic + audio + net buffers. The
heap is sized at boot in `port/src/pdmain.c` (`mainProc`/memory init) — that's the
hook to pin to 32 MB on Xbox; **to wire when the build runs** (renderer-independent,
but needs a real footprint measurement first).

## The renderer reality (the hard part)

fast3d's OpenGL backend hard-requires **GL 2.1+/GLSL 1.30 with runtime shader
compilation and no fixed-function fallback** (`port/fast3d/gfx_opengl.cpp`; context
fallback chain `gfx_sdl.cpp:190`; GL>=2.1 gate `gfx_opengl.cpp:1357`). The NV2A
(2001) can't run GLSL. M2 confirms how far nxdk-sdl3's GL (pbgl) gets before
committing to a native backend implementing `GfxRenderingAPI` /
`GfxWindowManagerAPI` (`port/fast3d/gfx_rendering_api.h`,
`gfx_window_manager_api.h`).

### Native NV2A backend skeleton — `gfx_nxdk` (the fallback)

`port/fast3d/gfx_nxdk.{h,cpp}` is the **fallback renderer** for when M2 confirms the
GL path is dead. It is a **complete, fillable skeleton**: every `GfxRenderingAPI` +
`GfxWindowManagerAPI` member is present with sane stub behaviour and
`TODO(nv2a)`/`TODO(pbkit)` markers; the combiner decode (`gfx_cc_get_features`) is
already wired so `shader_get_info` reports the right vertex format and the engine
builds correct vertex buffers — only the actual NV2A rasterisation is left to write.
The whole file is `#ifdef PLATFORM_NXDK` (empty TU elsewhere; compiles clean on
desktop, CI-green). Key decisions baked in:
- **Display-list cache + palette report "unsupported"** (`cache_create_*` return 0),
  so `gfx_pc` cleanly falls back to the immediate path — matches dlcache-off on Xbox.
- **Windowing split is open**: use `gfx_nxdk_wm` for a native pbkit window, OR keep
  `gfx_sdl` (nxdk-sdl3) for windowing+input and use only `gfx_nxdk_api` for rendering
  (the lower-risk split most NXDK ports take). The skeleton supports either.
- **Not wired by default** — the pbgl-GL path is tried first (M2). To switch: in
  `videoInit`, under `PLATFORM_NXDK`, set `renderingAPI = &gfx_nxdk_api` (and
  optionally `wmAPI = &gfx_nxdk_wm`).

The big TODO is `nxdk_draw_triangles` (submit `buf_vbo` via pbkit) + `nxdk_load_shader`
(map the N64 combiner in `cc` to NV2A register combiners); the M4 upscale-blit lands
in `nxdk_copy_framebuffer`.

## Open unknowns — confirm on a real NXDK install

- [ ] **NXDK predefine** — is it `NXDK`? (`platform.h` + toolchain assume `-DNXDK` /
      `defined(NXDK)`.)
- [x] **CMake toolchain** — RESOLVED by *delegation*. `cmake/toolchain-nxdk.cmake`
      now `include()`s NXDK's own authoritative toolchain at
      `$NXDK_DIR/share/toolchain-nxdk.cmake` (the file the `nxdk-cmake` wrapper
      drives), then layers only `XBOX_NXDK` + the `cxbe` path on top. This was a
      correction: hand-setting `CMAKE_AR = nxdk-lib` broke the compiler probe
      (`nxdk-lib qc libfoo.a` → `qc: no such file` — `nxdk-lib` is `llvm-lib`,
      lib.exe-style, but CMake drives `CMAKE_AR` with GNU `ar qc ...` syntax). NXDK's
      toolchain instead archives static libs with **`llvm-ar`** (`CMAKE_C_COMPILER_AR`)
      and links the `.exe` with a custom `nxdk-link ... -out:<TARGET>` rule plus the
      standard Xbox libs (`libwinapi`/`libxboxkrnl`/`libxboxrt`/`libpdclib`/
      `libnxdk_hal`/`libnxdk`/`nxdk_usb`, +`libc++` for C++). It also sets `.lib`/
      `.exe` suffixes, `CMAKE_SYSROOT=$NXDK_DIR`, `WIN32 1`, and `NXDK 1`. Because it
      sets `WIN32 1`, every `XBOX_NXDK` branch in `CMakeLists.txt` is listed BEFORE
      its `WIN32` branch so `XBOX_NXDK` wins. `cxbe` is auto-located under
      `$NXDK_DIR/tools/cxbe`. The compiler probe now passes; next is the engine
      compile + `find_package(ZLIB)`.
- [ ] **PE→XBE step** — `cxbe` path/flags in the `CMakeLists.txt` POST_BUILD (NXDK
      normally drives this from its Makefile). Output: `default.xbe`.
- [x] **nxdk-sdl3 integration** — RESOLVED. It has **no install step**: per its
      README it's consumed as a CMake **subproject** exposing `SDL3::SDL3` +
      `SDL3::Headers`. `CMakeLists.txt`'s `XBOX_NXDK` SDL3 branch now does
      `add_subdirectory(${NXDK_SDL3_DIR})` (local checkout, offline) or, if
      `NXDK_SDL3_DIR` is unset, `FetchContent` from master. Pass it via
      `NXDK_SDL3_DIR=/path ./tools/buildscripts/xbox_nxdk.sh ...`. (The README also
      documents pulling SDL_image/ttf/mixer the same way if ever needed.) Its GL
      backend is "fully hardware accelerated" (pbgl) but `SDL_gpu.h` is unsupported
      — matches our `USE_SDLGPU OFF`. `SDL_GL_CreateContext` capability still to be
      probed in M2.
- [ ] **GL caps** — `GL_VERSION`/`GL_RENDERER`/GLSL strings pbgl reports (record in M2).
- [~] **ENet / sockets** — IMPLEMENTED (compile-stage). NXDK's lwIP has IPv6
      (`LWIP_IPV6=1`), `recvmsg`/`sendmsg`/`poll`, DNS, and POSIX-name compat
      (`LWIP_COMPAT_SOCKETS` + `LWIP_POSIX_SOCKETS_IO_NAMES` default-on), so the
      amalgamated `enet.h` now has a `defined(NXDK)` branch that includes lwIP's
      socket headers and **reuses ENet's existing UNIX socket impl** (routed via the
      `#if !defined(_WIN32) || defined(NXDK)` / `#if defined(_WIN32) && !defined(NXDK)`
      guards). Only `getnameinfo` is missing (lwIP omits it) → `enet_address_get_hostname`
      falls back to the numeric address on NXDK. CMake adds the lwIP include dirs
      (`lwip/src/include`, `nforceif/include`, `nvnetdrv`) and links `libnxdk_net`.
      **Runtime not yet wired:** lwIP still needs bring-up at boot (`nxNetInit`)
      before sockets carry traffic — a port-layer init TODO, not a build blocker.
- [~] **libc gaps** — IN PROGRESS. pdclib is missing a number of POSIX/MSVC
      functions the port/vendored code assumes. Approach: a force-included compat
      header `port/include/nxdk_compat.h` (added via `-include` in the `XBOX_NXDK`
      CMake branch) holds tiny `static inline` shims, visible in every TU without
      editing individual files; add new shims there as gaps surface. Filled so far:
      `strncasecmp`/`strcasecmp`. ENet's own gaps were handled inline in its
      `defined(NXDK)` branch: `clock_gettime`/`gettimeofday`/`CLOCK_MONOTONIC`
      shimmed on pdclib's C11 `timespec_get()`, `SOMAXCONN` fallback, atomics routed
      to the clang `__atomic` builtins, non-blocking via `ioctlsocket(FIONBIO)`.
      Done: `headless.c` console/signal handlers are a no-op on NXDK (no console);
      `fs.c` file size via `fopen`/`ftell` (no `stat`), writability via temp-file
      probe (no `access`), `fsCreateDir` stubbed (TODO: real FATX `CreateDirectoryA`
      — saves won't persist yet); `ext_tex.c` HD-texture dir scan stubbed out
      (`opendir`/`stat` return empty — no external texture packs on Xbox yet).
      Still ahead: threads (`port/src/system.c`), real FATX dir creation +
      `D:`-path mapping (`fs.c`), signals/backtrace (`crash.c`).
- [~] **zlib** — NXDK bundles it (`libzlib.lib` in the sample link lines), but
      `find_package(ZLIB)` can't see it under the win32-like sysroot. The `XBOX_NXDK`
      branch in `CMakeLists.txt` now points `ZLIB_LIBRARY`/`ZLIB_INCLUDE_DIR` at
      NXDK's copy directly (probing `$NXDK_DIR/lib/libzlib.lib` + `lib/zlib/zlib.h`,
      with `-DZLIB_LIBRARY=`/`-DZLIB_INCLUDE_DIR=` overrides). Confirm the exact paths
      on your install.
- [ ] **64 MB budget** — measure boot footprint (`romdataInit`, gfx/vtx pools, audio).

## Files touched (scaffolding)

| File | Change |
|---|---|
| `src/include/platform.h` | `NXDK` OS branch (before `_WIN32`) → `PLATFORM_NXDK` + `PLATFORM_POSIX` |
| `CMakeLists.txt` | `XBOX_NXDK` branches: platform id, SDL3 (portlibs fallback), GL libs, `USE_SDLGPU OFF`, extra libs, Win32-subsystem guard, `cxbe` POST_BUILD |
| `port/src/video.c` | perf/memory HUD accessors; per-second NXDK mem log; M4 inert HD-mode table (`g_XboxVideoModes` / `xboxVideoModeAvailable`, `PLATFORM_NXDK`) |
| `src/game/luaai_api.c` + `scripts/perf_overlay.lua` | `pd.perf()` cpu/gpu/mem fields + HUD lines |
| `port/fast3d/gfx_opengl.cpp` | unconditional GL-capability log at init (M2 bring-up evidence) |
| `port/fast3d/gfx_nxdk.{h,cpp}` (new) | native NV2A renderer + WM **skeleton** (`PLATFORM_NXDK`; vtable stubs + combiner decode wired; TODO bodies) |
| `cmake/toolchain-nxdk.cmake` (new) | NXDK clang toolchain skeleton (sets `XBOX_NXDK`) |
| `tools/buildscripts/xbox_nxdk.sh` (new) | configure+build wrapper (mirrors `nswitch_docker.sh`) |
| `docs/PORT_XBOX_NXDK.md` (new) | this doc |

All changes are additive and guarded behind `XBOX_NXDK` / `PLATFORM_NXDK` / the new
toolchain, so desktop (OpenGL + SDL_GPU) and Switch builds are unaffected.
