cmake_minimum_required(VERSION 3.16)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)

if(APPLE)
  if(CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
    # ARM Macs start at OS 11.
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum OS X deployment version")
  else()
    # SDL3 requires at least macOS 10.13.
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Minimum OS X deployment version")
  endif()
endif()

project(pd C CXX)

include(TargetArch)
include(FindPkgConfig)
include(AssetUtils)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# On first build set the build type to RelWithDebInfo if it's not already set
if (NOT EXISTS ${CMAKE_BINARY_DIR}/CMakeCache.txt)
  if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "" FORCE)
  endif()
endif()

# Options

set(ROMID "ntsc-final" CACHE STRING "Which ROM to build an executable for: ntsc-final, pal-final or jpn-final")
option(FORCE_CROSSCOMPILE "Force cross compiling" NO)
option(DEDICATED_SERVER "Build a headless dedicated server (no SDL/OpenGL/audio/input; never a client)" OFF)
option(USE_SDLGPU "Build the SDL_GPU (Vulkan) rendering backend in addition to OpenGL (desktop clients only; needs glslang)" ON)
# Static shader-toolchain link defaults ON for MinGW/Windows so a release
# build only needs SDL3/zlib1/libgcc/libwinpthread next to the exe (MSYS2
# ships the .a archives). Other platforms default OFF — distro static
# glslang/SPIRV archives aren't guaranteed, and the DLL-shipping problem this
# solves is Windows-specific. Override either way with -DUSE_SDLGPU_STATIC=.
if(MINGW)
  set(USE_SDLGPU_STATIC_DEFAULT ON)
else()
  set(USE_SDLGPU_STATIC_DEFAULT OFF)
endif()
option(USE_SDLGPU_STATIC "Link the SDL_GPU shader toolchain (glslang/SPIRV-Tools/spirv-cross) statically so no extra runtime DLLs are needed" ${USE_SDLGPU_STATIC_DEFAULT})

# Determine target arch

target_architecture(TARGET_ARCH)
if(TARGET_ARCH STREQUAL "i386")
  # for historical reasons
  set(TARGET_ARCH "i686")
endif()

# TODO
set(TARGET_IS_BIG_ENDIAN FALSE)

if((TARGET_ARCH MATCHES "unknown") OR (TARGET_ARCH STREQUAL ""))
  message(FATAL_ERROR "Unknown target arch")
elseif(TARGET_ARCH MATCHES "64")
  set(TARGET_IS_64BIT TRUE)
else()
  set(TARGET_IS_64BIT FALSE)
endif()

message(STATUS "Target arch: ${TARGET_ARCH}")

# Determine target OS and platform identifier ($arch-$os)

if(XBOX_NXDK)
  # Original Xbox via the NXDK toolchain. MUST come before WIN32: NXDK's clang
  # targets a win32-like triple, so CMake's WIN32 may also be set -- listing
  # XBOX_NXDK first makes it win every if/elseif chain below. See the new
  # cmake/toolchain-nxdk.cmake (which sets XBOX_NXDK) and docs/PORT_XBOX_NXDK.md.
  set(TARGET_OS "xbox-nxdk")
elseif(WIN32)
  # Windows
  set(TARGET_OS "windows")
elseif(APPLE)
  # OSX
  set(TARGET_OS "osx")
elseif(NINTENDO_SWITCH)
  # Switch
  set(TARGET_OS "nswitch")
elseif(UNIX)
  # Generic *nix; assume Linux
  set(TARGET_OS "linux")
else()
  # ???
  message(FATAL_ERROR "Unknown target OS or compiler")
endif()

set(TARGET_PLATFORM "${TARGET_ARCH}-${TARGET_OS}")

message(STATUS "Target platform: ${TARGET_PLATFORM}")

# Calculate version info and binary name

set(VERSION_IS_PAL 0)

if(ROMID STREQUAL "ntsc-final")
  set(VERSION 2)
  set(BIN_REGION "")
elseif(ROMID STREQUAL "pal-final")
  set(VERSION 4)
  set(BIN_REGION ".pal")
  set(VERSION_IS_PAL 1)
elseif(ROMID STREQUAL "jpn-final")
  set(VERSION 5)
  set(BIN_REGION ".jpn")
else()
  message(FATAL_ERROR "Unknown ROM ID: ${ROMID}")
endif()

# git commit hash
execute_process(
  COMMAND git rev-parse --short HEAD
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  OUTPUT_VARIABLE VERSION_HASH
  OUTPUT_STRIP_TRAILING_WHITESPACE
)

# git branch name
execute_process(
  COMMAND git rev-parse --abbrev-ref HEAD
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  OUTPUT_VARIABLE VERSION_BRANCH
  OUTPUT_STRIP_TRAILING_WHITESPACE
)

