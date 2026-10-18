# shell Architecture

## 1. Purpose

`shell` is a small, header-only C++ framework for building extensible
Flutter-based embedded meta-applications.

Primary use cases include automotive HMI, infotainment, embedded kiosks,
industrial HMI, appliance interfaces, and touchscreen control systems.

`shell` provides native runtime infrastructure. Flutter provides the visible
shell and final scene composition. Plugins provide product functionality.
Native renderers provide high-performance visual surfaces.

## 2. Non-goals

`shell` is not a Wayland compositor, desktop environment, general-purpose
window manager, GTK/Qt/SDL/GLFW abstraction, replacement for Flutter, or a
traditional application framework.

Wayland is an output/backend integration mechanism. DRM/KMS provides direct
embedded display output. Hardware acceleration is optional.

## 3. Core philosophy

```text
namespace = component
struct    = state/data
function  = behavior
```

The framework avoids traditional class-oriented subsystem architecture.

## 4. Stageless runtime

There is no artificial runtime lifecycle such as:

```text
Created -> Initialized -> Configured -> Started -> Running -> Stopping
```

Runtime dependencies are explicit:

```text
backend
   |
   v
  EGL
   |
   v
engine
   |
   +-- input
   +-- texture
   +-- plugin
   `-- keyboard
```

Successfully created subsystem state is immediately usable.

## 5. Visual stages

Runtime stagelessness must not be confused with visual stages.

A visual **stage** is a logical presentation scene.

```text
runtime stage -> does not exist
visual stage  -> logical scene container
```

## 6. High-level architecture

```text
                    Flutter Meta Application
                              |
             +----------------+----------------+
             |                |                |
          System UI       Feature Plugins   Keyboard
             |                |                |
             +----------------+----------------+
                              |
                           stage::
                              |
                        transition::
                              |
                           layer::
                              |
              +---------------+---------------+
              |               |               |
        Flutter Widget   Plugin Widget   NativeSurface
                                              |
                                          surface::
                                              |
                                          texture::
                                              |
                             Native content
                                  |
                        +---------+---------+
                        |                   |
                    GPU path            CPU path
                        |                   |
                 DMA-BUF/EGLImage      CPU buffer
                        |                   |
                        +---------+---------+
                                  |
                               Flutter
                                  |
                          final composition
                                  |
                              renderer::
                           +------+------+
                           |             |
                          gl::       software::
                           |             |
                           +------+------+
                                  |
                              backend::
                           +------+------+
                           |             |
                        Wayland       DRM/KMS
```

## 7. Major namespaces

```text
shell::
|
+-- stage::
+-- transition::
+-- layer::
|
+-- shared::
|   +-- clipboard::
|   +-- selection::
|   `-- state::
|
+-- surface::
+-- texture::
|
+-- plugin::
+-- native::
|
+-- keyboard::
+-- input::
|
+-- engine::
+-- event::
+-- vsync::
+-- egl::
+-- renderer::
+|   +-- gl::
+|   `-- software::
+|
`-- backend::
    +-- wayland::
    `-- drm::
```

## 8. Scene model

```text
Stage
  |
  +-- Layer
  |     +-- Flutter content
  |     +-- plugin content
  |     `-- NativeSurface
  |
  `-- Layer
        `-- ...
```

Definitions:

- Stage defines the logical scene.
- Layer defines composition and input policy.
- Surface represents native visual content.
- Transition defines scene-change intent.
- Shared services carry state independently of scenes.
- Flutter performs actual visual composition and animation.

## 9. Stage

Examples:

```text
home
media
navigation
rear_camera
incoming_call
diagnostics
```

Multiple stages may be visible simultaneously. There is no architectural
assumption that exactly one stage is active.

Note: a `stage` here is a logical scene container *within one engine*
(`stage::registry` lives inside `engine::state`), not a physical display --
see "9.1 Multi-display" below for how actual separate screens are modeled.

## 9.1 Multi-display

`shell_app` can drive N physical displays from one process: one Flutter
engine + backend output per display, each with its own private
stage/layer registry (section 9/10 above apply per-display, unchanged).
A crash in one display's Dart code stays isolated to that display's engine
instance; a crash in the shared native process (e.g. a native segfault)
still takes down every display -- this is in-process isolation. A single
layer *within* one display's engine can additionally get real OS-process
isolation instead -- see "9.2 Process isolation" below.

