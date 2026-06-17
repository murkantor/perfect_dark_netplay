#ifndef XBOXTRACE_H
#define XBOXTRACE_H

// Boot-stage trace for the Original Xbox bring-up. Writes to the debug overlay and to
// E:\pdboot.log on a writable partition. See port/src/xboxtrace.c. No-op on non-NXDK.

#ifdef __cplusplus
extern "C" {
#endif

// Append a plain stage label (prefixed/newlined for you).
void xboxTraceStage(const char *stage);

// printf-style trace for callers that need formatting (a trailing '\n' is trimmed).
void xboxTracef(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif
