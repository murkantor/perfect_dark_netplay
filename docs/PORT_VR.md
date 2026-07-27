# Port-only VR (PCVR / OpenXR) — verbatim port of the Alex-LeTux VR mod

**Status: complete port, compile-verified in both configurations. NOT runtime-tested.**
Both binaries build and link from one tree: `-DUSE_VR=ON` produces the VR exe, the
normal build is unchanged. Nothing here has been run in a headset yet.

Source: [Alex-LeTux/perfect_dark_VR][vr] (`vr/port`, HEAD `cdcae050c` at port time).
Common ancestor with this fork: `19541418b`.

[vr]: https://github.com/Alex-LeTux/perfect_dark_VR

---

## The one rule

**Upstream's VR is already tuned against real hardware. We copy it verbatim — same
constants, same math, same function decomposition, same names, including its quirks.**

We deviate only where this fork's constraints force it, and every deviation carries an
in-code comment saying what and why. If VR ever feels wrong, the fix is almost always
"we deviated somewhere we shouldn't have", not "upstream's number was wrong".

## The gate: compile-time, not runtime

`cmake -DUSE_VR=ON` defines `PD_ENABLE_VR`. That is the only switch.

```c
#ifdef PD_ENABLE_VR
    /* upstream verbatim */
#else
    /* our original code, byte-for-byte untouched */
#endif
```

- **Flat build compiles zero lines of VR code.** `port/vr/` is excluded from the source
  glob when `USE_VR=OFF`, and every hook is `#ifdef`-gated.
- **The VR exe is VR-only, like upstream.** It blocks at boot waiting for an OpenXR
  runtime (`vrShowWaitingWindow`), hides its main window, and renders through the
  headset. There is no runtime fallback to flat play — that is upstream's design and
  changing it would change how VR behaves.
- VR forces `USE_SDLGPU=OFF` (stereo needs `GL_OVR_multiview2`; the SDL_GPU backend has
  no implementation) and is forced off for `DEDICATED_SERVER` / `NINTENDO_SWITCH`.

Build:

```sh
pacman -S mingw-w64-x86_64-openxr-sdk     # once
cmake -G "Unix Makefiles" -DUSE_VR=ON ..  # USE_SDLGPU is forced OFF
```

We link the **MSYS2 MinGW-native** OpenXR loader via `find_package(OpenXR)`. Upstream
vendors an MSVC `openxr_loader.lib` that GCC cannot reliably link. Nothing is vendored.

---

## Layout

| File | Role |
|---|---|
| `port/vr/vr_openxr.cpp/.h` | OpenXR instance/session/swapchains, head pose, eye matrices, frame loop, `vr_world_scale` consumers |
| `port/vr/vr_input.cpp/.h` | Controller poses, buttons/axes, haptics |
| `port/vr/vr_settings.cpp/.h` | `pd-vr.ini` persistence |
| `port/vr/vr_log.c/.h` | `vr_debug.txt` |
| `port/vr/vr_runtime_launcher.h` | Win32 runtime auto-start (upstream, kept) |

All five copied verbatim from upstream, LF-normalised, with only the SDL2→SDL3 include
seam and `../port/fast3d/` → `../fast3d/` path fixes changed.

## What renders

Stereo is **one geometry pass** into the OpenXR swapchain's 2-layer texture array;
per-eye divergence happens in the vertex shader from `gl_ViewID_OVR`. There is no
per-eye draw loop and no per-eye projection matrix.

The shader prelude (`vr_shader` in `gfx_opengl.cpp`) is upstream's verbatim, with **one
deliberate deviation**: its first line is

```c
vec4 mvPos = uMVP * aVtxPos;   // upstream: vec4 mvPos = aVtxPos;
```

so VR composes with our display-list cache's `uMVP`. On the immediate path `uMVP` is
identity, making this byte-identical to upstream; on cached geometry it is what gives
cached rooms stereo separation at all. **If you touch the dlcache matrix, re-check this.**

Other renderer notes:
- GLSL is raised to 330 in VR builds (multiview needs it; the compat profile would ask
  for 130).
