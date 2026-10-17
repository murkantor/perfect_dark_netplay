#ifndef _IN_PORT_RAPHNETRAW_H
#define _IN_PORT_RAPHNETRAW_H

#include <PR/ultratypes.h>

/*
 * Raphnet N64-to-USB adapter RAW input (port/src/raphnetraw.c): the real N64
 * poll response (button word + signed stick bytes) read over the adapter's
 * vendor HID interface on a background thread, bypassing the OS joystick
 * stack. Ported from Project Midas (GoldenEye PC port). Separate from
 * raphnet.h, which is the Controller Pak transfer transport.
 *
 * Only compiled when PD_ENABLE_RAPHNET_RAW is defined.
 */

#ifdef PD_ENABLE_RAPHNET_RAW

#define RAPHNET_RAW_MAX_CHAN 2

/* Is the raw path usable (or reconnecting)? May open the adapter. */
s32 portRaphnetAvailable(void);

/* Poll one adapter channel. Returns 1 with the real N64 pad state (button
 * word in CONT_* layout, stick in signed N64 units), 0 when no controller
 * answers on that channel, -1 on adapter error. Reads the poller's cache:
 * no USB traffic, never blocks. */
s32 portRaphnetPoll(s32 chan, u16 *btn, s8 *x, s8 *y);

/* Rumble Pak motor on/off (desired state, applied by the poller). */
s32 portRaphnetRumble(s32 chan, s32 on);

/* Stop/restart the poller's USB traffic so the Controller Pak transport
 * (raphnet.c) can own the adapter. Blocks until the poller is idle. */
void portRaphnetPause(s32 on);

#endif /* PD_ENABLE_RAPHNET_RAW */

#endif
