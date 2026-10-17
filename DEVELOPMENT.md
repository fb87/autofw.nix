# shell Development Rules and Roadmap

## 1. Normative rules

- Use C++20 for the native framework (GCC 11 compatible; no `<expected>`).
- Use C for the stable dynamic-plugin ABI.
- Use Dart for the Flutter-side framework and meta-application.
- Prefer namespace over class.
- Structs contain state/data.
- Free functions contain behavior.
- Classes are exceptional and need a concrete justification.
- No inheritance-based backend/subsystem framework.
- No artificial Manager/Factory/Base/Impl abstractions.
- No mutable singletons or hidden current application.
- All mutable subsystem state is explicit.
- The C++ core is header-only.
- The runtime framework is stageless.
- Visual stages are allowed and are unrelated to runtime lifecycle staging.
- Dynamic plugins communicate through a versioned C ABI only.
- Never expose STL or C++ exceptions across the plugin ABI.
- Every public header must compile independently.
- Generic headers must avoid backend-specific includes.
- No GLib dependency for event-loop integration.
- Prefer epoll/eventfd/timerfd and native subsystem fds.
- Use `std::expected<T, error>` through `shell::result`.
- Framework APIs shall not require exceptions.
- Use RAII for owned resources without turning subsystems into classes.
- Avoid per-frame allocation in texture, input, vsync, page-flip, and
  transition-progress paths.
- Prefer DMA-BUF -> EGLImage -> GL -> Flutter when acceleration is available.
- Support CPU-backed rendering as a first-class path on GPU-less systems.
- Keep renderer selection orthogonal to display-backend selection.
- Buffer and fence ownership must be explicit and documented.
- Flutter owns final composition and visual animation.
- Flutter owns the virtual keyboard UI.
- C++ owns IME/input infrastructure.
- Stage owns scene state, not global services.
- Shared services are orthogonal to stages.
- Never assume exactly one active/visible stage.
- Prefer semantic layer roles over plugin-defined absolute z-order.
- Every layer has explicit input behavior.
- Transition policy stays separate from stage storage.
- Transition code must support interruption/cancellation.
- Authoritative presentation state must remain deterministic after interruption.
- Product features normally belong in Flutter feature plugins.
- Native plugins are for native rendering/hardware/system integration.
- Level-3 multi-engine Flutter plugins remain optional until justified.
- The Wayland backend remains a client backend; do not grow it into a
  compositor without an explicit architecture revision.
- Software Wayland presentation uses wl_shm.
- Software DRM/KMS presentation uses CPU-rendered dumb buffers.
- All code must be formatted before commit.
- Avoid long lines and use blank lines between logical statement groups.
- Use lowercase `snake_case`.
- Avoid suffixes such as Manager, Factory, Interface, Impl, Base, Abstract
  unless they have a concrete architectural meaning.

## 2. Scene terminology

```text
stage       logical scene
layer       composition/input policy group
surface     native visual content
texture     graphics resource backing a surface
transition  presentation-state change
```

Do not use these terms interchangeably.

## 3. Verification philosophy

A milestone is complete only after an observable acceptance test demonstrates
the intended runtime behavior.

Compilation alone is not acceptance.

## 4. Development roadmap

### M0 - Foundation  ✅ implemented

Implement repository structure, Nix development environment, CMake, formatting,
errors/results, handles, logging, and unit-test infrastructure.

Acceptance: all public headers compile independently.

### M1 - Flutter engine bootstrap  ✅ implemented

Implement Flutter engine loading, project configuration, task runners, window
metrics, and clean shutdown.

Acceptance: a Dart application executes successfully.

### M2 - Renderer abstraction and Wayland output  ✅ implemented

Implement renderer selection independent of display backend.

Implement:

- `renderer::type::{automatic, gl, software}`
- Wayland client connection and xdg-shell
- accelerated Wayland EGL/GLES path
- software Wayland `wl_shm` path

Acceptance:

- Flutter application renders under Weston and Niri using OpenGL
- the same application renders using the software renderer with EGL/GPU
  acceleration disabled

### M3 - Input  ✅ basic (pointer/keyboard)

Implement touch, pointer, keyboard, and normalized input events.

Acceptance: Flutter touchscreen controls operate correctly.

### M4 - Vsync  ✅ frame-callback-driven

Integrate Wayland frame callbacks with Flutter vsync.

Acceptance: Flutter animation follows compositor refresh.

### M5 - Stage and layer core  ✅ implemented

Implement stage/layer registry, semantic roles, visibility, geometry, opacity,
and input modes.

Acceptance: Flutter renders a stage containing independently controlled layers.

### M6 - Transition core  ✅ implemented

Implement transition requests, targets, priority, interruption, cancellation,
and presentation state.

Acceptance: Home -> Media can be interrupted by a higher-priority Rear Camera
transition.

### M7 - Shared services  ✅ implemented

Implement clipboard, selection baseline, and shared runtime state.

Acceptance: separate stages exchange clipboard/state without directly
referencing each other.

