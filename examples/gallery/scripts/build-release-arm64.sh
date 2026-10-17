#!/usr/bin/env bash
# Builds a release-mode (AOT) Flutter asset bundle for linux-arm64, from an
# x86_64 dev machine -- no target-device build step, no emulation needed for
# gen_snapshot (it's a genuine x86_64 binary that cross-generates arm64 code).
#
# Run scripts/fetch-release-toolchain.sh first. This follows the same manual
# kernel-compile + gen_snapshot recipe documented by flutter-pi
# (https://github.com/ardera/flutter-pi#building-the-appso-for-running-your-app-in-releaseprofile-mode),
# bypassing `flutter build`'s own (host-arch-only) AOT target entirely, since
# that always resolves gen_snapshot through Flutter's own artifact cache --
# which, for a Linux arm64 *target*, only ships a gen_snapshot that *runs on*
# arm64, not one that cross-generates arm64 code from an x86_64 host.
#
# Output: build/release-arm64/flutter_assets/ (assets + app.so). Copy that
# directory to the arm64 target alongside the cross-compiled shell_app
# (nix build .#shell-aarch64-release) and the matching icudtl.dat from
# nix build .#flutter-engine-release-cross-aarch64 ($out/share/icudtl.dat) --
# that one stays paired with the release engine .so you deploy, not with
# this script's Flutter SDK.
set -euo pipefail
cd "$(dirname "$0")/.."

TOOLCHAIN=.release-toolchain
FLUTTER="$TOOLCHAIN/flutter/bin/flutter"
DART="$TOOLCHAIN/flutter/bin/cache/dart-sdk/bin/dartaotruntime"
FRONTEND_SERVER="$TOOLCHAIN/flutter/bin/cache/dart-sdk/bin/snapshots/frontend_server_aot.dart.snapshot"
PATCHED_SDK="$TOOLCHAIN/flutter/bin/cache/artifacts/engine/common/flutter_patched_sdk_product"
GEN_SNAPSHOT="$TOOLCHAIN/gen_snapshot_linux_x64_release"

for f in "$FLUTTER" "$DART" "$FRONTEND_SERVER" "$GEN_SNAPSHOT"; do
  if [ ! -e "$f" ]; then
    echo "missing $f -- run scripts/fetch-release-toolchain.sh first" >&2
    exit 1
  fi
done

OUT=build/release-arm64
rm -rf "$OUT"
mkdir -p "$OUT"

echo "== flutter pub get =="
"$FLUTTER" pub get

echo "== flutter build bundle --release (assets only; app.so comes next) =="
"$FLUTTER" build bundle --release --asset-dir="$OUT/flutter_assets"

echo "== compiling AOT kernel snapshot =="
"$DART" "$FRONTEND_SERVER" \
  --sdk-root "$PATCHED_SDK" \
  --target=flutter \
  --aot --tfa \
  -Ddart.vm.product=true \
  --packages .dart_tool/package_config.json \
  --output-dill "$OUT/kernel_snapshot.dill" \
  --depfile "$OUT/kernel_snapshot.d" \
  package:shell_gallery/main.dart

echo "== gen_snapshot: kernel_snapshot.dill -> app.so (arm64) =="
"$GEN_SNAPSHOT" \
  --deterministic \
  --snapshot_kind=app-aot-elf \
  --elf="$OUT/flutter_assets/app.so" \
  --strip \
  "$OUT/kernel_snapshot.dill"

echo "done: $OUT/flutter_assets/ (with app.so)"
echo "deploy alongside the cross-compiled shell_app and the release engine's icudtl.dat:"
echo "  nix build .#shell-aarch64-release"
echo "  nix build .#flutter-engine-release-cross-aarch64"
