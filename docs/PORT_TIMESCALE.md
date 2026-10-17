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

## Phase 2a (2026-10-17): the player's own view/body + two world fixes

Helpers (lv.c):
- `lvLagStep(x, d, in, q)`: Midas `portLagStep`. A whole q runs the original
  per-quantum loop, so it is bit-exact on every fallback frame, where
  `g_LvUpdate60fFrac*4 == lvupdate240`. A fractional q uses the closed form.
- `lvPowf`.
- `g_LvFracActive` (1 = a real fractional frame). Sites whose original form
  steps by whole fields (`lvupdate60`, `applySpeed`) run their original code
  when it is 0.
- `g_LvFrame240`: the frame's global step, used to spot props on a catch-up
  step.

Converted to real fractional time:

| What | Where | How |
|---|---|---|
| Walk/strafe displacement and every other `spcc` term (turbo, forced push, ladder threshold) | bondwalk.c `bwalk0f0c69b8` | FracReal. The velocity is the head anim's `headpos`, ticked on integer steps and held between them, so distance per second is unchanged. |
| Thrown/launched projectile inherited velocity | bondgun.c, two sites | Divides the frame's movement by FracReal, the same delta it was integrated with. |
| Step-up easing (eye on stairs/ledges) | bondwalk.c `sumground` | `lvLagStep` |
| Falling | bondwalk.c | FracReal (trapezoid) |
| Crouch/stand easing (was **60 Hz**) | bondwalk.c `bwalkApplySpeedFrac` | Crouch-local copy of `applySpeed` integrated over real time, sub-steps of at most one field. `applySpeed` itself is shared with doors and lifts and is unchanged. |
| Lean cap ramp | bondwalk.c | closed form |
| Classic snap lean | bondwalk.c | 50% per field instead of per frame |
| Turn/pitch ramps (aim edge turn, C-buttons) | bondmove.c | FracReal |
| Look-ahead pitch | bondmove.c | FracReal |
| Post-teleport momentum `moveinitspeed` (decayed per frame) | bondmove.c | `0.9^fields` |
| Death camera drop (per frame) | bondmove.c | per field |
| Gun position/orientation lag, bob amplitude, bob cycle | bondgun.c | `lvLagStep` |
| Gun bob phase | bondgun.c | FracReal |
| Dual-wield phase drift (per frame) | bondgun.c | per field |
| Crosshair + gun-aim lag | bondgun.c `bgunSwivel` | `lvLagStep` |
| Zoom tween | player.c | FracReal |
| Manual sniper/FarSight zoom | game_0b0fd0.c | per-field factor compounded over real time |
| Chr push decay (per frame) | chr.c | `0.9^fields` of the chr's own step. A high-fps bug; identical at 60. |
| Chr ground smoothing (height on stairs, was 60 Hz) | chr.c | `lvLagStep`, only when the chr runs on the frame's own step |
| **MP/netplay animation snap** | model.c | N64-only now (see below) |

**About the MP/netplay animation snap.** This was not a time-scale issue.
`modelSetMatricesWithAnim` kept the N64 multiplayer shortcut: fast
animations skip the frame blend whenever `PLAYERCOUNT() >= 2`, and remote
pawns count. So every run cycle snapped to whole anim frames in MP and
netplay. It is now `#ifdef PLATFORM_N64`.

Deliberately left alone:
- Knockback decay. It feeds speed into the integer-ticked head anim, so
  converting the decay alone would lose distance.
- The head-anim velocity derivation.
- Run-boost timers.
- Auto-aim lock timers.
- Mauler charge (gameplay).
- Recoil jitter (RNG on fire ticks).

## Phase 2b: still to do (from the world audit)

Ranked by visual impact at high fps:
1. **Lifts** step at 60 Hz and carry the player and camera, because
   `applySpeed` loops in whole fields. This needs a render-side sub-field
   offset for the lift *and* its riders, or a fractional `applySpeedF` that
   keeps the discrete parts per field.
2. **Doors** step at 60 Hz. Use a render-only `renderfrac = frac +
   fracspeed*phase60` in `doorInitMatrices`, leaving logic and collision
   untouched.
3. **Chr animation and guard root motion** step at 240 Hz. Preferred fix for
   SP: render-only animation phase extrapolation in `chrTick`'s
   `needsupdate` block, saving and restoring `frac`/`frac2` and the root
   offset around `modelSetMatricesWithAnim`, with no logic change. The
   matrices are also shot hitboxes, so keep it local and off on the server.
4. **Simulant movement** `bot0f1921f8` has the bug-16 loop. Closed form,
   with its `lvupdate240 > 0` gate relaxed, SP only.
5. **Projectiles** (with spin), sparks, shards, smoke: render-only
   extrapolation by phase. Their logic stays per tick or field.
6. **Netplay** steps the whole world at 60 Hz, so the fractional delta can
   never help there. It needs render-only interpolation from
   `g_TickInterpAlpha`: a fractional `desired` in `netChrInterpolate`, and
   door and projectile render offsets.
7. Player leftovers:
   - the landing dip (a per-quantum loop with an integer countdown, which
     needs the render-phase approach);
   - the eye-height bob target (needs fractional head-anim ticks);
   - gun animations and recoil pose, which step at 60 Hz (needs fractional
     anim playback decoupled from the guncmd state machine);
   - grab-mode and hoverbike movement and yaw, gated on integer time.
8. Optional: a render thread and the vsync lock.

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

Phase 2a checklist (compile-verified only):
7. Uncapped, 300+ fps: walking/strafing, crouching, leaning, stairs (eye
   height) and falling are smooth. Walk speed and jump/fall distances match
   60 fps.
8. The gun sways and bobs smoothly, the crosshair lag feels the same as at
   60 fps, and the sniper zoom speed matches 60 fps.
9. Grenades thrown while running inherit your speed as before (not about
   half).
10. MP/netplay: sims' and players' run cycles are blended, not snapping
    between poses.
11. Sims and guards on stairs move vertically smoothly. Pushes (melee,
    explosions) travel the same distance at 60 and 300 fps.
