/**
 * Raphnet GC/N64-to-USB adapter RAW input backend.
 *
 * Ported from Project Midas (the GoldenEye PC port, port/src/raphnet.c) so an
 * N64 pad on a raphnet adapter reads exactly as it does there: the true N64
 * poll bytes, no deadzone, no reshaping. Midas-specific bits removed: the
 * launcher probe mode and the device-name query. Logging goes through
 * sysLogPrintf; the periodic rate line only prints with PD_RAPHDBG=1. Adds
 * portRaphnetPause() so the Controller Pak transport (raphnet.c) can own the
 * adapter while it reads/writes a pak.
 *
 * Stick calibration through the SDL joystick layer came out wrong:
 * Windows applies its own per-device joystick calibration/scaling to
 * DInput axes, so what SDL reports is neither the raw stick nor stable
 * across machines. The raphnet adapters expose a vendor HID interface
 * (mi_01, "HID-compliant vendor-defined device") that returns the ACTUAL
 * N64 poll response - the same bytes a console would see - which this
 * backend uses instead, bypassing the OS joystick stack entirely.
 *
 * Clean-room implementation from the publicly documented wire format
 * (the pj64raphnetraw/gcn64lib protocol; constants are interoperability
 * facts, no GPL code copied):
 *  - HID FEATURE reports, report id 0, report size 63 (v3.4+; 40 older).
 *  - Request: [0x80 = raw SI command, channel, tx_len, payload...].
 *  - Reply (poll get-feature until the first byte echoes the request):
 *    [0x80, channel, rx_len, data...].
 *  - N64 get-status SI command 0x01 -> 4 bytes: buttons hi, buttons lo,
 *    stick X, stick Y. The button word IS the CONT_* bit layout
 *    (A 0x8000 ... C-right 0x0001) and the stick bytes are signed N64
 *    units - zero translation needed.
 *
 * Windows: SDL's bundled hidapi FILTERS the vendor interface out of
 * SDL_hid_enumerate (probe-verified: only mi_00 was listed while Windows
 * shows mi_01 present), so the Win32 HID API is used directly - SetupDi
 * interface enumeration + HidD_SetFeature/HidD_GetFeature. Other hosts
 * use SDL_hid_*, whose hidraw enumeration is unfiltered.
 */
#ifdef PD_ENABLE_RAPHNET_RAW

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <PR/ultratypes.h>
#include "system.h"
#include "raphnetraw.h"

/* NO SDL.h in the Windows branch: SDL_stdinc pulls the repo's
 * freestanding string.h, which fights the windows.h header chain's
 * strlen declarations (known conflict). The Win32 HID transport needs
 * nothing from SDL anyway. */
#ifndef _WIN32
#include <SDL3/SDL.h>
#endif

#define RAPHNET_VID          0x289B
#define RQ_RAW_SI_COMMAND    0x80
#define RQ_SUSPEND_POLLING   0x03
#define RQ_SET_VIBRATION     0x07
#define N64_GET_STATUS       0x01
#define RAPHNET_MAX_CHAN     2
#define RAPHNET_REPORT     63   /* v3.4+; opening pre-3.4 falls back to 40 */

static int s_ReportSize = RAPHNET_REPORT;
static int s_Tried;

/* Transport health: the adapter handle can go stale without the game noticing
 * (USB re-enumeration, hub power event, selective suspend), after which every
 * feature-report call fails forever and the controller stays dead for the
 * session. After RN_FAIL_LIMIT consecutive failures the handle is closed and
 * re-opened, retried once per RN_RETRY_MS until the adapter answers
 * (raphnetOpenRetry). */
#define RN_FAIL_LIMIT   20   /* consecutive failed exchanges before reopen */
#define RN_RETRY_MS   1000   /* min wall-clock gap between reopen attempts */
static int s_Fails;
static int s_Lost;           /* 1 = reported the drop, waiting to recover */
static int s_Suspended;      /* adapter's own polling suspended (raw mode) */
static unsigned s_NextTry;
static unsigned s_OpenRetries;  /* raphnetOpenRetry attempts since the last good open (device-owner thread) */
static int s_Dbg = -1;

/* Midas logged through portConLogf; here it is the port log. Trailing
 * newlines are dropped (sysLogPrintf adds its own). */
static void rnLog(const char *fmt, ...)
{
    char line[512];
    size_t len;
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    len = strlen(line);
    while (len > 0 && line[len - 1] == '\n')
    {
        line[--len] = '\0';
    }
    sysLogPrintf(LOG_NOTE, "%s", line);
}

/* ==== Windows: native HID feature-report transport ======================= */
#ifdef _WIN32

#include <windows.h>
#include <setupapi.h>

static void rnMemset(void *dst, int v, unsigned n)
{
    unsigned char *d = (unsigned char *) dst;
    while (n--) *d++ = (unsigned char) v;
}
static void rnMemcpy(void *dst, const void *src, unsigned n)
{
    unsigned char *d = (unsigned char *) dst;
    const unsigned char *sp = (const unsigned char *) src;
    while (n--) *d++ = *sp++;
}
static int rnContains(const char *hay, const char *needle)
{
    int i, j;
    for (i = 0; hay[i] != '\0'; i++)
    {
        for (j = 0; needle[j] != '\0' && hay[i + j] == needle[j]; j++) {}
        if (needle[j] == '\0') return 1;
    }
    return 0;
}
#define RN_MEMSET rnMemset
#define RN_MEMCPY rnMemcpy
#define RN_DELAY(ms) Sleep(ms)

/* hid.dll (linked via -lhid); declared locally so no DDK header games */
typedef struct { ULONG Size; USHORT VendorID, ProductID, VersionNumber; } RN_HIDD_ATTRIBUTES;
__declspec(dllimport) void __stdcall HidD_GetHidGuid(GUID *guid);
__declspec(dllimport) BOOLEAN __stdcall HidD_GetAttributes(HANDLE h, RN_HIDD_ATTRIBUTES *attr);
__declspec(dllimport) BOOLEAN __stdcall HidD_SetFeature(HANDLE h, PVOID buf, ULONG len);
__declspec(dllimport) BOOLEAN __stdcall HidD_GetFeature(HANDLE h, PVOID buf, ULONG len);

