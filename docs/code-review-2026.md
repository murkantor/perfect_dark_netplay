# Perfect Dark Port — Full Codebase Review

**Branch reviewed:** `claude/port-net-predict-review-rZby6` (identical to `port` HEAD `132e59d`)
**Date:** 2026-06-01
**Scope:** Entire repository — `src/game`, `src/lib` (libultra/OS emulation), `port/src` + `port/fast3d` (PC backend), `src/setups`, build system, tooling, headers.
**Mandate:** No compromise. Honest, brutal, for-improvement assessment of duplication, comments, spaghetti, inferior code, N64 holdbacks, and modernization (memory, graphics, audio, multiplayer/netplay).

> ⚠️ The actual netplay/prediction code lives on a **separate** `port-net-predict` remote branch and is **not present** in this tree. This review covers the engine the netplay layer must build on, and gives a brutally honest netplay-readiness assessment based on that engine (§8).

---

## 0. Verdict up front

This is a **genuinely high-quality decompilation** with a **competent but thin PC compatibility shim** bolted on top. The reverse-engineering is near-complete (almost no `func_xxxx` left), comments are frequently excellent, and a real platform-abstraction layer (`PLATFORM_N64`/`PLATFORM_64BIT`, 561 guard sites) has been started.

But — and this is the headline — **it is still structurally an N64 game running on a software RCP, not a native port.** Every frame it:

- re-interprets N64 RSP/RDP display-list bytecode on the CPU (`port/fast3d/gfx_pc.cpp`),
- decodes fixed-point S15.16 matrices and segmented pointers,
- emulates the N64 audio microcode sample-by-sample (`port/src/mixer.c`),
- runs a full reimplementation of Nintendo's `OSSched` task scheduler (`src/lib/sched.c`, `port/src/pdsched.c`),
- and drives all of it from a single god object (`g_Vars`, **9,353 references** in `src/game` alone) through an **implicit "current player" register** instead of passed state.

None of that is a *bug*. It's *architecture* — and that architecture is the single biggest obstacle to both modernization and netplay.

---

## 1. Hard metrics

| Metric | Value | Note |
|---|---|---|
| Total C/H LOC (src + port) | ~470k | `src/game` 245k, `src/lib` 48k, `port` 20k, rest assets/setups |
| Largest single file | `src/game/propobj.c` **21,508 lines** | followed by `chraction.c` 16k, `bondgun.c` 13k |
| `g_Vars` god-object references | **9,353** (game) / 9,677 (all) | one global holds nearly all mutable state |
| Distinct `g_*` globals | ~1,824 | |
| `g_Vars.currentplayer` / `players[]` touches | ~4,831 | the implicit-current-player anti-pattern |
| `struct g_vars` fields | 139 | timing + sim + render scratch all mixed |
| `struct player` size | ~7.2 KB, ~431 pointer refs | pointer-graph, not flat state |
| TODO/HACK/FIXME/@bug markers | **289** total | bugs *documented*, rarely *fixed* |
| `#if VERSION/PAL/PLATFORM/AVOID_UB` blocks (game) | ~1,681 | duplication-by-preprocessor |
| Hex-address-named files | 44 (13 real + 31 stubs) | incomplete-RE smell |
| `xxx0fHHHHHH` functions / `var80xxxxxx` globals | ~1,197 / ~1,341 | partially reversed, semantically opaque |
| `goto` count | 9,332 | **see caveat below** |
| Unit/regression tests | **0** | the single biggest process gap |

### Metric caveat (honesty correction)
The raw 9,332 `goto` count is **misleading and should not be cited as spaghetti**: **~8,099 are in `src/setups/`** and **~1,222 in `gailists.c`** — these are *data tables* (level scripts and AI bytecode) where opcodes like `beginloop`/`goto`/`endloop` are macros. **Real hand-written `goto` in all of `src/game` + `src/lib` is ~11.** The decomp's control flow is clean; the spaghetti here is **function length and global coupling**, not jumps. Anyone reviewing this codebase should know that distinction.

---

## 2. The core architectural problem (read this first)

Three intertwined design facts shape everything else:

