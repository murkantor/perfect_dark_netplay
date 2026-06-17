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
# DESIGN: rather than re-derive clang's Xbox target triple / freestanding flags /
# include roots by hand (and drift from whatever NXDK actually does), this file
# drives NXDK's own `nxdk-cc` / `nxdk-cxx` / `nxdk-as` wrapper scripts as the CMake
# compilers. Those wrappers already bake in the verified invocation:
#
#   clang -target i386-pc-win32 -march=pentium3 -fuse-ld=nxdk-link \
#         -ffreestanding -nostdlib -fno-builtin \
#         -I$NXDK_DIR/lib ... -isystem $NXDK_DIR/lib/pdclib/include ... \
#         -DNXDK -D__STDC__=1 -U__STDC_NO_THREADS__
#
# so the project build matches the (known-good) `nxdk-cc`-driven triangle/sample
# builds exactly. The wrappers expand ${NXDK_DIR} internally, so NXDK_DIR MUST be
# exported in the environment (tools/buildscripts/xbox_nxdk.sh does this).

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
# NXDK ships wrapper scripts in $NXDK_DIR/bin that invoke clang/clang++ with the
# full Xbox flag set (target triple, -ffreestanding, -nostdlib, the nxdk/pdclib/
# winapi include roots, -DNXDK, the nxdk-link linker, ...). Use them verbatim so the
# project build is byte-for-byte the same invocation as the verified sample builds.
find_program(NXDK_CC  NAMES nxdk-cc  PATHS "${NXDK_DIR}/bin" NO_DEFAULT_PATH)
find_program(NXDK_CXX NAMES nxdk-cxx PATHS "${NXDK_DIR}/bin" NO_DEFAULT_PATH)
find_program(NXDK_AS  NAMES nxdk-as  PATHS "${NXDK_DIR}/bin" NO_DEFAULT_PATH)
find_program(NXDK_LIB NAMES nxdk-lib PATHS "${NXDK_DIR}/bin" NO_DEFAULT_PATH)
if(NOT NXDK_CC OR NOT NXDK_CXX)
  message(FATAL_ERROR
    "toolchain-nxdk.cmake: nxdk-cc / nxdk-cxx not found in ${NXDK_DIR}/bin -- "
    "bootstrap NXDK (run its 'make' once) so the wrapper scripts are generated.")
endif()
set(CMAKE_C_COMPILER   "${NXDK_CC}")
set(CMAKE_CXX_COMPILER "${NXDK_CXX}")
if(NXDK_AS)
  set(CMAKE_ASM_COMPILER "${NXDK_AS}")
endif()

# clang underneath these wrappers targets i386-pc-win32; help CMake's compiler-id
# step pick clang (the wrapper has no -- version banner of its own).
set(CMAKE_C_COMPILER_ID   Clang)
set(CMAKE_CXX_COMPILER_ID Clang)

# NXDK's link (nxdk-link == lld-link) emits a PE .exe (the sample builds produce
# main.exe). CMAKE_SYSTEM_NAME Generic would otherwise give an empty suffix; pin it
# to .exe so the link output matches the ${BIN_NAME}.exe the cxbe POST_BUILD step in
# CMakeLists.txt feeds into the PE->XBE conversion.
set(CMAKE_EXECUTABLE_SUFFIX     ".exe")
set(CMAKE_EXECUTABLE_SUFFIX_C   ".exe")
set(CMAKE_EXECUTABLE_SUFFIX_CXX ".exe")

# nxdk-lib drives llvm-lib for static archives; the link of the final .exe goes
# through the compiler driver (the wrappers carry -fuse-ld=nxdk-link), so we do NOT
# set a separate CMAKE_LINKER here.
if(NXDK_LIB)
  set(CMAKE_AR "${NXDK_LIB}" CACHE FILEPATH "NXDK static archiver (llvm-lib)" FORCE)
endif()

# The wrappers already carry every required flag; keep our additions empty so we
# don't shadow or duplicate them. Per-target include/define needs (SDL3, ROM defs)
# are added by CMakeLists.txt's XBOX_NXDK branch, not here.
set(CMAKE_C_FLAGS_INIT   "")
set(CMAKE_CXX_FLAGS_INIT "")

# Don't try to run/link test executables during compiler detection (no host runtime).
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Search behaviour: only look inside the NXDK tree for libs/headers/packages.
set(CMAKE_FIND_ROOT_PATH "${NXDK_DIR}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# The final PE->XBE conversion is a POST_BUILD step in CMakeLists.txt. cxbe lives in
# the NXDK tools tree; expose its path so CMakeLists.txt can invoke it.
find_program(NXDK_CXBE NAMES cxbe PATHS "${NXDK_DIR}/tools/cxbe" "${NXDK_DIR}/bin" NO_DEFAULT_PATH)
if(NXDK_CXBE)
  set(CXBE "${NXDK_CXBE}" CACHE FILEPATH "NXDK PE->XBE tool")
endif()
