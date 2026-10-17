# Midas time scale (unlimited fps, logic locked)

Kai renders at any frame rate while game logic stays locked to its 60 Hz
behaviour. The design comes from Project Midas, the GoldenEye PC port. Read
this doc when touching `frametimeCalculate`, lv.c's delta derivation,
`detPinTimestep`, anything that reads `lvupdate60f`/`lvupdate60freal`, or the
frame-rate settings.

## The model

There are two clocks per frame, as in Midas.

- **Discrete logic.** The integer clocks are unchanged: `lvupdate240`, and
  `lvupdate60` with its remainder carry. AI, timers, cadences and every
  `for (i < lvupdate240)` filter keep exactly their old behaviour. Above
  240 fps `lvupdate240` is 0 on some frames, and those systems simply wait,
  which is correct.
- **Continuous motion.** `g_LvUpdate60fFrac` / `g_LvUpdate60fFracReal` in lv.c
  hold the frame's *real* elapsed time in 60 Hz fields, as a fraction. It
  comes from `g_FrameDiff240f` (timing.c), smoothed with Midas's
  EMA-plus-bank smoother.
  - Systems converted to it advance on **every** frame at any fps.
  - On any frame whose time was overridden it falls back to `lvupdate60f`, so
    converted systems obey the override exactly as before. Overrides are
    pause, slow motion, the chaos time effects, the netplay / det pin, and
    anything else that rewrote `lvupdate240`.

### Why opt-in, unlike Midas

Midas started from a fixed 60 Hz engine and converted everything. PD already
runs a variable step, so `lvupdate60f`/`freal` stay quantised and safe.
Systems move to the fractional delta **one at a time**.

The hazard is Midas's bug 16. Code that uses the float delta *inside* an
integer-time gate or loop loses time on zero-integer frames. For example,
`bot.c`'s sim movement multiplies the float delta inside
`for (i < lvupdate240)`, so it would move about half as far at 500 fps.
Kai has 136 integer gates against 316 float-delta uses, so a global switch
needs that full audit.

## Phase 1 (2026-10-17, branch kai-timing-midas)

- **Clock.** `sysGetMicroseconds` uses QPC on Windows and `CLOCK_MONOTONIC`
  elsewhere. It was `gettimeofday`: wall time, and coarse on some CRTs.
- **Fractional delta and smoother.** `g_FrameDiff240f` in timing.c (smoother
  verbatim from Midas, `PD_NODELTASMOOTH=1` turns it off);
  `g_LvUpdate60fFrac` in lv.c.
- **Converted to fractional time:**
  - camera yaw (`bwalkUpdateTheta`);
  - pitch (the `vv_verta` integration in bondmove, which covers natural look
    and the aim/C-button ramps);
  - mouse look scale: `mlookscale = 1/g_LvUpdate60fFrac`, was
    `4/lvupdate240`. This also fixes vanilla uncapped losing about half the
    mouse motion above 240 fps.
- **Hand loop sounds.** The Reaper spin, laser stream and Mauler charge stop
  only on a *paused* zero frame (the `lvUpdateMiscSfx` gate). They used to
  stop and restart on every render-only frame.
- **Frame-rate settings.**
  - **Max FPS** (`Video.FramerateLimit`, slider 0-500, 0 = Uncapped) and vsync
    are the only frame-rate knobs.
  - Retired on clients, forced off at startup:
    - `Game.TickRateDivisor` (default 1, which was a hard 60 fps cap);
    - `Game.FixedTick` (the Logic Tick Rate dropdown, with its camera
      interpolation).
  - The "Uncap Tickrate" and "Logic Tick Rate" menu items are gone.
  - The dedicated server keeps its own pacing.
  - `/forcetick` still exists as a developer console toggle.

**Unchanged:**
- **Netplay:** pinned at a true 60 Hz. The fractional delta falls back there,
  and frames beyond 60 duplicate except for the banked mouse.
- **Determinism harness:** unchanged.
- **Audio thread:** device-paced, so unaffected.

## Phase 2: convert the rest (each with its own check)

- Player translation (bwalk movement, crouch and lean easing), bike, grab.
- Chr and prop motion, projectiles, doors and lifts. Watch the bot.c pattern:
  inside `for (i < lvupdate240)`, use the per-quantum delta, not the frame
  delta.
- Animation playback (Midas plays fractional animation ticks).
- Gun sway and the hand model. Midas blends the hand pose by sub-field phase.
- Lag filters that iterate per call rather than per quantum, using Midas's
  closed form `x = x*d^dt + in*(1-d^dt)/(1-d)`.
- Optional, later: a render thread with a one-slot hand-off (Midas measured
  +17%) and the vsync lock.

## Honest status

Compile-verified only. Runtime checklist:
1. Uncapped Max FPS, vsync off: the camera turns and pitches smoothly at
   high fps (300+), and mouse sensitivity is the same at 60 and 500 fps.
2. Game speed at 500 fps matches 60 fps: AI, timers, door speeds, fire rate.
   Movement and world still step at up to 240 Hz until Phase 2.
3. Reaper, laser and Mauler loop sounds are continuous at high fps.
4. Slow motion, pause and the chaos SUPERHOT / Fake Crash still freeze or
   slow the camera exactly as before.
5. Netplay feels unchanged.
6. An old `pd.ini` with `TickRateDivisor=1` or `FixedTick=1` no longer caps
   you at 60 fps or turns the fixed tick on.