static HANDLE s_Dev = INVALID_HANDLE_VALUE;

static int raphnetSendFeature(const u8 *buf, int len)
{
    return HidD_SetFeature(s_Dev, (PVOID) buf, (ULONG) len) ? 0 : -1;
}

static int raphnetGetFeature(u8 *buf, int len)
{
    return HidD_GetFeature(s_Dev, (PVOID) buf, (ULONG) len) ? len : -1;
}

static int raphnetExchange(const u8 *cmd, int cmdlen, u8 *rep, int repmax);
static int raphnetExchangeOnce(const u8 *cmd, int cmdlen, u8 *rep, int repmax);

/* Enumerate HID interface paths, open the raphnet vendor interface (the
 * path carries "vid_289b" and "mi_01") and verify it answers a raw SI
 * exchange. */
static int raphnetOpen(void)
{
    GUID hidGuid;
    HDEVINFO devs;
    SP_DEVICE_INTERFACE_DATA ifd;
    DWORD index;

    if (s_Tried)
    {
        return s_Dev != INVALID_HANDLE_VALUE;
    }
    s_Tried = 1;

    HidD_GetHidGuid(&hidGuid);
    devs = SetupDiGetClassDevsA(&hidGuid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devs == INVALID_HANDLE_VALUE)
    {
        return 0;
    }

    ifd.cbSize = sizeof(ifd);
    for (index = 0; SetupDiEnumDeviceInterfaces(devs, NULL, &hidGuid, index, &ifd); index++)
    {
        char detailbuf[sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A) + 512];
        SP_DEVICE_INTERFACE_DETAIL_DATA_A *detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_A *) detailbuf;
        const char *p;

        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
        if (!SetupDiGetDeviceInterfaceDetailA(devs, &ifd, detail, sizeof(detailbuf), NULL, NULL))
        {
            continue;
        }
        p = detail->DevicePath;
        /* Probe EVERY raphnet HID interface, not a hardcoded index: the
         * vendor/config interface is mi_01 on single-port adapters but
         * mi_02 on dual-port ones (e.g. PID_0064 exposes MI_00/01/02 and was
         * never recognised when everything but
         * mi_01 was skipped). The raw-SI answer below IS the test -
         * joystick interfaces simply do not reply. */
        if (!rnContains(p, "vid_289b"))
        {
            continue;
        }

        s_Dev = CreateFileA(p, GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                            OPEN_EXISTING, 0, NULL);
        if (s_Dev == INVALID_HANDLE_VALUE)
        {
            if (s_OpenRetries <= 1)
            {
                rnLog("raphnet: open failed for %s\n", p);
            }
            continue;
        }

        {
            u8 cmd[4] = { RQ_RAW_SI_COMMAND, 0, 1, N64_GET_STATUS };
            u8 rep[8];

            s_ReportSize = RAPHNET_REPORT;
            if (raphnetExchangeOnce(cmd, 4, rep, sizeof(rep)) >= 3)
            {
                rnLog("raphnet RAW adapter online (true N64 poll path)\n"
                                "raphnet interface: %s\n", p);
                break;
            }
            s_ReportSize = 40; /* pre-3.4 firmware report size */
            if (raphnetExchangeOnce(cmd, 4, rep, sizeof(rep)) >= 3)
            {
                rnLog("raphnet RAW adapter online (pre-3.4 firmware)\n"
                                "raphnet interface: %s\n", p);
                break;
            }
            /* Expected for the joystick interfaces of a multi-interface
             * adapter - only the vendor one answers. Logged so a user who
             * gets NO "adapter online" line can still show which
             * interfaces were tried. Only the first retry of an outage:
             * a silent device would otherwise log this every RN_RETRY_MS. */
            if (s_OpenRetries <= 1)
            {
                rnLog("raphnet: no protocol answer from %s\n", p);
            }
        }
        CloseHandle(s_Dev);
        s_Dev = INVALID_HANDLE_VALUE;
    }
    SetupDiDestroyDeviceInfoList(devs);
    return s_Dev != INVALID_HANDLE_VALUE;
}

static int raphnetIsOpen(void)
{
    return s_Dev != INVALID_HANDLE_VALUE;
}

#define RN_TICKS() ((unsigned) GetTickCount())

static unsigned long long rnUsec(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;

    if (freq.QuadPart == 0)
    {
        QueryPerformanceFrequency(&freq);
    }
    QueryPerformanceCounter(&now);
    {   /* split, or count * 1e6 overflows after ~21 days of uptime */
        const unsigned long long q = (unsigned long long) now.QuadPart;
        const unsigned long long f = (unsigned long long) freq.QuadPart;
        return (q / f) * 1000000ull + (q % f) * 1000000ull / f;
    }
}
#define RN_USEC() rnUsec()

static void raphnetCloseDev(void)
{
    if (s_Dev != INVALID_HANDLE_VALUE)
    {
        CloseHandle(s_Dev);
        s_Dev = INVALID_HANDLE_VALUE;
    }
}

/* ==== other hosts: SDL's hidapi ========================================== */
#elif SDL_VERSION_ATLEAST(2, 0, 18)

#define RN_MEMSET SDL_memset
#define RN_MEMCPY SDL_memcpy
#define RN_DELAY(ms) SDL_Delay(ms)

static SDL_hid_device *s_Dev;

static int raphnetSendFeature(const u8 *buf, int len)
{
    return (SDL_hid_send_feature_report(s_Dev, buf, len) < 0) ? -1 : 0;
}

static int raphnetGetFeature(u8 *buf, int len)
{
    return SDL_hid_get_feature_report(s_Dev, buf, len);
}

