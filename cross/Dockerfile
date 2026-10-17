# Cross-compile environment for shell_app: an x86_64 host builds this image
# natively (no emulation) and cross-compiles aarch64 output from it.
#
# Default target is arm64/aarch64-linux-gnu. TARGET_DEB_ARCH/TARGET_TRIPLE
# exist so the same Dockerfile can be re-pointed at another target (used in
# this repo's own CI/sandbox verification to prove the pattern without an
# arm64 target, by cross-compiling the *other* direction instead).
ARG TARGET_DEB_ARCH=arm64
ARG TARGET_TRIPLE=aarch64-linux-gnu

FROM ubuntu:24.04
ARG TARGET_DEB_ARCH
ARG TARGET_TRIPLE
ENV DEBIAN_FRONTEND=noninteractive

RUN dpkg --add-architecture "${TARGET_DEB_ARCH}" && \
    apt-get update && \
    apt-get install -y --no-install-recommends \
      crossbuild-essential-${TARGET_DEB_ARCH} \
      cmake \
      ninja-build \
      pkg-config \
      curl \
      ca-certificates \
      xz-utils \
      unzip \
      "libwayland-dev:${TARGET_DEB_ARCH}" \
      "libxkbcommon-dev:${TARGET_DEB_ARCH}" \
      "libdrm-dev:${TARGET_DEB_ARCH}" \
      "libgbm-dev:${TARGET_DEB_ARCH}" \
      "libegl-dev:${TARGET_DEB_ARCH}" \
      "libgles-dev:${TARGET_DEB_ARCH}" \
      "libinput-dev:${TARGET_DEB_ARCH}" \
      "libudev-dev:${TARGET_DEB_ARCH}" && \
    rm -rf /var/lib/apt/lists/*

ENV TARGET_TRIPLE=${TARGET_TRIPLE}
WORKDIR /work
