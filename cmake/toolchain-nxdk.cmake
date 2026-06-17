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
# DESIGN: we do NOT re-derive the Xbox compiler / archiver / linker setup by hand.
# NXDK ships its OWN CMake toolchain at $NXDK_DIR/share/toolchain-nxdk.cmake (this is
# exactly what the `nxdk-cmake` wrapper drives), and it is the authoritative source:
# it points CC/CXX at nxdk-cc/nxdk-cxx, archives static libs with llvm-ar
# (CMAKE_C_COMPILER_AR -- NOT nxdk-lib/llvm-lib, which is lib.exe-style and breaks
# CMake's GNU `ar qc ...` invocation), links the .exe via a custom nxdk-link rule
# plus the standard Xbox libs, sets the .lib/.exe suffixes, CMAKE_SYSROOT, and
# WIN32/NXDK. We just include() it and layer our own bits on top:
#   * XBOX_NXDK  -- the discriminator read all over CMakeLists.txt
#   * cxbe       -- located for the PE->XBE POST_BUILD step
#
# Note: NXDK's toolchain sets WIN32=1 (its clang triple is win32-like). That is why
# CMakeLists.txt lists every XBOX_NXDK branch BEFORE its WIN32 branch, so XBOX_NXDK
# wins. NXDK_DIR must be exported (the nxdk-cc/cxx wrappers expand it per compile).

if(NOT DEFINED ENV{NXDK_DIR})
  message(FATAL_ERROR "toolchain-nxdk.cmake: set the NXDK_DIR environment variable to your NXDK checkout")
endif()
set(NXDK_DIR "$ENV{NXDK_DIR}")

set(_NXDK_TOOLCHAIN "${NXDK_DIR}/share/toolchain-nxdk.cmake")
if(NOT EXISTS "${_NXDK_TOOLCHAIN}")
  message(FATAL_ERROR
    "toolchain-nxdk.cmake: NXDK's own toolchain not found at ${_NXDK_TOOLCHAIN} -- "
    "bootstrap NXDK (run its 'make' once) so share/toolchain-nxdk.cmake exists.")
endif()

# Authoritative NXDK compiler/archiver/linker/suffix/sysroot setup.
include("${_NXDK_TOOLCHAIN}")

# Our discriminator, read all over CMakeLists.txt.
set(XBOX_NXDK TRUE CACHE BOOL "Building for the Original Xbox via NXDK" FORCE)

# Don't try to run/link a full test executable during compiler detection (no host
# runtime). NXDK's archiver (llvm-ar) makes the static-library probe succeed.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# The final PE->XBE conversion is a POST_BUILD step in CMakeLists.txt. cxbe lives in
# the NXDK tools tree; expose its path so CMakeLists.txt can invoke it.
find_program(NXDK_CXBE NAMES cxbe PATHS "${NXDK_DIR}/tools/cxbe" "${NXDK_DIR}/bin" NO_DEFAULT_PATH)
if(NXDK_CXBE)
  set(CXBE "${NXDK_CXBE}" CACHE FILEPATH "NXDK PE->XBE tool")
endif()