set(VERSION_TARGET "${TARGET_PLATFORM}")
set(VERSION_ARCH "${TARGET_ARCH}")
set(VERSION_ROMID "${ROMID}")
set(VERSION_BUILD "${CMAKE_BUILD_TYPE}")

if((VERSION_BRANCH STREQUAL "") OR (VERSION_BRANCH STREQUAL "HEAD"))
  set(VERSION_BRANCH "port-custom")
endif()

message(STATUS "Version: ${VERSION_ROMID}, hash ${VERSION_HASH}, branch ${VERSION_BRANCH}")

configure_file("${CMAKE_SOURCE_DIR}/port/include/versioninfo.h.in" "${CMAKE_BINARY_DIR}/port/include/versioninfo.h" @ONLY)

set(ASSET_DIR "src/assets/${ROMID}")
set(GENERATED_DIR "src/generated/${ROMID}")

set(ENV{ROMID} ${ROMID})

# Build folder postfix

set(BUILD_ID "${VERSION_ROMID}-${VERSION_TARGET}")

# Compiler defs

add_definitions(
  -DMATCHING=0
  -DPIRACYCHECKS=0
  -DROM_SIZE=32
  -DAVOID_UB=1
  -D_LANGUAGE_C=1
  -DPAL=${VERSION_IS_PAL}
  -DVERSION=${VERSION}
)

if(TARGET_IS_64BIT)
  add_definitions(-DPLATFORM_64BIT=1)
endif()

if(DEDICATED_SERVER)
  add_definitions(-DDEDICATED_SERVER=1)
  message(STATUS "Building DEDICATED SERVER: headless, no SDL/OpenGL/audio/input")
endif()

# Compiler flags

# Include directories
include_directories("${CMAKE_SOURCE_DIR}/include")
include_directories("${CMAKE_SOURCE_DIR}/include/PR")
include_directories("${CMAKE_SOURCE_DIR}/src/include")
include_directories("${CMAKE_BINARY_DIR}/src/generated/${ROMID}")
include_directories("${CMAKE_SOURCE_DIR}/src/lib/ultra/audio")
include_directories("${CMAKE_SOURCE_DIR}/port/include")
include_directories("${CMAKE_BINARY_DIR}/port/include")
include_directories("${CMAKE_SOURCE_DIR}/port/lua")

# Set Release optimization to -Og until I fix the -O2 issues
if(NOT (CMAKE_BUILD_TYPE STREQUAL "Debug"))
  add_compile_options(-Og)
endif()

# Always embed DWARF debug info so the crash handler's addr2line fallback
# (port/src/crash.c -> crashAppendDwarf) can symbolise pd.crash.log on ANY build
# type. CMake's plain `Release` config compiles WITHOUT -g, which is what produces
# raw-offset (un-symbolised) crash dumps. The desktop exe is never stripped (only
# the Switch .nro is stripped separately below), so this just guarantees symbols.
add_compile_options(-g)

# Common CFLAGS
add_compile_options(
  -fno-inline-functions
  -fno-strict-aliasing
  -funsigned-char
  -fwrapv
  -include "versioninfo.h"
)

# Common warning flags
add_compile_options(
  -Wall
  -Wno-address
  -Wno-int-in-bool-context
  -Wno-misleading-indentation
  -Wno-missing-braces
  -Wno-multichar
  -Wno-tautological-compare
  -Wno-unused-but-set-variable
  -Wno-unused-value
  -Wno-unused-variable
  -fdiagnostics-color=always
)

# C specific flags
add_compile_options($<$<COMPILE_LANGUAGE:C>:-Wno-pointer-sign>)

# C++ specific flags
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-Wno-unused-function>)

# Platform-specific flags
if (APPLE)
  # Make it look for libs in the executable folder manually since CMake's rpath system is wack
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -rpath @executable_path")
else()
  # This warning is not recognized by XCode/Clang for some reason.
  add_compile_options(-Wno-format-truncation)
endif()

# Dependencies

set(LIBS )

