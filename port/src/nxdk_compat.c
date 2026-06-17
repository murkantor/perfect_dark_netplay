// Real-symbol libc shims for the Original Xbox (NXDK) build: functions NXDK's
// pdclib *declares* (in its standard headers) but does not *define*, so they show
// up as undefined symbols at link. These can't live in the force-included
// port/include/nxdk_compat.h as `static inline` because that clashes with the
// pdclib prototypes -- they need to be real, externally-visible definitions, so
// they go here in one translation unit. Add more as link-time gaps appear.
//
// This whole file is empty on non-NXDK builds (it is auto-globbed into the port
// source list); the NXDK guard keeps it inert everywhere else.

#ifdef NXDK

#include <stdlib.h>

// pdclib provides strtod() but not atof(); the port (demo.c, net.c console
// commands) uses atof. Standard definition in terms of strtod.
double atof(const char *nptr)
{
	return strtod(nptr, NULL);
}

#endif // NXDK