static int raphnetExchange(const u8 *cmd, int cmdlen, u8 *rep, int repmax);
static int raphnetExchangeOnce(const u8 *cmd, int cmdlen, u8 *rep, int repmax);

static int raphnetOpen(void)
{
    struct SDL_hid_device_info *list;
    struct SDL_hid_device_info *d;

    if (s_Tried)
    {
        return s_Dev != NULL;
    }
    s_Tried = 1;

    /* Log the reason, as the Windows branch does (input_pc.c's failure text
     * points here), but only on the first retry of an outage. */
    if (SDL_hid_init() != 0)
    {
        if (s_OpenRetries <= 1)
        {
            rnLog("raphnet: SDL_hid_init failed: %s\n", SDL_GetError());
        }
        return 0;
    }
    list = SDL_hid_enumerate(RAPHNET_VID, 0);
    for (d = list; d != NULL; d = d->next)
    {
        s_Dev = SDL_hid_open_path(d->path);
        if (s_Dev == NULL)
        {
            if (s_OpenRetries <= 1)
            {
                rnLog("raphnet: open failed for %s (%s) - check the hidraw permissions / udev rule for 289b\n",
                            d->path, SDL_GetError());
            }
            continue;
        }
        {
            u8 cmd[4] = { RQ_RAW_SI_COMMAND, 0, 1, N64_GET_STATUS };
            u8 rep[8];

            s_ReportSize = RAPHNET_REPORT;
            if (raphnetExchangeOnce(cmd, 4, rep, sizeof(rep)) >= 3)
            {
                rnLog("raphnet RAW adapter online (true N64 poll path)\n");
                break;
            }
            s_ReportSize = 40;
            if (raphnetExchangeOnce(cmd, 4, rep, sizeof(rep)) >= 3)
            {
                rnLog("raphnet RAW adapter online (pre-3.4 firmware)\n");
                break;
            }
            if (s_OpenRetries <= 1)
            {
                rnLog("raphnet: no protocol answer from %s\n", d->path);
            }
        }
        SDL_hid_close(s_Dev);
        s_Dev = NULL;
    }
    SDL_hid_free_enumeration(list);
    return s_Dev != NULL;
}

static int raphnetIsOpen(void)
{
    return s_Dev != NULL;
}

#define RN_TICKS() ((unsigned) SDL_GetTicks())
static unsigned long long rnUsec(void)
{   /* split, or count * 1e6 overflows after ~21 days of uptime */
    const unsigned long long q = SDL_GetPerformanceCounter();
    const unsigned long long f = SDL_GetPerformanceFrequency();
    return (q / f) * 1000000ull + (q % f) * 1000000ull / f;
}
#define RN_USEC() rnUsec()

static void raphnetCloseDev(void)
{
    if (s_Dev != NULL)
    {
        SDL_hid_close(s_Dev);
        s_Dev = NULL;
    }
}

#else /* SDL too old, no backend */

static int raphnetSendFeature(const u8 *buf, int len) { (void) buf; (void) len; return -1; }
static int raphnetGetFeature(u8 *buf, int len) { (void) buf; (void) len; return -1; }
static int raphnetOpen(void) { return 0; }
static int raphnetIsOpen(void) { return 0; }
static void raphnetCloseDev(void) {}
#define RN_TICKS() 0u
#define RN_USEC() 0ull
static void rnNopDelay(int ms) { (void) ms; }
#define RN_MEMSET(d, v, n) ((void) 0)
#define RN_MEMCPY(d, s, n) ((void) 0)
#define RN_DELAY(ms) rnNopDelay(ms)

#endif

/* ==== shared protocol ==================================================== */

/* Transport timing for the periodic 'raphnet: rate' log line (PD_RAPHDBG=1), to tell slow
 * exchanges from too many; read and written only by the thread that owns the device. */
static unsigned long long s_ExchUs;
static unsigned s_ExchN;
static unsigned s_ExchMaxUs;
static unsigned s_ConfirmN;
static unsigned s_RumbleWrites;          /* pak writes (arm + motor) since the last rate line */
#define RN_NOPAK_RETRY_MS 3000
static unsigned s_NoPakUntil[RAPHNET_MAX_CHAN]; /* RN_TICKS before which an unarmed pak is not retried */
static u8 s_NoPakLogged[RAPHNET_MAX_CHAN];

static int raphnetExchangeOnceRaw(const u8 *cmd, int cmdlen, u8 *rep, int repmax);

static int raphnetExchangeOnce(const u8 *cmd, int cmdlen, u8 *rep, int repmax)
{
    const unsigned long long t0 = RN_USEC();
    const int n = raphnetExchangeOnceRaw(cmd, cmdlen, rep, repmax);
    const unsigned us = (unsigned) (RN_USEC() - t0);

    s_ExchUs += us;
    s_ExchN++;
    if (us > s_ExchMaxUs)
    {
        s_ExchMaxUs = us;
    }
    return n;
}

static int raphnetExchangeOnceRaw(const u8 *cmd, int cmdlen, u8 *rep, int repmax)
{
    u8 buf[RAPHNET_REPORT + 1];
    int tries;
    int n;

    RN_MEMSET(buf, 0, sizeof(buf));
    RN_MEMCPY(buf + 1, cmd, (cmdlen <= RAPHNET_REPORT) ? cmdlen : RAPHNET_REPORT);
    if (raphnetSendFeature(buf, s_ReportSize + 1) < 0)
    {
        return -1;
    }

    for (tries = 0; tries < 64; tries++)
    {
        RN_MEMSET(buf, 0, sizeof(buf));
        n = raphnetGetFeature(buf, s_ReportSize + 1);
        if (n < 0)
        {
            return -1;
        }
        if (buf[1] == cmd[0])
        {
            n = s_ReportSize;
            if (n > repmax)
            {
                n = repmax;
            }
            RN_MEMCPY(rep, buf + 1, n);
            return n;
        }
        RN_DELAY(1);
    }
    return -1;
}

