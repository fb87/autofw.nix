# shell

`shell` is a small, header-only C++20 framework for building extensible
Flutter-based embedded meta-applications such as automotive HMIs and kiosks.

Core principles:

- namespace = component
- struct = state/data
- free function = behavior
- header-only C++ framework
- stageless runtime
- visual Stage -> Layer -> Surface scene model
- Flutter owns final visual composition and animation
- Wayland and DRM/KMS output backends
- renderer selection independent of display backend
- EGL/OpenGL ES accelerated rendering
- software rendering for GPU-less systems
- DMA-BUF external textures on accelerated paths
- CPU-backed native surfaces on software paths
- Flutter-rendered virtual keyboard
- Flutter feature plugins
- versioned C ABI for native plugins
- explicit state, no singletons, no backend inheritance hierarchy

See:

- `DESIGN.md`
- `DEVELOPMENT.md`

This bundle is an architecture/API baseline. It is not yet a production
Flutter embedder implementation.

## Message channels

The engine bridges Flutter platform messages through a `shell::channel` registry
(`shell/channel.hpp`) backed by `shell::codec.hpp` (StandardMethodCodec). Named
channels register a method handler; the engine routes each inbound
`FlutterPlatformMessage` to the matching handler and replies with its payload.

- Built-in `path_provider` returns a writable documents path (used by
  `get_storage` / `path_provider`).
- A gallery channel `shell/demo` echoes a canned reply so a Dart app can verify
  the two-way bridge.
- `shell/calc` decodes `[a, b]` int method arguments and returns add/mul results.
- `shell/texture` registers external textures and pushes CPU RGBA frames that the
  GL renderer uploads and Flutter composites.

Flutter-side: `flutter/` holds the framework widgets; a runnable gallery lives in
`examples/gallery/` (custom method channels + external textures).

```sh
# build the Flutter gallery assets, then run
(cd examples/gallery && flutter build bundle --target-platform linux-x64)
./result-shell/bin/shell_app --assets=examples/gallery/build/flutter_assets ...
```

## Run the gallery with nix run

```sh
nix run .#gallery
```

## Release builds without Nix (Docker/Podman, cross-compiled to arm64)

For developing on a plain Debian/Ubuntu (or any Docker/Podman-capable)
machine with no Nix at all. `cross/` cross-compiles `shell_app` for aarch64
using a real `aarch64-linux-gnu-gcc` cross toolchain (no emulation to
compile); `examples/gallery/scripts/` (used below) are already Nix-free --
plain curl/bash/dart/gen_snapshot.

```sh
# 1. fetch an AOT-capable engine root for aarch64 (pinned; see
#    DEVELOPMENT.md's "Release/AOT toolchain" for why this isn't Google's)
./cross/fetch-engine.sh release

# 2. cross-compile shell_app for aarch64, in a Docker/Podman container
#    (auto-detected; override with CONTAINER_ENGINE=podman)
./cross/build.sh release wayland gl
#   -> cross/build-release-wayland-gl/shell_app, a real aarch64 ELF

# 3. release Flutter asset bundle + app.so (same as the Nix path below)
cd examples/gallery
./scripts/fetch-release-toolchain.sh
./scripts/build-release-arm64.sh
```

If the target device's own libraries don't match Ubuntu's `:arm64` packages
(different library versions/ABI -- e.g. a Yocto-built image), point
`cross/build.sh` at an already-extracted copy of the target's rootfs instead:
`TARGET_ROOTFS=/path/to/target-rootfs ./cross/build.sh release wayland gl`.

Copy `cross/build-release-wayland-gl/shell_app`,
`cross/engine-release/lib/libflutter_engine.so` +
`cross/engine-release/share/icudtl.dat`, and
`examples/gallery/build/release-arm64/flutter_assets/` to the arm64 target,
then run exactly as in the Nix path below.

## Release builds with Nix (AOT, cross-compiled to arm64)

Same idea, but using Nix's own cross-compilation (`pkgsCross.aarch64-multiplatform`)
instead of `cross/`'s Docker-based toolchain:

```sh
# 1. native shell_app, cross-compiled to aarch64 (true Nix cross, no emulation)
nix build .#shell-aarch64-release -o result-shell-release

# 2. release-mode engine for arm64 (AOT-capable; see flake.nix's
#    arderaReleaseEnginePin comment for why this isn't Google's own embedder.zip)
nix build .#flutter-engine-release-cross-aarch64 -o result-engine-release

# 3. release Flutter asset bundle + app.so, built with a version-matched
#    standalone toolchain (see examples/gallery/scripts/)
cd examples/gallery
./scripts/fetch-release-toolchain.sh
./scripts/build-release-arm64.sh
```

Copy `result-shell-release/bin/shell_app`, `result-engine-release/lib/libflutter_engine.so`
+ `result-engine-release/share/icudtl.dat`, and
`examples/gallery/build/release-arm64/flutter_assets/` (step 3) to the arm64
target, then:

```sh
./shell_app --assets=flutter_assets --icu=icudtl.dat --renderer=gl
```

`--aot-lib` doesn't need to be passed explicitly: `shell_app` auto-detects the
`app.so` that `build-release-arm64.sh` places inside `flutter_assets/`.

See `DEVELOPMENT.md` for why this needs a separate (pinned) Flutter SDK and
engine build rather than the debug/JIT toolchain the rest of this README uses.
