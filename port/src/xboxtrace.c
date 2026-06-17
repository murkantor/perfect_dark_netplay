// Timing-independent boot-stage trace for the Original Xbox (NXDK) bring-up.
//
// The screen scrolls a multi-stage boot burst too fast to read, and every delay
// primitive we tried to pace it is unreliable on the NV2A/xemu: thrd_sleep and
// KeStallExecutionProcessor both HANG once pb_init() has reconfigured the timer
// state, and an rdtsc spin doesn't pace at all (xemu advances the TSC in large
// jumps). So instead of pacing, accumulate every stage in memory and reprint the
// WHOLE list from the top of a freshly-cleared screen on each call. No timing is
// involved: a frozen screen always shows the complete numbered sequence, and the
// last line is exactly the stage that hung. Works the same before and after
// pb_init(). Remove this scaffolding once boot is solid. See docs/PORT_XBOX_NXDK.md.

#include "xboxtrace.h"

#ifdef NXDK

#include <string.h>
#include <hal/debug.h>

#define XBT_MAX 64
#define XBT_LEN 48

static char s_stages[XBT_MAX][XBT_LEN];
static int s_count;

void xboxTraceStage(const char *stage)
{
	if (s_count < XBT_MAX) {
		const char *s = stage ? stage : "(null)";
		strncpy(s_stages[s_count], s, XBT_LEN - 1);
		s_stages[s_count][XBT_LEN - 1] = '\0';
		s_count++;
	}

	debugClearScreen();
	for (int i = 0; i < s_count; i++) {
		debugPrint("PDBOOT %02d: %s\n", i, s_stages[i]);
	}
}

#else

void xboxTraceStage(const char *stage) { (void)stage; }

#endif