Every display currently runs the *same* asset bundle
(`--assets`/`--icu`/`--aot-lib` are process-wide, not per-display); serving
different content per display is not yet implemented.

CLI surface (`src/main.cpp`):

```text
--display width=W,height=H,primary        (Wayland: one dev-mode window per
                                             --display)
--display drm-device=PATH,connector=ID,primary
                                            (DRM: one physical output per
                                             --display; connector is a
                                             numeric connector id, auto-
                                             assigned from the connected
                                             connectors on that device if
                                             omitted)
--input-device MATCH=DISPLAY_INDEX         (DRM only; MATCH is a sysname
                                             "/dev/input/eventN", a
                                             "vendor:product" hex pair, or a
                                             substring of the device name)
```

Zero `--display` flags synthesizes exactly one display from the legacy
top-level flags (`--drm-device` etc.) -- every pre-multi-display invocation
keeps working unchanged. Exactly one display must be `primary` (default:
the first) -- it alone runs `--test-texture`/`--test-input`/`--test-layers`.

Shutdown is kiosk-style, all-or-nothing: any display closing (or failing to
start) tears every other display down and the process exits. This matches
a single HMI system where losing one screen is a fault condition, not a
reason to keep partially running.

Two DRM outputs may share one GPU/card (common on automotive SoCs driving
e.g. cluster + center stack off one chip): `shell::backend::drm::card`
holds the one shared fd/`gbm_device`/`EGLDisplay` for a device node,
refcounted across however many `shell::backend::drm::state` (output)
instances are opened on it via `open_output()`/`close_output()`, so the
shared `EGLDisplay` is only torn down once the last output on that card
closes.

Input on DRM is owned by exactly one process-wide `shell::input_router`
(one libinput/udev `seat0` context total) rather than one per output --
each `drm::state` grabbing its own seat context would double-deliver every
physical input event. `--input-device` rules assign specific physical
devices to specific displays; decoded events are sent directly to that
display's engine (`engine::send_pointer`/`send_key`, safe to call from any
thread since they internally marshal onto that engine's own platform
runner thread).

## 9.2 Process isolation

A single layer within one display's primary engine can run its Flutter
content in a genuinely separate OS process (`shell::isolated_app`), not
just a separate Dart isolate or a separate same-process engine
(`shell::secondary_engine`, the in-process sibling of this mechanism: a
headless Flutter engine sharing a GL context group with the primary,
zero-copy, but still inside the one native process -- a native crash there
still takes the whole process down, same as section 9.1's per-display
isolation). A native crash (segfault, abort, OOM-kill, ...) in an
isolated-process layer is contained to that one layer; every other
layer/display and the shell process itself keep running.

```text
primary process                          isolated child process
----------------                         -----------------------
shell_app --display ...                  shell_app --child-renderer
  engine::state (primary)                  --ipc-fd=N --width=W --height=H
    |                                      --assets=... --icu=... [--aot-lib=...]
    | isolated_app::create()
    |   ipc::spawn_child()  -- socketpair + fork + execv,
    |                          same binary, re-exec'd --------->  shell::child_renderer::run()
    |                                                               GBM render node
    |                                                               (/dev/dri/renderD128,
    |                                                               decoupled from the
    |                                                               primary's own display
    |                                                               backend) -> 3 dma-buf-
    |                                                               backed ring buffers,
    |                                                               GBM_BO_USE_LINEAR
    |                                                               headless engine::state
    |                                                               (offscreen=true,
    |                                                               surfaceless EGL context,
    |                                                               no window surface)
    |   ipc::recv_handshake()  <--- one-time: dimensions/format/
    |     (SCM_RIGHTS, N fds)       stride + N dma-buf fds
    |   texture::import_dmabuf() x N, in a context sharing with
    |     primary.gl_ctx (never primary.gl_ctx itself -- that's
    |     Flutter's own, used on its own threads)
    |   texture::bind_external(primary.textures, target_texture, ...)
    |
    |   receiver thread:
    |     ipc::recv_frame_ready()  <--- per frame: {buffer_index,
    |       (SCM_RIGHTS, 1 fd =       frame_number} + an EGL native
    |        acquire fence)           fence fd (EGL_ANDROID_native_
    |     egl::wait_fence_cpu()       fence_sync) -- "GPU done
    |       (poll() on the sync_      writing this buffer"
    |        file fd -- this
    |        thread has no GL
    |        context actively
    |        doing the real read,
    |        so a GPU-side
    |        eglWaitSyncKHR has
    |        nothing useful to
    |        wait inside)
    |     texture::bind_external()  -- repoint at the new buffer
    |     engine::mark_texture_available()
    |     egl::create_fence() + ipc::send_buffer_released()
    |       (SCM_RIGHTS, 1 fd) ------------------------------>  egl::wait_fence()
    |                                                             (GPU-side wait, cheap,
    |                                                             before reusing this
    |                                                             buffer slot)
    |
    |   child dies (any reason) -> socket EOF -> recv_frame_ready()
    |   returns false -> waitpid() -> log WIFSIGNALED/WTERMSIG ->
    |   stop, WITHOUT touching stop_all
```