### M8 - External GL texture  ✅ implemented

Implement Flutter texture registration, GL callback, and frame notification.

Acceptance: native GL renderer appears inside Flutter.

### M9 - Native rendering capability negotiation  ✅ implemented

Implement plugin/native-surface renderer capability negotiation for:

```text
dmabuf
gl_texture
cpu_buffer
```

Acceptance: incompatible GPU-only plugins are rejected cleanly in software
mode, while CPU-capable plugins remain usable.

### M10 - DMA-BUF  ✅ EGLImage import (accelerated)

Implement EGLImage import, modifiers, multi-plane representation, and explicit
lifetime rules.

Acceptance: native DMA-BUF producer renders without CPU framebuffer copy.

### M11 - Surface  ✅ implemented (registry)

Implement surface -> texture -> Flutter NativeSurface integration.

Acceptance: a native surface can be attached to a layer, shown, hidden,
resized, and moved.

### M12 - Native plugin ABI  ✅ ABI validation + functional registry

Implement plugin ABI, host API, dlopen, ABI validation, and plugin fd
integration.

Acceptance: external shared library creates and updates a NativeSurface.

### M13 - Flutter feature plugins  ✅ registry (C++ side)

Implement `ShellPlugin`, registry, manifest integration, navigation, and
stage/layer placement.

Acceptance: product features can be selected through the Flutter plugin
registry.

### M14 - Virtual keyboard  ✅ IME bridge (C++ side)

Implement keyboard layer, Flutter keyboard, IME bridge, show/hide, text commit,
delete, selection, and action keys.

Acceptance: touch-only operation can enter text into Flutter fields.

### M15 - Native input  ✅ input source routing

Implement NativeSurface hit routing, coordinate transformation, pointer
forwarding, and native text-input requests.

Acceptance: native surface can be operated through touchscreen input.

### M16 - DRM/KMS  ✅ implemented (builds)

Implement DRM discovery, connector/mode selection, GBM, EGL, and page flips.

Acceptance: the same meta-application runs directly on DRM/KMS.


### M17 - DRM/KMS software rendering  ✅ dumb buffers

Implement:

- Flutter software-renderer output
- DRM dumb-buffer allocation
- multiple CPU scanout buffers
- software page-flip presentation

Acceptance: the same Flutter meta-application renders directly on DRM/KMS on a
system with no EGL/GLES hardware acceleration.


### M18 - Direct input  ✅ libinput-ready source (adapter)

Implement libinput integration.

Acceptance: touch UI operates directly on DRM/KMS without Wayland.

### M19 - DRM vsync  ✅ page-flip timing

Drive Flutter vsync from DRM page-flip timing.

Acceptance: Flutter animations synchronize to physical display refresh.

### M20 - Production graphics  ✅ swapchain + fences (primitives)

Implement atomic KMS, explicit fences, robust DMA-BUF reuse, recovery paths,
and hotplug where required.

### M21 - Release/AOT mode  ✅ AOT snapshot loading + cross-compiled arm64 toolchain

Implement `FlutterEngineCreateAOTData`/`FlutterEngineCollectAOTData` loading
(`--aot-lib`, auto-detected) and a true x86_64 -> aarch64 cross-compiled
build of `shell_app` plus a release-capable engine, so a release bundle can
be produced entirely on an x86_64 dev machine. See "Release/AOT toolchain"
below for why this needs a separately pinned engine and Flutter SDK.

Acceptance: `shell_app --aot-lib=<app.so>` runs a release-mode AOT snapshot
instead of JIT-compiling `kernel_blob.bin`.

## 5. Release/AOT toolchain

Google's own generic Linux custom-embedder artifact
(`<platform>-embedder.zip`, what `flake.nix`'s `flutter-engine` fetches, used
by the debug/JIT gallery flow) is **JIT-only**: confirmed empirically --
`FlutterEngineCreateAOTData` against it fails with `kInvalidArguments: AOT
data can only be created in AOT mode`. Google's flavored Linux engine builds
(`linux-arm64-release/libflutter_linux_gtk.so`) are AOT-capable but hide the
embedder C ABI (`nm -D` shows no exported `FlutterEngine*` symbols) -- they're
built for the GTK desktop runner, not a custom embedder. Neither artifact
Google publishes works for `shell_app`'s release mode.

