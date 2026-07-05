# NV2A Renderer (`gfx_nxdk.cpp`) — Original Xbox port

> Auto-loads when working under `port/fast3d/`. This file is the **living handoff** for
> the native **NV2A fast3d backend** (`gfx_nxdk.cpp`) on branch **`port-net-xbox`**
> (draft **PR #19**, repo `murkantor/perfect_dark_netplay`). For build scaffolding,
> boot bring-up (M0–M2), and the libc/CMake/NXDK details, see
> [`docs/PORT_XBOX_NXDK.md`](../../docs/PORT_XBOX_NXDK.md) (its renderer status section
> is **stale** — *this* file is the current renderer truth).
>
> Everything else in `port/fast3d/` (gfx_opengl, gfx_sdlgpu, gfx_pc, glad) is the
> stock libultraship renderer — **don't modify it for Xbox work**; only `gfx_nxdk.cpp`
> is ours. `gfx_pc.cpp` is shared and may be *read* to understand what it hands the
> backend (clip params, depth mode, scissor, vertex layout), but edit it only with care.

## Environment & workflow (NEW: WSL, not MSYS2)

- **Build host: WSL / Ubuntu + NXDK** (XboxDev/nxdk) + **nxdk-sdl3** (Ryzee119). The
  old `CLAUDE.md` MSYS2 instructions are for the Windows desktop build — **ignore them
  for Xbox.**
  ```sh
  export NXDK_DIR=/path/to/nxdk            # bootstrap NXDK's own `make` once first
  export NXDK_SDL3_DIR=/path/to/nxdk-sdl3  # or omit to let CMake FetchContent it
  ./tools/buildscripts/xbox_nxdk.sh build_xbox -DROMID=ntsc-final
  # -> build_xbox/default.xbe
  ```
- **Run only on real Xbox hardware.** xemu gives **zero video output** for this NV2A
  path, so it can't validate the renderer. The **user** copies `default.xbe` + the ROM
  (`D:\data\pd.ntsc-final.z64`) to the console, runs, and reports back with a **photo
  of the screen + `pdboot.log`** (on-screen `PDBOOT:`/`rdr:` traces via `xboxTracef`).
- **Loop:** I edit `gfx_nxdk.cpp` → commit → `git push origin port-net-xbox` (PR #19) →
  user pulls/builds/tests → reports screen + log. **I cannot build or run here** (no
  NXDK toolchain in this container); reason carefully and ship minimal, reversible
  changes because each test costs the user a full build+copy+boot cycle.
- Reference material lives in `/tmp/nxdkref/nxdk_glue/` on the dev box (extracted from a
  user-supplied 7z): **`render/SDL_render_xgu.c`** (the canonical 2D NV2A backend — our
  north star for combiner/texture/scissor register usage), `render/xgu/{xgu,xgux,nv2a_regs}.h`,
  `render/swizzle.c`. If absent in a new session, ask the user to re-supply it.
- **Commit msgs:** end with the repo's `Co-Authored-By:` + `Claude-Session:` trailers.
  **Never** put the model id in commits/PRs/code.

## Architecture of `gfx_nxdk.cpp` (how a frame works)

The NV2A can't run GLSL, so this is a hand-written `GfxRenderingAPI` + (in the same
file) `GfxWindowManagerAPI` using **pbkit + xgu/xgux** helpers. The core design choice:

> **fast3d hands CLIP-space verts and expects the GPU to do the perspective divide,
> near-plane clip, and viewport. We do the divide + viewport + clip ON THE CPU and feed
> the NV2A pre-transformed screen-space pixels** (like `SDL_render_xgu`'s `float pos[2]`).
> The fixed-function pipeline runs with identity matrices + identity X/Y viewport.

`get_clip_parameters` advertises `{ z_is_from_0_to_1 = true, invert_y = false }` (D3D-
style: near plane at clip `z == 0`, far at `z == w`).

Per-draw path in **`nxdk_draw_triangles`** (line ~600):
1. **De-interleave** fast3d's variable vertex layout (`nxdk_vertex_layout`) into our
   fixed arena layout `[x,y,z,w, r,g,b,a, u,v]` = **`NXDK_VTX_FLOATS` (10 floats)**.
2. **Near-plane clip** each depth-tested triangle (Sutherland-Hodgman vs clip `z >= 0`)
   → 1–2 output triangles. *Required* because we CPU-divide and the NV2A does no
   homogeneous clip; a vertex with `w <= 0` (behind the eye) sign-flips and stretches
   across the screen. Gated on `g.depth_test` so 2D/HUD (w==1) is untouched.
3. **Perspective divide + screen map** on the CPU: `ndc = clip.xy / w`, then
   `screen_x = (ndc.x*0.5+0.5)*vpw + vpx`, `screen_y = (1 - (ndc.y*0.5+0.5))*vph + vpy`
   (Y flipped here — that's why the viewport stays identity).
   `dst[2] = clamp(ndc.z, 0,1) * 0xFFFFFF` (depth pre-scaled to the Z24 range — the
   fixed-function path applies no viewport transform of its own; see the depth lesson).
4. **sfence** (the arena is write-combined — see gotchas), apply texture/combiner, bind
   attrib arrays, `xgux_draw_arrays(XGU_TRIANGLES, …)`, advance the bump allocator.

Frame lifecycle: `wm_start_frame` (pbkit: wait vbl, reset, target back buffer,
`nxdk_bind_back_surface`, erase depth+colour) → `nxdk_start_frame` (per-frame transform
state, combiner, toggles the double-buffered arena) → draws → WM swap/present.

## Hard-won lessons (each cost real debugging — DON'T regress these)

- **32-byte vertex-arena alignment.** The NV2A vertex DMA needs the array base 32-byte
  aligned. Each vertex is 40 B (10 floats); 4 verts = 160 B = mult of 32, so the bump
  allocator rounds the offset up to a **multiple of 4 verts** (`& ~3`). Unaligned bases
  → flickering streaks/garbage verts. *(If you change `NXDK_VTX_FLOATS`, re-check this:
  12 floats = 48 B, 4 verts = 192 B still works; 11 would break it.)*
- **Write-combined GPU memory.** Vertex arena is `MmAllocateContiguousMemoryEx(…,
  PAGE_WRITECOMBINE|PAGE_READWRITE)`. After CPU writes you MUST `sfence` before the draw
  or pbkit may DMA stale verts (flicker on movement).
- **64 MB address cap.** `xgux_set_attrib_pointer` masks the address `& 0x03FFFFFF`, so
  GPU-visible allocations must sit in the low 64 MB — `nxdk_gpu_alloc` caps the highest
  address at `0x03FFFFFF`. A single 10 MB arena fails to allocate after the 33 MB ROM;
  use **two ~2 MB double-buffered** blocks (`g.vtx[0]/[1]`, toggled per frame so the GPU
  isn't reading the buffer the CPU is overwriting).
- **Depth (fixed, awaiting hardware confirm):** the NV2A fixed-function pipeline does
  **NOT** apply `SET_VIEWPORT_SCALE/OFFSET` as a vertex transform — the composite matrix
  output is treated as **already screen-space, Z included** (xemu applies an *inverse*
  viewport to the composite result; the nxdk xgu samples bake a viewport matrix into
  their composite). With composite = identity, the old "viewport Z-scale = 0xFFFFFF"
  never multiplied anything: the raster got raw NDC z in **[0,1]**, which quantises to
  ~1 code of the Z24 buffer → **every depth-tested fragment tied → pure submission-order
  rendering**. The world only *looked* sorted because PD draws rooms front-to-back.
  Current scheme: **pre-scale z on the CPU** (`dst[2] = ndcz * 0xFFFFFF`), viewport
  Z-scale **1.0**, **explicit `clip_min=0` / `clip_max=0xFFFFFF`** (pbkit's default clip
  range clamps everything to one depth — that's what sank the earlier pre-scaled
  attempt, 0bb0b518), clear `0xFFFFFF` (far), func `LESS_OR_EQUAL` (`ALWAYS` when
  `!depth_compare`, matching gfx_opengl). Bind position as **3 components when
  `depth_test`**, 2 otherwise. This combination is safe under either viewport semantics
  (scale 1.0 can't double-apply).
- **Blend gating:** the N64 sets the alpha-blend blender bits for **anti-aliased OPAQUE**
  surfaces too (coverage AA), and fast3d forwards that as `use_alpha`. Our combiner's
  `alpha = tex_alpha*shade_alpha` is < 1 for many opaque textures, so blending them made
  every wall translucent ("see-through everything"). Fix: blend only when
  **`use_alpha && !depth_mask`** (`nxdk_update_blend`) — opaque geometry writes depth
  (Z_UPD) so stays solid; true translucents + 2D/HUD (depth-off) still blend.
- **Scissor = portal clipping.** PD scissors each room/draw-slot to its on-screen portal
  (door/window) rect; `nxdk_set_scissor` was a no-op → adjacent rooms spilled over walls.
  fast3d hands the rect in **GL bottom-left** window coords (like gfx_sdlgpu), so flip Y
  (`ry = fbh - y - h`) and clamp to the surface before `xgu_set_scissor_rect`.
- **Config (`port/src/config.c`):** NXDK `strtof` **asserts** on bad input and `printf
  %f` is **broken** → manual `configParseFloat`/`configFormatFloat` on NXDK. Without them
  FOV resets to 5 / sticks to 0 every boot, or boot crashes.
- **zlib (`src/lib/rzip_c.c`):** NXDK libzlib is `Z_SOLO` (no default allocators) — must
  set `zalloc`/`zfree` before `inflateInit2`, else every file load returns NULL.

## Approaches that FAILED (don't retry without reading why)

- **Clip-space pass-through vertex shader** (`mov oPos, v0`): came out **zoomed + upside
  down** — the post-shader viewport divide/scale didn't behave as assumed. Reverted.
- **Fixed-function with clip-space input:** identity matrices force `w=1` (no perspective
  divide) → "everything radiates from a point."
- **3-component projective texcoord `(s/w, t/w, 1/w)`** to fix affine texture warp:
  **3D textures vanished entirely.** The NV2A `2D_PROJECTIVE` stage didn't divide by the
  3rd component as expected; the reference uses **2 components**. Reverted to affine.
  Perspective-correct texturing is still **unsolved** (see next steps).
- **`dst[2] = ndcz * 0xFFFFFF` with the clip range left at pbkit's default** (0bb0b518):
  clamped to one value → no sorting. The pre-scale itself was RIGHT — the missing piece
  was `clip_min=0/clip_max=0xFFFFFF`. Its "fix" (d8412207) moved the scale into the
  viewport Z-scale register, which the fixed-function path never applies (see the depth
  lesson above), so depth stayed silently non-functional while draw order faked it.

## Current state (Z24 pre-scale fix, on top of `f99d66acf`)

WORKING: boots to gameplay, controllable, missions load; textured (affine);
near-plane clip stops the stretching; opaque walls solid (blend gate); 2D/HUD/menus
correct; portal scissor implemented; config persists; input; save paths.

**"Objects only visible against sky" — root cause found (code analysis), fix shipped,
NEEDS HARDWARE CONFIRM.** The chain:

- PD draws props/chrs in **`RENDERPASS_OPA_PREBG`** (`prop.c:696` — pre-BG is the
  *default*): each prop renders **before its own room's background geometry** and
  survives on screen only if the depth test rejects the room walls/floor drawn right
  after it (`bg.c:1281-1327`).
- Depth rejection was **silently non-functional** — every depth-tested fragment landed
  on ~one Z24 code because raw NDC z [0,1] reached the raster untransformed (the
  viewport Z-scale is not a transform stage in the NV2A fixed-function path; see the
  depth lesson). All ties + `LESS_OR_EQUAL` ⇒ later draw always wins.
- So each prop was erased by its own room's BG pass, then by every farther room —
  leaving it visible only where nothing draws after it: **the sky**. The pre-scissor
  "rooms through walls" was the *same* defect (masked since by the portal scissor,
  which is also what the real N64 relies on for wrap-around adjacent rooms).
- `depth_source_prim`/G_ZS_PRIM was a red herring: the game only uses it in DEBUG
  builds (`src/lib/main.c:1170`). Prop draws carry the same projection as rooms
  (`camGetOrthogonalMtxL` = the *same* perspective × view; the 2×zfar projection in
  `vi0000ab78` is only for the special sky rooms), so the CPU-side z values were
  always fine — the `zmin/zmax` trace will show a healthy ~9xx…1000 span and canNOT
  detect this failure (it logs CPU values, not what the GPU landed).

What to verify on hardware with this build: (1) a chr/prop standing in front of a wall
is visible; (2) walking around a prop keeps it correctly occluded/visible; (3) menus/
HUD unchanged; (4) decals (bullet marks, blood) may be patchy — known follow-up
(ZMODE_DEC needs a polygon-offset equivalent), don't chase it as a regression.

OPEN / BROKEN:
1. **Depth fix above unconfirmed on hardware** (everything else depends on it).
2. **Affine texture warp** — textures swim (no perspective correction) because position
   `w=1` kills perspective interpolation. Projective-texcoord hack failed (above). Likely
   real fix: revisit a **GPU-transform-from-clip-space** path — with the (now understood)
   rule that the composite matrix must include the viewport mapping, the earlier
   "radiates from a point" / "zoomed+upside-down" failures make sense and that path may
   finally be tractable: bake clip→screen into the composite and feed raw clip verts.
3. **Missing font letters** — some glyphs don't render (font atlas / UV-clamp issue,
   separate from depth).
4. **No sound** (audio subsystem not wired).
5. **Decals z-fight** (ZMODE_DEC has no polygon offset yet — cosmetic).
6. **Portal scissor untested** on hardware.

## Next steps (priority order)

1. **Hardware-confirm the depth fix** (props visible in rooms). If props are *still*
   sky-only, the fallback experiment is two hardcoded interpenetrating triangles at
   z≈0.95 band (like the old green test triangle) to isolate raster depth from game
   data, and `xgu_set_control0` (Z_FORMAT fixed / z-perspective OFF) as the next knob.
2. Confirm the **portal scissor** fixed rooms-through-walls; if a sub-rect scissor ever
   clips the HUD/player room, re-check the Y-flip/clamp.
3. **Perspective-correct textures** (de-warp) — see OPEN #2: clip-space composite path.
4. **ZMODE_DEC decal offset** (small constant Z bias on decal draws).
5. **Fonts**, then **sound**, then the **perf/mem HUD** (FPS/CPU%/GPU%/mem vs the 64 MB
   budget — extends `pd.perf()` / `scripts/perf_overlay.lua`; see `docs/PORT_XBOX_NXDK.md`).
6. **Strip diagnostics** (`rdr:`/`PDBOOT:` traces, the `g_NxdkZMin/Max` span, per-frame
   draw counter) once the renderer is stable.

## Function map (`gfx_nxdk.cpp`)

`nxdk_get_clip_parameters` (z/invert_y convention) · `nxdk_upload_texture` +
`nxdk_swizzle_rgba8`/`nxdk_npot2pot`/`nxdk_ulog2` (swizzled A8R8G8B8 into POT containers,
`us/vs` UV scale) · `nxdk_update_blend` (the `use_alpha && !depth_mask` gate) ·
`nxdk_set_depth_mode` · `nxdk_set_viewport` (identity XY, Z-scale 0xFFFFFF + clip min/max)
· `nxdk_set_scissor` (portal clip, Y-flip) · `nxdk_vertex_layout` · `nxdk_combiner_mode`
(unlit vs `2D_PROJECTIVE` textured input combiner) · `nxdk_apply_texture` ·
`nxdk_draw_triangles` (**the hot path**: de-interleave + near-clip + divide + bind) ·
`nxdk_setup_combiner` (register-combiner output, adapted from `SDL_render_xgu`) ·
`nxdk_start_frame` / `wm_start_frame` / `nxdk_bind_back_surface` (surface clip/format/pitch).

## Other Xbox-touched files (outside fast3d)

`port/src/config.c` (NXDK float parse/format) · `src/lib/rzip_c.c` (Z_SOLO zlib allocs) ·
`port/src/input.c` (xbox gamepad default-on) · `port/src/system.c` + `port/src/fs.c`
(save paths `$E/$H/$S`, `fsCreateDir` stub) · `CMakeLists.txt` + `cmake/toolchain-nxdk.cmake`
+ `tools/buildscripts/xbox_nxdk.sh` (build) · `src/include/platform.h` (NXDK branch).
