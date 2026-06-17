#ifndef XBOXTRACE_H
#define XBOXTRACE_H

// Timing-independent boot-stage trace for the Original Xbox bring-up. See
// port/src/xboxtrace.c. No-op on non-NXDK builds.

#ifdef __cplusplus
extern "C" {
#endif

void xboxTraceStage(const char *stage);

#ifdef __cplusplus
}
#endif

#endif