/**
 * Exchange with self-healing transport (see the health block up top).
 * A run of failures closes the stale handle and re-opens the adapter -
 * throttled to one attempt per RN_RETRY_MS so a genuinely unplugged
 * adapter costs a couple of enumerations per second, not a spin.
 * PD_RAPHDBG=1 traces every failure; drops/recoveries always print one
 * line each so a user's log tells the story without extra setup.
 */
static int raphnetExchange(const u8 *cmd, int cmdlen, u8 *rep, int repmax)
{
    int n;

    if (s_Dbg < 0)
    {
        s_Dbg = getenv("PD_RAPHDBG") != NULL;
    }

    n = raphnetExchangeOnce(cmd, cmdlen, rep, repmax);
    if (n >= 0)
    {
        if (s_Lost)
        {
            rnLog("raphnet: adapter recovered after %d failed exchanges\n", s_Fails);
            s_Lost = 0;
        }
        s_Fails = 0;
        return n;
    }

    s_Fails++;
    if (s_Dbg)
    {
        rnLog("raphnet: exchange failed (cmd %02x, %d consecutive)\n", cmd[0], s_Fails);
    }
    if (s_Fails < RN_FAIL_LIMIT)
    {
        return -1;
    }

    /* Transport looks dead - drop the handle and try to re-acquire it. */
    {
        unsigned now = RN_TICKS();
        if (!s_Lost)
        {
            rnLog("raphnet: adapter stopped responding - reconnecting\n");
            s_Lost = 1;
        }
        if (s_NextTry != 0 && (int)(now - s_NextTry) < 0)
        {
            return -1;   /* not yet time to retry */
        }
        s_NextTry = now + RN_RETRY_MS;

        raphnetCloseDev();
        s_Suspended = 0;    /* the reopened adapter starts unsuspended */
        s_Tried = 0;        /* allow re-enumeration */
        if (!raphnetOpen())
        {
            return -1;
        }
        rnLog("raphnet: adapter re-opened\n");
        s_Fails = 0;
        n = raphnetExchangeOnce(cmd, cmdlen, rep, repmax);
        if (n >= 0 && s_Lost)
        {
            rnLog("raphnet: adapter recovered\n");
            s_Lost = 0;
        }
    }
    return n;
}

/**
 * raphnetOpen() with periodic retry: while the adapter is closed, s_Tried
 * expires every RN_RETRY_MS so an unplugged pad comes back on replug instead of
 * staying dead for the session. Device-owner thread only; the clock is set
 * before the attempt so a failure right after cannot flap the handle.
 */
static int raphnetOpenRetry(void)
{
    if (s_Tried && !raphnetIsOpen())
    {
        const unsigned now = RN_TICKS();

        if (s_NextTry != 0 && (int) (now - s_NextTry) < 0)
        {
            return 0;   /* not yet time to retry */
        }
        s_NextTry = now + RN_RETRY_MS;
        s_Suspended = 0;    /* a reopened adapter starts unsuspended */
        s_Tried = 0;        /* allow re-enumeration */
        s_OpenRetries++;
        if (!raphnetOpen())
        {
            return 0;
        }
        s_OpenRetries = 0;
        if (s_Lost)
        {
            rnLog("raphnet: adapter re-opened\n");
        }
        return 1;
    }
    if (!raphnetOpen())
    {
        s_NextTry = RN_TICKS() + RN_RETRY_MS;   /* a failed first open starts the clock */
        return 0;
    }
    return 1;
}

/* ---- public API (input.c) ----------------------------------------------- */

/**
 * The adapter POLLS the controller itself for its joystick interface -
 * including pak management, which fights raw pak writes (every write is ACKed
 * but the motor stays still, because the firmware's own management resets it
 * straight after). pj64raphnetraw suspends that polling for its
 * whole session; we suspend lazily on first raw use and best-effort
 * resume at exit so the adapter's normal joystick mode isn't left dead.
 */
static void raphnetResumeAtExit(void)
{
    if (s_Suspended && raphnetIsOpen())
    {
        u8 cmd[2] = { RQ_SUSPEND_POLLING, 0 };
        u8 rep[8];
        raphnetExchange(cmd, 2, rep, sizeof(rep));
        s_Suspended = 0;
    }
}

/* Register the exit-time resume only once, and never after the poller thread
 * exists (raphnetStopThread resumes after the join), or atexit would resume
 * before the stop and hang exit. */
static s32 s_ResumeArmed;

static void raphnetSuspendOnce(void)
{
    if (!s_Suspended && raphnetIsOpen())
    {
        u8 cmd[2] = { RQ_SUSPEND_POLLING, 1 };
        u8 rep[8];
        if (raphnetExchange(cmd, 2, rep, sizeof(rep)) >= 0)
        {
            s_Suspended = 1;
            if (!s_ResumeArmed)
            {
                s_ResumeArmed = 1;
                atexit(raphnetResumeAtExit);
            }
            rnLog("raphnet: adapter polling suspended (raw session)\n");
        }
    }
}

/* ---- asynchronous transport -------------------------------------------
 *
 * An exchange is a synchronous USB control round trip: HidD_SetFeature then
 * a HidD_GetFeature retry loop. Control transfers are scheduled on 1ms bus
 * frames, so one poll costs several milliseconds NO MATTER how tight the
 * retry loop is - measured ~5.3ms, which pinned an uncapped 550fps to 140
 * because the frame time simply became the USB round trip.
 *
 * It cannot be fixed by polling less often: joyPoll is the PRODUCER of the
 * sample ring that joyConsumeSamples drains every tick, and starving that
 * ring kills input outright (tried it). Producer and consumer must stay
 * 1:1. So the transport moves off the frame thread instead.
 *
 * This is LATENCY HIDING, not parallelism - the thread is blocked in the
 * kernel on the bus, not burning CPU - so it works on a single-core target
 * (Original Xbox came up) exactly as well as on a many-core one. Three
 * properties keep that true:
 *
 *   - The thread NEVER spins. It blocks in the USB call, and sleeps when
 *     there is nothing to do. A hot loop here would starve the game on a
 *     single core.
 *   - No mutex. On one core, blocking the game thread on a lock held by a
 *     preempted poller is a full time slice lost to priority inversion.
 *     The whole sample is 4 bytes - button word, stick X, stick Y - so it
 *     packs into ONE aligned 32-bit word and a plain volatile store/load is
 *     atomic everywhere this port runs. The two spare bits come free:
 *     the button word is masked with 0xFF3F, leaving 0x40 and 0x80 unused.
 *   - Rumble is carried as DESIRED STATE, not a queue, so it cannot back up
 *     if the game toggles faster than USB can drain.
 *
 * Exactly ONE thread ever touches the device: raphnetOpen()'s reopen/health
 * logic is not reentrant. The game thread's first poll happens BEFORE the
 * thread exists (it primes the cache synchronously and then starts it);
 * after that the game thread only ever reads the cache.
 *
 * PD_RAPHNET_SYNC=1 forces the old inline behaviour for A/B.
 */
