#!/usr/bin/env bash
# Fetches a Flutter engine root for aarch64 into cross/engine-<mode>/, in the
# shape CMakeLists.txt's FLUTTER_ENGINE_ROOT expects (include/, lib/, share/).
# Plain curl/unzip -- no Nix, runs on the bare host or inside cross/Dockerfile.
#
#   debug    Google's generic linux-arm64-embedder.zip (JIT-only -- confirmed
#            FlutterEngineCreateAOTData refuses "AOT data can only be
#            created in AOT mode" against it), matching whatever `flutter`
#            is on PATH (its bin/internal/engine.version). Use for the
#            day-to-day debug/JIT gallery flow, cross-compiled to aarch64.
#   release  ardera's AOT-capable build, pinned to the exact commit
#            examples/gallery/scripts/{fetch-release-toolchain,
#            build-release-arm64}.sh are matched to. See flake.nix's
#            arderaReleaseEnginePin comment for why Google doesn't publish
#            an AOT-capable artifact usable by a custom (non-GTK) embedder.
set -euo pipefail

MODE="${1:-}"
if [[ "$MODE" != "debug" && "$MODE" != "release" ]]; then
  echo "usage: $0 {debug|release}" >&2
  exit 1
fi

cd "$(dirname "$0")"
OUT="engine-$MODE"
rm -rf "$OUT" "$OUT.unzip"
mkdir -p "$OUT/include" "$OUT/lib" "$OUT/share"

if [[ "$MODE" == "release" ]]; then
  # Keep in sync with flake.nix's arderaReleaseEnginePin.
  COMMIT=054a6d0328c72e8e2e70f3e092ea5ba91b3cf8fd
  BASE="https://raw.githubusercontent.com/ardera/flutter-engine-binaries-for-arm/$COMMIT"
  curl -sL -o "$OUT/lib/libflutter_engine.so" "$BASE/arm64/libflutter_engine.so.release"
  curl -sL -o "$OUT/include/flutter_embedder.h" "$BASE/flutter_embedder.h"
  curl -sL -o "$OUT/share/icudtl.dat" "$BASE/arm64/icudtl.dat"
  ln -sf flutter_embedder.h "$OUT/include/embedder.h"
  echo "$COMMIT" > "$OUT/share/ardera-commit.version"
  echo "libflutter_engine.so sha256: $(sha256sum "$OUT/lib/libflutter_engine.so" | cut -d' ' -f1)"
  echo "  (expect 65b9d9b57fba49e4e0b57c18c06b6df65c1b7f22525013436d9c8901f2737ab3)"
else
  FLUTTER_BIN="$(command -v flutter)" || {
    echo "error: no 'flutter' on PATH -- needed to pick a matching debug" \
         "engine.version. Install a Flutter SDK, or use 'release' instead." >&2
    exit 1
  }
  FLUTTER_ROOT="$(dirname "$(dirname "$(readlink -f "$FLUTTER_BIN")")")"
  ENGINE_VERSION="$(cat "$FLUTTER_ROOT/bin/internal/engine.version")"
  echo "using local flutter's engine.version=$ENGINE_VERSION ($FLUTTER_ROOT)"
  BASE="https://storage.googleapis.com/flutter_infra_release/flutter/$ENGINE_VERSION/linux-arm64"
  curl -sL -o /tmp/shell-embedder.zip "$BASE/linux-arm64-embedder.zip"
  unzip -oq /tmp/shell-embedder.zip -d "$OUT.unzip"
  cp "$OUT.unzip"/*.so "$OUT/lib/"
  cp "$OUT.unzip"/*.h "$OUT/include/"
  cp "$OUT.unzip"/LICENSE* "$OUT/share/" 2>/dev/null || true
  ln -sf flutter_embedder.h "$OUT/include/embedder.h"
  echo "$ENGINE_VERSION" > "$OUT/share/engine.version"
  rm -rf "$OUT.unzip" /tmp/shell-embedder.zip
fi

echo "done: $(cd "$OUT" && pwd)/"
