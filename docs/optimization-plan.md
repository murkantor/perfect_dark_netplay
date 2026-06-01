# Perfect Dark Port — Optimization Plan (B-section work)

This document tracks the optimization roadmap from the code review (`docs/code-review-2026.md` §B/§9) and records what has been implemented and validated versus what remains.

## Validation environment

A reproducible build + headless smoke-test loop was established (Ubuntu 24.04, GCC 13.3, SDL2 2.30, software GL via Mesa llvmpipe under Xvfb):

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)                       # ~24s clean
# headless boot (attract mode exercises level setup + prop code):
timeout 30 xvfb-run -a -s "-screen 0 1280x720x24" \
  env LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe SDL_AUDIODRIVER=dummy \
  ./build/pd.x86_64
```

"Boots and runs an attract-mode session with no crash/assert" is the regression gate used below. It is **not** a substitute for real gameplay/visual testing on actual hardware — items marked *needs runtime validation* require that before they can ship.

---

## Implemented in this change (validated: builds in -Og/-O2/sanitizer, boots clean)

### B1 enablement — optimization + sanitizer builds
`CMakeLists.txt` now exposes:
- `-DPD_OPTIMIZE` → `-O2` instead of the conservative `-Og`. **Now defaults ON** (see the "risky batch" note below).
- `-DPD_SANITIZE=address,undefined` → adds `-fsanitize=…`, frame pointers, and debug info to compile + link.

### B1 investigation — *why* `-O2` was disabled ("until I fix the -O2 issues")
Built with `-DPD_SANITIZE=address,undefined` and ran the headless smoke test. Result:

- **0 AddressSanitizer (memory) errors** during boot + attract mode.
- **916 distinct UndefinedBehaviorSanitizer sites**, ~99% **"member access within misaligned address"**.

These are the decompiled engine reading N64 big-endian, byte-packed structures directly from ROM/segment data at unaligned addresses — level-setup objects (`defaultobj` ×232, `doorobj` ×141, `hovercarobj` ×101, `weaponobj`, `tvscreen`, …), waypoints, model rodata, and the audio bank format (`ALInstrument`/`ALSound`/`ALWaveTable`/`ALADPCM*`). Hot files: `propobj.c` (311), `port/src/preprocess/filesetup.c` (157), `setup.c` (132), `model.c` (56), the naudio/ultra audio loaders.

**This is the root cause of the `-O2` risk:** on x86 unaligned *scalar* loads are tolerated, but at `-O2` the compiler may auto-vectorize assuming alignment, and on stricter ISAs they fault outright. `-fno-strict-aliasing -fwrapv` (already set) do **not** cover misalignment.

The `-O2` binary builds and boots cleanly under llvmpipe, but that is a single-platform smoke test — **not** proof. Flipping the default requires resolving the alignment class first (below).

### B1 down-payment — fixed the two *non-systemic* UB bugs (confirmed gone via UBSan)
These were the only UB outside the alignment class, and clearing UB is a prerequisite for `-O2`:
- `src/lib/anim.c:439` (`animReadSignedShort`) — `1 << (readbitlen - 1)` shifted by **−1** when `readbitlen == 0` (a 0-bit field). Added a `readbitlen > 0` guard; behavior unchanged (it previously only "worked" via x86 shift-count masking).
- `src/game/propobj.c:5073` (`liftUpdateTiles`) — formed `&rodata->type19` on a **null** `rodata`. Pass `NULL` instead; the callee `func0f070ca0` already handles `rodata == NULL`.

### B7 — texture upload buffer leak + overflow (`port/fast3d/gfx_pc.cpp`)
- `malloc(max_tex_size * max_tex_size * 4)` used `int` math (overflows if a backend ever reports a max ≥ 23170). Now `size_t`.
- The buffer was **never freed**. Now released in `gfx_destroy`. (Size kept — it must hold high internal-res framebuffer-effect captures; lazy growth is a future item.)

### Build hygiene — stale incremental builds
`file(GLOB_RECURSE …)` for `port/`, `src/game/`, and the lib audio globs now use `CONFIGURE_DEPENDS`, so adding/removing a source file re-triggers CMake instead of silently building stale.

---

## Risky batch — landed, but REQUIRES HUMAN RUNTIME VALIDATION

These are higher-impact changes that build, boot, run an attract-mode session,
and pass ASan/UBSan **here**, but cannot be fully validated in a headless,
software-GL, x86-only container. They are now **on by default** so a human can
test them; each reverts with a single CMake flag.

### `-O2` is now the default (`PD_OPTIMIZE=ON`)
Replaces the conservative `-Og` that release had shipped for years.
- *Validated here:* `-O2` build compiles, boots, runs attract mode, shuts down clean.
- *Known hazard:* the 916-site misaligned-access UB class is benign on x86 but can
  fault or be miscompiled on **strict-alignment targets (arm64 / Switch)** and is
  exactly the kind of thing `-O2` auto-vectorization can expose. `-fno-strict-aliasing`
  and `-fwrapv` are retained.
- *Needs a human to:* play through real levels on x86, **and** build+run on arm64/Switch.
- *Revert:* `-DPD_OPTIMIZE=OFF`.

### Float matrix pipeline is now the default (`PD_GBI_FLOATS=ON`, `-DGBI_FLOATS`)
Enables the fully-wired `GBI_FLOATS` path (`gbi.h` makes `Mtx` a `float[4][4]`;
`mtxF2L`/`mtxF2LBulk`/`guMtxF2L`/`gfx_sp_matrix` become passthroughs), removing the
float→fixed-point→float round-trip done on every matrix load and improving precision.
- *Validated here:* compiles, boots, attract mode runs with no crash/NaN; **UBSan shows
  no new UB and ASan shows no memory errors** vs the fixed-point path.
- *Needs a human to:* confirm rendering is visually correct (geometry placement,
  no jitter/warping) across several levels and split-screen.
- *Revert:* `-DPD_GBI_FLOATS=OFF`.

### Deliberately NOT in this batch
Allocator replacement (memp/mema → malloc+arena) and native threaded audio were
left for separate work: a subtle bug there only manifests under extended gameplay
/ memory pressure that this environment cannot reproduce.

---

## GPU vertex transform (`PD_GPU_VERTEX`, default OFF) — EXPERIMENTAL

Moves vertex **transform, aspect adjust, and fog** from the CPU into the GLSL
vertex shader. CPU still does lighting/texgen (they use model-space normals and
don't affect batching).

**Why it's hard / why it's default-OFF:** fast3d pre-transforms vertices on the
CPU so that objects with *different* matrices batch into one draw call (the flush
never breaks on a matrix change). To preserve that with GPU transform, each
`gSPVertex` records its transform state into a per-frame **matrix palette**
(`{mat4, fog_mul, fog_offset, aspect_scale, aspect_ofs}`) uploaded as a **UBO**;
the vertex shader looks up `palette[index]` (the index rides in `aVtxPos.w`, so the
attribute layout is unchanged at 4 floats). invert-Y and the aspect divide are
folded into the entry so the shader needs no extra uniforms. Culling moves to
`glCullFace` (flush-on-cull-mode-change); trivial clip-reject is left to hardware.

**Baseline constraint:** no SSBO on GL3.0/ES3.0/macOS-4.1, so a UBO is used,
capped at `GFX_VTX_PALETTE_MAX = 192` entries (≤16 KiB). On overflow the batch is
flushed and the palette reset — correct except that a vertex loaded before an
overflow flush and reused after it can pick up a wrong matrix (rare; only very
heavy frames). UBO syntax uses `GL_ARB_uniform_buffer_object` on the GLSL-130
desktop path (core in ES 300).

**Likely perf outcome:** for N64-era geometry, CPU transform is microseconds and
the palette bookkeeping adds CPU work, so this may be a wash or a slight
regression. It exists to be **benchmarked on real hardware**.

**Validated here:** builds (default/`-O2`/sanitizer); the city-intro flyby and the
rotating-logo (many per-frame matrices) render correctly vs the CPU path with no
distortion/inversion; ASan + UBSan show **0 memory errors and 0 new UB** vs the
baseline. **Needs a human to:** play through varied scenes (esp. heavy MP for the
overflow caveat, and transparency/cull-sensitive geometry), check macOS/ES/Switch,
and benchmark. Revert: `-DPD_GPU_VERTEX=OFF` (and it's off by default).

---

## Roadmap — remaining B items (not yet implemented)

Ordered by impact-to-effort. Each notes the prerequisite and validation cost.

### Phase 1 — make the now-default `-O2` safe everywhere (high impact)
`-O2` is already the default (see risky batch above). The remaining work is to
**fix the misalignment UB class** so it is safe on strict-alignment targets, not
just x86. Options, cheapest first:
   - Read packed data through `memcpy`/byte accessors (or `__attribute__((packed))` views) at the load/parse boundary (`filesetup.c`, `segaudio.c`, `bnkf.c`, `n_load.c`, `model.c`) so the rest of the engine sees aligned structs.
   - Or copy setup/object data into aligned heap structs once at level load (also helps cache locality).
   - *Validation:* UBSan smoke test → 0 misalignment sites; then `-O2` boot + gameplay test on x86 **and** a strict-alignment target (arm64 Switch).
2. **LTO** (`-flto`) once `-O2` is stable. Build-time tradeoff; verify the C/C++/asm mix links.

### Phase 2 — remove per-frame N64 ceremony (medium effort, *needs runtime validation*)
3. **Delete the `OSSched` RSP/RDP arbitration on PC** (`src/lib/sched.c`, `port/src/pdsched.c`) and run audio on its own thread (the PC `amgrFrame` path at `audiomgr.c:324` is half-built). Submit gfx directly to the backend.
4. **Strip cache ops / `osVirtualToPhysical` / DMA chunking** on PC (already collapses to `bcopy`).
5. ~~Skip the per-call fixed-point matrix decode via `GBI_FLOATS`.~~ **Done** (risky batch, default ON).

### Phase 3 — renderer modernization (high effort, *needs GPU runtime validation*)
6. **Move vertex transform/lighting/texgen/fog to the GPU** (currently CPU, single-threaded, `gfx_pc.cpp:1055-1196`). **Transform + fog done** behind `PD_GPU_VERTEX` (default OFF, experimental — see the GPU vertex transform section above). Lighting/texgen still CPU.
7. **Stop per-draw `glBufferData` re-upload**; use persistent-mapped/larger batches and instancing.
8. **Precompile/cache combiner pipelines** instead of runtime `sprintf`+`glCompileShader` per combiner.
9. **Restore dropped mipmaps** (`gfx_pc.cpp:1351,1456`) and enable anisotropic filtering.
10. Longer term: a modern explicit API (Vulkan/D3D12/Metal via SDL_gpu or bgfx).

### Phase 4 — audio / assets (medium effort)
11. **Retire the `mixer.c` N64-ucode emulation**; decode ADPCM/MP3 once to PCM and mix with a host library. Replace the `SDL_QueueAudio` push model that drops buffers under load (`audio.c:60`); fix the `22020 Hz` rate.
12. **Byteswap assets once** at extraction/cache instead of on every load (`romdata.c`).

### Phase 5 — memory (medium effort, *needs runtime validation*)
13. **Replace memp/mema/alHeap** with host `malloc` + a real per-stage arena; removes the O(n²) defrag on the alloc hot path (`mema.c:458`), the 124-slot leak, the 300 KB cap, and the 4/8MB split.
14. **Lazy-grow `tex_upload_buffer`** instead of the up-front max allocation (requires per-importer capacity guards — defer until each importer's worst-case write size is verified).

---

## Notes for CI

Add to CI (currently compile-only): a `-DPD_SANITIZE=address,undefined` job running the headless smoke test, and (once Phase 1 lands) a `-DPD_OPTIMIZE=ON` job. A state-hash determinism test is tracked separately under the netplay work.