#define RN_CACHE_PRESENT 0x0040u
#define RN_CACHE_ERROR   0x0080u

static s32 raphnetPollHw(s32 chan, u16 *btn, s8 *x, s8 *y);
static s32 raphnetRumbleHw(s32 chan, s32 on);

/* This file deliberately has NO SDL.h on Windows (see the include block -
 * SDL_stdinc fights the windows.h string.h chain), so threads follow the
 * same per-platform pattern the transport and RN_DELAY already use. */
#ifdef _WIN32
typedef HANDLE rn_thread_t;
#else
typedef SDL_Thread *rn_thread_t;
#endif

/* Refresh counter (Midas showed it in the title bar): is the stick actually
 * sampled faster than the frame rate, or is the transport the bottleneck? */
static unsigned int g_PortRaphnetPolls;

static volatile u32 s_Cache[RAPHNET_MAX_CHAN];
static volatile u8  s_ChanActive[RAPHNET_MAX_CHAN];
static volatile s32 s_RumbleWant[RAPHNET_MAX_CHAN];
static volatile unsigned s_ChanAskMs[RAPHNET_MAX_CHAN]; /* RN_TICKS of the game's last read */
static u8 s_HwPresent[RAPHNET_MAX_CHAN]; /* raphnetPollHw's last answer was "pad" (device thread only) */
static volatile s32 s_ThreadStop;
static volatile s32 s_PauseReq;   /* PD: raphnet.c owns the adapter for a pak transfer */
static volatile s32 s_PauseAck;   /* poller saw s_PauseReq and is idle */
static rn_thread_t  s_Thread;
static s32          s_ThreadMode = -1;  /* -1 unresolved, 0 inline, 1 threaded */

static u32 raphnetPack(s32 status, u16 btn, s8 x, s8 y)
{
    u32 w = (u32) (btn & 0xFF3Fu)
          | ((u32) (u8) x << 16)
          | ((u32) (u8) y << 24);

    if (status > 0)
    {
        w |= RN_CACHE_PRESENT;
    }
    else if (status < 0)
    {
        w |= RN_CACHE_ERROR;
    }
    return w;
}

/* A channel the game has read within RN_HOT_MS is polled every pass, while other
 * active channels are only polled every RN_IDLE_POLL_MS, so an empty port no
 * longer steals bus time from the pad being played (short taps were being
 * dropped). */
#define RN_HOT_MS        2000
#define RN_IDLE_POLL_MS   250
#define RN_RATE_LOG_MS  10000
#define RN_RUMBLE_MIN_MS  100