# SDL3 — not needed by the dedicated server (no window/audio/input).
if(NOT DEDICATED_SERVER)
  if(XBOX_NXDK)
    # nxdk-sdl3 (https://github.com/Ryzee119/nxdk-sdl3) has NO install step: per its
    # README it is consumed as a CMake subproject, exposing the targets SDL3::SDL3
    # (link) and SDL3::Headers (include dirs). Point NXDK_SDL3_DIR at a local
    # checkout (preferred, offline), else FetchContent pulls it at configure time
    # (needs network). See docs/PORT_XBOX_NXDK.md.
    if(NXDK_SDL3_DIR)
      message(STATUS "nxdk-sdl3: add_subdirectory(${NXDK_SDL3_DIR})")
      add_subdirectory("${NXDK_SDL3_DIR}" "${CMAKE_BINARY_DIR}/nxdk-sdl3-build")
    else()
      message(STATUS "nxdk-sdl3: NXDK_SDL3_DIR unset -- fetching via FetchContent (master)")
      include(FetchContent)
      FetchContent_Declare(nxdk_sdl3
        GIT_REPOSITORY https://github.com/Ryzee119/nxdk-sdl3.git
        GIT_TAG master)
      FetchContent_MakeAvailable(nxdk_sdl3)
    endif()
    list(APPEND LIBS SDL3::SDL3 SDL3::Headers)
  elseif(NINTENDO_SWITCH)
    # devkitPro portlibs fallback in case the SDL3 CMake config is missing
    # (code includes <SDL3/SDL.h>, so point at the include root).
    # NOTE: Switch + SDL3 is untested since the SDL3 migration.
    find_package(SDL3 CONFIG)
    if(TARGET SDL3::SDL3)
      list(APPEND LIBS SDL3::SDL3)
    else()
      include_directories(SYSTEM "${DEVKITPRO}/portlibs/switch/include")
      list(APPEND LIBS "SDL3")
    endif()
  else()
    find_package(SDL3 REQUIRED CONFIG)
    list(APPEND LIBS SDL3::SDL3)
  endif()
endif()

# ZLib
if(XBOX_NXDK)
  # NXDK bundles zlib (the sample link lines pull in libzlib.lib), but find_package
  # can't locate it under the win32-like sysroot, so point at NXDK's copy directly.
  # zlib.h lives in NXDK's zlib component dir; the built archive is in lib/. If your
  # NXDK lays these out differently, override -DZLIB_INCLUDE_DIR / -DZLIB_LIBRARY.
  if(NOT ZLIB_LIBRARY)
    foreach(_z "$ENV{NXDK_DIR}/lib/libzlib.lib" "$ENV{NXDK_DIR}/lib/zlib/libzlib.lib")
      if(EXISTS "${_z}")
        set(ZLIB_LIBRARY "${_z}")
        break()
      endif()
    endforeach()
  endif()
  if(NOT ZLIB_INCLUDE_DIR)
    foreach(_zi "$ENV{NXDK_DIR}/lib/zlib/zlib" "$ENV{NXDK_DIR}/lib/zlib" "$ENV{NXDK_DIR}/lib")
      if(EXISTS "${_zi}/zlib.h")
        set(ZLIB_INCLUDE_DIR "${_zi}")
        break()
      endif()
    endforeach()
  endif()
  if(NOT ZLIB_LIBRARY OR NOT ZLIB_INCLUDE_DIR)
    message(FATAL_ERROR
      "XBOX_NXDK: could not locate NXDK's zlib. Set -DZLIB_LIBRARY=<.../libzlib.lib> "
      "and -DZLIB_INCLUDE_DIR=<dir containing zlib.h>. "
      "(found ZLIB_LIBRARY='${ZLIB_LIBRARY}', ZLIB_INCLUDE_DIR='${ZLIB_INCLUDE_DIR}')")
  endif()
  message(STATUS "zlib (NXDK): ${ZLIB_LIBRARY} / ${ZLIB_INCLUDE_DIR}")
else()
  find_package(ZLIB REQUIRED)
endif()
if(ZLIB_INCLUDE_DIR)
  include_directories(SYSTEM "${ZLIB_INCLUDE_DIR}")
endif()
list(APPEND LIBS "${ZLIB_LIBRARY}")

# GL — not needed by the dedicated server (OpenGL backend is excluded).
if(NOT DEDICATED_SERVER)
  if(NOT GL_LIBRARY)
    if(XBOX_NXDK)
      # OG Xbox GL is provided by nxdk-sdl3 / pbgl, pulled in by the NXDK link
      # rules + the SDL3 lib above, so no separate GL lib here. (If pbgl must be
      # linked explicitly, set GL_LIBRARY in the toolchain file.) Renderer bring-up
      # is expected to hit a GLSL wall -- see Milestone 2 in docs/PORT_XBOX_NXDK.md.
      set(GL_LIBRARY "")
    elseif(WIN32)
      set(GL_LIBRARY opengl32)
    elseif(NINTENDO_SWITCH)
      set(GL_LIBRARY EGL glapi drm_nouveau)
    elseif(APPLE)
      set(GL_LIBRARY "-framework OpenGL")
    else()
      set(GL_LIBRARY GL)
    endif()
  endif()
  list(APPEND LIBS "${GL_LIBRARY}")
endif()

# SDL_GPU (Vulkan) rendering backend — desktop client builds only. Needs
# glslang for runtime GLSL -> SPIR-V shader translation (MSYS2:
# mingw-w64-x86_64-glslang). Selected at runtime via Video.Renderer=sdlgpu
# or --renderer sdlgpu; OpenGL stays the default either way.
if(DEDICATED_SERVER OR NINTENDO_SWITCH OR XBOX_NXDK)
  # No Vulkan / glslang on the Switch or OG Xbox.
  set(USE_SDLGPU OFF)
endif()

if(XBOX_NXDK)
  # NXDK's clang triple (i386-pc-win32) defines _MSC_VER and _WIN32, so several
  # vendored libs take their MSVC/Windows code paths, which don't suit the OG Xbox:
  #  * minimp3 selects its SSE2 SIMD path, but the Pentium III only has SSE1 ->
  #    MINIMP3_NO_SIMD forces the scalar decoder.
  #  * glad's GL-version parse calls MSVC's sscanf_s (pdclib has no sscanf_s) ->
  #    remap the token to plain sscanf. Only glad uses it, and nothing on NXDK
  #    declares a real sscanf_s, so the macro is contained.
  add_compile_definitions(MINIMP3_NO_SIMD)
  add_compile_definitions(sscanf_s=sscanf)

  # Stub headers for POSIX bits NXDK's pdclib lacks but vendored code reaches for
  # (e.g. <sys/types.h> for zlib's off_t). Lowest precedence: real NXDK headers win.
  include_directories(SYSTEM "${CMAKE_SOURCE_DIR}/port/include/nxdk-stubs")

  # PD ships its own bit-exact N64 math routines (acosf/asinf/atan2f/ceilf/cosf/
  # floorf/sinf) for cross-platform determinism, but pdclib defines the same names
  # and its objects get pulled transitively, so lld-link sees duplicates. PD's
  # regular objects link before the pdclib archive, so /force:multiple keeps PD's
  # (first wins) and drops pdclib's -- matching how the desktop build's archive
  # semantics resolve it. (Desktop/GNU-ld never pulls libm's copies at all.)
  add_link_options("-force:multiple")

  # Force-include a tiny libc compat header into every TU to fill gaps in NXDK's
  # pdclib (e.g. strncasecmp/strcasecmp). New shims go in port/include/nxdk_compat.h.
  # The SHELL: prefix keeps "-include <path>" together (CMake would otherwise
  # de-duplicate/split the two tokens, making clang treat the header as a second
  # input file -> "cannot specify -o when generating multiple output files").
  add_compile_options("SHELL:-include ${CMAKE_SOURCE_DIR}/port/include/nxdk_compat.h")

  # ENet (port/include/external/enet.h) is routed onto NXDK's lwIP TCP/IP stack
  # (see the `defined(NXDK)` branch there). Mirror the include dirs NXDK's own
  # net Makefile uses (lwip/src/include + the nForce-NIC lwipopts in
  # nforceif/include + the nvnetdrv driver header) and link libnxdk_net, which
  # the base NXDK link set does not pull in automatically.
  include_directories(SYSTEM
    "$ENV{NXDK_DIR}/lib/net/lwip/src/include"
    "$ENV{NXDK_DIR}/lib/net/nforceif/include"
    "$ENV{NXDK_DIR}/lib/net/nvnetdrv")
  if(EXISTS "$ENV{NXDK_DIR}/lib/libnxdk_net.lib")
    list(APPEND LIBS "$ENV{NXDK_DIR}/lib/libnxdk_net.lib")
  else()
    message(WARNING "XBOX_NXDK: libnxdk_net.lib not found in $ENV{NXDK_DIR}/lib -- "
                    "build NXDK's net lib (it ships with the networking samples) or "
                    "ENet will fail to link.")
  endif()
