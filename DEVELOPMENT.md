# shell Development Rules and Roadmap

## 1. Normative rules

- Use C++23 for the native framework.
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

### M6 - Transition core

Implement transition requests, targets, priority, interruption, cancellation,
and presentation state.

Acceptance: Home -> Media can be interrupted by a higher-priority Rear Camera
transition.

### M7 - Shared services

Implement clipboard, selection baseline, and shared runtime state.

Acceptance: separate stages exchange clipboard/state without directly
referencing each other.

### M8 - External GL texture  ✅ implemented

Implement Flutter texture registration, GL callback, and frame notification.

Acceptance: native GL renderer appears inside Flutter.

### M9 - Native rendering capability negotiation

Implement plugin/native-surface renderer capability negotiation for:

```text
dmabuf
gl_texture
cpu_buffer
```

Acceptance: incompatible GPU-only plugins are rejected cleanly in software
mode, while CPU-capable plugins remain usable.

### M10 - DMA-BUF

Implement EGLImage import, modifiers, multi-plane representation, and explicit
lifetime rules.

Acceptance: native DMA-BUF producer renders without CPU framebuffer copy.

### M11 - Surface

Implement surface -> texture -> Flutter NativeSurface integration.

Acceptance: a native surface can be attached to a layer, shown, hidden,
resized, and moved.

### M12 - Native plugin ABI

Implement plugin ABI, host API, dlopen, ABI validation, and plugin fd
integration.

Acceptance: external shared library creates and updates a NativeSurface.

### M13 - Flutter feature plugins

Implement `ShellPlugin`, registry, manifest integration, navigation, and
stage/layer placement.

Acceptance: product features can be selected through the Flutter plugin
registry.

### M14 - Virtual keyboard

Implement keyboard layer, Flutter keyboard, IME bridge, show/hide, text commit,
delete, selection, and action keys.

Acceptance: touch-only operation can enter text into Flutter fields.

### M15 - Native input

Implement NativeSurface hit routing, coordinate transformation, pointer
forwarding, and native text-input requests.

Acceptance: native surface can be operated through touchscreen input.

### M16 - DRM/KMS  ✅ implemented (builds)

Implement DRM discovery, connector/mode selection, GBM, EGL, and page flips.

Acceptance: the same meta-application runs directly on DRM/KMS.


### M17 - DRM/KMS software rendering

Implement:

- Flutter software-renderer output
- DRM dumb-buffer allocation
- multiple CPU scanout buffers
- software page-flip presentation

Acceptance: the same Flutter meta-application renders directly on DRM/KMS on a
system with no EGL/GLES hardware acceleration.


### M18 - Direct input

Implement libinput integration.

Acceptance: touch UI operates directly on DRM/KMS without Wayland.

### M19 - DRM vsync

Drive Flutter vsync from DRM page-flip timing.

Acceptance: Flutter animations synchronize to physical display refresh.

### M20 - Production graphics

Implement atomic KMS, explicit fences, robust DMA-BUF reuse, recovery paths,
and hotplug where required.

## 5. Definition of done

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

## 6. Guiding rule

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