static void raphnetPollLoop(void)
{
    s32 rumbleHave[RAPHNET_MAX_CHAN];
    unsigned lastPoll[RAPHNET_MAX_CHAN];
    unsigned lastRumble[RAPHNET_MAX_CHAN];
    unsigned polls[RAPHNET_MAX_CHAN];
    unsigned rateMs = RN_TICKS();
    s32 chan;

    for (chan = 0; chan < RAPHNET_MAX_CHAN; chan++)
    {
        rumbleHave[chan] = -1;
        lastPoll[chan] = 0;
        lastRumble[chan] = rateMs - RN_RUMBLE_MIN_MS;
        polls[chan] = 0;
    }

    while (!s_ThreadStop)
    {
        int didwork = 0;
        unsigned now;

        if (s_PauseReq)
        {
            s_PauseAck = 1;
            RN_DELAY(5);
            continue;
        }
        s_PauseAck = 0;
        now = RN_TICKS();

        for (chan = 0; chan < RAPHNET_MAX_CHAN; chan++)
        {
            u16 btn = 0;
            s8 x = 0;
            s8 y = 0;
            s32 r;

            if (!s_ChanActive[chan])
            {
                continue;   /* only channels the game has actually asked for */
            }

            /* Read first: hot channels every pass, idle ones on the slow rescan.
             * The age is signed because the game thread can stamp an ask later
             * than this sweep's `now`. */
            if ((int) (now - s_ChanAskMs[chan]) < RN_HOT_MS
                || (unsigned) (now - lastPoll[chan]) >= RN_IDLE_POLL_MS)
            {
                lastPoll[chan] = now;
                r = raphnetPollHw(chan, &btn, &x, &y);
                s_Cache[chan] = raphnetPack(r, btn, x, y);
                g_PortRaphnetPolls++;   /* title-bar 'rn' = cache refresh rate */
                polls[chan]++;
                didwork = 1;
            }

            /* Then rumble: the motor write goes after this pass's read, at most
             * once per RN_RUMBLE_MIN_MS per channel, so rumble pulses no longer
             * delay pad reads (input lag while holding fire); skipped pulses
             * coalesce because the state is desired-state. */
            if (s_RumbleWant[chan] != rumbleHave[chan]
                && (unsigned) (now - lastRumble[chan]) >= RN_RUMBLE_MIN_MS)
            {
                const s32 want = s_RumbleWant[chan];

                lastRumble[chan] = now;
                if (raphnetRumbleHw(chan, want) == 0)
                {
                    rumbleHave[chan] = want;
                }
                didwork = 1;
            }
        }

        /* Every ~10s, log per-channel refresh and USB exchange cost. Uses printf,
         * not portConLogf, since this isn't the game thread and the console
         * ring may drop the line. */
        if ((unsigned) (now - rateMs) >= RN_RATE_LOG_MS && s_Dbg <= 0)
        {
            for (chan = 0; chan < RAPHNET_MAX_CHAN; chan++)
            {
                polls[chan] = 0;
            }
            s_ExchUs = 0;
            s_ExchN = 0;
            s_ExchMaxUs = 0;
            s_ConfirmN = 0;
            s_RumbleWrites = 0;
            rateMs = now;
        }
        else if ((unsigned) (now - rateMs) >= RN_RATE_LOG_MS)
        {
            const unsigned dt = now - rateMs;
            const unsigned avg10 = s_ExchN ? (unsigned) (s_ExchUs * 10 / s_ExchN / 1000) : 0;
            const unsigned max10 = s_ExchMaxUs / 100;
            /* Built whole and written with one call, since stdio locks per call
             * and other threads' lines would land inside a pieced line. */
            char line[256];
            int len;

            len = snprintf(line, sizeof(line), "raphnet: rate");
            for (chan = 0; chan < RAPHNET_MAX_CHAN; chan++)
            {
                const u32 w = s_Cache[chan];

                if (len >= 0 && len < (int) sizeof(line))
                {
                    len += snprintf(line + len, sizeof(line) - (size_t) len, " | ch%d %uHz %s %s", (int) chan,
                           (polls[chan] * 1000 + dt / 2) / dt,
                           !s_ChanActive[chan] ? "off" : ((int) (now - s_ChanAskMs[chan]) < RN_HOT_MS) ? "hot" : "idle",
                           (w & RN_CACHE_ERROR) ? "error" : (w & RN_CACHE_PRESENT) ? "pad" : "empty");
                }
                polls[chan] = 0;
            }
            if (len >= 0 && len < (int) sizeof(line))
            {
                snprintf(line + len, sizeof(line) - (size_t) len,
                         " | exchange avg %u.%ums max %u.%ums, %u/s, %u confirms, %u rumble writes\n",
                         avg10 / 10, avg10 % 10, max10 / 10, max10 % 10,
                         (s_ExchN * 1000 + dt / 2) / dt, s_ConfirmN, s_RumbleWrites);
            }
            fputs(line, stdout);
            fflush(stdout);
            s_ExchUs = 0;
            s_ExchN = 0;
            s_ExchMaxUs = 0;
            s_ConfirmN = 0;
            s_RumbleWrites = 0;
            rateMs = now;
        }

        /* Never spin. The USB exchange is the yield in the normal case;
         * this covers "no channel active yet" and "adapter gone", where the
         * calls return immediately. One active channel gives roughly a
         * 160Hz refresh - already better than the 60Hz the frame thread
         * managed, at zero frame cost. */
        RN_DELAY(didwork ? 1 : 20);
    }
}

#ifdef _WIN32
static DWORD WINAPI raphnetThreadEntry(LPVOID unused)
{
    (void) unused;
    raphnetPollLoop();
    return 0;
}
#else
static int raphnetThreadEntry(void *unused)
{
    (void) unused;
    raphnetPollLoop();
    return 0;
}
#endif

static void raphnetStopThread(void)
{
    if (s_Thread != NULL)
    {
        s_ThreadStop = 1;
#ifdef _WIN32
        /* bounded by one exchange; resume only once the device has a
         * single owner again */
        if (WaitForSingleObject(s_Thread, 2000) == WAIT_OBJECT_0)
        {
            raphnetResumeAtExit();
        }
        CloseHandle(s_Thread);
#else
        SDL_WaitThread(s_Thread, NULL);
        raphnetResumeAtExit();   /* the poller's suspend, undone after the join */
#endif
        s_Thread = NULL;
    }
}

/* 1 = cache is being refreshed by the thread, 0 = poll inline as before. */
static s32 raphnetAsyncReady(void)
{
    if (s_ThreadMode < 0)
    {
        if (getenv("PD_RAPHNET_SYNC") != NULL)
        {
            s_ThreadMode = 0;
        }
        else
        {
            /* Set before the thread can run, so the poller never
             * registers the resume itself. */
            const s32 resumeArmed = s_ResumeArmed;

            s_ResumeArmed = 1;
#ifdef _WIN32
            s_Thread = CreateThread(NULL, 0, raphnetThreadEntry, NULL, 0, NULL);
#else
            s_Thread = SDL_CreateThread(raphnetThreadEntry, "ge-raphnet", NULL);
#endif
            if (s_Thread == NULL)
            {
                rnLog("raphnet: poller thread failed to start - polling inline\n");
                s_ThreadMode = 0;
                s_ResumeArmed = resumeArmed;
            }
            else
            {
                /* Joined at exit: leaving a detached thread mid-transfer
                 * while the process tears down is a mystery-crash source,
                 * and the wait is bounded by one exchange. */
                atexit(raphnetStopThread);
                s_ThreadMode = 1;
            }
        }
    }
    return s_ThreadMode;
}

/**
 * Is the raw path usable (or on its way back)? Game thread; it may open the
 * device only until the poller exists (raphnetOpen() is not reentrant), and an
 * adapter being re-acquired (s_Lost) counts so a replugged pad keeps its raw
 * slot.
 */
s32 portRaphnetAvailable(void)
{
    if (s_Thread != NULL)
    {
        return raphnetIsOpen() || s_Lost;
    }
    return raphnetOpenRetry() || s_Lost;
}

/**
 * Poll one channel. Returns 1 with the REAL N64 pad state (button word in
 * CONT_* layout, stick in signed N64 units), 0 when no controller answers
 * on that channel, -1 on adapter error.
 *
 * Threaded: reads the cache, no USB, no blocking. See the block above.
 */
