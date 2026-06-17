#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#ifdef NXDK
#include <threads.h> // C11 thrd_sleep (NXDK has no <sys/time.h>/nanosleep)
#include <xboxkrnl/xboxkrnl.h> // MmAllocateContiguousMemory for large allocations
#else
#include <sys/time.h>
#endif
#ifndef DEDICATED_SERVER
#include <SDL3/SDL.h>
#elif !defined(_WIN32)
#include <unistd.h> // readlink for the POSIX dedicated-server exe-path resolver
#endif
#include <PR/ultratypes.h>
#include "platform.h"
#include "console.h"
#include "system.h"

#ifdef PLATFORM_WIN32

#include <windows.h>

// on win32 we use waitable timers instead of nanosleep
typedef HANDLE WINAPI (*CREATEWAITABLETIMEREXAFN)(LPSECURITY_ATTRIBUTES, LPCSTR, DWORD, DWORD);
static HANDLE timer;
static CREATEWAITABLETIMEREXAFN pfnCreateWaitableTimerExA;

// winapi also provides a yield macro
#define DO_YIELD() YieldProcessor()

// ask system for high performance GPU, if any
__attribute__((dllexport)) u32 NvOptimusEnablement = 1;
__attribute__((dllexport)) u32 AmdPowerXpressRequestHighPerformance = 1;

#else

#include <unistd.h>

// figure out how to yield
#if defined(NXDK)
// Pentium III has no SSE2, so clang's _mm_pause() (which it places in <emmintrin.h>)
// isn't available. The PAUSE opcode (F3 90) decodes as NOP on pre-P4 CPUs, so emit
// it directly.
#define DO_YIELD() __asm__ __volatile__("pause" ::: "memory")
#elif defined(PLATFORM_X86) || defined(PLATFORM_X86_64)
// this should work even if the code is not built with SSE enabled, at least on gcc and clang,
// but if it doesn't we'll have to use  __builtin_ia32_pause() or something
#include <immintrin.h>
#define DO_YIELD() _mm_pause()
#elif defined(PLATFORM_ARM) && (defined(PLATFORM_64BIT) || PLATFORM_ARM == 7 || PLATFORM_ARM == 8)
// same as YieldProcessor() on ARM Windows
#define DO_YIELD() __asm__ volatile("dmb ishst\n\tyield":::"memory")
#else
// fuck it
#define DO_YIELD() do { } while (0)
#endif

#endif

#define LOG_FNAME "pd.log"
#define CRASHLOG_FNAME "pd.crash.log"
#define USEC_IN_SEC 1000000ULL

#ifdef NXDK
// Temporary boot-bring-up tracing: routed through xboxTraceStage so it also lands in
// E:\pdboot.log (see port/src/xboxtrace.c). Remove once boot is solid.
#include <hal/debug.h>
#include "xboxtrace.h"
#define NXDK_BOOT_TRACE(s) xboxTraceStage(s)
#else
#define NXDK_BOOT_TRACE(s) ((void)0)
#endif

static u64 startTick = 0;
static char logPath[2048];

static s32 sysArgc;
static const char **sysArgv;

static inline void sysLogSetPath(const char *fname)
{
	// figure out where the log is and clear it
	// try working dir first
	snprintf(logPath, sizeof(logPath), "./%s", fname);
	FILE *f = fopen(logPath, "wb");
	if (!f) {
		// try home dir
		sysGetHomePath(logPath, sizeof(logPath) - 1);
		strncat(logPath, "/", sizeof(logPath) - 1);
		strncat(logPath, fname, sizeof(logPath) - 1);
		f = fopen(logPath, "wb");
	}
	if (f) {
		fclose(f);
	}
}

void sysInitArgs(s32 argc, const char **argv)
{
	sysArgc = argc;
	sysArgv = argv;
}

