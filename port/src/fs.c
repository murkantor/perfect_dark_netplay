#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <ctype.h>
#include <unistd.h>
#ifndef NXDK
#include <sys/stat.h> // NXDK's pdclib has no <sys/stat.h>; fs uses fopen/ftell instead
#endif
#include <stdbool.h>
#include <PR/ultratypes.h>
#include "constants.h"
#include "config.h"
#include "system.h"
#include "platform.h"
#include "utils.h"
#include "fs.h"
#ifdef PLATFORM_WIN32
#include <direct.h>
#endif

#ifdef NXDK
// Temporary boot-bring-up tracing: the ROM load path is invisible via sysLogPrintf
// on NXDK (stdout/console not surfaced during boot), so trace the resolved fopen
// path + result to the debug overlay. Remove once boot is solid.
#include <hal/debug.h>
#define NXDK_FS_TRACE(...) debugPrint(__VA_ARGS__)

// For the boot-time memory readout before the big ROM alloc. Declared extern to
// avoid pulling video.h (and SDL) into fs.c.
extern void videoGetMemoryUsage(u32 *used, u32 *total);

// NXDK's file API (kernel CreateFile) requires BACKSLASH path separators and chokes
// on the forward slashes our path code builds (e.g. "D:/data/pd.ntsc-final.z64" hung
// the box mid-fopen instead of opening or cleanly failing). Translate '/'->'\' at the
// OS open boundary. The result lives in a private static buffer, distinct from
// fsFullPath's, so fopen(fsOsPath(fsFullPath(x))) is safe.
static const char *fsOsPath(const char *path)
{
	static char osbuf[FS_MAXPATH + 1];
	u32 i = 0;
	for (; path[i] && i < FS_MAXPATH; ++i) {
		osbuf[i] = (path[i] == '/') ? '\\' : path[i];
	}
	osbuf[i] = '\0';
	return osbuf;
}
#else
#define NXDK_FS_TRACE(...) ((void)0)
#define fsOsPath(p) (p)
#endif

#define DEFAULT_BASEDIR_NAME "data"

static char baseDir[FS_MAXPATH + 1]; // replaces $B
static char modDir[FS_MAXPATH + 1];  // replaces $M
static char saveDir[FS_MAXPATH + 1]; // replaces $S
static char homeDir[FS_MAXPATH + 1]; // replaces $H
static char exeDir[FS_MAXPATH + 1];  // replaces $E

static char gexModDir[FS_MAXPATH + 1];          // GoldenEye X Mod
static char kakarikoModDir[FS_MAXPATH + 1];     // Kakariko Village Mod
static char darknoonModDir[FS_MAXPATH + 1];     // Dark Moon Mod
static char goldfinger64ModDir[FS_MAXPATH + 1]; // Goldfinger 64 Mod

u32 g_ModNum = 0;

static s32 fsPathIsWritable(const char *path)
{
#if defined(PLATFORM_WIN32) || defined(NXDK)
	// on windows access() on directories will only check if the directory exists, so
	// (NXDK has no access()/W_OK either; probe by trying to create a temp file).
	char tmp[FS_MAXPATH + 1] = { 0 };
	snprintf(tmp, sizeof(tmp), "%s/.tmp", path);
	FILE *f = fopen(fsOsPath(tmp), "wb");
	if (f) {
		fclose(f);
		remove(fsOsPath(tmp));
		return 1;
	}
	return 0;
#else
	return (access(path, W_OK) == 0);
#endif
}

s32 fsPathIsAbsolute(const char *path)
{
 return (path[0] == '/' || (isalpha(path[0]) && path[1] == ':'));
}

s32 fsPathIsCwdRelative(const char *path)
{
	// ., .., ./, ../
	return (path[0] == '.' && (path[1] == '.' || path[1] == '/' || path[1] == '\\' || path[1] == '\0'));
}