endif()
if(USE_SDLGPU)
  if(USE_SDLGPU_STATIC)
    # Static link. MSYS2 ships .a archives alongside the DLLs, but the glslang
    # CMake config package only exports SHARED imported targets, so locate the
    # archives directly (explicit lib*.a names so a .dll.a import lib can never
    # satisfy the search). Not going through the config also drops its
    # GLSLANG_IS_SHARED_LIBRARY interface define, which is what makes the
    # headers emit plain (non-dllimport) declarations for static linking.
    # Link order matters: glslang -> SPIRV-Tools-opt -> SPIRV-Tools.
    find_library(GLSLANG_STATIC_LIB libglslang.a)
    find_library(GLSLANG_RESLIMITS_STATIC_LIB libglslang-default-resource-limits.a)
    find_library(SPIRV_TOOLS_OPT_STATIC_LIB libSPIRV-Tools-opt.a)
    find_library(SPIRV_TOOLS_STATIC_LIB libSPIRV-Tools.a)
    if(NOT (GLSLANG_STATIC_LIB AND GLSLANG_RESLIMITS_STATIC_LIB AND SPIRV_TOOLS_OPT_STATIC_LIB AND SPIRV_TOOLS_STATIC_LIB))
      message(FATAL_ERROR "USE_SDLGPU_STATIC requires the static glslang + SPIRV-Tools archives (MSYS2: pacman -S mingw-w64-x86_64-glslang mingw-w64-x86_64-spirv-tools); configure with -DUSE_SDLGPU_STATIC=OFF to link the DLLs instead")
    endif()
    list(APPEND LIBS "${GLSLANG_STATIC_LIB}" "${GLSLANG_RESLIMITS_STATIC_LIB}" "${SPIRV_TOOLS_OPT_STATIC_LIB}" "${SPIRV_TOOLS_STATIC_LIB}")
    message(STATUS "SDL_GPU: glslang + SPIRV-Tools linked statically")
  else()
    find_package(glslang CONFIG)
    if(NOT TARGET glslang::glslang)
      message(FATAL_ERROR "USE_SDLGPU requires glslang (MSYS2: pacman -S mingw-w64-x86_64-glslang); configure with -DUSE_SDLGPU=OFF to build without the SDL_GPU backend")
    endif()
    list(APPEND LIBS glslang::glslang)
    if(TARGET glslang::SPIRV)
      list(APPEND LIBS glslang::SPIRV)
    endif()
    if(TARGET glslang::glslang-default-resource-limits)
      list(APPEND LIBS glslang::glslang-default-resource-limits)
    endif()
  endif()
  add_compile_definitions(USE_SDLGPU)

  # SPIRV-Cross (optional): enables the D3D12 (DXBC) and Metal (MSL) shader
  # formats for the SDL_GPU backend (MSYS2: mingw-w64-x86_64-spirv-cross).
  # Without it the backend is Vulkan/SPIR-V only. USE_SDLGPU_STATIC skips the
  # shared config package so the find_library fallback picks up the .a libs.
  if(NOT USE_SDLGPU_STATIC)
    find_package(spirv_cross_c_shared QUIET)
  endif()
  if(TARGET spirv-cross-c-shared)
    list(APPEND LIBS spirv-cross-c-shared)
    add_compile_definitions(GFX_SDLGPU_HAS_SPIRV_CROSS)
    message(STATUS "SDL_GPU: spirv-cross found (shared), D3D12/Metal shader formats enabled")
  else()
    # static libs; order matters: c -> backends -> glsl -> core
    find_library(SPIRV_CROSS_C_LIB spirv-cross-c)
    find_library(SPIRV_CROSS_HLSL_LIB spirv-cross-hlsl)
    find_library(SPIRV_CROSS_MSL_LIB spirv-cross-msl)
    find_library(SPIRV_CROSS_CPP_LIB spirv-cross-cpp)
    find_library(SPIRV_CROSS_REFLECT_LIB spirv-cross-reflect)
    find_library(SPIRV_CROSS_GLSL_LIB spirv-cross-glsl)
    find_library(SPIRV_CROSS_CORE_LIB spirv-cross-core)
    if(SPIRV_CROSS_C_LIB AND SPIRV_CROSS_HLSL_LIB AND SPIRV_CROSS_MSL_LIB AND SPIRV_CROSS_GLSL_LIB AND SPIRV_CROSS_CORE_LIB)
      list(APPEND LIBS "${SPIRV_CROSS_C_LIB}" "${SPIRV_CROSS_HLSL_LIB}" "${SPIRV_CROSS_MSL_LIB}")
      if(SPIRV_CROSS_CPP_LIB)
        list(APPEND LIBS "${SPIRV_CROSS_CPP_LIB}")
      endif()
      if(SPIRV_CROSS_REFLECT_LIB)
        list(APPEND LIBS "${SPIRV_CROSS_REFLECT_LIB}")
      endif()
      list(APPEND LIBS "${SPIRV_CROSS_GLSL_LIB}" "${SPIRV_CROSS_CORE_LIB}")
      add_compile_definitions(GFX_SDLGPU_HAS_SPIRV_CROSS)
      message(STATUS "SDL_GPU: spirv-cross found (static), D3D12/Metal shader formats enabled")
    else()
      message(STATUS "SDL_GPU: spirv-cross not found, backend is Vulkan/SPIR-V only")
    endif()
  endif()
endif()

