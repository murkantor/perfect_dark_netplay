# CMake toolchain file for the Original Xbox via NXDK (https://github.com/XboxDev/nxdk)
# with SDL3 from https://github.com/Ryzee119/nxdk-sdl3.
#
# Usage:
#   export NXDK_DIR=/path/to/nxdk        # your NXDK checkout (must be `make` bootstrapped)
#   cmake -G "Unix Makefiles" \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-nxdk.cmake \
#         -DROMID=ntsc-final -B build_xbox .
#   cmake --build build_xbox
#
# STATUS: scaffolding skeleton. NXDK natively builds via its own GNU-Make rules, so
# the exact clang target triple, flag set, lib search paths and the PE->XBE step
# must be confirmed against your NXDK install (and Ryzee119's CMake setup, if any).
# Every value below sourced from $ENV{NXDK_DIR} is a starting point -- see the
# "Milestone 0 / NXDK <-> CMake integration" and "packaging" notes in
# docs/PORT_XBOX_NXDK.md and fill in / correct as you bring it up on hardware/xemu.

if(NOT DEFINED ENV{NXDK_DIR})
  message(FATAL_ERROR "toolchain-nxdk.cmake: set the NXDK_DIR environment variable to your NXDK checkout")
endif()
set(NXDK_DIR "$ENV{NXDK_DIR}")

# The OG Xbox is a 32-bit x86 (Pentium III), little-endian. NXDK's clang targets a
# win32-like triple (PE/XBE), which is why src/include/platform.h checks `NXDK`
# BEFORE `_WIN32`, and CMakeLists.txt lists XBOX_NXDK first in every if/elseif.
# We use a generic system name so CMake does not enable its host/Windows logic.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR i386)

# Discriminator read all over CMakeLists.txt.
set(XBOX_NXDK TRUE CACHE BOOL "Building for the Original Xbox via NXDK" FORCE)

# --- Compilers -------------------------------------------------------------------
# NXDK builds with clang/clang++ + lld. Confirm the wrapper/flags NXDK expects;
# the canonical reference is $NXDK_DIR/Makefile (CFLAGS/CXXFLAGS/NXDK_CFLAGS).
find_program(NXDK_CC  NAMES clang)
find_program(NXDK_CXX NAMES clang++)
find_program(NXDK_LD  NAMES lld-link ld.lld lld)
if(NOT NXDK_CC OR NOT NXDK_CXX)
  message(FATAL_ERROR "toolchain-nxdk.cmake: clang/clang++ not found (NXDK uses the LLVM toolchain)")
endif()
set(CMAKE_C_COMPILER   "${NXDK_CC}")
set(CMAKE_CXX_COMPILER "${NXDK_CXX}")

# NXDK target + freestanding flags. THESE ARE PLACEHOLDERS to reconcile with the
# NXDK Makefile (target triple, -ffreestanding, -nostdlib, the nxdk/pdclib include
# roots, -D NXDK, the XBE entry, etc.). Treat as the first thing to fix on bring-up.
set(NXDK_TARGET_TRIPLE "i386-pc-win32")
set(NXDK_COMMON_FLAGS
  "--target=${NXDK_TARGET_TRIPLE} -march=pentium3 -fno-builtin -DNXDK"
  "-I${NXDK_DIR}/lib -I${NXDK_DIR}/lib/pdclib/include -I${NXDK_DIR}/lib/winapi"
  "-I${NXDK_DIR}/lib/xboxrt/include -I${NXDK_DIR}/lib/sdl/SDL3/include")
string(REPLACE ";" " " NXDK_COMMON_FLAGS "${NXDK_COMMON_FLAGS}")

set(CMAKE_C_FLAGS_INIT   "${NXDK_COMMON_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${NXDK_COMMON_FLAGS}")

# Don't try to run/link test executables during compiler detection (no host runtime).
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Search behaviour: only look inside the NXDK tree for libs/headers/packages.
set(CMAKE_FIND_ROOT_PATH "${NXDK_DIR}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# The final PE->XBE conversion is a POST_BUILD step in CMakeLists.txt (cxbe). Point
# CXBE here if it isn't at $NXDK_DIR/tools/cxbe/cxbe.
# set(CXBE "${NXDK_DIR}/tools/cxbe/cxbe" CACHE FILEPATH "NXDK PE->XBE tool")
