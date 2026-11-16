#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Configures and builds dekopon for Switch 2 inside the dekopon-switch2 image.
# Run it with the repo bind-mounted at /src so the build is incremental and you
# can re-run it after each code fix:
#
#   docker run --rm -it -v "$PWD:/src" dekopon-switch2 docker/switch2/build.sh
#
# Pass extra cmake args after the script name, e.g. a clean reconfigure:
#   ... dekopon-switch2 docker/switch2/build.sh --fresh
set -euo pipefail

: "${DEVKITPRO:=/opt/devkitpro}"
BUILD_DIR=${BUILD_DIR:-build/switch}
cd /src

echo ">>> Submodules (the Switch 2 build does not use nxvk) ..."
git config --global --add safe.directory /src 2>/dev/null || true
git submodule update --init --recursive

echo ">>> Sourcing switch2vars.sh ..."
# shellcheck disable=SC1090,SC1091
source "$DEVKITPRO/switch2vars.sh"

# Compiler cache, when the image has ccache. Precompiled headers are turned off
# with it, as in .ci/linux.sh: GCC's PCH files defeat the cache.
CACHE_ARGS=()
if command -v ccache >/dev/null 2>&1; then
  export CCACHE_DIR="${CCACHE_DIR:-/src/.ccache}"
  export CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-3G}"
  CACHE_ARGS=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
              -DCITRA_USE_PRECOMPILED_HEADERS=OFF)
  ccache -z >/dev/null
fi

echo ">>> Configuring ($BUILD_DIR) ..."
cmake -S . -B "$BUILD_DIR" \
  -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/libounce/cmake/Switch2.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DENABLE_LSFG=OFF \
  ${CACHE_ARGS[@]+"${CACHE_ARGS[@]}"} \
  "$@"

echo ">>> Building citra_switch_nro ..."
cmake --build "$BUILD_DIR" --target citra_switch_nro -j"$(nproc)"

if command -v ccache >/dev/null 2>&1; then
  echo ">>> ccache statistics ..."
  ccache -s
fi

echo
NRO="$BUILD_DIR/src/citra_switch/dekopon.nro"
if [ -f "$NRO" ]; then
  echo "=== OK: $NRO ==="
  ls -la "$NRO"
else
  echo "=== No NRO produced — the build stopped at an error above. ==="
  exit 1
fi
