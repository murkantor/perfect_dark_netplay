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

## Phase 2b (2026-10-17): doors, lifts, netplay remote chrs, UI at 60 tps

- **Doors and lifts** stepped at 60 Hz: `applySpeed` eases in whole fields,
  and lifts carry the player and camera. They now call `applySpeedTimed`
  (propobj.c). On a real fractional frame, for a prop on the frame's own
  step, that runs `applySpeedFrac`, the same easing integrated over real
  time in sub-steps of at most one field. On any other frame it runs the
  original loop. Lift riders are displaced from the new position as before,
  so player and camera ride smoothly. Crouch uses the same helper.
  `applySpeed` itself is untouched: AI turning, turret aim and hovercar bob
  use it inside integer-gated logic, which is the bug-16 hazard.
- **Netplay remote chrs.** `netChrInterpolate` timed its interpolation on the
  whole-tick `g_NetTick`, so sims and enemies on a client stepped at 60 Hz.
  `netTickSubFrac()` adds the real time since the last tick to the
  interpolation target. This is render-side only: no simulation or wire
  change.
- **UI at 60 ticks/s.** Removing the 60 fps cap exposed UI code that
  animates per rendered frame. Fixed:
  - **menu dialogs**: the open/slide/resize/scroll 1-pixel nudges now fire
    only on a frame where a 60 Hz field elapsed; the text wave, the MP
    Ready pre-open and the hudpiece retarget step by fields;
  - **list scroll nudge and marquee minimum** (menuitem.c);
  - **radial weapon menu glide and expand** (`activemenutick.c`, once per
    field);
  - **hudmsg delay** and **Lua timed overlays**;
  - **combat-boost transition** (`speedpillchange`, which also engages
    slow-mo, so this was a gameplay bug);
  - **lock-screen blur count** and the **Infiltration static roll**;
  - **IR/FarSight scanline**;
  - renderer clocks: the **retro post-FX time** (GL and SDL_GPU) and the
    **chaos DVD bounce** now use real seconds.

  Menu *input* still runs every frame, so no press is dropped or repeated.
  Already time-based and left alone: hudmsg popups, health bar, damage
  flash, crosshair, ammo HUD, screen fades, kill feed, chaos.lua (it uses
  `pd.lvupdate()` fields).

## Phase 2c (2026-10-17): audio, netplay view + remote players, sims, chr animation

- **Audio crackle when turning** (user report). `psTickChannel` posted a
  vol+pan+pitch event for every playing positional sound on every rendered
  frame. With the camera now moving every frame, that flooded the sound
  player, and each pan step lands instantly in the synth.
  - Positional re-pan/re-volume now posts only on frames where a 60 Hz
    field elapsed, the rate the game had while capped at 60.
  - The same applies to the gas cloud re-pan, the Reaper spin vol/pitch and
    the Mauler charge pitch.
  - A sound's first play is still immediate.
- **Netplay local view.** The camera rode the netplay 60 Hz pin.
  `g_LvViewFrac`/`ViewFracReal` (lv.c) is the real frame time even under
  the pin. It still falls back on pause, slow-mo, det and the fixed tick.
  - Camera yaw (local player only), pitch and `mlookscale` use it.
  - View angles are client-authoritative and only sampled when a move is
    sent, so the sim contract is unchanged.
  - The view mouse is per-frame in netplay now (it was banked per tick). A
    separate tick bank (`inputMouseGetScaledTickDelta`) feeds whole-tick
    consumers: Slayer fly-by-wire sends a per-tick rotation.
- **Netplay remote players**, client only:
  - `bwalkUpdateRemote`'s interp target adds `netTickSubFrac()`, and the
    chase uses the closed form of the per-tick gain over real time;
  - the remote facing lerp (`bmoveProcessRemoteInput`) adds the sub-tick
    fraction;
  - the host's copy stays per tick, because hits are validated against it.
- **Simulant movement.** `botMoveFrac` (bot.c) is a closed form of the
  bug-16 per-quantum velocity filter. It is used on real fractional frames
  for sims on the frame's step without forced slow updates.