const char *fsFullPath(const char *relPath)
{
	static char pathBuf[FS_MAXPATH + 1];

	if (relPath[0] == '$') {
		// expandable placeholder $X; will be replaced with the corresponding path, if any
		const char *expStr = NULL;
		switch (relPath[1]) {
			case 'E': expStr = exeDir; break;
			case 'H': expStr = homeDir; break;
			case 'M': expStr = modDir; break;
			case 'B': expStr = baseDir; break;
			case 'S': expStr = saveDir; break;
			default: break;
		}
		if (expStr) {
			const u32 len = strlen(expStr);
			if (len > 0) {
				memcpy(pathBuf, expStr, len);
				strncpy(pathBuf + len, relPath + 2, FS_MAXPATH - len);
				return pathBuf;
			}
		}
		// couldn't expand anything, return as is
		return relPath;
	} else if (!baseDir[0] || fsPathIsAbsolute(relPath) || fsPathIsCwdRelative(relPath)) {
		// user explicitly wants working directory or this is an absolute path or we have no baseDir set up yet
		return relPath;
	}

	// path relative to mod or base dir; this will be a read request, so check where the file actually is
	if (gexModDir[0] && g_ModNum == MOD_GEX) {
		snprintf(pathBuf, FS_MAXPATH, "%s/%s", gexModDir, relPath);
		if (fsFileSize(pathBuf) >= 0) {
			return pathBuf;
		}
	} else if (kakarikoModDir[0] && g_ModNum == MOD_KAKARIKO) {
		snprintf(pathBuf, FS_MAXPATH, "%s/%s", kakarikoModDir, relPath);
		if (fsFileSize(pathBuf) >= 0) {
			return pathBuf;
		}
	} else if (darknoonModDir[0] && g_ModNum == MOD_DARKNOON) {
		snprintf(pathBuf, FS_MAXPATH, "%s/%s", darknoonModDir, relPath);
		if (fsFileSize(pathBuf) >= 0) {
			return pathBuf;
		}
	} else if (goldfinger64ModDir[0] && g_ModNum == MOD_GOLDFINGER_64) {
		snprintf(pathBuf, FS_MAXPATH, "%s/%s", goldfinger64ModDir, relPath);
		if (fsFileSize(pathBuf) >= 0) {
			return pathBuf;
		}
	} else if (modDir[0]) {
		snprintf(pathBuf, FS_MAXPATH, "%s/%s", modDir, relPath);
		if (fsFileSize(pathBuf) >= 0) {
			return pathBuf;
		}
	}

	// fall back to basedir
	snprintf(pathBuf, FS_MAXPATH, "%s/%s", baseDir, relPath);
	return pathBuf;
}