# Platform-specific extra libs
if(NOT EXTRA_LIBRARIES)
  if(XBOX_NXDK)
    # NXDK link rules pull in the core Xbox libs (nxdk, hal, pbkit, winapi,
    # pdclib) automatically; only the C++ runtime needs naming here. Adjust in
    # the toolchain file if the NXDK libc++ is named differently.
    set(EXTRA_LIBRARIES stdc++)
  elseif(WIN32)
    # Statically link the whole GCC runtime (libstdc++ was already static):
    # -static-libgcc folds in libgcc_s_seh-1.dll and the winpthread section
    # folds in libwinpthread-1.dll (whole-archive — its TLS/init machinery is
    # pulled lazily and gets dropped otherwise; the trailing -Bdynamic
    # restores normal linking for the driver's default libs). Without this,
    # players with a stale runtime DLL next to the exe hit loader errors like
    # "entry point clock_gettime64 could not be located" whenever the build
    # toolchain moves ahead of the shipped DLLs. Distribution now only needs
    # SDL3.dll + zlib1.dll (both self-contained, system imports only).
    set(EXTRA_LIBRARIES ws2_32 winmm dbghelp -static-libstdc++ -static-libgcc
        -Wl,-Bstatic,--whole-archive -lwinpthread -Wl,--no-whole-archive,-Bdynamic)
  elseif(NINTENDO_SWITCH)
    set(EXTRA_LIBRARIES stdc++ pthread)
  else()
    set(EXTRA_LIBRARIES stdc++ m dl)
  endif()
endif()
list(APPEND LIBS "${EXTRA_LIBRARIES}")

# Controller Pak support
#   PD_ENABLE_CPAK    - Virtual Pak: live, file-backed N64 Controller Paks (.mpk). No deps.
#   PD_ENABLE_RAPHNET - physical Controller Pak backup via a Raphnet USB adapter (hidapi).
#                       Implies PD_ENABLE_CPAK.
option(PD_ENABLE_CPAK    "Controller Pak support: Virtual Pak (.mpk files)" OFF)
option(PD_ENABLE_RAPHNET "Raphnet physical Controller Pak transfer (hidapi)" OFF)
if(PD_ENABLE_RAPHNET)
  set(PD_ENABLE_CPAK ON)
endif()
set(SRC_CPAK "")
if(PD_ENABLE_CPAK)
  add_definitions(-DPD_ENABLE_CPAK=1)
  # so port/src/mempak.c can find the engine's controller.h
  include_directories("${CMAKE_SOURCE_DIR}/src/lib/ultra/io")
  # reuse the real decomp PFS engine against the in-memory pak image; the four
  # hardware primitives it needs are supplied by port/src/mempak.c
  set(SRC_CPAK
    src/lib/ultra/io/contpfs.c
    src/lib/ultra/io/pfsallocatefile.c
    src/lib/ultra/io/pfsreadwritefile.c
    src/lib/ultra/io/pfsdeletefile.c
    src/lib/ultra/io/pfsfindfile.c
    src/lib/ultra/io/pfsfreeblocks.c
    src/lib/ultra/io/pfsnumfiles.c
    src/lib/ultra/io/pfsfilestate.c
    src/lib/ultra/io/pfsresizefile.c
    src/lib/ultra/io/pfschecker.c
  )
endif()
if(PD_ENABLE_RAPHNET)
  find_package(hidapi REQUIRED)
  add_definitions(-DPD_ENABLE_RAPHNET=1)
  list(APPEND LIBS hidapi::hidapi)
endif()

# Sources

file(GLOB_RECURSE SRC_PORT "${CMAKE_SOURCE_DIR}/port/*.c")
file(GLOB_RECURSE SRC_PORT_CXX "${CMAKE_SOURCE_DIR}/port/*.cpp")

# Exclude vendored Lua from the main target; it is built as its own library
# (pd_lua) below so its sources are not compiled with the game's flags.
list(FILTER SRC_PORT EXCLUDE REGEX "/port/lua/")

file(GLOB_RECURSE SRC_GAME "${CMAKE_SOURCE_DIR}/src/game/*.c")

