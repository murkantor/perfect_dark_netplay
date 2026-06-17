#!/bin/bash
#
# Cross-compile the port for the Original Xbox via NXDK + nxdk-sdl3.
# Mirrors tools/buildscripts/nswitch_docker.sh.
#
#   NXDK_DIR=/path/to/nxdk ./tools/buildscripts/xbox_nxdk.sh build_xbox -DROMID=ntsc-final
#
# Arg $1 = build dir; $2.. = extra cmake args (e.g. -DROMID=pal-final).
#
# STATUS: scaffolding. NXDK builds natively via its own Make rules; the CMake path
# here drives cmake/toolchain-nxdk.cmake, which in turn drives NXDK's own verified
# nxdk-cc / nxdk-cxx / nxdk-as wrapper scripts (so the compile/link invocation
# matches the known-good sample builds). The PE->XBE (cxbe) packaging step is wired
# in CMakeLists.txt's XBOX_NXDK branch -- see docs/PORT_XBOX_NXDK.md.

set -e

git config --global --add safe.directory '*' || true

if [ -z "${NXDK_DIR}" ]; then
  echo "error: set NXDK_DIR to your NXDK checkout (and run NXDK's 'make' bootstrap once first)" >&2
  exit 1
fi
# The nxdk-cc/nxdk-cxx wrappers expand ${NXDK_DIR} internally at every compile, so it
# must be present in the environment CMake hands down to them.
export NXDK_DIR

BUILD_DIR="${1:-build_xbox}"
shift || true

# NXDK's activate script exports the LLVM toolchain + tool paths it expects.
if [ -f "${NXDK_DIR}/bin/activate" ]; then
  # shellcheck disable=SC1091
  source "${NXDK_DIR}/bin/activate" || true
fi
export PATH="${NXDK_DIR}/bin:${PATH}"

echo "Configuring (NXDK_DIR=${NXDK_DIR})..."
cmake -G "Unix Makefiles" \
  -DCMAKE_TOOLCHAIN_FILE="$(pwd)/cmake/toolchain-nxdk.cmake" \
  -B "${BUILD_DIR}" "$@" . || exit 1

echo "Building..."
cmake --build "${BUILD_DIR}" -j4 || exit 1

echo "Done. Look for default.xbe in ${BUILD_DIR}/"
