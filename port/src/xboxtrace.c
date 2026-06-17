// Boot-stage trace for the Original Xbox (NXDK) bring-up.
//
// This is deliberately just a plain debugPrint append. We tried fancier schemes and
// every one broke on the NV2A/xemu:
//   - timed pacing (thrd_sleep / KeStallExecutionProcessor) HANGS once pb_init() has
//     reconfigured the timer state; an rdtsc spin doesn't pace (xemu jumps the TSC).
//   - an accumulating "clear + reprint the whole list" scheme CRASHES: debugClearScreen()
//     faults once pb_init() has taken over the framebuffer (raw debugPrint still works,
//     but the clear does not), which rebooted the box in a loop right after "pb_init ok".
// So: no clear, no delay -- just append. debugPrint works both before and after
// pb_init(). On a HANG the frozen screen shows the last line at the bottom; on success
// the render loop's own frame counter takes over. Remove once boot is solid.
// See docs/PORT_XBOX_NXDK.md.

#include "xboxtrace.h"

#ifdef NXDK

#include <hal/debug.h>

void xboxTraceStage(const char *stage)
{
	debugPrint("PDBOOT: %s\n", stage ? stage : "(null)");
}

#else

void xboxTraceStage(const char *stage) { (void)stage; }

#endif