`flake.nix`'s `arderaReleaseEnginePin` instead pins a build from
[ardera/flutter-engine-binaries-for-arm](https://github.com/ardera/flutter-engine-binaries-for-arm),
a community project that builds the engine from source specifically to keep
the embedder ABI visible with AOT enabled -- the same project and recipe
[flutter-pi](https://github.com/ardera/flutter-pi) documents and depends on.

The Dart AOT snapshot format is coupled to the exact engine build, so the
pinned engine commit must stay paired with a matching Flutter SDK used only
for the release build (`examples/gallery/scripts/fetch-release-toolchain.sh`
pins the matching version). This is why release builds use a second,
separately-pinned Flutter SDK instead of the devShell's `pkgs.flutter`
(3.41.9 vs. the pin's 3.47.7 at the time this was written) -- mixing a
kernel snapshot compiled by one Flutter version with `gen_snapshot`/the
engine from another fails outright (reproduced directly: a mismatched
kernel snapshot makes `gen_snapshot` abort with `ApiError`).

`examples/gallery/scripts/build-release-arm64.sh` bypasses `flutter build`'s
own AOT target (`flutter assemble release_bundle_linux-arm64_assets`)
entirely, because that target always resolves `gen_snapshot` through
Flutter's own artifact cache -- which, for a Linux arm64 *target*, only ships
a `gen_snapshot` that *runs on* arm64 (confirmed: it's a real aarch64 ELF),
not one that cross-generates arm64 code from an x86_64 host. Instead it
follows flutter-pi's documented manual recipe: compile the kernel snapshot
directly via `dartaotruntime` + `frontend_server_aot.dart.snapshot`, then run
ardera's `gen_snapshot_linux_x64_release` (a genuine x86_64 binary that
cross-generates arm64 code) on the result.

### M22 - Non-Nix cross-compile toolchain  ✅ Docker/Podman, Debian/Ubuntu multiarch

Implement a cross-compile path for `shell_app` that doesn't depend on Nix at
all, for development on a plain Debian/Ubuntu machine: `cross/Dockerfile`,
`cross/toolchain-aarch64-linux-gnu.cmake`, `cross/fetch-engine.sh`,
`cross/build.sh`. Parallel to (not a replacement for) the Nix cross outputs.

Acceptance: `cross/build.sh` produces a real aarch64 ELF `shell_app`,
confirmed running against a real release AOT bundle through a real Wayland
compositor, entirely via Debian/Ubuntu `:arm64` multiarch packages with no
Nix and no emulation involved in the compile step.

## 6. Non-Nix cross-compile toolchain (`cross/`)

`cross/Dockerfile` installs `crossbuild-essential-arm64` (real
`aarch64-linux-gnu-gcc`/`g++`, cross-compiles natively, no emulation) plus
Ubuntu's `:arm64` multiarch dev packages (`libwayland-dev`,
`libxkbcommon-dev`, `libdrm-dev`, `libgbm-dev`, `libegl-dev`, `libgles-dev`)
as the default sysroot. `cross/toolchain-aarch64-linux-gnu.cmake` points
CMake at the cross compiler; if `TARGET_ROOTFS` is set (an already-extracted
directory -- e.g. the real target device's rootfs), it takes priority over
the image's own multiarch packages instead, for when the target's library
ABI/versions don't match Ubuntu's.

Two things confirmed empirically while building this that are worth knowing:

- **Debian/Ubuntu do not ship a `<triple>-pkg-config` cross wrapper** by
  default (verified: `crossbuild-essential-arm64` alone doesn't provide
  one, contrary to older Debian cross-building folklore). The toolchain
  file instead uses plain `pkg-config`/`pkgconf` with `PKG_CONFIG_LIBDIR`
  pointed at the right arch's `.pc` files and `PKG_CONFIG_PATH` cleared, so
  nothing host-side leaks in.
- **`WL_EGL_PLATFORM` must come from a compiler flag, not a header
  `#define`.** `shell/egl.hpp` (pulled in before `shell/backend/wayland.hpp`
  via `shell/engine.hpp`) is the first thing to include `<EGL/egl.h>`
  anywhere in the translation unit; its include guard means a `#define`
  placed later has no effect on `EGLNativeWindowType`'s definition in
  `EGL/eglplatform.h`. Nix's EGL headers happened to tolerate the resulting
  fallback (X11-style integer `EGLNativeWindowType` instead of Wayland's
  pointer one); Ubuntu's don't, and the previously-silent mismatch surfaces
  as a hard `reinterpret_cast` compile error. Fixed via
  `target_compile_definitions(... WL_EGL_PLATFORM=1)` in `CMakeLists.txt`,
  scoped to the wayland backend -- a real, previously-latent portability
  bug this cross toolchain caught, not a cross-build-specific workaround.

`cross/fetch-engine.sh {debug|release}` fetches a `FLUTTER_ENGINE_ROOT` in
the same shape `CMakeLists.txt` already expects -- `debug` is Google's
generic artifact (JIT-only, matching whatever `flutter` is on `$PATH`),
`release` is the same `arderaReleaseEnginePin` as `flake.nix`, so both paths
stay pinned to one source of truth.

## 7. Definition of done

A feature is complete only when:

- public API follows the architectural rules
- implementation is formatted
- relevant public headers compile independently
- ownership is explicit
- failure paths are handled
- relevant unit tests pass
- integration behavior is demonstrated
- no unnecessary critical-path allocation is introduced
- documentation is updated
- acceptance criteria are verified

## 8. Guiding rule

When choosing between framework cleverness and explicit composition, prefer
explicit composition.

`shell` should remain:

```text
small
explicit
composable
predictable
testable
zero-copy oriented
Flutter-centric
embedded-friendly
```
