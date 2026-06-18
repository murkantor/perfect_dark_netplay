// see https://github.com/n64decomp/007/blob/master/tools/mktex/src/libpdtex/reader.c
// and https://github.com/doomhack/perfect_dark/blob/master/src/lib/rzip.c

#include <stdint.h>
#include <zlib.h>

#include "lib/rzip.h"

#ifdef NXDK
#include "xboxtrace.h" // boot bring-up tracing (port/src/xboxtrace.c)
// Capped per-call-site trace so the per-file decompress path doesn't spam the log.
#define RZIP_RTRACE(...) do { static int _n = 0; if (_n < 12) { _n++; xboxTracef(__VA_ARGS__); } } while (0)
#else
#define RZIP_RTRACE(...) do {} while (0)
#endif

void *var80091558; // g_RzipUnused

bool rzipIs1172(void *buffer)
{
	const u8* src = buffer;
	return (src[0] == 0x11 && src[1] == 0x72);
}

bool rzipIs1173(void *buffer)
{
	const u8* src = buffer;
	return (src[0] == 0x11 && src[1] == 0x73);
}

static inline s32 rzipInflate1172(z_stream *strm, u8 *src, void *dst)
{
	strm->avail_in = 0x2000;
	strm->next_in = src;

	do {
		strm->avail_out = 0x2000;
		strm->next_out = dst;
		if (inflate(strm, Z_FINISH) == Z_STREAM_ERROR) {
			rmonPrintf("rzipInflate1172: Z_STREAM_ERROR\n");
			return 0;
		}
	} while (strm->avail_out == 0);

	return strm->total_out;
}

static inline s32 rzipInflate1173(z_stream *strm, u8 *src, void *dst, u32 dstLen)
{
	strm->next_in = src;
	strm->avail_out = dstLen;
	strm->next_out = dst;

	// "compressed size unknown": the original passed (uInt)-1 (0xFFFFFFFF) and inflated
	// in a SINGLE Z_SYNC_FLUSH call. On 64-bit desktop that fills the whole buffer in
	// one shot, but on a 32-bit target (Original Xbox / NV2A) the single-call /
	// "infinite avail_in" assumption stops early -- inflate returns with avail_out > 0
	// and the rest of dst stays zero (this truncated the ROM data segment so the file
	// offset table at +0x28080 read as zeros and EVERY file load returned NULL).
	//
	// Fix: bound avail_in to the headroom between src and the top of the address space
	// (so the count can't wrap a 32-bit pointer), then LOOP inflate until the output
	// buffer is full or the stream ends. Desktop is unaffected -- the loop runs once.
	const uintptr_t headroom = (uintptr_t)-1 - (uintptr_t)src;
	strm->avail_in = (headroom > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uInt)headroom;

	int rc;
	int iters = 0;
	do {
		rc = inflate(strm, Z_SYNC_FLUSH);
		iters++;
	} while (rc == Z_OK && strm->avail_out != 0 && strm->avail_in != 0);

	RZIP_RTRACE("PDBOOT: rzip1173 rc=%d iters=%d out=%lu ai=%lu ao=%lu dst=0x%x",
		rc, iters, (unsigned long)strm->total_out,
		(unsigned long)strm->avail_in, (unsigned long)strm->avail_out, (unsigned)dstLen);

	if (rc == Z_STREAM_ERROR) {
		rmonPrintf("rzipInflate1173: Z_STREAM_ERROR\n");
		return 0;
	}

	return strm->total_out;
}

s32 rzipInflate(void *srcp, void *dst, void *scratch)
{
	s32 ret = 0;
	u8 *src = srcp;
	z_stream strm = { 0 };

	ret = inflateInit2(&strm, -15);
	if (ret != Z_OK) {
		RZIP_RTRACE("PDBOOT: rzip inflateInit2 FAILED ret=%d hdr=%s lib=%s sz=%u",
			ret, ZLIB_VERSION, zlibVersion(), (unsigned)sizeof(z_stream));
		rmonPrintf("rzipInflate: inflateInit2 failed: %d\n", ret);
		return 0;
	}
	RZIP_RTRACE("PDBOOT: rzip init ok ver=%s is1173=%d is1172=%d b0=%02x b1=%02x",
		zlibVersion(), (int)rzipIs1173(src), (int)rzipIs1172(src), src[0], src[1]);

	if (rzipIs1173(src)) {
		// 1173, we know the uncompressed length
		const u32 dstLen = ((u32)src[2] << 16) | ((u32)src[3] << 8) | (u32)src[4];
		ret = rzipInflate1173(&strm, src + 5, dst, dstLen);
	} else if (rzipIs1172(src)) {
		// 1172, uncompressed length unknown
		ret = rzipInflate1172(&strm, src + 2, dst);
	} else {
		rmonPrintf("rzipInflate: input not in any known rare zip format\n");
		ret = 0;
	}

	inflateEnd(&strm);

	if (ret) {
		var80091558 = strm.next_in;
		return strm.total_out;
	} else {
		return 0;
	}
}

u32 rzipInit(void)
{
	// this builds tables in the original assembly version, we don't need that
	return 0;
}

void *rzipGetSomething(void)
{
	return var80091558;
}