file(GLOB SRC_LIB_MP3 "${CMAKE_SOURCE_DIR}/src/lib/mp3/*.c")
file(GLOB SRC_LIB_NAUDIO "${CMAKE_SOURCE_DIR}/src/lib/naudio/*.c")
file(GLOB SRC_LIB_ULTRA_AUDIO "${CMAKE_SOURCE_DIR}/src/lib/ultra/audio/*.c")
file(GLOB SRC_LIB_GU "${CMAKE_SOURCE_DIR}/src/lib/ultra/gu/*.c")
set(SRC_LIB
  src/lib/ultra/io/vimodentsclan1.c
  src/lib/ultra/io/vimodepallan1.c
  src/lib/ultra/io/vimodempallan1.c
  src/lib/ultra/io/vitbl.c
  src/lib/ailist.c
  src/lib/anim.c
  src/lib/args.c
  src/lib/audiodma.c
  src/lib/audiomgr.c
  src/lib/base.c
  src/lib/collision.c
  src/lib/debughud.c
  src/lib/dma.c
  src/lib/fault.c
  src/lib/joy.c
  src/lib/lib_2f490_c.c
  src/lib/lib_2fc60.c
  src/lib/lib_3e3e0.c
  src/lib/lib_04f60nb.c
  src/lib/lib_17ce0.c
  src/lib/lib_034d0.c
  src/lib/lib_39c80.c
  src/lib/lib_15850.c
  src/lib/mema.c
  src/lib/memp.c
  src/lib/model.c
  src/lib/modelasm_c.c
  src/lib/mp3.c
  src/lib/mtx_c.c
  src/lib/mtx.c
  src/lib/music.c
  src/lib/path.c
  src/lib/profile.c
  src/lib/rdp.c
  src/lib/rmon.c
  src/lib/rng_c.c
  src/lib/rzip_c.c
  src/lib/snd.c
  src/lib/speaker.c
  src/lib/varsinit.c
  src/lib/vi.c
  src/textureconfig.c
)
list(PREPEND SRC_LIB "${CMAKE_SOURCE_DIR}")

set(SRC
  ${SRC_GAME}
  ${SRC_PORT}
  ${SRC_PORT_CXX}
  ${SRC_LIB_MP3}
  ${SRC_LIB_NAUDIO}
  ${SRC_LIB_ULTRA_AUDIO}
  ${SRC_LIB_GU}
  ${SRC_LIB}
  ${SRC_CPAK}
)

if(WIN32)
  list(APPEND SRC "${CMAKE_SOURCE_DIR}/dist/windows/icon.rc")
endif()

# dedicated_stubs.c is server-only; the recursive port/*.c glob picks it up
# unconditionally, so always drop it and re-add it only for the server build
# (otherwise the client links it alongside input.c -> duplicate symbols).
list(REMOVE_ITEM SRC "${CMAKE_SOURCE_DIR}/port/src/dedicated_stubs.c")

# SDL_GPU backend sources are desktop-client-only; drop them when the backend
# is disabled (dedicated server, Switch, or -DUSE_SDLGPU=OFF). Their bodies
# are also #ifdef USE_SDLGPU as a belt-and-braces guard.
if(NOT USE_SDLGPU)
  list(REMOVE_ITEM SRC
    "${CMAKE_SOURCE_DIR}/port/fast3d/gfx_sdlgpu.cpp"
    "${CMAKE_SOURCE_DIR}/port/fast3d/gfx_sdlgpu_shader.cpp"
  )
endif()

# Dedicated server: drop the SDL window backend, the OpenGL renderer + glad
# loader, and the SDL input layer; swap in no-op input stubs. The remaining
# fast3d files (gfx_pc/gfx_cc) only call through function pointers, so they
# link without GL.
if(DEDICATED_SERVER)
  list(REMOVE_ITEM SRC
    "${CMAKE_SOURCE_DIR}/port/fast3d/gfx_sdl.cpp"
    "${CMAKE_SOURCE_DIR}/port/fast3d/gfx_opengl.cpp"
    "${CMAKE_SOURCE_DIR}/port/fast3d/glad/glad.c"
    "${CMAKE_SOURCE_DIR}/port/src/input.c"
  )
  list(APPEND SRC "${CMAKE_SOURCE_DIR}/port/src/dedicated_stubs.c")
endif()

# Asset files

generate_asset_headers("lang/" "${CMAKE_SOURCE_DIR}/tools/assetmgr/mklang" "en" JSON_HEADERS)
generate_asset_headers("pads/" "${CMAKE_SOURCE_DIR}/tools/assetmgr/mkpads" "" JSON_HEADERS)
generate_asset_headers("tiles/" "${CMAKE_SOURCE_DIR}/tools/assetmgr/mktiles" "" JSON_HEADERS)
generate_asset_headers("animations.json" "${CMAKE_SOURCE_DIR}/tools/assetmgr/mkanims" "" JSON_HEADERS)
generate_asset_headers("sequences.json" "${CMAKE_SOURCE_DIR}/tools/assetmgr/mksequences" "" JSON_HEADERS)
generate_asset_headers("textures.json" "${CMAKE_SOURCE_DIR}/tools/assetmgr/mktextures" "" JSON_HEADERS)

add_custom_target(pd_headers ALL DEPENDS ${JSON_HEADERS})

