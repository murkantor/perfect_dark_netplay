# Midas input feel (pad sticks, mouse, raphnet raw)

Kai's pad, mouse and raphnet input matches Project Midas, the GoldenEye PC
port (`C:\Users\tidbu\source\repos\goldeneye_src_pcport`, branch `pc-port`),
where the owner tuned it. Both games run the same Rare look code: hip look is
`(stick-5)/70` signed-squared, times fov/basefov, 3.5 degrees per 60 Hz field,
so 210 degrees/s at full deflection, with no ramp. So "feel" is almost
entirely the port layer in front of that code. This doc says what that layer
does now. Read it when touching `inputReadController`, `inputAxisScale`, the
mouse scaling, `schedStartFrame`/`schedEndFrame`, or `raphnetraw.c`.

## What changed (Phase 1, 2026-10-17)

| Piece | Before (PD) | Now (Midas) | Where |
|---|---|---|---|
| Stick to N64 units | `/256`, so +/-127. Turning saturated at ~64% of the stick. | `v*85/32768`, so +/-84 | `inputAxisToN64`, input.c |
| Look (main) stick gate | none | N64 octagon: axis <= 85, \|x\|+\|y\| <= 130 | `inputStickGate` |
| Move (r) stick | `/256`, saturated at ~55% | circle stretched onto the octagon, then the gate | `inputCircleToOctagon` |
| Move stick dead band | none in PC mode | Rare's +/-5, as on the look stick | bondmove.c, after the c2 read |
| Deadzones | 12.5% (look Y 18.75%) | 10% on every axis | `DEFAULT_DEADZONE` |
| Trigger point | 23% | 10% | `TRIG_THRESHOLD` |
| Digital stick binds | +/-127 | +/-85 | `inputReadController` |
| Mouse units | 0.055 degrees/count at speed 2.5 | **0.1 degrees/count at 2.5** (same number = same feel as Midas) | `INPUT_MOUSE_DEG_PER_COUNT` (input.h) |
| Mouse raw | no hints | `MOUSE_RELATIVE_SYSTEM_SCALE=0`, `WARP_MOTION=0`, `SPEED_SCALE=1` | `inputInit` |
| Mouse in netplay | overwritten on render-only frames (lost ~half at 120 fps) | banked until a tick consumes it | `inputUpdateMouse` (+ the aim-crosshair hold in bondmove) |
| Sampling | end of frame, before the swap and vsync/frame-cap wait | start of frame, right after the event pump, right before the tick | `schedStartFrame` (pdsched.c) |
| Raphnet | Controller Pak only; stick through SDL + deadzone + `/256` | true N64 poll bytes, ~160 Hz background thread, no deadzone or reshaping | `port/src/raphnetraw.c` |

Unchanged on purpose: the game-side look math, FOV scaling, pitch inversion,
and the stick-swap default.

## Phase 2 (2026-10-17): aim mode + auto-aim

| Piece | Before (PD) | Now (Midas) | Where |
|---|---|---|---|
| Mouse crosshair rate | `MouseAimSpeed*320/1080/aspect` per rate unit, reach +/-1 | 0.0173 of the 4:3 band per degree; same pixels per degree on both axes (X x(4/3)/aspect, Y x4/3); full +/-0.65 reach in ~376 counts at speed 2.5 | `AIM_CROSSHAIR_PER_DEG`, bondmove.c manual-aim block |
| Crosshair Speed (`MouseAimSpeedX/Y`) | default 0.7 | default **1.0** = Midas `mouse_sens_aim` | mplayer.c defaults + migration |
| Stick + mouse | mouse replaced the stick | stick position + mouse offset, clamped to +/-0.65 | same |
| Aim entry | crosshair from the centre | seeded where the gun points (`crosspossum*(1-guncrossdamp)`); dropped if the pad aims first | same |
| Mouse crosshair motion | low-pass at 0.01 | **snaps**: `crosspossum = target/(1-crossdamp)` this tick | `g_BgunCrossSnap`, `bgunSwivel` (bondgun.c) |
| Shots vs crosshair | aim filter (`crosssum2` -> aimpos) lagged the crosshair by the weapon's aimdamp | solved from the crosshair: `crosssum2 = crosspossum*(1-crossdamp)/(1-aimdamp)` (local players, every swivel, as Midas) | `bgunSwivel` |
| Edge turn | `(pos-0.7)/0.3` of +/-1, added to the stick's | `(|pos|/0.65 - edge)/(1-edge)`, edge default **0.72**, last tick's combined crosshair, max with the stick's | bondmove.c edge block |
| Auto-aim | same for every device | off while player 1 is on keyboard + mouse (`Input.MouseAutoAim` 0); pads keep it | `bmoveIsAutoAimX/YEnabled`, `inputMouseBlocksAutoAim` |