### 2a. One god object + implicit "current player"
`g_Vars` (`src/include/types.h:122`, instantiated `src/lib/varsinit.c:11`) bundles frame timing, the player array, prop linked-lists, the AI list pointer, cutscene state, and render scratch. The engine never passes player state — it mutates `g_Vars.currentplayer` via `setCurrentPlayerNum(i)` (`src/game/playermgr.c:664`) and reads it from ~4,831 sites. `playerTick()` (`src/game/player.c:3243`, ~1,100 lines) takes **no player argument**.

**Consequence:** there is no "world" or "player" object you can instantiate twice, snapshot, or serialize. This is the prerequisite blocker for netplay (§8).

### 2b. Simulation and rendering are fused
`mainTick()` (`port/src/pdmain.c:525`) in a *single* call: grabs the master display list, emits `gDPSetTile` GBI commands, runs the sim (`lvTick`/`lvTickPlayer`), renders (`lvRender`), swaps buffers, and submits an RDP task — looping per-local-player to set viewports (`pdmain.c:548-561`). **411 functions in `src/game` take/return a `Gfx *`** they append to. There is no retained-mode/scene-graph boundary; sim cannot run "headless" for prediction.

### 2c. Hardcoded 4-local-player ceiling
`MAX_PLAYERS 4` (`src/include/constants.h:25`), and `PLAYERCOUNT()` is a **compile-time-unrolled macro** (`constants.h:71-79`), not a counter — bumping the constant does *not* add players. `MAX_MPCHRS = 12`. The entire model is "4 split-screen viewports on one machine"; there is no concept of a remote player.

---

## 3. Must-fix bugs & unsafe code (severity-ordered)

These are real defects in the **PC port layer** (not decomp-faithfulness artifacts) and should be fixed regardless of any larger refactor:

1. **Unbounded EEPROM `memcpy`** — `port/src/libultra.c:312,325`: `memcpy(eeprom + address*8, buffer, nbytes)` with `address`/`nbytes` unvalidated against `EEPROM_SIZE`. Memory-corruption vector on a static buffer.
2. **Unbounded `sprintf` into fixed shader buffers** — `port/fast3d/gfx_opengl.cpp:242-243` (`char vs_buf[2048]; char fs_buf[8192]`) built via dozens of unchecked `sprintf(fs_buf+len, ...)`; `append_str` is a raw `strcpy`. A complex color-combiner overruns the stack.
3. **256 MiB upload buffer, allocated up front and never freed** — `port/fast3d/gfx_pc.cpp:2570-2574` (`8192*8192*4`), `int` multiply can overflow for larger caps; `gfx_destroy` deliberately does not free it.
4. **Stubbed message queues make the threading model unsound** — `port/src/libultra.c:101-109`: `osSendMesg`/`osRecvMesg` are no-ops that never enqueue/dequeue. Works only because everything is single-threaded; UB-by-omission the moment real concurrency (i.e. netplay threads) is added.
5. **Unguarded `prefix[level]`** — `port/src/system.c:181`: 3-entry array indexed by arbitrary `s32` param.
6. **`strncat` misuse** — `port/src/system.c:71-73`: passes total buffer size where remaining space is required; can overflow `logPath[2048]`.
7. **Recursive display-list interpreter with no depth limit** — `gfx_run_dl` self-calls on `G_DL` (`gfx_pc.cpp:2318`); a malformed/cyclic DL blows the C stack.
8. **MP3 decode straight into a 1160-byte buffer** — `port/src/mixer.c:699` (self-flagged `// FIXME: ... bite us in the ass`); guarded only by an `assert` compiled out in release.
9. **`fread` return values ignored throughout** — `port/src/fs.c:215,249`, `libultra.c:280`, `romdata.c`: truncated files yield partially-initialized buffers treated as valid.
10. **Leaks on error paths** — `port/src/mod.c:352` (`// FIXME: this leaks`), `mod.c:496-524` (descriptor leaked on parse error).
11. **Non-reentrant static-buffer returns** — `fs.c:57` (`fsFullPath` → `static char pathBuf`), `input.c:1073`. Two calls in one expression alias.
12. **Data race in SDL event watch** — `port/src/input.c:739` `SDL_AddEventWatch` runs on SDL's thread and mutates `lastKey`/`mouseWheel`/`pads[]`/`connectedMask` with no synchronization (`input.c:501-548`).
13. **Cosmetic but ugly:** `port/src/pdmain.c:370` `if (index);` (no-op dead statement); `pdmain.c:357-393` stray trailing `\` **line-continuation backslashes left in plain function code** from a de-inlined macro; `pdmain.c:346` an "infinite" outer loop whose `ending` flag is never changed.

### Documented-but-unfixed decomp bugs worth promoting to real fixes
The decomp annotates rather than fixes (correct for matching, wrong for a shipped port): `src/lib/memp.c:246` (`@dangerous` heap overflow, no bounds check), `src/lib/mema.c:228` (`@bug @dangerous` `+0x8e0` overflow into the gun-names file), `src/lib/naudio/n_csplayer.c:942` (`chanstate` uninitialised read), `src/lib/ultra/io/conteepread.c:37` (off-by-one `>=` checks). 92 `@bug` annotations exist in `src/game` alone.

---

## 4. Memory management — the weakest subsystem

The game ships **four** hand-rolled allocators, none using the host `malloc`, all encoding 4MB/8MB cartridge-era assumptions:

- **memp** (`src/lib/memp.c`) — a pair of bump pointers; **no freeing exists at all** (`memp.c:43-45`), memory reclaimed only by stage reset. `mempRealloc` is self-described `@dangerous` with no bounds check (`memp.c:246-273`). The 8 "pools" are fiction — comment admits only 2 are used (`memp.c:26`). PC expansion size hardcoded `8*1024*1024` with `// TODO: set this in a config` (`memp.c:47`).
- **mema** (`src/lib/mema.c`) — the *only* freeable allocator; an O(n²) free-space tracker with `MAX_SPACES 124` slots that **deliberately leaks** when exhausted (`mema.c:160`), runs "8 defrag passes" on the allocation hot path (`mema.c:458`), and admits "the excessive looping is just wasting CPU cycles" (`mema.c:131`). Heap can never be compacted because there's no handle abstraction (`mema.c:34`). Total heap hardcoded to 300 KB (`src/lib/main.c:79`).
- **alHeap audio** (`src/lib/ultra/audio/heap.c`) — bump allocator with an **empty overflow branch** (`heap.c:24`); **no `alHeapFree` exists anywhere** in the tree. Callers don't null-check.
- **OOM policy** is `CRASH()` (a deliberate `*(u8*)0 = 69`, `constants.h`) in debug, silent `return 0` in release.

