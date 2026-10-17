#!/usr/bin/env bash
# Cross-compiles shell_app for aarch64 from an x86_64 (or any) host, via
# cross/Dockerfile -- no Nix required, no emulation (real cross toolchain).
#
# usage: cross/build.sh {debug|release} [wayland|drm] [gl|software]
#
# Run cross/fetch-engine.sh <mode> first.
#
# env:
#   CONTAINER_ENGINE   docker | podman (auto-detected if unset)
#   TARGET_ROOTFS      path to an already-extracted target device rootfs
#                      (a directory), to link against instead of the
#                      image's own Ubuntu :arm64 multiarch packages --
#                      useful when the target's library ABI/versions don't
#                      match Ubuntu's.
#   IMAGE_TAG          override the built image's tag
set -euo pipefail
cd "$(dirname "$0")/.."

MODE="${1:-}"
BACKEND="${2:-wayland}"
RENDERER="${3:-gl}"
if [[ "$MODE" != "debug" && "$MODE" != "release" ]]; then
  echo "usage: $0 {debug|release} [wayland|drm] [gl|software]" >&2
  exit 1
fi

ENGINE_ROOT="cross/engine-$MODE"
if [[ ! -f "$ENGINE_ROOT/lib/libflutter_engine.so" ]]; then
  echo "error: $ENGINE_ROOT not found -- run ./cross/fetch-engine.sh $MODE first" >&2
  exit 1
fi

if [[ -z "${CONTAINER_ENGINE:-}" ]]; then
  if command -v docker >/dev/null 2>&1; then
    CONTAINER_ENGINE=docker
  elif command -v podman >/dev/null 2>&1; then
    CONTAINER_ENGINE=podman
  else
    echo "error: neither docker nor podman found on PATH" >&2
    exit 1
  fi
fi

IMAGE_TAG="${IMAGE_TAG:-shell-cross-aarch64}"
echo "== building cross-compile image ($CONTAINER_ENGINE) =="
"$CONTAINER_ENGINE" build -t "$IMAGE_TAG" -f cross/Dockerfile cross/

BUILD_DIR="cross/build-$MODE-$BACKEND-$RENDERER"
MOUNT_ARGS=(-v "$(pwd):/work")
ENV_ARGS=()
if [[ -n "${TARGET_ROOTFS:-}" ]]; then
  MOUNT_ARGS+=(-v "$(realpath "$TARGET_ROOTFS"):/target-rootfs:ro")
  ENV_ARGS+=(-e "TARGET_ROOTFS=/target-rootfs")
  echo "== using target rootfs: $TARGET_ROOTFS =="
else
  echo "== using the image's own :arm64 multiarch packages as sysroot =="
fi

echo "== configuring + building ($MODE, $BACKEND, $RENDERER) =="
# No --user override: rootless Podman already remaps container root to the
# invoking host user transparently, and fighting that with an explicit
# --user causes its own UID-mapping permission errors (confirmed). Under
# plain root-daemon Docker, output under cross/build-*/ may come out
# root-owned -- `sudo chown -R $(id -u):$(id -g) cross/build-*` if that's a
# problem.
"$CONTAINER_ENGINE" run --rm \
  "${MOUNT_ARGS[@]}" "${ENV_ARGS[@]}" -w /work "$IMAGE_TAG" bash -c "
    set -e
    cmake -S . -B '$BUILD_DIR' -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=cross/toolchain-aarch64-linux-gnu.cmake \
      -DFLUTTER_ENGINE_ROOT=\$(pwd)/$ENGINE_ROOT \
      -DOA_BACKEND=$BACKEND -DOA_RENDERER=$RENDERER \
      -DSHELL_BUILD_EXAMPLES=OFF -DSHELL_BUILD_TESTS=OFF
    cmake --build '$BUILD_DIR'
  "

echo "done: $BUILD_DIR/shell_app"
command -v file >/dev/null 2>&1 && file "$BUILD_DIR/shell_app" || true
