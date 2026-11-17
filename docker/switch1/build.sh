#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Configures and builds the Switch 1 version inside the same image as the Switch 2 one
# (it is devkitA64 with libnx and the Switch portlibs, plus the Switch 2 additions):
#
#   docker run --rm -it -v "$PWD:/src" dekopon-switch2 docker/switch1/build.sh
#
# Needs the NXVK driver archives under externals/nxvk/switch/build/cross (see
# .github/workflows/switch1.yml for how CI gets them, or the NXVK repository's README).
set -euo pipefail

: "${DEVKITPRO:=/opt/devkitpro}"
BUILD_DIR=${BUILD_DIR:-build/switch1}
cd /src

echo ">>> Submodules ..."
git config --global --add safe.directory /src 2>/dev/null || true
git submodule update --init --recursive

# Local changes to submodules we do not own (shared with the Switch 2 build).
for patch in docker/switch2/patches/dynarmic-*.patch; do
  if git -C externals/dynarmic apply --reverse --check "/src/$patch" 2>/dev/null; then
    echo ">>> $patch already applied"
  else
    echo ">>> Applying $patch"
    git -C externals/dynarmic apply "/src/$patch"
  fi
done

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
  -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DENABLE_LSFG=OFF \
  -DENABLE_ZONE_PROFILER=${DEKOPON_ZONE_PROFILER:-OFF} \
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
  echo "=== No NRO produced - the build stopped at an error above. ==="
  exit 1
fi