**Recommendation:** Replace memp/mema/alHeap with the host allocator behind a thin `pdMalloc/pdFree` shim, keeping a *proper* per-stage arena (`arena_reset()` on stage load) only where the bump-reset semantics are genuinely depended on — with real bounds checks, 16-byte alignment baked into the primitive (today alignment is re-done ad hoc at call sites, e.g. `vi.c:222`), and `assert`-on-OOM. Delete the 124-slot leak, the O(n²) defrag, the `+0x8e0` overflow, the 4/8MB split, and the 300 KB cap. There is **no reason** a PC port retains any of them. Convert all fixed-buffer `sprintf` (`mema.c:290`, `memp.c:216`, `boot.c:317`, `main.c:979`) to `snprintf`.

---

## 5. N64 holdbacks (the big section)

A large, **removable** layer of N64-hardware ceremony sits between the game and the host OS. On PC the following are dead weight retained for structural fidelity, not function:

### In the OS-emulation layer (`src/lib`)
- **DMA "from ROM"** (`dma.c`) collapses to a single `bcopy` on PC (`dma.c:78`), yet retains 32-slot busy tracking, message-queue waits, 0x4000-byte chunking math, and `dmaCheckPiracy` XOR-decrypt against ROM `0x340`.
- **The scheduler** (`sched.c`, 878 lines) is a full reimplementation of `OSSched` arbitrating a single RSP between audio and gfx — a constraint that **does not exist** on a PC with a real GPU + audio thread. Author flags own dead code: "This is unreachable" (`sched.c:380`), "the condition in this loop never passes" (`sched.c:374`).
- **VI retrace register munging** (`vi.c:281-463`, ~160 lines of `osViModeTable` xScale/yScale/hStart magic) — almost entirely emulated away on PC.
- **Cache ops** (`osWritebackDCacheAll`/`osInvalDCache`) — no-ops on a coherent PC; pure ceremony.
- **Segment/virtual-address translation** (`osVirtualToPhysical`, `OS_K0_TO_PHYSICAL`) used pervasively for an MMU model PC doesn't have (`audiomgr.c:271`, `vi.c:552`, `main.c:633`).
- **Threads with hand-allocated right-to-left stacks** from `K0BASE+4MB` and `0xdeadbabe` manual overflow guards (`boot.c:190-221,306-330`); `idleproc` is `while(true);` (`boot.c:240`). Release builds (`VERSION >= NTSC_1_0`) have **no** stack-overflow protection at all.
- **N64 audio microcode emulated sample-by-sample** on a fake 3072-byte DMEM with big-endian XOR hacks (`mixer.c:37-49,548-552`).