s32 portRaphnetPoll(s32 chan, u16 *btn, s8 *x, s8 *y)
{
    u32 w;

    if (chan < 0 || chan >= RAPHNET_MAX_CHAN)
    {
        return -1;
    }
    s_ChanAskMs[chan] = RN_TICKS();   /* keeps the channel hot in the poller */

    if (!s_ChanActive[chan] && !s_PauseReq)
    {
        /* First use of this channel. If the poller has not started yet we
         * are still single-threaded, so prime the cache with a real
         * synchronous exchange and hand back the true answer - the startup
         * probe in input_pc.c depends on getting present/empty right.
         * A channel first touched AFTER the thread is up reads empty for
         * one sweep (~10ms) rather than racing the device. */
        if (s_Thread == NULL && s_ThreadMode != 0)
        {
            u16 b = 0;
            s8 bx = 0;
            s8 by = 0;
            s32 r = raphnetPollHw(chan, &b, &bx, &by);

            s_Cache[chan] = raphnetPack(r, b, bx, by);
        }
        s_ChanActive[chan] = 1;
    }

    if (s_PauseReq)
    {
        w = s_Cache[chan];   /* hold the last state for the pak transfer */
    }
    else if (raphnetAsyncReady() != 1)
    {
        return raphnetPollHw(chan, btn, x, y);
    }
    else
    {
        w = s_Cache[chan];
    }
    if (btn != NULL)
    {
        *btn = (u16) (w & 0xFF3Fu);
    }
    if (x != NULL)
    {
        *x = (s8) ((w >> 16) & 0xFFu);
    }
    if (y != NULL)
    {
        *y = (s8) ((w >> 24) & 0xFFu);
    }

    return (w & RN_CACHE_ERROR) ? -1 : (w & RN_CACHE_PRESENT) ? 1 : 0;
}

/**
 * Is a pad on this channel? Like portRaphnetPoll but without stamping the
 * channel as read, so input_pc.c's once-a-second recheck doesn't keep an empty
 * channel hot.
 */
s32 portRaphnetPeek(s32 chan)
{
    u32 w;

    if (chan < 0 || chan >= RAPHNET_MAX_CHAN)
    {
        return -1;
    }
    if (s_Thread == NULL || !s_ChanActive[chan])
    {
        return portRaphnetPoll(chan, NULL, NULL, NULL);
    }
    w = s_Cache[chan];
    return (w & RN_CACHE_ERROR) ? -1 : (w & RN_CACHE_PRESENT) ? 1 : 0;
}

static s32 raphnetPollHw(s32 chan, u16 *btn, s8 *x, s8 *y)
{
    u8 cmd[4] = { RQ_RAW_SI_COMMAND, 0, 1, N64_GET_STATUS };
    u8 rep[16];
    int n;

    if (!raphnetOpenRetry() || !raphnetIsOpen() || chan < 0 || chan >= RAPHNET_MAX_CHAN)
    {
        return -1;
    }
    raphnetSuspendOnce();
    cmd[1] = (u8) chan;
    n = raphnetExchange(cmd, 4, rep, sizeof(rep));
    if (n < 3)
    {
        return -1;
    }
    if (rep[2] != 4) /* rx_len: 0 = nothing plugged into that port */
    {
        /* Confirm before believing it. Poll and the Rumble Pak writes
         * share command byte 0x80, and raphnetExchange matches replies on
         * that byte alone - so a late pak-write reply can be consumed by
         * the next poll, whose rx_len is then 1 instead of 4. Reporting
         * that as "nothing plugged in" is exactly what a mid-session
         * disconnect looks like. One clean re-exchange settles it.
         * Only a pad that WAS there needs it: an empty port read empty
         * again costs nothing to believe, and confirming it on every poll
         * doubled the cost of each sweep of an empty port 2. */
        if (!s_HwPresent[chan])
        {
            return 0;
        }
        s_ConfirmN++;
        n = raphnetExchange(cmd, 4, rep, sizeof(rep));
        if (n < 3)
        {
            return -1;
        }
        if (rep[2] != 4)
        {
            s_HwPresent[chan] = 0;
            return 0;
        }
    }
    s_HwPresent[chan] = 1;
    if (btn != NULL)
    {
        *btn = (u16)((rep[3] << 8) | rep[4]) & 0xFF3F; /* drop reset/unused bits */
    }
    if (x != NULL)
    {
        *x = (s8) rep[5];
    }
    if (y != NULL)
    {
        *y = (s8) rep[6];
    }
    return 1;
}

/**
 * Rumble Pak drive - the REAL pak protocol over raw SI, byte-identical
 * to what GE itself does on console (src/motor.c): arm the pak once by
 * writing 32x0x80 to block 1024 (byte 0x8000), then drive the motor with
 * 32x0x01 / 32x0x00 blocks to block 1536 (byte 0xC000), each write a
 * CONT_CMD_WRITE_MEMPACK (0x03) with the 5-bit address CRC. (The
 * adapter's managed SET_VIBRATION 0x07 exists but did nothing on v3.6 firmware
 * in raw use - the SI path needs no firmware feature at all.)
 * GE pulses start/stop for intensity; forwarding only the TRANSITIONS is
 * exactly the waveform the physical pak receives anyway.
 */
static u8 rnPakAddrCrc(u16 addr)
{
    /* N64 pak address CRC - same algorithm as the repo's own
     * src/libultra/io/crc.c __osContAddressCrc */
    u8 temp = 0;
    u8 temp2;
    int i;

    for (i = 0; i < 16; i++)
    {
        temp2 = (temp & 0x10) ? 21 : 0;
        temp <<= 1;
        temp |= (u8)((addr & 0x400) ? 1 : 0);
        addr <<= 1;
        temp ^= temp2;
    }
    return temp & 0x1f;
}

/* N64 pak data CRC of 32 bytes of `fill` - same algorithm as
 * src/libultra/io/crc.c __osContDataCrc */
