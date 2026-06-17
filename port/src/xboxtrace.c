// Boot-stage trace for the Original Xbox (NXDK) bring-up.
//
// Writes each stage to the debug overlay (debugPrint) AND appends it to a log file on
// a writable partition (E:\pdboot.log) so the boot trace can be pulled off real
// hardware over FTP / a file manager -- the on-screen text flashes and scrolls and is
// hard to read, but the file is complete and ordered. The file is opened/written/closed
// per line (truncated on the first line of a boot, appended after) so a hard lock still
// leaves everything written up to the hang on disk.
//
// We deliberately avoid the fancier on-screen schemes that broke after pb_init:
//   - timed pacing (thrd_sleep / KeStallExecutionProcessor) HANGS post-pb_init; rdtsc
//     doesn't pace (xemu jumps the TSC).
//   - clear+reprint CRASHES (debugClearScreen faults once pbkit owns the framebuffer).
// debugPrint append works both before and after pb_init; the log file is the reliable
// record. Remove this scaffolding once boot is solid. See docs/PORT_XBOX_NXDK.md.

#include "xboxtrace.h"

#ifdef NXDK

#include <stdio.h>
#include <stdarg.h>
#include <hal/debug.h>

// Writable partition. E:\ is the standard Xbox data partition (writable on retail
// hardware and on xemu's default HDD image); the booted XISO (D:) is read-only.
#define XBOX_LOG_PATH "E:\\pdboot.log"

static int s_logStarted;

static void xboxLogLine(const char *line)
{
	// "w" on the very first line of the boot truncates last run's log; "a" after.
	FILE *f = fopen(XBOX_LOG_PATH, s_logStarted ? "a" : "w");
	if (f) {
		s_logStarted = 1;
		fputs(line, f);
		fputc('\n', f);
		fclose(f);
	}
}

void xboxTraceStage(const char *stage)
{
	char line[160];
	snprintf(line, sizeof(line), "PDBOOT: %s", stage ? stage : "(null)");
	debugPrint("%s\n", line);
	xboxLogLine(line);
}

void xboxTracef(const char *fmt, ...)
{
	char msg[224];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);

	// Strip one trailing newline so the log line isn't doubled (callers' formats
	// typically end in "\n" for debugPrint's sake).
	size_t n = 0;
	while (msg[n]) { n++; }
	if (n && msg[n - 1] == '\n') { msg[n - 1] = '\0'; }

	debugPrint("%s\n", msg);
	xboxLogLine(msg);
}

#else

void xboxTraceStage(const char *stage) { (void)stage; }
void xboxTracef(const char *fmt, ...) { (void)fmt; }

#endif