### In the game/port layers
- **4MB-vs-8MB Expansion Pak branching gates *gameplay and content*** — 197 `IS4MB()`/`IS8MB()` references (`body.c:199`, `texdecompress.c:234`, `menu.c:3466`); the per-frame stage-allocation tables in `pdmain.c:355-413` are duplicated for each RAM size.
- **Display-list/GBI rendering threaded through the sim** (§2b) — 411 `Gfx *`-passing functions; fixed-point matrix decode (`gfx_pc.cpp:986`); segmented pointers (`gfx_pc.cpp:235,2262`); CPU vertex transform + 4-light Gouraud + texgen + fog all re-running the RSP vertex program on the CPU, single-threaded (`gfx_pc.cpp:1055-1196`).
- **Cycle-counter timing** — `CYCLES_PER_FRAME = OS_CPU_COUNTER/(PAL?50:60)` (`constants.h:60`), `osGetCount()` busy-wait frame pump (`pdmain.c:501-511`).
- **Big-endian asset byteswap on every load** — `romdata.c` (`PD_BE32` everywhere), self-described "fucking terrible" (`romdata.c:21`).
- **Self-modifying piracy/checksum code** that overwrites its own `.text` (`game_006900.c:61`, `#if PIRACYCHECKS`) — dead in the port but a landmine.
- **`M_BADPI 3.141092641f`** (`src/include/math.h:14`) — the original game's deliberately-imprecise pi, preserved. A neat determinism/holdback emblem.

### In the build/repo
- **Linker scripts** `ld/*.ld` + `gamefiles.*.inc` (`OUTPUT_ARCH(mips)`), **`checksums.*.md5`** (6 × ~118 KB), `tools/mkrom`, `tools/asm_processor`, `tools/patchmips3`, `stagetable.txt`, and the **uninitialized `tools/recomp` submodule** — all N64 ROM/MIPS tooling **referenced nowhere** in the port build or CI. ~1 MB+ of dead weight that still forces `--recursive` clones. `tools/mkrom/Makefile:4` is even broken (`%<` instead of `$<`).

---

## 6. Duplication, structure & comments

### Duplication / doubled-up features
- **44 lifecycle-split files** (`*reset.c`/`*stop.c`/`*init.c` per subsystem) — one logical module fragmented across 3-4 translation units (decomp artifact).
- **Nine `bond*` files** for one player's first-person state machine.
- **4MB/8MB stage-allocation lookup** copy-pasted verbatim (`pdmain.c:355-413`).
- **Boot framebuffer/copyright-logo block** ~100 lines duplicated between version paths (`main.c:391-496` vs `519-621`), *including the same bug comment* about double-zeroed pixels.
- **`amgrHandleFrameMsg` (N64) vs `amgrFrame` (PC)** — near-identical audio-frame logic reimplemented (`audiomgr.c:243-312` vs `324-379`).
- Smaller: `sysGetExecutablePath`/`sysGetHomePath` (`system.c`), `inputMouseGetScaledDelta`/`*AbsScaledDelta` (`input.c`), `dmaExec`/`dmaExecHighPriority`, `configClampInt/UInt/Float`, `g_RdpTaskA`/`B` byte-identical initializers, triple SSE/NEON/scalar DSP kernels in `mixer.c` (the scalar `aResample` uses a *different* algorithm than the SIMD paths — silent divergence risk).
- **~1,681 preprocessor `#if` blocks** in `src/game` = duplication-by-macro, many differing only by a constant.