- fb 0 becomes the layered swapchain, so `glBlitFramebuffer` cannot read it — upstream's
  `mv_blit` shader path copies via the texture array instead (menu backgrounds, camspy,
  cloak). Kept verbatim, including the Meta-runtime `fb_dst == 25` special case.
- The desktop **mirror window** is a second SDL3 window sharing the GL context; the main
  window is hidden. Upstream drives its mirror from a vendored ImGui toolbar — **not
  ported**; use `gfx_sdl_set_mirror_mode(0..3)` (off / left eye / right eye / SbS)
  instead. Same for upstream's ImGui "waiting for VR" splash: ours is a plain window.

## The resolution regime (read before touching sizing)

Upstream retuned every menu/HUD constant against a changed resolution regime, so we take
the whole regime under `PD_ENABLE_VR` rather than half of it:

- `constants.h`: `SCREEN_320/240` → 640/480, `FBALLOC_*` doubled, `SCREEN_WIDTH/HEIGHT_*`
  → inert `10` (real sizing comes from `VrSmallW/H` at runtime), `SCREEN_ASPECT` → `1.0f`.
- `vi.c`: the VI register math is doubled throughout (`xScale`, `yScale`, `vStart`).
- `videoGetAspect()` returns `XrAspect`; the resolution dropdown becomes eye-render-scale
  presets (0.5×…4.0×) that call `vr_restart_with_new_scale`.
- FOV-normalised sizing for the sun (`sky.c`), light glares (`artifact.c`) and sparks
  (`sparks.c`) — this is what stops high-FOV VR looking wrong.

Taking only part of this regime is worse than taking none of it. Don't.

## World scale — the thing that makes VR feel right

`vr_world_scale` is **not a constant**. Upstream derives it from FOV
(`-11/14·fovy + 745/7`), multiplies cutscenes by 0.13 or 0.5 depending on stage/anim,
switches between 42.5 and 85.0 per situation, special-cases The Duel, and scales
everything by the user's `VrSetWorldScale`. All of that lives in `player.c` and is copied
exactly. An earlier attempt at this port hardcoded 85.0 and the result felt wrong.

---

## Netplay, splitscreen and shared files

**The flat exe is structurally unaffected.** Verified mechanically, not assumed: the
entire port is 47 files, +9532/−16, and every one of those 16 deletions is a line being
wrapped in a guard with our original preserved in the `#else`. No wire struct changed,
`NET_PROTOCOL_VER` is untouched, and the `s32`→`f32` crouch conversion in `types.h` is
VR-only *and* those fields never appear in any serialisation path (the wire carries
`crouchofs`, a separate field). A VR client and a flat client speak the same protocol
version.

**Inside the VR exe, netplay needed real work.** Upstream is single-player VR, so it
reads *the local headset* unconditionally inside functions this fork also runs **for
remote players** (`lvRender` sets `forcesingleplayer = false` whenever `g_NetMode` is
set; `bwalkUpdateRemote`/`bmoveProcessRemoteInput` drive remote pawns through the same
code). Left verbatim, the host's head and hands would drive every remote player. An
adversarial review confirmed these; all are now gated on the pawn being the local player:

| Site | Consequence before fix |
|---|---|
| `bondwalk.c` `vr_player_rot` | **every remote player's facing** driven by the host's head |
| `bondwalk.c` `vr_player_pos` | local roomscale translation added to remote pawns' `prop->pos` |
| `bondmove.c` headset crouch | local head height clobbered the wire-applied crouch of remote pawns |
| `bondhead.c` `bheadUpdate` | remote players' head height followed the local HMD |
| `lv.c` Y-button reload | holding Y reloaded whichever remote pawn was being iterated |
| `lv.c` wire `JO_ACTION_RELOAD` | remote dual-wield left reload consumed but never performed server-side |
| `player.c` `playerStartNewLife` | dropped `vv_theta` write our `UCMD_FL_FORCEANGLE` correction derives from |
| `net.c` profile restore | VR player re-seated into `CONTROLMODE_11`, which VR input does not drive |

The guards are **no-ops in single-player VR** — there is only one player — so upstream's
feel is untouched; they are marked in-code with `// VR DEVIATION (netplay):`. VR + netplay
is now structurally sane but still **completely untested**.

