# SPDX-License-Identifier: GPL-2.0-or-later
#
# Switch 2 build environment for dekopon: devkitPro + ounce-devkit (libounce,
# OunceVK, Switch2.cmake). No source is baked in; mount the repo at /src and
# run docker/switch2/build.sh so you can iterate without rebuilding the image.
#
#   docker build -t dekopon-switch2 -f docker/switch2/Dockerfile .
#
# Base image as in ounce-devkit's own Docker setup
# (https://github.com/ouncebrew/ounce-devkit); the devkit's packages are pinned below.
FROM docker.io/devkitpro/devkita64:20260219

# glslang for Vulkan shader tooling, matching ounce-devkit's image.
# ccache so CI (and local rebuilds) only recompile what changed.
# cmake/git come with the base image.
RUN apt-get update \
 && apt-get install -y --no-install-recommends glslang-tools ccache \
 && rm -rf /var/lib/apt/lists/*

# Switch 2 devkit: $DEVKITPRO/libounce, portlibs/switch2 (OunceVK, libkaihen),
# Switch2.cmake and switch2vars.sh.
#
# install.sh adds the [ounce] pacman repository and installs whatever it holds
# that day, with the devkitPro packages those depend on. The three packages are
# then replaced by the releases named here, each checked against its SHA-256:
# the driver is linked into the .nro, so which one a build carries has to be a
# decision made in this file and not the date the image happened to be built.
# To move to another release, change its version and its sum.
ARG OUNCE_DEVKIT_COMMIT=5e957599b
ARG LIBOUNCE_VERSION=20261117.1
ARG LIBOUNCE_SHA256=74336b088305dabb4c12a6060cb7fbf38936e137a0f2b2abc86413b561264810
ARG OUNCEVK_VERSION=20261118
ARG OUNCEVK_SHA256=fdb15b647d06f7306c7e2573bd73f13b05346fd17c16ae690489850bfe1dba29
ARG LIBKAIHEN_VERSION=20261117.1
ARG LIBKAIHEN_SHA256=9b53139a92ca8b7b1d3c4249a5f7a0987f089f7a9b74e6193889eef9df820606
RUN git clone https://github.com/ouncebrew/ounce-devkit /tmp/ounce-devkit \
 && git -C /tmp/ounce-devkit checkout -q ${OUNCE_DEVKIT_COMMIT} \
 && sh /tmp/ounce-devkit/install.sh \
 && rm -rf /tmp/ounce-devkit \
 && mkdir /tmp/pkg && cd /tmp/pkg \
 && base=https://github.com/ouncebrew \
 && curl -fsSL --retry 5 -O $base/libounce/releases/download/${LIBOUNCE_VERSION}/libounce-${LIBOUNCE_VERSION}-1-any.pkg.tar.zst \
 && curl -fsSL --retry 5 -O $base/ouncevk/releases/download/${OUNCEVK_VERSION}/ouncevk-${OUNCEVK_VERSION}-1-any.pkg.tar.zst \
 && curl -fsSL --retry 5 -O $base/kaihen/releases/download/${LIBKAIHEN_VERSION}/libkaihen-${LIBKAIHEN_VERSION}-1-any.pkg.tar.zst \
 && printf '%s  %s\n' \
      ${LIBOUNCE_SHA256} libounce-${LIBOUNCE_VERSION}-1-any.pkg.tar.zst \
      ${OUNCEVK_SHA256} ouncevk-${OUNCEVK_VERSION}-1-any.pkg.tar.zst \
      ${LIBKAIHEN_SHA256} libkaihen-${LIBKAIHEN_VERSION}-1-any.pkg.tar.zst > sums \
 && sha256sum -c sums \
 && dkp-pacman -U --noconfirm *.pkg.tar.zst \
 && cd / && rm -rf /tmp/pkg /opt/devkitpro/pacman/cache/* \
 && dkp-pacman -Q libounce ouncevk libkaihen | tee /opt/devkitpro/ounce-versions.txt

# Switch portlibs dekopon links. Switch2.cmake searches the Switch 2 portlibs
# first and falls back to these Switch 1 portlibs for plain C libraries.
RUN dkp-pacman -S --needed --noconfirm \
      switch-freetype switch-bzip2 switch-libpng switch-zlib \
      switch-curl switch-ntfs-3g switch-lwext4 \
 && rm -rf /opt/devkitpro/pacman/cache/*

ENV DEVKITPRO=/opt/devkitpro
WORKDIR /src
ENTRYPOINT ["/bin/bash"]