### Spaghetti (it's *size*, not jumps)
Monster functions: `projectileTick` ~1,300 lines (`propobj.c:6245`), `playerTick` ~1,100 (`player.c:3243`), `playerUpdateDamageStats` ~900 (`chraction.c`), `propGetShieldThing` ~890 (`chr.c`), `gfx_run_dl` 260-line god-switch with dead locals (`gfx_pc.cpp:2277`). Deep ≥5-tab nesting dominated by interleaved `#if PAL/VERSION/PLATFORM` branches.

### Comments / notes
Density is ~9% (low) but **the comments that exist are good** — genuine prose explaining intent (the mema/memp/gfxmemory header essays, the `g_vars` timing-units legend). The pathology is **self-aware admissions instead of fixes**: `mixer.c:522` `// why the fuck is this here?`, `main.c:972` "maybe graphics tasks stop being created... ?" (maintainer doesn't understand the code), `sched.c:505` `@TODO: Investigate. I'm not sure how this list works`, plus the 92 `@bug` / 289 total markers. For a *maintained port*, the documented memory/UB bugs (§3) should be fixed, not annotated.

---

## 7. Build, tooling & testing

- **Zero automated tests.** No `add_test`/`ctest`/unit/regression/smoke test anywhere. For a project whose value is *behavioral fidelity* — and which is now adding *determinism-sensitive netcode* — this is the **single biggest process gap**. A desync with no repro harness is unfixable.
- **CI is compile-only** (`.github/workflows/c-cpp.yml`): 7 platform combos, "it compiled" is the only pass criterion, no `-Werror`, ~12 warning classes disabled (`CMakeLists.txt:180-198`). Only the `port` branch is built — this branch and the netplay branch get **no CI**. Bonus hygiene bugs: `retention-days: 0` (invalid), `sudo dpkg --add-architecture amd64` on an amd64 host, destructive force-push of the `ci-dev-build` tag on every push.
- **No static analysis / sanitizers / formatting** despite emitting `compile_commands.json`. Given `AVOID_UB`, `-fwrapv`, `-fno-strict-aliasing` and the admitted **`-O2` breakage** (release ships `-Og` "until I fix the -O2 issues", `CMakeLists.txt:165`), ASan/UBSan would find real bugs immediately.
- **CMake is a flat 361-line pile** of platform conditionals using fragile `GLOB_RECURSE` (stale incremental builds) *inconsistently* (neighboring `SRC_LIB` is a hand list). `list(PREPEND SRC_LIB "${CMAKE_SOURCE_DIR}")` (`:313`) is an unexplained no-op hack. Big-endian is `# TODO`-hardcoded `FALSE` (`:46`) despite full BE support in `platform.h`.
- **God headers:** `types.h` **6,186 lines / 147 KB**, `constants.h` 4,766, `commands.h` 4,397. Touching one struct recompiles the world. Base types come from SGI's 1995 `ultratypes.h`, not `<stdint.h>` — and `vu32`/`vs32` are `volatile long`, which is **64-bit on LP64 Linux/macOS** (a genuine width landmine). `bool` is `#define`d to `s32`.
- **No `CONTRIBUTING.md`, `ARCHITECTURE.md`, or netplay design doc**; `docs/` holds only 4 game-trivia files. SGI-proprietary headers sit next to the project `LICENSE` with no `NOTICE` reconciling them.

---

## 8. Netplay suite — brutally honest readiness

> The netplay code is on `port-net-predict`. This is an assessment of whether **this engine** can support the major netcode models.

### Determinism: three independent showstoppers
1. **Variable timestep tied to wall-clock cycles.** `frametimeCalculate()` derives the sim step from `osGetCount()` (`timing.c:33`); `lv.c:2124-2195` turns it into `lvupdate60`/`lvupdate60freal`, and **every movement/physics/timer line is scaled by it** (e.g. `bot.c:2408`). Two machines at different framerates take different-magnitude steps → guaranteed divergence.
2. **Prop sim is time-sliced round-robin across frames** with framerate-dependent catch-up steps (`prop.c:1823`, `runstateindex`). Which entity updates on which frame depends on framerate and slot phase. Bot AI additionally throttles on **what's on screen** (`bot.c:898-906`, `bgRoomIsOnscreen`) — behavior depends on each viewport's rendering.
3. **Float-heavy sim + shared RNG seeded from the cycle counter.** `bondmove.c` alone has 128 float lines / 9 trig calls; `rngRandom()` is deterministic but seeded from `osGetCount()` at boot (`main.c:792`) and **shared between gameplay and cosmetics** (e.g. sunglasses pick `body.c:302`), so any render/sim divergence desyncs the stream.

### State serialization: none exists
No snapshot/savestate/rollback mechanism anywhere. Live state is a **pointer graph** — `g_Vars` linked-list heads, `players[4]` (~7.2 KB, ~431 pointers each), `prop` doubly-linked lists (`next/prev/parent/child`), `chrdata` (158 pointers), `aibot`, plus transient handles (`Mtx*`, `sndstate*`, display lists). Snapshotting requires deep copy + pointer fix-up + transient relinking every frame.

### Input path: tangled into the sim, sub-frame sample history
Not a clean per-frame snapshot. `joy.c` keeps a **ring buffer of up to 20 intra-frame samples** (`joy.c:35`); gameplay iterates that history for double-taps/edges (`bondmove.c` makes 54 `joyGet*` calls, many `*OnSample`). Input is read *inside* the sim, keyed by physical controller index (`player.c:1939`). The only injection seam is overwriting `g_JoyData->samples[].pads[contpadnum]` before each per-player tick — and you'd have to synthesize a plausible sample history or refactor all `*OnSample` consumers.

### Feasibility verdict
| Model | Feasible as-is? | Why |
|---|---|---|
| **Rollback (GGPO/GGRS)** | ❌ No | Fails all three prereqs: no determinism, no cheap snapshot/restore, no fixed timestep. Entity pointer-graph makes per-frame save/restore impractical. |
| **Delay-based lockstep** | ❌ No | Avoids snapshots but still needs determinism — variable timestep + fps-dependent prop/bot ticking desync within seconds. |
| **Client-server authoritative** | ✅ Realistic near-term | Host runs the existing (self-consistent, non-deterministic) sim; clients send inputs, receive replicated state. Sidesteps determinism *and* snapshots. Costs: latency without added prediction; you replicate a chosen subset of `player`/`chrdata`/`prop` fields rather than the whole graph. The separate bot path is *fine* here — host runs bots locally. |

### Prerequisite refactors for true peer determinism (priority order)
1. **Kill the implicit "current player"** — thread an explicit `struct player *` through `playerTick`/`lvTickPlayer`/the `bond*` machines/AI (~4,831 touch sites). Mechanical but enormous; **prerequisite for everything else**.
2. **Fixed 60 Hz timestep**, render-interpolated separately. Remove `lvupdate60f`/`diffframe240` scaling from gameplay math and the fps-dependent hacks (`propobj.c:9020`). Touches nearly every gameplay file.
3. **Input abstraction layer** — replace direct `joyGetButtons`/`*OnSample` calls with a defined per-tick input record fed by local controller *or* network.
4. **Isolate gameplay RNG** from cosmetic RNG; seed from the session, not `osGetCount()`. (`rng_c.c` core is already deterministic + serializable — good.)
5. **Decouple sim from render** — remove per-player render+sim interleaving (`mainTick`, `lv.c`) and onscreen-dependent throttling (`bot.c:898`).
6. **Flatten the entity system** into index-addressed pools (only if pursuing rollback) so a snapshot is a `memcpy` of arrays + indices, transient pointers rebuilt on restore.
7. **Make `MAX_PLAYERS`/`PLAYERCOUNT()` runtime** (replace the unrolled macro and the 12 hardcoded `players[0..3]` sites).

**Honest distance estimate:** Items 1, 2, 5 alone are multi-month efforts touching most of `src/game`; item 6 is a near-rewrite of the entity system. Pragmatic plan: **ship client-server authoritative first** (achievable via a replication layer + the `g_JoyData->samples` input seam), treat fixed-timestep determinism as the long-term goal that *might* later enable lockstep, and consider rollback only after the entity system is flattened.

---

## 9. Modernization recommendations (unconstrained by N64)

**Graphics** — the backend (`gfx_opengl.cpp`) is OpenGL-only, GLSL-130-baseline (2008), immediate-style. It re-uploads geometry per draw (`glBufferData(...STREAM_DRAW)` + `glDrawArrays`, batches of ≤256 tris), runtime-`sprintf`-and-compiles a GLSL program per color-combiner (first-seen hitches), drops mipmaps (`gfx_pc.cpp:1351,1456` `// TODO: fix this`), and fakes depth-clamp with `gl_Position.z *= 0.3f`. Recommend: a modern explicit API (Vulkan/D3D12/Metal via SDL_gpu or bgfx) for proper command-buffer batching; precompiled/cached PSOs to kill combiner hitches; **move vertex transform/lighting/texgen/fog to a vertex shader** (currently all CPU, single-threaded); then the free wins — per-pixel lighting, anisotropic filtering, real HDR (the `vidOverexposure`/`vidGlareBrightness` 0..1 clamps are hacks), modern AA (TAA/FSR/DLSS), bindless texture arrays instead of the 1024-entry LRU + 256 MB scratch buffer.

**Audio** — retire `mixer.c` (impressive but it's N64-RSP cosplay). Decode ADPCM/MP3 once to PCM and mix with a modern library (miniaudio/SoLoud) for effects, 3D positioning, reverb. Replace the `SDL_QueueAudio` push model that **drops buffers under load** (`audio.c:60`) with a pull/callback or ring buffer. Fix the hardcoded, slightly-wrong `22020 Hz` (`audio.c:25`) and resample to the device's native rate once at output.

**Input** — the SDL2 `SDL_GameController` layer is the most modern part already (hot-plug, rumble, per-pad deadzone). Migrate to SDL3 `SDL_Gamepad` for gyro/touchpad (half-referenced as `JOY1_TOUCHPAD`); add synchronization to the event-watch callback (§3.12); document the magic `0.022f/3.5f` mouse-sensitivity literals.

**Threading & I/O** — replace the `osCreateThread` + hand-allocated stacks + `0xdeadbabe` guards with `std::thread`/`pthread` and OS guard pages; finish the PC audio thread (`amgrFrame`) and **delete the scheduler emulation** on PC; replace `dmaStart`/chunking with `pread`/`mmap` (it's already just `bcopy`). Remove cache ops and `osVirtualToPhysical` on PC entirely.

**Memory** — §4. **Process** — §7: add a determinism/state-hash regression test, an ASan+UBSan CI job, clang-tidy, `-Werror` on a curated set, CMake presets, and resolve the `-O2` problem. Quarantine the N64-only build artifacts into a labeled `n64-decomp/` area or separate repo.

---

## 10. Prioritized action list

**Now (correctness, low effort):**
- Fix the unsafe buffers/leaks/races in §3 items 1-6, 12; convert `sprintf`→`snprintf` everywhere; promote the documented memory/UB `@bug`s (§3) to real fixes.
- Clean the `pdmain.c` macro residue (`if(index);`, stray `\`, dead `ending` loop).
- Add an ASan+UBSan CI job and a headless smoke test; build *all* branches in CI; fix the CI hygiene bugs.

**Next (enables everything, medium effort):**
- A determinism harness (same inputs → identical state hash) — mandatory before any peer netcode.
- Replace the four allocators with a host-malloc + arena shim (§4).
- Begin carving `types.h` into domain headers; migrate base types to `<stdint.h>` and fix the `vu32`=`long` LP64 bug.

**Strategic (architecture, high effort):**
- Thread explicit player state (kill `setCurrentPlayerNum` global), fixed timestep, input abstraction, sim/render decoupling (§8.1-5).
- Ship **client-server authoritative netplay first**; defer lockstep/rollback behind the determinism + entity-flattening work.
- Move rendering to a modern GPU pipeline (§9) and retire the software RCP/scheduler/VI/audio-ucode emulation on PC.

---

*The decompilation itself is admirable work — near-complete RE, honest annotations, a real platform layer already underway. The brutal truth is simply that it is still an N64 game in a PC costume: one god object, an implicit current-player register, GBI rendering fused into the tick, four cartridge-era allocators, a software RCP, and a hardwired 4-local-player ceiling. Those aren't bugs — they're the architecture, and they are exactly what stands between this codebase and both modern hardware and modern netplay.*