void sysInit(void)
{
	NXDK_BOOT_TRACE("sysInit: micros");
	startTick = sysGetMicroseconds();
	NXDK_BOOT_TRACE("sysInit: args");

	if (sysArgCheck("--log")) {
		// --log [path]: optional path after the flag (e.g. master-spawned
		// instances each need their own file — multiple processes sharing one
		// directory's pd.log overwrite each other). Bare --log keeps the
		// pd.log default; a following token starting with '-' is the next
		// flag, not a path.
		const char *logpath = sysArgGetString("--log");
		if (logpath && logpath[0] && logpath[0] != '-') {
			sysLogSetPath(logpath);
		} else {
			sysLogSetPath(LOG_FNAME);
		}
	}

	NXDK_BOOT_TRACE("sysInit: verlog");
#ifdef VERSION_HASH
	sysLogPrintf(LOG_NOTE, "version: " VERSION_BRANCH " " VERSION_HASH " (" VERSION_TARGET ")");
#endif

	NXDK_BOOT_TRACE("sysInit: date");
#ifdef NXDK
	// pdclib's localtime() can return NULL on the Xbox (no timezone database),
	// which would then fault inside strftime(); skip the cosmetic startup-date log.
	sysLogPrintf(LOG_NOTE, "startup date: (n/a on xbox)");
#else
	char timestr[256];
	const time_t curtime = time(NULL);
	strftime(timestr, sizeof(timestr), "%d %b %Y %H:%M:%S", localtime(&curtime));
	sysLogPrintf(LOG_NOTE, "startup date: %s", timestr);
#endif
	NXDK_BOOT_TRACE("sysInit: done");

#ifdef PLATFORM_WIN32
	// this function is only present on Vista+, so try to import it from kernel32 by hand
	pfnCreateWaitableTimerExA = (CREATEWAITABLETIMEREXAFN)GetProcAddress(GetModuleHandleA("kernel32.dll"), "CreateWaitableTimerExA");
	if (pfnCreateWaitableTimerExA) {
		// function exists, try to create a hires timer
		timer = pfnCreateWaitableTimerExA(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
	}
	if (!timer) {
		// no function or hires timers not supported, fallback to lower resolution timer
		sysLogPrintf(LOG_WARNING, "SYS: hires waitable timers not available");
		timer = CreateWaitableTimerA(NULL, FALSE, NULL);
	}
#endif
}

s32 sysArgCheck(const char *arg)
{
	for (s32 i = 1; i < sysArgc; ++i) {
		if (!strcasecmp(sysArgv[i], arg)) {
			return 1;
		}
	}
	return 0;
}

const char *sysArgGetString(const char *arg)
{
	for (s32 i = 1; i < sysArgc; ++i) {
		if (!strcasecmp(sysArgv[i], arg)) {
			if (i < sysArgc - 1) {
				return sysArgv[i + 1];
			}
		}
	}
	return NULL;
}

s32 sysArgGetInt(const char *arg, s32 defval)
{
	for (s32 i = 1; i < sysArgc; ++i) {
		if (!strcasecmp(sysArgv[i], arg)) {
			if (i < sysArgc - 1) {
				return strtol(sysArgv[i + 1], NULL, 0);
			}
		}
	}
	return defval;
}

u64 sysGetMicroseconds(void)
{
#ifdef NXDK
	// NXDK has no gettimeofday/struct timeval; use C11 timespec_get.
	struct timespec ts;
	timespec_get(&ts, TIME_UTC);
	return ((u64)ts.tv_sec * USEC_IN_SEC + (u64)ts.tv_nsec / 1000) - startTick;
#else
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return ((u64)tv.tv_sec * USEC_IN_SEC + (u64)tv.tv_usec) - startTick;
#endif
}

float sysGetSeconds(void)
{
	u64 t = sysGetMicroseconds();
	return (f32)t / 1000000.f;
}

s32 sysLogIsOpen(void)
{
	return (logPath[0] != '\0');
}

void sysLogPrintf(s32 level, const char *fmt, ...)
{
	static const char *prefix[3] = {
		"", "WARNING: ", "ERROR: "
	};

	char logmsg[2048];

	va_list ap;
	va_start(ap, fmt);
	vsnprintf(logmsg, sizeof(logmsg), fmt, ap);
	va_end(ap);

	if (logPath[0]) {
		FILE *f = fopen(logPath, "ab");
		if (f) {
			// mask off LOGFLAG_* bits like the console path below — indexing
			// prefix[] with a flagged level (e.g. LOG_CHAT) read garbage
			// pointers and wrote binary junk prefixes into the log file
			fprintf(f, "%s%s\n", prefix[level & 0x0f], logmsg);
			fclose(f);
		}
	}

	FILE *fout = ((level & 0x0f) == LOG_NOTE) ? stdout : stderr;
	fprintf(fout, "%s%s\n", prefix[level & 0x0f], logmsg);
	fflush(fout);

	if ((level & LOGFLAG_NOCON) == 0) {
		conPrintLn((level & LOGFLAG_SHOWMSG) != 0, logmsg);
	}
}

void sysFatalError(const char *fmt, ...)
{
	static s32 alreadyCrashed = 0;

	if (alreadyCrashed) {
		abort();
	}

	char errmsg[2048] = { 0 };

	alreadyCrashed = 1;

	va_list ap;
	va_start(ap, fmt);
	vsnprintf(errmsg, sizeof(errmsg), fmt, ap);
	va_end(ap);

	sysLogPrintf(LOG_ERROR, "FATAL: %s", errmsg);

	fflush(stdout);
	fflush(stderr);

#ifdef NXDK
	// On NXDK sysLogPrintf goes nowhere visible (stdout/stderr are dropped during
	// boot and the in-game console isn't rendered yet) and SDL_ShowSimpleMessageBox
	// crashes nxdk-sdl3. Print the reason to the debug text overlay and halt so it
	// stays readable on screen / in xemu instead of corrupting into garbage.
	debugPrint("PDFATAL: %s\n", errmsg);
	for (;;) { /* spin so the message stays on screen */ }
#elif !defined(DEDICATED_SERVER)
	SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Fatal error", errmsg, NULL);
#endif

	exit(1);
}

void sysGetExecutablePath(char *outPath, const u32 outLen)
{
#ifndef DEDICATED_SERVER
	// try asking SDL; in SDL3 the returned string is owned by SDL - do not free
	const char *sdlPath = SDL_GetBasePath();

	if (sdlPath && *sdlPath) {
		// -1 to trim trailing slash
		const u32 len = strlen(sdlPath) - 1;
		if (len < outLen) {
			memcpy(outPath, sdlPath, len);
			outPath[len] = '\0';
		}
	} else if (sysArgc && sysArgv[0] && sysArgv[0][0]) {
		// get exe path from argv[0]
		strncpy(outPath, sysArgv[0], outLen - 1);
		outPath[outLen - 1] = '\0';
	} else if (outLen > 1) {
		// give up, use working directory instead
		outPath[0] = '.';
		outPath[1] = '\0';
	}

#ifdef PLATFORM_WIN32
	// replace all backslashes with forward slashes, windows supports both
	for (u32 i = 0; i < outLen && outPath[i]; ++i) {
		if (outPath[i] == '\\') {
			outPath[i] = '/';
		}
	}
#endif
#else
	// Dedicated server build links no SDL: resolve the exe directory natively.
	char buf[1024] = { 0 };
	s32 got = 0;
#ifdef _WIN32
	const DWORD wn = GetModuleFileNameA(NULL, buf, sizeof(buf) - 1);
	if (wn > 0 && wn < sizeof(buf)) {
		buf[wn] = '\0';
		got = 1;
	}
#else
	const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
	if (n > 0) {
		buf[n] = '\0';
		got = 1;
	}
#endif
	if (got) {
		// strip the filename to leave the directory
		char *bslash = strrchr(buf, '\\');
		char *fslash = strrchr(buf, '/');
		char *slash = (bslash > fslash) ? bslash : fslash;
		if (slash) {
			*slash = '\0';
		}
		strncpy(outPath, buf, outLen - 1);
		outPath[outLen - 1] = '\0';
	} else if (sysArgc && sysArgv[0] && sysArgv[0][0]) {
		strncpy(outPath, sysArgv[0], outLen - 1);
		outPath[outLen - 1] = '\0';
	} else if (outLen > 1) {
		outPath[0] = '.';
		outPath[1] = '\0';
	}
#ifdef _WIN32
	for (u32 i = 0; i < outLen && outPath[i]; ++i) {
		if (outPath[i] == '\\') { outPath[i] = '/'; }
	}
#endif
#endif
}

void sysGetHomePath(char *outPath, const u32 outLen)
{
#ifndef DEDICATED_SERVER
	// try asking SDL
	char *sdlPath = SDL_GetPrefPath("", "perfectdark");

	if (sdlPath && *sdlPath) {
		// -1 to trim trailing slash
		const u32 len = strlen(sdlPath) - 1;
		if (len < outLen) {
			memcpy(outPath, sdlPath, len);
			outPath[len] = '\0';
		}
	} else if (outLen > 1) {
		// give up, use working directory instead
		outPath[0] = '.';
		outPath[1] = '\0';
	}

#ifdef PLATFORM_WIN32
	// replace all backslashes with forward slashes, windows supports both
	for (u32 i = 0; i < outLen && outPath[i]; ++i) {
		if (outPath[i] == '\\') {
			outPath[i] = '/';
		}
	}
#endif

	SDL_free(sdlPath);
#else
	// Dedicated server build links no SDL: mirror SDL_GetPrefPath's per-OS
	// layout. Normally overridden by --savedir (e.g. systemd StateDirectory or
	// a Windows data dir), so this is just a fallback.
#ifdef _WIN32
	// %APPDATA%\perfectdark (org is empty, as passed to SDL_GetPrefPath).
	const char *base = getenv("APPDATA");
	if (!base || !*base) { base = getenv("USERPROFILE"); }
	if (base && *base) {
		snprintf(outPath, outLen, "%s/perfectdark", base);
		for (u32 i = 0; i < outLen && outPath[i]; ++i) {
			if (outPath[i] == '\\') { outPath[i] = '/'; }
		}
	} else if (outLen > 1) {
		outPath[0] = '.';
		outPath[1] = '\0';
	}
#else
	// $HOME/.local/share/perfectdark
	const char *home = getenv("HOME");
	if (home && *home) {
		snprintf(outPath, outLen, "%s/.local/share/perfectdark", home);
	} else if (outLen > 1) {
		outPath[0] = '.';
		outPath[1] = '\0';
	}
#endif
#endif
}

#ifdef NXDK
// NXDK's malloc hangs on large single allocations (the 32 MB ROM buffer froze the box
// despite 45 MB free; the engine heap and the inflated data segment are large too).
// Route big allocations through the kernel's physically-contiguous allocator and track
// them so sysMemFree picks the matching free. Small allocations stay on malloc (they
// are frequent and MmAllocateContiguousMemory is page-granular + a scarcer resource).
#define NXDK_BIG_ALLOC_MIN (1u * 1024u * 1024u) // 1 MB
#define NXDK_BIG_ALLOC_MAX 64
static void *g_NxdkBigPtrs[NXDK_BIG_ALLOC_MAX];
static u32 g_NxdkBigSizes[NXDK_BIG_ALLOC_MAX];
static u32 g_NxdkBigCount;

static void nxdkBigTrack(void *p, u32 size)
{
	if (p && g_NxdkBigCount < NXDK_BIG_ALLOC_MAX) {
		g_NxdkBigSizes[g_NxdkBigCount] = size;
		g_NxdkBigPtrs[g_NxdkBigCount++] = p;
	}
}

// Returns the tracked size of p and removes it, or 0 if p wasn't a tracked big alloc.
static u32 nxdkBigUntrack(void *p)
{
	for (u32 i = 0; i < g_NxdkBigCount; ++i) {
		if (g_NxdkBigPtrs[i] == p) {
			const u32 sz = g_NxdkBigSizes[i];
			--g_NxdkBigCount;
			g_NxdkBigPtrs[i] = g_NxdkBigPtrs[g_NxdkBigCount];
			g_NxdkBigSizes[i] = g_NxdkBigSizes[g_NxdkBigCount];
			return sz;
		}
	}
	return 0;
}
#endif

void *sysMemAlloc(const u32 size)
{
#ifdef NXDK
	if (size >= NXDK_BIG_ALLOC_MIN) {
		void *p = MmAllocateContiguousMemory(size);
		nxdkBigTrack(p, size);
		return p;
	}
#endif
	return malloc(size);
}

void *sysMemZeroAlloc(const u32 size)
{
#ifdef NXDK
	if (size >= NXDK_BIG_ALLOC_MIN) {
		void *p = MmAllocateContiguousMemory(size);
		if (p) {
			nxdkBigTrack(p, size);
			memset(p, 0, size);
		}
		return p;
	}
#endif
	return calloc(1, size);
}

void *sysMemRealloc(void *ptr, const u32 newSize)
{
#ifdef NXDK
	// A tracked contiguous block must not be handed to libc realloc (which would read
	// malloc metadata off a kernel allocation). Re-allocate and copy. Small malloc'd
	// blocks (untracked) fall through to realloc as usual -- in practice only small
	// buffers (display modes, ext_tex tables) are realloc'd; the big allocations
	// (heap/ROM/dataSeg) are fixed-size and never reach here.
	const u32 oldBig = ptr ? nxdkBigUntrack(ptr) : 0;
	if (oldBig) {
		void *p = MmAllocateContiguousMemory(newSize);
		if (p) {
			nxdkBigTrack(p, newSize);
			memcpy(p, ptr, oldBig < newSize ? oldBig : newSize);
		}
		MmFreeContiguousMemory(ptr);
		return p;
	}
#endif
	return realloc(ptr, newSize);
}

void sysMemFree(void *ptr)
{
#ifdef NXDK
	if (ptr && nxdkBigUntrack(ptr)) {
		MmFreeContiguousMemory(ptr);
		return;
	}
#endif
	free(ptr);
}

void sysSleep(const s64 hns)
{
#ifdef PLATFORM_WIN32
	static LARGE_INTEGER li;
	li.QuadPart = -hns;
	SetWaitableTimer(timer, &li, 0, NULL, NULL, FALSE);
	WaitForSingleObject(timer, INFINITE);
#elif defined(NXDK)
	// NXDK has no nanosleep; use C11 thrd_sleep (hns = 100ns units).
	const struct timespec spec = { 0, hns * 100 };
	thrd_sleep(&spec, NULL);
#else
	const struct timespec spec = { 0, hns * 100 };
	nanosleep(&spec, NULL);
#endif
}

void sysCpuRelax(void)
{
	DO_YIELD();
}
