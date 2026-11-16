# SPDX-License-Identifier: GPL-2.0-or-later
#
# Switch 2 build environment for dekopon: devkitPro + ounce-devkit (libounce,
# OunceVK, Switch2.cmake). No source is baked in; mount the repo at /src and
# run docker/switch2/build.sh so you can iterate without rebuilding the image.
#
#   docker build -t dekopon-switch2 -f docker/switch2/Dockerfile .
#
# Base image and devkit version are pinned to match ounce-devkit's own Docker
# setup (see https://github.com/ouncebrew/ounce-devkit).
FROM docker.io/devkitpro/devkita64:20260219

# glslang for Vulkan shader tooling, matching ounce-devkit's image.
# ccache so CI (and local rebuilds) only recompile what changed.
# cmake/git come with the base image.
RUN apt-get update \
 && apt-get install -y --no-install-recommends glslang-tools ccache \
 && rm -rf /var/lib/apt/lists/*

# Switch 2 devkit: installs $DEVKITPRO/libounce, portlibs/switch2 (OunceVK),
# Switch2.cmake and switch2vars.sh. install.sh pulls switch-dev / switch-zlib /
# dkp-toolchain-vars via dkp-pacman if missing, then downloads and unpacks the
# default devkit release (verified by SHA-256).
ARG OUNCE_DEVKIT_VERSION=
RUN git clone --depth 1 https://github.com/ouncebrew/ounce-devkit /tmp/ounce-devkit \
 && sh /tmp/ounce-devkit/install.sh ${OUNCE_DEVKIT_VERSION} \
 && rm -rf /tmp/ounce-devkit

# Switch portlibs dekopon links. Switch2.cmake searches the Switch 2 portlibs
# first and falls back to these Switch 1 portlibs for plain C libraries.
RUN dkp-pacman -S --needed --noconfirm \
      switch-freetype switch-bzip2 switch-libpng switch-zlib \
      switch-curl switch-ntfs-3g switch-lwext4 \
 && rm -rf /opt/devkitpro/pacman/cache/*

ENV DEVKITPRO=/opt/devkitpro
WORKDIR /src
ENTRYPOINT ["/bin/bash"]
