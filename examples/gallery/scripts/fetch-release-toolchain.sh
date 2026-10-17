#!/usr/bin/env bash
# Fetches the release/AOT build toolchain for the gallery, into
# .release-toolchain/ (gitignored): an official Flutter SDK, plus the
# x86_64->aarch64 cross gen_snapshot from
# https://github.com/ardera/flutter-engine-binaries-for-arm.
#
# These two must stay paired: the Dart AOT snapshot format is coupled to the
# exact engine build. Google's own generic Linux embedder.zip (used for the
# debug/JIT gallery flow) is JIT-only and can't run AOT at all -- ardera's
# project builds the engine from source specifically to keep the embedder C
# ABI visible with AOT enabled, which is what build-release-arm64.sh and
# shell_app's --aot-lib link against. See flake.nix's `arderaReleaseEnginePin`
# comment for the full story.
#
# Re-run this if you bump FLUTTER_VERSION/ARDERA_COMMIT; both must keep
# pointing at the same engine build (flutter/engine.version inside the
# downloaded SDK must equal ardera's arm64/flutter.version file, which is
# this script's own sanity check below).
set -euo pipefail
cd "$(dirname "$0")/.."

FLUTTER_VERSION=3.47.7
FLUTTER_SHA256=af7455f540df951184c7953fbe3cf884766e7541dacf8571f9f582db03dd6230
ARDERA_COMMIT=054a6d0328c72e8e2e70f3e092ea5ba91b3cf8fd
GEN_SNAPSHOT_SHA256=57f7e0191e02b1735dfdcd089469e4fa0457a0d68591a057b2cc8999b4221abf

OUT=.release-toolchain
mkdir -p "$OUT"

FLUTTER_TARBALL="$OUT/flutter_linux_${FLUTTER_VERSION}-stable.tar.xz"
if [ ! -d "$OUT/flutter" ]; then
  echo "fetching Flutter $FLUTTER_VERSION SDK..."
  curl -sL -o "$FLUTTER_TARBALL" \
    "https://storage.googleapis.com/flutter_infra_release/releases/stable/linux/flutter_linux_${FLUTTER_VERSION}-stable.tar.xz"
  echo "$FLUTTER_SHA256  $FLUTTER_TARBALL" | sha256sum -c -
  tar -xf "$FLUTTER_TARBALL" -C "$OUT"
  rm -f "$FLUTTER_TARBALL"
else
  echo "Flutter $FLUTTER_VERSION SDK already present, skipping"
fi

GEN_SNAPSHOT="$OUT/gen_snapshot_linux_x64_release"
if [ ! -f "$GEN_SNAPSHOT" ]; then
  echo "fetching ardera gen_snapshot (commit $ARDERA_COMMIT)..."
  curl -sL -o "$GEN_SNAPSHOT" \
    "https://raw.githubusercontent.com/ardera/flutter-engine-binaries-for-arm/${ARDERA_COMMIT}/arm64/gen_snapshot_linux_x64_release"
  echo "$GEN_SNAPSHOT_SHA256  $GEN_SNAPSHOT" | sha256sum -c -
  chmod +x "$GEN_SNAPSHOT"
else
  echo "ardera gen_snapshot already present, skipping"
fi

sdk_engine_version=$(cat "$OUT/flutter/bin/internal/engine.version")
echo "Flutter SDK engine.version: $sdk_engine_version"
echo "(must match flake.nix's arderaReleaseEnginePin / the engine behind $GEN_SNAPSHOT --"
echo " this script doesn't re-derive ardera's own version files to check, since the SDK"
echo " and ardera's binaries were picked by hand to match; if you bump either, verify by"
echo " hand against https://github.com/ardera/flutter-engine-binaries-for-arm)"

echo "done: $OUT/flutter (SDK), $GEN_SNAPSHOT (gen_snapshot)"
