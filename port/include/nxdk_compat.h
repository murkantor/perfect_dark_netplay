#ifndef NXDK_COMPAT_H
#define NXDK_COMPAT_H

// Small libc shims for gaps in NXDK's pdclib. This header is FORCE-INCLUDED into
// every translation unit of the Original Xbox build (CMakeLists.txt adds it via
// `-include` under XBOX_NXDK), so the shims are visible everywhere without editing
// individual port/game files. Keep each shim tiny, dependency-free, and `static
// inline` (no link-time duplication). Add new entries here as further NXDK libc
// gaps surface during bring-up. See docs/PORT_XBOX_NXDK.md.

#ifdef NXDK

#include <stddef.h>
#include <ctype.h>
#include <stdio.h>

// MSVC "secure CRT" fopen_s: vendored libs (e.g. stb_image) select it because
// NXDK's clang triple defines _MSC_VER, but pdclib has no fopen_s. Wrap fopen;
// stb checks `0 != fopen_s(...)`, so return 0 on success and non-zero on failure.
static inline int fopen_s(FILE **f, const char *name, const char *mode) {
	if (f == NULL) {
		return 1;
	}
	*f = fopen(name, mode);
	return (*f != NULL) ? 0 : 1;
}

// POSIX <strings.h> case-insensitive compares: pdclib doesn't provide them.
static inline int strncasecmp(const char *a, const char *b, size_t n) {
	while (n-- != 0) {
		int ca = tolower((unsigned char)*a++);
		int cb = tolower((unsigned char)*b++);
		if (ca != cb) {
			return ca - cb;
		}
		if (ca == 0) {
			break;
		}
	}
	return 0;
}

static inline int strcasecmp(const char *a, const char *b) {
	int ca, cb;
	do {
		ca = tolower((unsigned char)*a++);
		cb = tolower((unsigned char)*b++);
	} while (ca == cb && ca != 0);
	return ca - cb;
}

#endif /* NXDK */

#endif /* NXDK_COMPAT_H */