Same downstream integration point section 9.1/12's in-process paths use:
`texture::bind_external` + `engine::mark_texture_available` +
`engine::register_texture`/`unregister_texture`. Process isolation only
changes *how a GL texture name gets populated* (dma-buf import across a
real process boundary instead of a shared GL context within one process),
not what happens after.

Explicit GPU synchronization (required by section 12, "do not design the
production path around `glFinish()`") uses `EGL_ANDROID_native_fence_sync`
in both directions over the same full-duplex IPC socket: the child's
`frame_ready` message carries an *acquire* fence ("I'm done writing"), the
parent's `buffer_released` message carries a *release* fence ("I'm done
reading, safe to overwrite"). The parent's wait is CPU-side (`poll()` on
the fence's underlying sync_file fd) rather than GPU-side, because the
thread that notices a new frame is not the thread that actually samples
the texture -- Flutter's own rasterizer does that later, asynchronously,
on its own GL context. The release fence sent back to the child is
correspondingly a documented approximation: created right after
`mark_texture_available` rather than genuinely after the primary's
rasterizer finishes reading, because the embedder API exposes no
per-external-texture frame-completion hook to key a precise fence off of.
In practice this is safe: a 3-buffer ring means a given slot isn't due for
reuse again until the child has produced two more frames, comfortably
longer than one vsync period.

The child process is the same `shell_app` binary, re-exec'd with hidden
flags (`--child-renderer --ipc-fd=N --width=W --height=H`, plus the normal
`--assets`/`--icu`/`--aot-lib`) rather than a separate dedicated binary --
consistent with every other mode in this codebase. These flags are not
listed in `--help`; nothing spawns a process with them except
`shell::ipc::spawn_child`.

No auto-restart on crash: an isolated layer that dies stays blank
(detached) until the owning layer/surface lifecycle explicitly recreates
it. This first cut optimizes for "a crash is contained and visible," not
"a crash self-heals."

Not yet implemented: multi-GPU dma-buf import (the child's render node and
the primary's display GPU are assumed to be the same physical device --
true on this project's target single-SoC-GPU automotive hardware, and on
every sandbox this was verified against); a plugin-manifest-level policy
for *which* layers get process isolation vs. the in-process
`shell::secondary_engine` path (today it is a direct API choice made by
whatever spawns the layer, not a declarative plugin property -- see
section 13's "Level 3").

## 10. Layer

Recommended semantic roles:

```text
background
application
system_ui
overlay
notification
keyboard
emergency
```

Plugins normally request semantic roles rather than absolute z-order.

Recommended default policy:

```text
background        0
application     100
system_ui       200
overlay         300
notification    350
keyboard        400
emergency      1000
```

Input modes:

```text
passthrough
normal
exclusive
```

Input routing must not depend solely on visual ordering.

## 11. Surface

A surface represents cooperative native visual content:

```text
native renderer
      |
      v
   surface::
      |
      v
   texture::
      |
      v
NativeSurface
      |
      v
   Flutter
```

A native surface is not an arbitrary desktop window.

Flutter can apply clipping, scaling, transforms, opacity, position, gestures,
and animations to it.

## 12. External textures

Supported paths should include:

```text
GL texture
DMA-BUF
CPU buffer fallback
```

DMA-BUF is the preferred production Linux path.

```text
native producer
      |
      v
   DMA-BUF
      |
      v
eglCreateImageKHR()
      |
      v
   EGLImage
      |
      v
GL texture
      |
      v
Flutter external texture
      |
      v
NativeSurface
```

Explicit synchronization must be supported architecturally. Do not design the
production path around `glFinish()`.

## 13. Plugin model

Three plugin levels are defined:

### Level 1 - Flutter feature plugin

Default product-extension mechanism. Compiled into the meta-application.

Examples: home, navigation, media, climate, settings, diagnostics.

### Level 2 - Native plugin

Dynamically loaded `.so` using a stable, versioned C ABI.

Examples: camera pipeline, OpenGL/OpenVX renderer, video decoder, hardware
adapter, Android projection renderer.

C++ ABI and STL types must never cross the dynamic plugin boundary.

### Level 3 - Independent Flutter application

Optional future support for additional Flutter engines when genuine isolation
is required. Level 3 must not influence the initial architecture.

The underlying mechanism -- a layer backed by a genuinely separate Flutter
engine, in-process (`shell::secondary_engine`) or in a separate OS process
(`shell::isolated_app`) -- exists today; see section 9.2. What Level 3 still
lacks is a declarative plugin-manifest surface for requesting it (section
14's `[ui] stage=/layer=` placement has no isolation-level field yet).

## 14. Plugin placement

Plugins may request semantic placement:

```toml
id = "navigation"

[ui]
stage = "main"
layer = "application"
```

The shell remains final policy authority.

## 15. Transition system

Transitions are independent from stage storage.

```text
transition request
       |
       v
transition policy
       |
       v
stage/layer presentation state
       |
       v
Flutter animation
       |
       v
transition completion
```

C++ owns transition intent and policy. Flutter owns visual animation.

Targets may be:

```text
stage
layer
surface
```

Initial transition kinds:

```text
none
fade
crossfade
slide_left
slide_right
slide_up
slide_down
scale
```

Transitions must support pending, running, completed, cancelled, and interrupted
states.

Higher-priority events may interrupt an in-progress transition.

Example policy:

```text
home             100
media            100
navigation       100
incoming_call    500
rear_camera      900
emergency       1000
```

Transition groups may coordinate several targets while keeping persistent
layers unchanged.

## 16. Shared services

Shared services are orthogonal to stages:

```text
                  shared::
                     |
       +-------------+-------------+
       |             |             |
 clipboard::     selection::     state::
       |             |             |
       +-------------+-------------+
                     |
              interested stages
```

Stages consume shared services but do not own them.

Clipboard is system-wide by default. Future scopes may include system, stage,
and plugin.

`shared::state` is for small runtime values such as current media source,
navigation destination, vehicle profile, locale, theme, or active source. It
is not a database.

`shared::selection` is reserved for transient selected/shared content and
future drag/drop or selection ownership.

## 17. Input architecture

```text
Wayland input ----+
                  |
libinput ----------+
                  |
                  v
               input::
                  |
                  v
           scene input policy
                  |
          stages / layers
             /         \
            /           \
       Flutter       NativeSurface
```

Input routing respects visible stages, stage priority, layer ordering, layer
input policy, Flutter hit testing, and native-surface routing.

Flutter owns system gesture policy.

## 18. Virtual keyboard

The onscreen keyboard is rendered by Flutter.

C++ provides focus state, IME bridge, physical keyboard handling, text commit,
deletion, selection, and action handling.

The same keyboard should eventually support cooperative native surfaces.


## 19. Renderer architecture

Renderer selection is independent of display-backend selection.

Supported combinations are:

```text
Wayland + OpenGL
Wayland + software

DRM/KMS + OpenGL
DRM/KMS + software
```

The renderer abstraction is:

```text
                   renderer::
                  /          \
                 /            \
              gl::          software::
               |                |
             EGL              CPU buffer
               |                |
        +------+-----+     +----+----+
        |            |     |         |
     Wayland       DRM  Wayland     DRM
```

Hardware acceleration is optional.

Automatic renderer selection should try the accelerated path first and fall
back to software when the required GPU stack is unavailable.

Explicit software selection must also be supported for testing, CI, virtual
machines, and GPU-less embedded systems.

### 19.1 Accelerated renderer

The accelerated renderer uses EGL/OpenGL ES.

Typical paths:

```text
Wayland -> EGL -> GLES -> Flutter
DRM/KMS -> GBM -> EGL -> GLES -> Flutter
```

DMA-BUF/EGLImage remains the preferred native-surface path when accelerated
rendering is active.

### 19.2 Software renderer

The software renderer uses Flutter's software-renderer path and CPU-owned pixel
buffers.

Wayland software presentation:

```text
Flutter software renderer
        |
        v
CPU pixel buffer
        |
        v
wl_shm buffer
        |
        v
wl_surface
```

DRM/KMS software presentation:

```text
Flutter software renderer
        |
        v
CPU pixel buffer
        |
        v
DRM dumb buffer
        |
        v
KMS page flip
        |
        v
display
```

The DRM software path should use multiple dumb buffers so rendering can proceed
while the previous buffer is being scanned out.

### 19.3 Renderer configuration

Conceptual API:

```cpp
namespace shell::renderer
{

enum class type
{
    automatic,
    gl,
    software,
};

}
```

Renderer selection must remain orthogonal to `backend_type`.

### 19.4 Native-surface compatibility

Native plugins must declare rendering capabilities.

Recommended capability vocabulary:

```text
dmabuf
gl_texture
cpu_buffer
```

Accelerated mode may consume DMA-BUF or GL textures.

Software mode must support CPU-backed native surfaces. A plugin that only
supports DMA-BUF/GL must fail capability negotiation cleanly when software mode
is active.

Example manifest:

```toml
[rendering]
supports = ["dmabuf", "cpu_buffer"]
```

or:

```toml
[rendering]
requires = ["dmabuf"]
```

CPU-backed external surfaces are a supported production compatibility path,
but zero-copy DMA-BUF remains preferred when hardware acceleration exists.

### 19.5 Software-rendering goals

Software rendering is intended to support:

- simple kiosk UI
- forms and settings
- text-heavy interfaces
- modest animations
- automated tests and CI
- virtual machines without GPU acceleration
- embedded targets with DRM/KMS but no usable GPU

High-frame-rate video, camera, and graphics-heavy native content may require
hardware acceleration for acceptable performance.


## 20. Backends

Both initial output backends converge at EGL.

```text
backend::wayland
       |
       +----------+
                  v
                 EGL
                  ^
       +----------+
       |
backend::drm
```

The Wayland backend is a Wayland client, not a compositor.

DRM/KMS is the direct production path and should eventually use atomic
modesetting.

## 21. Vsync

Preferred sources:

```text
Wayland -> frame callback
DRM/KMS -> page flip
```

Timer-based vsync is fallback-only.

## 22. Event loop

No GLib dependency is required.

Use Linux file descriptors:

```text
epoll
 +-- Wayland
 +-- DRM
 +-- libinput
 +-- timerfd
 +-- eventfd
 `-- plugin fds
```

## 23. Architectural invariants

1. Namespace = component; struct = state; function = behavior.
2. The native runtime remains stageless.
3. Visual stages are scene abstractions, not lifecycle stages.
4. `shell` is not a Wayland compositor.
5. Flutter owns final visual composition.
6. Stage defines logical scene.
7. Layer defines composition and input policy.
8. Surface represents native visual content.
9. Transition represents scene-change intent and policy.
10. Flutter performs transition animation.
11. Shared services are orthogonal to stages.
12. Native visual content uses external textures.
13. DMA-BUF is the preferred Linux native graphics transport.
14. Flutter feature plugins are the default product extension mechanism.
15. Native dynamic plugins use a stable C ABI.
16. The virtual keyboard is rendered in Flutter.
17. Renderer selection is orthogonal to display-backend selection.
18. Hardware acceleration is optional.
19. Accelerated Wayland/DRM paths use EGL/OpenGL ES.
20. Software Wayland uses wl_shm; software DRM/KMS uses CPU/dumb buffers.
21. The framework remains small, explicit, modular, and independently testable.