# Generated AI scripting artifacts: docs/aicommands.md (command reference) and
# scripts/ai.lua (Lua helper library) are produced from the engine source
# (src/include/commands.h + src/game/chrai.c) by tools/gen_aicommands.py so they
# can never drift. Regenerate them at build time when those inputs change. If no
# Python interpreter is found we silently use the committed copies.
find_program(PYTHON3_EXECUTABLE NAMES python3 python)
if(PYTHON3_EXECUTABLE)
  set(AISCRIPT_GEN "${CMAKE_SOURCE_DIR}/tools/gen_aicommands.py")
  set(AISCRIPT_OUTPUTS
    "${CMAKE_SOURCE_DIR}/docs/aicommands.md"
    "${CMAKE_SOURCE_DIR}/scripts/ai.lua")
  add_custom_command(
    OUTPUT ${AISCRIPT_OUTPUTS}
    COMMAND "${PYTHON3_EXECUTABLE}" "${AISCRIPT_GEN}"
    DEPENDS "${AISCRIPT_GEN}"
            "${CMAKE_SOURCE_DIR}/src/include/commands.h"
            "${CMAKE_SOURCE_DIR}/src/game/chrai.c"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Generating AI command reference + Lua helper library"
    VERBATIM)
  add_custom_target(pd_aiscripts ALL DEPENDS ${AISCRIPT_OUTPUTS})
else()
  message(STATUS "python not found: using committed docs/aicommands.md + scripts/ai.lua")
endif()

# Game target

set(BIN_NAME "pd${BIN_REGION}.${TARGET_ARCH}")
if(DEDICATED_SERVER)
  set(BIN_NAME "pd-server${BIN_REGION}.${TARGET_ARCH}")
endif()

# Vendored Lua scripting library (action block -> Lua layer)
file(GLOB SRC_LUA "${CMAKE_SOURCE_DIR}/port/lua/*.c")
list(FILTER SRC_LUA EXCLUDE REGEX "/(lua|luac|onelua)\\.c$")
add_library(pd_lua STATIC ${SRC_LUA})
target_include_directories(pd_lua PUBLIC "${CMAKE_SOURCE_DIR}/port/lua")

add_executable(pd ${SRC})
add_dependencies(pd pd_headers)
if(TARGET pd_aiscripts)
  add_dependencies(pd pd_aiscripts)
endif()
target_link_libraries(pd ${LIBS} pd_lua)
set_target_properties(pd PROPERTIES OUTPUT_NAME "${BIN_NAME}")
if(WIN32 AND NOT DEDICATED_SERVER AND NOT XBOX_NXDK)
  # Client is a GUI-subsystem app. WinMain comes from the MinGW runtime's
  # fallback that forwards to plain main() (SDL3 has no SDL3main library). The
  # dedicated server stays a console app: it needs a console for its log
  # output + the Ctrl-C/close signal handler.
  # NXDK is excluded: its XBE subsystem is handled by the NXDK link rules.
  set_target_properties(pd PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

# Additional commands for specific platforms.

if(NINTENDO_SWITCH)
  add_custom_command(TARGET pd POST_BUILD
    COMMAND nacptool --create "Perfect Dark" "fgsfds and contributors" "${VERSION_ROMID}/${VERSION_BRANCH}/${VERSION_HASH}" "${BIN_NAME}.nacp"
    COMMAND aarch64-none-elf-strip -o ${CMAKE_BINARY_DIR}/${BIN_NAME}.stripped.elf ${CMAKE_BINARY_DIR}/${BIN_NAME}.elf
    COMMAND elf2nro ${CMAKE_BINARY_DIR}/${BIN_NAME}.stripped.elf ${CMAKE_BINARY_DIR}/${BIN_NAME}.nro --icon=${CMAKE_SOURCE_DIR}/dist/nswitch/icon.jpg --nacp=${BIN_NAME}.nacp
  )
endif()

if(XBOX_NXDK)
  # The NXDK link emits a PE .exe; convert it to an Xbox executable (default.xbe)
  # with NXDK's cxbe tool. NXDK normally drives this from its own Makefile, so the
  # exact tool path/flags must be confirmed against the NXDK install (see the
  # "packaging" note in docs/PORT_XBOX_NXDK.md). XBE_TITLE/XBE_OUTPUT overridable.
  if(NOT XBE_OUTPUT)
    set(XBE_OUTPUT "${CMAKE_BINARY_DIR}/default.xbe")
  endif()
  if(NOT CXBE)
    set(CXBE "$ENV{NXDK_DIR}/tools/cxbe/cxbe")
  endif()
  add_custom_command(TARGET pd POST_BUILD
    COMMAND "${CXBE}" "-OUT:${XBE_OUTPUT}" "-TITLE:Perfect Dark"
            "${CMAKE_BINARY_DIR}/${BIN_NAME}.exe"
    COMMENT "Packaging ${XBE_OUTPUT} (NXDK cxbe)"
  )
endif()