s32 fsInit(void)
{
	sysGetExecutablePath(exeDir, FS_MAXPATH);

	// if this is set, default to exe path for everything
	const s32 portable = sysArgCheck("--portable");
	if (portable) {
		strcpy(homeDir, exeDir);
	} else {
		sysGetHomePath(homeDir, FS_MAXPATH);
	}

	// get path to base dir and expand it if needed
	const char *path = sysArgGetString("--basedir");
	if (!path) {
		// check if there's a `data` directory in working directory or homeDir, otherwise default to exe directory
		path = "$E/" DEFAULT_BASEDIR_NAME;
		if (!portable) {
			if (fsFileSize("./" DEFAULT_BASEDIR_NAME) >= 0) {
				path = "./" DEFAULT_BASEDIR_NAME;
			} else if (fsFileSize("$H/" DEFAULT_BASEDIR_NAME) >= 0) {
				path = "$H/" DEFAULT_BASEDIR_NAME;
			}
		}
	}
	strncpy(baseDir, fsFullPath(path), FS_MAXPATH);

	// get path to mod dir and expand it if needed
	// mod directory is overlaid on top of base directory
	path = sysArgGetString("--moddir");
	if (path) {
		if (fsPathIsAbsolute(path) || fsPathIsCwdRelative(path) || path[0] == '$') {
			// path is explicit; check as-is
			if (fsFileSize(path) >= 0) {
				strncpy(modDir, fsFullPath(path), FS_MAXPATH);
			}
		} else {
			// path is relative to workdir; try to find it
			const char *priority[] = { ".", "$E", "$H" };
			for (s32 i = 0; i < 2 + (portable != 0); ++i) {
				char *tmp = strFmt("%s/%s", priority[i], path);
				if (fsFileSize(tmp) >= 0) {
					strncpy(modDir, fsFullPath(tmp), FS_MAXPATH);
					break;
				}
			}
		}
		if (!modDir[0]) {
			sysLogPrintf(LOG_WARNING, "could not find specified moddir `%s`", path);
		}
	}

	// GoldenEye X Mod Dir
	path = sysArgGetString("--gexmoddir");
	if (path) {
		if (fsPathIsAbsolute(path) || fsPathIsCwdRelative(path) || path[0] == '$') {
			// path is explicit; check as-is
			if (fsFileSize(path) >= 0) {
				strncpy(gexModDir, fsFullPath(path), FS_MAXPATH);
			}
		} else {
			// path is relative to workdir; try to find it
			const char *priority[] = { ".", "$E", "$H" };
			for (s32 i = 0; i < 2 + (portable != 0); ++i) {
				char *tmp = strFmt("%s/%s", priority[i], path);
				if (fsFileSize(tmp) >= 0) {
					strncpy(gexModDir, fsFullPath(tmp), FS_MAXPATH);
					break;
				}
			}
		}
		if (!gexModDir[0]) {
			sysLogPrintf(LOG_WARNING, "could not find specified gexmoddir `%s`", path);
		}
	}

	// Kakariko Village Mod Dir
	path = sysArgGetString("--kakarikomoddir");
	if (path) {
		if (fsPathIsAbsolute(path) || fsPathIsCwdRelative(path) || path[0] == '$') {
			// path is explicit; check as-is
			if (fsFileSize(path) >= 0) {
				strncpy(kakarikoModDir, fsFullPath(path), FS_MAXPATH);
			}
		} else {
			// path is relative to workdir; try to find it
			const char *priority[] = { ".", "$E", "$H" };
			for (s32 i = 0; i < 2 + (portable != 0); ++i) {
				char *tmp = strFmt("%s/%s", priority[i], path);
				if (fsFileSize(tmp) >= 0) {
					strncpy(kakarikoModDir, fsFullPath(tmp), FS_MAXPATH);
					break;
				}
			}
		}
		if (!kakarikoModDir[0]) {
			sysLogPrintf(LOG_WARNING, "could not find specified kakarikomoddir `%s`", path);
		}
	}

	// Dark Moon Mod Dir
	path = sysArgGetString("--darknoonmoddir");
	if (path) {
		if (fsPathIsAbsolute(path) || fsPathIsCwdRelative(path) || path[0] == '$') {
			// path is explicit; check as-is
			if (fsFileSize(path) >= 0) {
				strncpy(darknoonModDir, fsFullPath(path), FS_MAXPATH);
			}
		} else {
			// path is relative to workdir; try to find it
			const char *priority[] = { ".", "$E", "$H" };
			for (s32 i = 0; i < 2 + (portable != 0); ++i) {
				char *tmp = strFmt("%s/%s", priority[i], path);
				if (fsFileSize(tmp) >= 0) {
					strncpy(darknoonModDir, fsFullPath(tmp), FS_MAXPATH);
					break;
				}
			}
		}
		if (!darknoonModDir[0]) {
			sysLogPrintf(LOG_WARNING, "could not find specified darknoonmoddir `%s`", path);
		}
	}

	// Goldfinger 64 Mod Dir
	path = sysArgGetString("--goldfinger64moddir");
	if (path) {
		if (fsPathIsAbsolute(path) || fsPathIsCwdRelative(path) || path[0] == '$') {
			// path is explicit; check as-is
			if (fsFileSize(path) >= 0) {
				strncpy(goldfinger64ModDir, fsFullPath(path), FS_MAXPATH);
			}
		} else {
			// path is relative to workdir; try to find it
			const char *priority[] = { ".", "$E", "$H" };
			for (s32 i = 0; i < 2 + (portable != 0); ++i) {
				char *tmp = strFmt("%s/%s", priority[i], path);
				if (fsFileSize(tmp) >= 0) {
					strncpy(goldfinger64ModDir, fsFullPath(tmp), FS_MAXPATH);
					break;
				}
			}
		}
		if (!goldfinger64ModDir[0]) {
			sysLogPrintf(LOG_WARNING, "could not find specified goldfinger64moddir `%s`", path);
		}
	}

	// get path to save dir and expand it if needed
	path = sysArgGetString("--savedir");
	if (!path) {
		if (portable) {
			path = "$E";
		} else {
#if defined(PLATFORM_LINUX) || defined(PLATFORM_OSX)
			// check if there's a config in the working directory, otherwise default to homeDir
			if (fsFileSize("./" CONFIG_FNAME) >= 0) {
				path = ".";
			} else {
				path = "$H";
			}
#else
			// check if working directory is writable, otherwise default to homeDir
			if (fsPathIsWritable("./")) {
				path = ".";
			} else {
				sysLogPrintf(LOG_WARNING, "cannot write to working directory, will use %s for saves instead", homeDir);
				path = "$H";
			}
#endif
		}
	}

	strncpy(saveDir, fsFullPath(path), FS_MAXPATH);

	if (modDir[0]) {
		sysLogPrintf(LOG_NOTE, " mod dir: %s", modDir);
	}
	if (gexModDir[0]) {
		sysLogPrintf(LOG_NOTE, " gex mod dir: %s", gexModDir);
	}
	if (kakarikoModDir[0]) {
		sysLogPrintf(LOG_NOTE, " kakariko mod dir: %s", kakarikoModDir);
	}
	if (darknoonModDir[0]) {
		sysLogPrintf(LOG_NOTE, " darknoon mod dir: %s", darknoonModDir);
	}
	if (goldfinger64ModDir[0]) {
		sysLogPrintf(LOG_NOTE, " goldfinger64 mod dir: %s", goldfinger64ModDir);
	}
	sysLogPrintf(LOG_NOTE, "base dir: %s", baseDir);
	sysLogPrintf(LOG_NOTE, "save dir: %s", saveDir);

	return 0;
}

