#include <ultra64.h>
#include "constants.h"
#include "game/timing.h"
#include "bss.h"
#include "data.h"
#include "types.h"
#ifndef PLATFORM_N64
#include <stdlib.h>
#include "system.h"
#endif

#ifndef PLATFORM_N64
// Midas time scale (docs/PORT_TIMESCALE.md): this frame's real elapsed time in
// 1/240 s units, fractional and smoothed. The integer diffframe240/diffframe60
// (rounded with their remainder carry) are untouched and still drive every
// discrete system; converted continuous systems read this through lv.c's
// g_LvUpdate60fFrac.
f32 g_FrameDiff240f = 0.f;

/**
 * Delta smoothing, verbatim from Project Midas (portTickSmoothElapsed).
 *
 * The per-frame delta is measured, which keeps game speed exact but passes
 * scheduling noise straight into the motion: evenly presented uneven steps
 * look like judder. Give the continuous delta a low-pass estimate of the frame
 * period and BANK the difference against wall time, paying it back slowly so
 * the long run stays exact (beyond half a period it is repaid at once). A
 * frame more than 75% off the estimate is a real hitch or rate change: it
 * passes through and quickly re-seats the estimate. Only the float is
 * smoothed; the integer carries stay on real time. PD_NODELTASMOOTH=1 off.
 */
static f32 frametimeSmoothElapsed(f32 measured)
{
	static s32 on = -1;
	static f32 ema = 0.0f;  // estimated frame period, counts
	static f32 bank = 0.0f; // wall time owed to the sim, counts (+ = behind)
	f32 used;
	f32 diff;

	if (on < 0) {
		const char *e = getenv("PD_NODELTASMOOTH");
		on = (e != NULL && e[0] != '\0' && e[0] != '0') ? 0 : 1;
	}

	if (!on || measured <= 0.0f) {
		return measured;
	}

	if (ema <= 0.0f) {
		ema = measured;
	}

	diff = measured - ema;

	if (diff > 0.75f * ema || diff < -0.75f * ema) {
		// hitch or rate change: pass through, re-seat quickly, keep the bank
		ema += diff * 0.25f;
		return measured;
	}

	ema += diff * (1.0f / 16.0f);
	used = ema;
	bank += measured - used;

	if (bank > 0.5f * ema || bank < -0.5f * ema) {
		used += bank; // drifted half a period: repay now
		bank = 0.0f;

		if (used < 0.5f * ema) {
			// never hand out a near-zero step after a hitch; keep owing
			bank = used - 0.5f * ema;
			used = 0.5f * ema;
		}
	} else {
		f32 b = bank * (1.0f / 8.0f);

		used += b;
		bank -= b;
	}

	return (used > 0.0f) ? used : measured;
}
#endif

void frametimeInit(void)
{
	g_Vars.thisframestartt = osGetCount();
	g_Vars.prevframestartt = g_Vars.thisframestartt;
}

void frametimeApply(s32 diffframe60, s32 diffframe240, s32 frametime)
{
	g_Vars.prevframestartt = g_Vars.thisframestartt;
	g_Vars.thisframestartt = frametime;

	g_Vars.diffframe60 = diffframe60;
	g_Vars.diffframe60f = diffframe60;
	g_Vars.diffframe60freal = PALUPF(g_Vars.diffframe60f);

	g_Vars.prevframestart240 = g_Vars.thisframestart240;
	g_Vars.thisframestart240 += diffframe240;
	g_Vars.diffframe240 = diffframe240;
	g_Vars.diffframe240f = diffframe240;
	g_Vars.diffframe240freal = PALUPF(g_Vars.diffframe240f);
}

void frametimeCalculate(void)
{
	u32 count;
	u32 diffframet;
	u32 diffframe60;
	u32 diffframe240;

#ifndef PLATFORM_N64
	// Sleep only when the wait condition actually holds - the old do/while
	// slept EXTRA_SLEEP_TIME unconditionally once per frame even when the
	// tick was already due, adding pure latency.
	while (true) {
		count = osGetCount();
		diffframet = count - g_Vars.thisframestartt;
		g_Vars.diffframet = diffframet;

		diffframe60 = (g_Vars.lostframetime60t + diffframet + CYCLES_PER_FRAME / 2) / CYCLES_PER_FRAME;
		diffframe240 = (g_Vars.lostframetime240t + diffframet + CYCLES_PER_FRAME / 2 / 4) / (CYCLES_PER_FRAME / 4);

		if (!(g_Vars.mininc60 && diffframe60 < g_Vars.mininc60)) {
			break;
		}

		if (g_TickExtraSleep) {
			sysSleep(EXTRA_SLEEP_TIME);
		}
	}
#else
	do {
		count = osGetCount();
		diffframet = count - g_Vars.thisframestartt;
		g_Vars.diffframet = diffframet;

		diffframe60 = (g_Vars.lostframetime60t + diffframet + CYCLES_PER_FRAME / 2) / CYCLES_PER_FRAME;
		diffframe240 = (g_Vars.lostframetime240t + diffframet + CYCLES_PER_FRAME / 2 / 4) / (CYCLES_PER_FRAME / 4);
	} while (g_Vars.mininc60 && diffframe60 < g_Vars.mininc60);
#endif

	g_Vars.lostframetime60t = g_Vars.lostframetime60t + diffframet - diffframe60 * CYCLES_PER_FRAME;
	g_Vars.lostframetime240t = g_Vars.lostframetime240t + diffframet - diffframe240 * (CYCLES_PER_FRAME / 4);

#ifndef PLATFORM_N64
	g_FrameDiff240f = frametimeSmoothElapsed((f32)diffframet) / ((f32)CYCLES_PER_FRAME / 4.f);
#endif

#ifdef PLATFORM_N64
	g_Vars.mininc60 = 1;
#else
	g_Vars.mininc60 = g_TickRateDiv;
#endif

	frametimeApply(diffframe60, diffframe240, count);
}

void func0f16cf8c(s32 arg0)
{
	// empty
}

void func0f16cf94(void)
{
	// empty
}
