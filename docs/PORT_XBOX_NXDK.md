# Original Xbox (NXDK) build — status & notes

Experimental OG Xbox port on branch **`port-net-xbox`** (branched from
`port-net-predict`; merge `port-net-predict` forward over time). Toolchain:
**NXDK** (https://github.com/XboxDev/nxdk) + **SDL3 for NXDK**
(https://github.com/Ryzee119/nxdk-sdl3).

> **Status: scaffolding (Milestone 1 in progress).** Build-system plumbing is in;
> the NXDK toolchain specifics and the renderer are unproven. Nothing here is
> verified on hardware/xemu yet. This doc is the living checklist.

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
- **M1 — build scaffolding** *(in progress)*: cross-compile + link to `default.xbe`.
  CMake/toolchain/platform plumbing landed; needs a real NXDK install to compile.
- **M2 — renderer: validate SDL3/pbgl GL** *(diagnostics in place)*: the existing
  `gfx_sdl` + `gfx_opengl` path now **records exactly where it fails** on first boot,
  no CLI flag needed (the Xbox can't pass one):
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

## Renderer findings (fill in on first boot)

```
(paste the GL: version / renderer / GLSL log lines + any shader-compile fatal here)
```

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
- [ ] **ENet / sockets** — `port/external/enet.c` over NXDK lwIP/BSD sockets (netplay).
- [ ] **libc gaps** — threads (`port/src/system.c`), filesystem/FATX paths + `D:`
      (`fs.c`), signals/backtrace (`crash.c`, `headless.c`). Guard with
      `#ifdef PLATFORM_NXDK`, mirroring the `DEDICATED_SERVER`/Switch stub idiom.
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