const char *fsGetModDir(void)
{
	if (g_ModNum == MOD_GEX) {
		return gexModDir[0] ? gexModDir : NULL;
	} else if (g_ModNum == MOD_KAKARIKO) {
		return kakarikoModDir[0] ? kakarikoModDir : NULL;
	} else if (g_ModNum == MOD_DARKNOON) {
		return darknoonModDir[0] ? darknoonModDir : NULL;
	} else if (g_ModNum == MOD_GOLDFINGER_64) {
		return goldfinger64ModDir[0] ? goldfinger64ModDir : NULL;
	} else {
		return modDir[0] ? modDir : NULL;
	}
}

s32 fsFileLoadTo(const char *name, void *dst, u32 dstSize)
{
	const char *fullName = fsFullPath(name);

	FILE *f = fopen(fsOsPath(fullName), "rb");
	if (!f) {
		return -1;
	}

	fseek(f, 0, SEEK_END);
	const s32 size = ftell(f);
	fseek(f, 0, SEEK_SET);

	if (size < 0) {
		sysLogPrintf(LOG_ERROR, "fsFileLoadTo: empty file or invalid size (%d): %s", size, fullName);
		fclose(f);
		return -1;
	}

	if ((u32)size > dstSize) {
		sysLogPrintf(LOG_ERROR, "fsFileLoadTo: file too big for buffer (%u > %u): %s", size, dstSize, fullName);
		fclose(f);
		return -1;
	}

	fread(dst, 1, size, f);
	fclose(f);

	return size;
}