Also: **VR is single-local-player.** Upstream's per-hand state is global. Splitscreen in
a VR build is not a supported combination.

**Shared game folder — the non-obvious regression path.** The VR exe and flat exe read
the same `eeprom.bin`, `mpsetups.bin`, gamefile saves and `pd.ini`. Code isolation via
`#ifdef` does **not** isolate data: a VR session that *persists* a VR-forced value
changes what the flat exe loads next boot. An adversarial review caught two live
instances of exactly this, now fixed:

| Leak | Effect before fix |
|---|---|
| `fovy` forced to `XrFov` each VR boot, and `Game.PlayerN.FovY` is a saved `pd.ini` key | flat exe booted at headset FOV after any VR session — and wire-synced it to other clients |
| `gamefileApplyOptions` forces `SCREENSIZE_FULL`, `gamefileSave` then serialises the forced value | VR saves erased the user's stored screen-size preference |

Rule for anything added later: **the VR exe may force whatever it needs at runtime, but
must never write a VR-forced value over a shared stored preference.** VR-private state
belongs in `pd-vr.ini` (`port/vr/vr_settings.cpp`), which already exists for this purpose.

`filemgr.c`/`savebuffer.c` changes are display-only string formatting, and `crash.c`
keeps our symbolication dump format.

## Deliberately not ported

- **Android / Quest standalone** — the Gradle project, JNI, GLES3 path.
- **Dear ImGui** (~40k vendored lines) — the mirror toolbar and boot splash; replaced as
  described above.
- Upstream's `options.json` version-string edit (`"1.2"` → `"VR-1"`), because the flat
  exe shares those assets.

## Upstream bugs found while porting — do not "fix" or re-introduce

Behavioural quirks are **feel** and were copied verbatim (dead `gSPClipRatio` call in
`lv.c`, `lookingatprop` written twice with the left hand clobbering the right, the
redundant nested `if` in `bondmove`'s sidestep, a spark-scale expression that reduces to
a constant 1/3). Only memory-unsafe code was guarded, minimally:

| Bug | Handling |
|---|---|
| `gVrReloadZones` indexed raw by a weapon global that reaches 255 | bounds-checked (`// VR: upstream UB guard`) |
| First recoil block reads `recoildist`/`recoilangle` before assignment | zero-initialised |

## Gotchas

- **`bool` width.** Game TUs (`types.h`) make `bool` an `s32`; renderer/C++ TUs use a
  1-byte `bool`. Never share a raw `bool` across that seam. `VrIsTitleLegal` is defined
  as `int32_t` in `gfx_opengl.cpp` for exactly this reason.
- **`videoInitDisplayModes` must be non-static under `PD_ENABLE_VR`** — `vr_openxr.cpp`
  calls it after a render-scale change. Easy to miss; it is the last link error you get.
- **This repo is LF; the VR fork is CRLF.** The Edit tool has CRLF'd files here before —
  always check `git diff --stat` looks like your change and not a whole-file rewrite.
- Raw diffs against upstream are ~90% reindent noise. Always diff whitespace-blind
  against the ancestor:
  ```sh
  git show 19541418b:F | tr -d '\r' > /tmp/b; git show vr/port:F | tr -d '\r' > /tmp/v
  diff -uw /tmp/b /tmp/v
  ```

## Read this before touching

`gfx_run()`'s VR branch or `gfx_adjust_viewport_or_scissor` in `gfx_pc.cpp`; the multiview
code or rapi vtable tail in `gfx_opengl.cpp` (a mis-ordered initializer silently binds the
wrong function pointers); the mirror window in `gfx_sdl.cpp`; `mainTick`'s VR frame hooks
in `pdmain.c`; the resolution regime above; or any `bgun*`/`bwalk*`/`bmove*` aiming and
movement code.

## Honest limits

- **No headset has run this build.** Every claim above is "it compiles and links".
- VR + netplay is unverified and has the known hazards tabled above.
- VR + splitscreen is unsupported.
- `g_TickRateDiv` defaults to 0 in VR builds — one headset uncaps the tick rate for the
  whole process.
