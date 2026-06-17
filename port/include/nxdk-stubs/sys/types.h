#ifndef _NXDK_STUB_SYS_TYPES_H
#define _NXDK_STUB_SYS_TYPES_H

// Minimal <sys/types.h> for the Original Xbox (NXDK) build. NXDK's pdclib has no
// <sys/types.h>, but vendored headers reach for it -- e.g. zlib's zconf.h wants
// off_t. This stub lives on the XBOX_NXDK include path (see CMakeLists.txt) so
// those headers compile. Keep it minimal; extend only as concrete needs appear.
//
// (Duplicate identical typedefs are permitted in C11, so this co-exists with any
// pdclib definitions of the same types.)

#include <stddef.h>
#include <stdint.h>

typedef long off_t;

#endif // _NXDK_STUB_SYS_TYPES_H