void *fsFileLoad(const char *name, u32 *outSize)
{
	const char *fullName = fsFullPath(name);

	NXDK_FS_TRACE("PDBOOT: fsFileLoad open '%s'\n", fsOsPath(fullName));

	FILE *f = fopen(fsOsPath(fullName), "rb");
	if (!f) {
		NXDK_FS_TRACE("PDBOOT: fsFileLoad fopen FAILED '%s'\n", fsOsPath(fullName));
		sysLogPrintf(LOG_ERROR, "fsFileLoad: could not find file: %s", fullName);
		return NULL;
	}

	NXDK_FS_TRACE("PDBOOT: fsFileLoad fopen ok, seeking\n");

	fseek(f, 0, SEEK_END);
	const s32 size = ftell(f);
	fseek(f, 0, SEEK_SET);

	NXDK_FS_TRACE("PDBOOT: fsFileLoad size %d, allocating\n", size);

	if (size < 0) {
		sysLogPrintf(LOG_ERROR, "fsFileLoad: empty file or invalid size (%d): %s", size, fullName);
		fclose(f);
		return NULL;
	}

	void *buf = NULL;
	if (size) {
#ifdef NXDK
		// calloc(size+1) hung on the 32 MB ROM despite 45 MB free: the issue isn't an
		// OOM but the 32 MB zero-fill (slow/faulting on NV2A). fread overwrites the
		// whole buffer anyway, so use a plain malloc and set just the trailing null
		// byte (the "free null terminator" the calloc was for). Trace right after the
		// malloc so a malloc-hang is distinguishable from a fill/read hang.
		buf = sysMemAlloc(size + 1);
		if (!buf) {
			NXDK_FS_TRACE("PDBOOT: fsFileLoad malloc FAILED (%d bytes)\n", size + 1);
			sysLogPrintf(LOG_ERROR, "fsFileLoad: could not alloc %d bytes for file: %s", size, fullName);
			fclose(f);
			return NULL;
		}
		((u8 *)buf)[size] = '\0';
		NXDK_FS_TRACE("PDBOOT: fsFileLoad malloc ok, reading %d bytes\n", size);
		fread(buf, 1, size, f);
		NXDK_FS_TRACE("PDBOOT: fsFileLoad read complete\n");
#else
		buf = sysMemZeroAlloc(size + 1); // sick hack for a free null terminator
		if (!buf) {
			sysLogPrintf(LOG_ERROR, "fsFileLoad: could not alloc %d bytes for file: %s", size, fullName);
			fclose(f);
			return NULL;
		}
		fread(buf, 1, size, f);
#endif
	}

	fclose(f);

	NXDK_FS_TRACE("PDBOOT: fsFileLoad done '%s' (%d bytes)\n", fullName, size);

	if (outSize) {
		*outSize = size;
	}

	return buf;
}

s32 fsFileSize(const char *name)
{
	const char *fullName = fsFullPath(name);
#ifdef NXDK
	// NXDK has no stat(); size via open + seek-to-end.
	FILE *f = fopen(fsOsPath(fullName), "rb");
	if (!f) {
		return -1;
	}
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fclose(f);
	return (sz < 0) ? -1 : (s32)sz;
#else
	struct stat st;
	if (stat(fullName, &st) < 0) {
		return -1;
	} else {
		return st.st_size;
	}
#endif
}

FILE *fsFileOpenWrite(const char *name)
{
	return fopen(fsOsPath(fsFullPath(name)), "wb");
}

FILE *fsFileOpenRead(const char *name)
{
	return fopen(fsOsPath(fsFullPath(name)), "rb");
}

void fsFileFree(FILE *f)
{
	// NULL guard: callers pass the result of a failed open straight in
	// (e.g. mpsetupOpenFile's create-if-missing path when the save dir is
	// bad) and glibc fclose(NULL) is a SIGSEGV, not an EOF error.
	if (f) {
		fclose(f);
	}
}

s32 fsCreateDir(const char *path)
{
#if defined(NXDK)
	// TODO(nxdk): real FATX directory creation (CreateDirectoryA). pdclib has no
	// mkdir(); stub success for now so save-dir setup doesn't fail the boot path.
	// Saves won't actually persist until this is implemented -- see
	// docs/PORT_XBOX_NXDK.md (filesystem).
	(void)path;
	return 0;
#elif defined(PLATFORM_WIN32)
	return _mkdir(fsFullPath(path));
#else
	return mkdir(fsFullPath(path), 0777);
#endif
}
