# OpenAuto Framework

A from-scratch Flutter **embedder shell** for embedded / automotive displays.
It renders a Flutter application directly onto either a **Wayland** compositor
or a **DRM/KMS** framebuffer, without copying any of the reference embedders
(shipped in `flutter/examples/embedder` or `shell/platform`).

```
┌─────────────────────────────────────────────┐
│               openauto_shell                │
│                                             │
│  ┌────────────┐       ┌─────────────────┐   │
│  │ Flutter    │  EGL/ │   Wayland or    │   │
│  │ Engine     │ GLES2 │   DRM/KMS       │   │
│  │ (embedder) │ ────► │   backend       │   │
│  └────────────┘       └─────────────────┘   │
│        │                    │               │
│   vsync/input          frame + present      │
└─────────────────────────────────────────────┘
```

## Layout

The code is **header-only** and organised as namespace-based functional modules
(no classes). Common (backend-agnostic) logic is split from concrete display
backends, selected at build time via `OA_BACKEND`.

- `shell/src/core/` — shared logic, independent of the display backend:
  - `oa_types.h`       — plain data structs (`frame_tick`, `input_event`, ...).
  - `oa_backend.h`     — `backend` dispatch table (vtable of function pointers)
    separating the engine from a specific backend.
  - `oa_engine.h`      — `oa::engine` handle + free functions (Flutter engine).
  - `oa_egl.h`, `oa_codec.h`, `oa_keyboard.h`, `oa_texture.h`,
    `oa_task_runner.h` — helper namespaces.
- `shell/src/backends/` — concrete display backends (`backend_api` impls):
  - `wayland/oa_wayland.h` — `oa::backend::wayland::create_wayland()`.
  - `kms/oa_kms.h`         — `oa::backend::kms::create_kms()`.
- `shell/src/main.cpp`  — entry point; builds the selected backend + engine.
- `shell/src/protocol/` — generated Wayland `xdg-shell` client bindings.
- `flake.nix` — builds `flutter-engine`, `shell` (Wayland) and `shell-kms`.

## Backends

| Backend | Display path | Frame pacing | Input |
|---------|--------------|--------------|-------|
| `wayland` | `xdg_surface` + `wl_egl_window` | 60 Hz timer → vsync | pointer / keyboard |
| `kms`     | GBM buffer + DRM page-flip | vblank wait | evdev (todo) |

Both output through EGL/OpenGLES2, giving Flutter a standard `kOpenGL` renderer.

## Build & run

```sh
# Wayland backend (default)
nix build .#shell
# DRM/KMS backend (build-time selection)
nix build .#shell-kms

# Run (the backend is fixed at build time; pass --assets/--icu/--test-texture)
./result-shell/bin/openauto_shell \
  --assets=/path/to/flutter_project/build/flutter_assets \
  --icu=/path/to/flutter_project/build/icudtl.dat \
  --test-texture   # optional demo external-texture producer

# Development shell with toolchain + engine
nix develop .#
```

`--assets` must point to a built Flutter app's asset directory containing
`kernel_blob.bin`, `icudtl.dat`, and the app's `AssetManifest`/`FontManifest`.
Produce one with `flutter build bundle` (or `flutter assemble` for the
`kernel_blob.bin`).

## Verification

The shell was verified end-to-end on a live Wayland compositor by building a
small animated sample app and running it with:

```sh
./result-shell/bin/openauto_shell --backend=wayland \
  --assets=/path/to/app/build/flutter_assets --icu=.../icudtl.dat
```

The app rendered a continuously animating full-screen color/counter at a
sustained ~45 fps, proving the full pipeline:

* Flutter `FlutterEngineRun` on the OpenGL renderer config
* vsync ticks (60 Hz timer) → `FlutterEngineOnVsync`
* per-frame `present` → `eglSwapBuffers` → `wl_surface_commit`

Observed shell logs:

```
[oa-engine] vsync request #1
[oa-engine] present #1
[oa-wayland] presented 89 frames total, 44.3 fps over 2007ms
```

The archived `flutter-team-archive/gallery` sample builds with a few patches
(imports `package:gallery/gen_l10n/...`, dependency overrides, and a `main()`
that does not block on `get_storage`), but it requires deferred-component
loading via the engine's platform channel/shelving APIs that a bare embedder
does not yet implement, so it is not the recommended first smoke test.

## Engine package

`flutter-engine` fetches the official Flutter **Linux embedder** archive and
installs `libflutter_engine.so` plus `flutter_embedder.h`. It is
architecture-aware (`linux-x64` / `linux-arm64`).

## Notes / next steps

- The KMS backend currently assumes the first connected connector and its
  preferred mode; it does not yet enumerate multiple planes or hotplug.
- Evdev input for the KMS backend is stubbed; keyboard/pointer forwarding
  is fully wired for Wayland.
- Semantics (accessibility) callbacks are acknowledged but not yet surfaced to
  an a11y stack.

## Input & resize

**Pointer** (mouse/touch) is forwarded to the Flutter framework with the
correct lifecycle (`kAdd`/`kRemove`), button masks, and `kDown`/`kUp`/`kMove`/
`kHover` phases.

**Keyboard** uses a libxkbcommon keymap (from the Wayland compositor) to map
raw evdev keycodes to Flutter key events: `physical` = USB HID usage,
`logical` = Flutter logical key id, and `character` = UTF-8 text, so typing in
`TextField`s works.

**Resize**: when the compositor reconfigures the window, the shell resizes the
EGL/`wl_egl_window` surface and calls `FlutterEngineSendWindowMetricsEvent`, so
the framework re-lays-out to the new physical size and pixel ratio.

## External textures

The shell supports Flutter **external textures**, letting native code (e.g. an
Android Auto video sink or a GStreamer pipeline) composite its own GL frames
into the scene inside a Dart `Texture(textureId: ...)` widget.

`Engine` exposes:

- `RegisterExternalTexture(int64 id)` /
  `UnregisterExternalTexture(int64 id)`
- `PushExternalTextureFrame(int64 id, w, h, rgba8)` — uploads a new RGBA8 frame
  (on a dedicated GL context in the render sharegroup) and signals the engine.

A demo producer is bundled behind the `--test-texture` flag (registers texture
id `1001` and pushes an animated gradient + moving bar at ~30 fps). The sample
app shows it on the "ExtTexture" panel.

```sh
./result-shell/bin/openauto_shell --assets=APP_ASSETS --icu=ICU --test-texture
```