- **Chr animation.** `modelTickAnimQuarterSpeedF(q)` (model.c) runs whole
  quanta, then a remainder step. The step function was factored out, and
  the original calls it with 0.25f, which is bit-identical. `chr0f0220ec`
  uses it for non-player chrs on the frame's step (not cutscenes or the
  chaos speed effect). Guard poses and root-motion walking now advance every
  frame. Accepted side effect: `chr->prevpos` (AI arrival checks) spans one
  frame of motion, as it already did above 240 fps.

## Still to do

1. ~~Gun animations and recoil pose~~ DONE (2c, render-side):
   `g_LvFieldPhase` (lv.c) plus per-hand tick snapshots (`bgunrendersnap`,
   bondgun.c). The anim is drawn `rate*phase` frames on and restored after
   the matrix build. The kick tween is re-evaluated at frames+phase. The
   firing shake is rolled once per field and held.
2. ~~Sparks, shards, smoke~~ DONE (2c): each is drawn `g_LvFieldPhase`
   along its current motion (a copy of the smoke puff, and local coords for
   shards and sparks; the tick state is untouched). Explosions are texture
   flipbooks and stay stepped. **Projectiles** (rockets, grenades) are left
   on the 1/240 clock (`lvupdate60freal`). They are smooth up to 240 fps
   and stall on zero-tick frames above that. Converting them means
   collision physics, so this is an accepted limit.
3. ~~Doors in netplay~~ DONE (2c, clients only): `doorGetRenderFrac` plus
   `doorCalcPose` (the pose half of `doorUpdateTiles`, factored out
   bit-identically) draw moving doors at `frac + fracspeed*phase`.
   Collision, sounds and the synced frac stay on the tick. Accepted limits:
   - DOORFLAG_0004 shrink doors are a vertex edit in the tick, so they
     still step;
   - lifts still step in netplay. They carry the player, so a display
     offset alone would jitter against the camera. A fix would mean
     smoothing the local player's position in netplay as a whole.
4. Player leftovers:
   - ~~the eye-height bob~~ DONE (2c): bondhead.c lags use `lvLagStep`, the
     head anim uses `modelTickAnimQuarterSpeedF`, and idle breathing uses
     FracReal;
   - the landing dip (`crouchtime240` countdown + `sumcrouch` lag in
     bondwalk.c) stays on 1/240 quanta. It feeds eye height and so
     collision, through an integer countdown. It is smooth up to 240 fps
     and is an accepted limit;
   - grab-mode and hoverbike movement (1/240 quanta, same limit).
5. Small per-frame UI counters that are input grace windows: left as they
   are.
6. Optional: a render thread and the vsync lock.

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

Phase 2b checklist (compile-verified only):
12. Uncapped: doors open/close smoothly, lifts glide, and you and the camera
    ride a lift smoothly. Door/lift timing matches 60 fps.
13. Netplay client: sims/enemies move smoothly at 120 fps (not stepping).
14. Pause menu, inventory, objectives, end screens and front-end dialogs
    open/slide at the same speed as at 60 fps. The radial weapon menu
    glides normally. The combat boost blur takes as long as at 60 fps.

Phase 2c checklist (compile-verified only):
15. Turning quickly with guards talking / HUD sounds playing: no crackle, and
    panning is still smooth enough.
16. Netplay client at 120+ fps: your own turning/looking is smooth (not
    60 Hz). Mouse sensitivity is unchanged. The Slayer rocket still steers
    with the mouse.
17. Netplay client: remote players move and turn smoothly.
18. Uncapped offline: sims and guards walk/run with smooth animation, and
    speeds and paths match 60 fps.
19. Uncapped: reloads/equips/fire anims smooth, no pop at the end of a
    reload; recoil kick smooth; the firing shake looks like 60 fps (not
    buzzing).
20. Uncapped: bullet sparks, glass/wood shards and smoke drift smoothly;
    while paused they stay frozen.
21. Uncapped above 240 fps: the walking head bob is smooth.
22. Netplay client at 120+ fps: sliding/swinging doors open and close
    smoothly, and their collision is unchanged.
