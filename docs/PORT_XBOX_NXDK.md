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
./tools/buildscripts/xbox_nxdk.sh build_xbox -DROMID=ntsc-final
# -> build_xbox/default.xbe  (run in xemu or on hardware)
```

Internally this drives `cmake/toolchain-nxdk.cmake` (sets `XBOX_NXDK`, points clang
at the NXDK target) and the `XBOX_NXDK` branches in `CMakeLists.txt`.

## Milestones

- **M0 — branch + unknowns** (this doc). Branch created; open unknowns below.
- **M1 — build scaffolding** *(in progress)*: cross-compile + link to `default.xbe`.
  CMake/toolchain/platform plumbing landed; needs a real NXDK install to compile.
- **M2 — renderer: validate SDL3/pbgl GL**: drive the existing `gfx_sdl` + `gfx_opengl`
  on nxdk-sdl3 and **record exactly where it fails** (expected: the GLSL wall). Use
  the result to scope a native NV2A backend (`gfx_nxdk`) as the *next* milestone.
- **M3 — perf/memory HUD**: extend `pd.perf()` (`src/game/luaai_api.c`) +
  `scripts/perf_overlay.lua` to show FPS, CPU%, GPU%, and **memory used/total** (the
  live 64 MB watchdog). Also log memory once/second before the renderer is up.

## The renderer reality (the hard part)

fast3d's OpenGL backend hard-requires **GL 2.1+/GLSL 1.30 with runtime shader
compilation and no fixed-function fallback** (`port/fast3d/gfx_opengl.cpp`; context
fallback chain `gfx_sdl.cpp:190`; GL>=2.1 gate `gfx_opengl.cpp:1357`). The NV2A
(2001) can't run GLSL. M2 confirms how far nxdk-sdl3's GL (pbgl) gets before
committing to a native backend implementing `GfxRenderingAPI` /
`GfxWindowManagerAPI` (`port/fast3d/gfx_rendering_api.h`,
`gfx_window_manager_api.h`).

## Open unknowns — confirm on a real NXDK install

- [ ] **NXDK predefine** — is it `NXDK`? (`platform.h` + toolchain assume `-DNXDK` /
      `defined(NXDK)`.)
- [ ] **CMake toolchain** — exact clang **target triple**, freestanding flags,
      include roots, and link rules. The skeleton in `cmake/toolchain-nxdk.cmake` is
      a starting point reconciled against `$NXDK_DIR/Makefile`. Check if Ryzee119/nxdk
      ships a CMake toolchain to base on instead.
- [ ] **PE→XBE step** — `cxbe` path/flags in the `CMakeLists.txt` POST_BUILD (NXDK
      normally drives this from its Makefile). Output: `default.xbe`.
- [ ] **nxdk-sdl3** — does it export an SDL3 CMake config (else the manual include
      fallback path in `CMakeLists.txt` must match its install layout)? Does it
      provide `SDL_GL_CreateContext`, audio, Xbox-gamepad input?
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
| `cmake/toolchain-nxdk.cmake` (new) | NXDK clang toolchain skeleton (sets `XBOX_NXDK`) |
| `tools/buildscripts/xbox_nxdk.sh` (new) | configure+build wrapper (mirrors `nswitch_docker.sh`) |
| `docs/PORT_XBOX_NXDK.md` (new) | this doc |

All changes are additive and guarded behind `XBOX_NXDK` / `PLATFORM_NXDK` / the new
toolchain, so desktop (OpenGL + SDL_GPU) and Switch builds are unaffected.