- **Player 1 device tracking.** `p1SourceIsPad` in input.c. It flips to pad
  on player 1's own pad events, or on a real press or push on a raw raphnet
  pad (raw pads raise no SDL events). It flips back on any keyboard or mouse
  event. A second player's pad can't flip it.
- **Migration step 2** (`Input.InputFeelVersion` 2). It sets every player's
  Crosshair Speed X/Y to 1.0 and the edge point to 0.72. It does not reset
  the sticks again.

## Worked numbers (defaults)

- Look onset: N64 value 6 after the +/-5 band, so raw about 16% of throw.
- Full turn speed: N64 value 75, so raw about 89% of throw (was 64%).
- Aim edge-turn (`c1stickraw > 60`) and the aim crosshair (`*0.65/80`) now
  see N64-range values, as on console.

## Raphnet raw pads

`port/src/raphnetraw.c` is a near-verbatim port of Midas's `raphnet.c`.
- It opens the adapter's vendor HID interface: Win32 `HidD_*` on Windows,
  because SDL's hidapi filters that interface out; `SDL_hid_*` elsewhere.
- It suspends the adapter's own polling and sends `GET_STATUS` over raw SI.
- One device-owner thread fills a lock-free 32-bit cache, with reconnects
  and the Rumble Pak driven by raw pak writes.

In input.c:
- Pads with VID `0x289b` are raphnet pads. Adapter channels go in player
  order (a dual-port adapter gives channels 0 and 1).
- A raphnet pad reads the raw button word straight into the low 16 bits
  (`CK_*` indices 0-15 are the N64 bit layout) and the stick straight into
  `stick_x/y`, with no deadzone, gate or swap.
- Its SDL joystick binds are skipped (that face goes stale while raw polling
  owns the adapter). Keyboard binds still OR in.
- If the raw path is down, the pad falls back to the SDL face at a raw 1:1
  `/256`.
- Rumble becomes on/off Rumble Pak writes on the `inputRumble` deadline.

In bondmove.c, a raphnet pad in PC control mode plays `CONTROLMODE_11` (1.1).
It is real N64 hardware with one stick, and Midas also plays it with Rare's
own styles. An explicit non-PC style is kept.

`raphnet.c` (Controller Pak transfer, hidapi) calls `portRaphnetPause(1)` in
`raphnetOpen` and `portRaphnetPause(0)` in `raphnetClose`. This parks the
poller for the whole pak transfer, so two owners never interleave SI
exchanges.

Debug: `PD_RAPHDBG=1` logs every failed exchange and a 10-second rate line
(Hz per channel, exchange cost). `PD_RAPHNET_SYNC=1` polls inline, without the
thread.

## Config

- `Input.RaphnetRaw` (1): raw path on or off.
- `Input.InputFeelVersion`: the one-time migration. When it is below 1,
  every player's four stick deadzones are reset to 3277 and the scales to
  1.0 (an inverting negative scale keeps its sign). Saved stick values were
  tuned around the old +/-127 mapping. Mouse speeds are not migrated: the
  saved number now means the Midas unit.
- `PD_ENABLE_RAPHNET_RAW` (CMake, ON): off for the dedicated server and
  Switch. It links `hid` and `setupapi` on Windows.

## Netplay

Nothing input-shaped goes on the wire: the client sends view angles,
crosshair and move speeds, and CSP never replays input. So there is no
protocol bump. Mixed builds differ only in how each player's own
controls feel.

## Not ported (deliberate)

- Midas's FOV-ratio quirk (zoom below 60 divides by 60). Kai keeps
  fov/default-fov.
- Midas's look-ahead tweaks. Look-ahead is off in Kai's PC mode anyway.

## Honest status

Compile-verified only. Runtime checklist:
1. Pad hip look reaches full speed near full throw, not at two thirds; the
   diagonal feels like an N64 octagon.
2. Walk/strafe ramps over the whole move stick; the diagonal is not slow.
3. Mouse at 2.5 feels the same as Midas at 2.5. Check no acceleration.
4. Netplay at 120 fps: mouse turn is the same speed as offline.
5. Input latency: pad and mouse should feel snappier (sampled after the wait).
6. Raphnet N64 pad: log shows `raphnet RAW adapter online`; stick and
   buttons are 1:1; the 1.1 layout; rumble with a Rumble Pak;
   unplug/replug recovers.
7. Controller Pak backup/restore through the same adapter still works (the pause).
8. First launch resets stick deadzones/scale once (log line), and later
   changes stick.
9. Aim mode, mouse: the crosshair starts on the gun, moves 1:1 with no glide,
   crosses about 65% of the half-screen, and turns the view past 72%. Shots
   land on the crosshair while turning.
10. Aim mode, pad: the crosshair still glides (vanilla); hip fire is unchanged.
11. Auto-aim: off on KBM, on as soon as player 1 touches the pad (SP and MP).