static u8 rnPakDataCrc(u8 fill)
{
    u8 temp = 0;
    u8 temp2;
    int i;
    int j;

    for (i = 0; i <= 32; i++)
    {
        for (j = 7; j >= 0; j--)
        {
            temp2 = (temp & 0x80) ? 133 : 0;
            temp <<= 1;
            if (i < 32)
            {
                temp |= (u8)((fill & (1 << j)) ? 1 : 0);
            }
            temp ^= temp2;
        }
    }
    return temp;
}

static s32 raphnetPakWrite(s32 chan, u16 block, u8 fill)
{
    u8 cmd[3 + 35];
    u8 rep[8];
    u16 af = (u16)((block << 5) | rnPakAddrCrc(block));
    int i;
    int n;

    cmd[0] = RQ_RAW_SI_COMMAND;
    cmd[1] = (u8) chan;
    cmd[2] = 35;                 /* CONT_CMD_WRITE_MEMPACK_TX */
    cmd[3] = 0x03;               /* CONT_CMD_WRITE_MEMPACK */
    cmd[4] = (u8)(af >> 8);
    cmd[5] = (u8)(af & 0xFF);
    for (i = 0; i < 32; i++)
    {
        cmd[6 + i] = fill;
    }
    n = raphnetExchange(cmd, sizeof(cmd), rep, sizeof(rep));
    if (n < 4)
    {
        return -1;   /* rx_len 1 needs four bytes: header, channel, length, CRC */
    }
    /* The controller answers the write with the data-CRC byte whether or not a
     * pak is in; only that CRC inverted means an empty slot. */
    if (rep[2] == 1 && rep[3] == (u8) ~rnPakDataCrc(fill))
    {
        return -1;
    }
    return (rep[2] == 1) ? 0 : -1; /* rx_len 1 = pak answered with data CRC */
}

/**
 * Threaded: records the DESIRED motor state and returns immediately; the
 * poller reconciles it on its next sweep. Desired-state rather than a queue
 * so rapid toggling cannot back up behind the USB bus.
 */
s32 portRaphnetRumble(s32 chan, s32 on)
{
    if (chan < 0 || chan >= RAPHNET_MAX_CHAN)
    {
        return -1;
    }

    if (raphnetAsyncReady() != 1)
    {
        return s_PauseReq ? 0 : raphnetRumbleHw(chan, on);
    }

    s_RumbleWant[chan] = (on != 0);
    return 0;
}

static s32 raphnetRumbleHw(s32 chan, s32 on)
{
    static s8 last[RAPHNET_MAX_CHAN] = { -1, -1 };
    static u8 armed[RAPHNET_MAX_CHAN];
    /* A failed motor write may still have landed, so until a write succeeds an
     * OFF still goes out; after a few failures in a row the edge counts as handled. */
    static u8 unsure[RAPHNET_MAX_CHAN];
    static u8 fails[RAPHNET_MAX_CHAN];

    if (!raphnetIsOpen() || chan < 0 || chan >= RAPHNET_MAX_CHAN)
    {
        return -1;
    }
    on = (on != 0);
    if (last[chan] == (s8) on)
    {
        return 0;
    }
    /* No rumble pak must cost nothing: stopping a never-armed motor is a no-op,
     * and a pak that didn't answer the arm is left alone for RN_NOPAK_RETRY_MS,
     * otherwise the failing write was retried ahead of every pad read and
     * caused input lag. */
    if (!armed[chan] && !on && !unsure[chan])
    {
        last[chan] = 0;
        return 0;
    }
    if (!armed[chan] && !(unsure[chan] && !on)
        && s_NoPakUntil[chan] != 0 && (int) (RN_TICKS() - s_NoPakUntil[chan]) < 0)
    {
        return 0;
    }
    raphnetSuspendOnce();

    if (!armed[chan])
    {
        s_RumbleWrites++;
        if (raphnetPakWrite(chan, 1024, 0x80) != 0) /* GE osMotorInit arm */
        {
            if (!s_NoPakLogged[chan])
            {
                s_NoPakLogged[chan] = 1;
                rnLog("raphnet: no Rumble Pak answered on ch%d - rumble retried every %ds, not every poll",
                       (int) chan, RN_NOPAK_RETRY_MS / 1000);
            }
            s_NoPakUntil[chan] = RN_TICKS() + RN_NOPAK_RETRY_MS;
            return 0;
        }
        armed[chan] = 1;
    }
    s_RumbleWrites++;
    if (raphnetPakWrite(chan, 1536, on ? 0x01 : 0x00) != 0)
    {
        armed[chan] = 0; /* pak vanished mid-session: re-arm next time */
        s_NoPakUntil[chan] = RN_TICKS() + RN_NOPAK_RETRY_MS;
        last[chan] = -1; /* motor state unknown */
        unsure[chan] = 1;
        if (++fails[chan] < 3)
        {
            return -1;   /* the poller keeps have behind want: retried after RN_RUMBLE_MIN_MS */
        }
        fails[chan] = 0;
        return 0;
    }
    last[chan] = (s8) on;
    unsure[chan] = 0;
    fails[chan] = 0;
    return 0;
}

/**
 * PD: hand the adapter to the Controller Pak transport (raphnet.c, hidapi) and
 * back. Two owners interleaving raw SI exchanges would mismatch replies, so the
 * poller parks (and says so) before the pak transfer starts. Bounded wait: one
 * exchange is a few ms; 2s means the poller is wedged in USB and the pak code
 * will fail on its own.
 */
void portRaphnetPause(s32 on)
{
    if (!on)
    {
        s_PauseReq = 0;
        return;
    }
    s_PauseAck = 0;
    s_PauseReq = 1;
    if (s_Thread != NULL)
    {
        int waited;

        for (waited = 0; waited < 2000 && !s_PauseAck; waited += 2)
        {
            RN_DELAY(2);
        }
    }
}

#endif /* PD_ENABLE_RAPHNET_RAW */
