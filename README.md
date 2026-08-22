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

- `shell/` — the embedder shell, written in C++17.
  - `src/main.cpp`           — entry point, backend selection, wiring.
  - `src/oa_backend.h`       — abstract display backend interface.
  - `src/oa_engine.h/.cpp`   — thin wrapper over the Flutter C embedder API.
  - `src/oa_egl.h/.cpp`      — shared EGL helper (config/context).
  - `src/oa_wayland.h/.cpp`  — Wayland backend (xdg-shell, wl_egl_window).
  - `src/oa_kms.h/.cpp`      — DRM/KMS backend (GBM + page-flip).
  - `src/oa_task_runner.h/.cpp` — platform task runner (vsync + messages).
  - `src/oa_codec.h/.cpp`    — minimal StandardMessageCodec for platform channels.
  - `src/protocol/`          — generated Wayland `xdg-shell` client bindings.
- `flake.nix` — Nix flake: builds the `flutter-engine` and `openauto-shell`
  packages, plus a development shell.

## Backends

| Backend | Display path | Frame pacing | Input |
|---------|--------------|--------------|-------|
| `wayland` | `xdg_surface` + `wl_egl_window` | 60 Hz timer → `OnVsync` | pointer / keyboard |
| `kms`     | GBM buffer + DRM page-flip | vblank wait | evdev (todo) |

Both output through EGL/OpenGLES2, giving Flutter a standard `kOpenGL` renderer.

## Build & run

```sh
# Build the shell (a Flutter assets dir is required at run time)
nix build .#shell

# Run against a Wayland compositor
./result-shell/bin/openauto_shell \
  --backend=wayland \
  --assets=/path/to/flutter_project/build/flutter_assets \
  --icu=/path/to/flutter_project/build/icudtl.dat

# Run directly on a DRM/KMS display (root privileges required)
./result-shell/bin/openauto_shell \
  --backend=kms \
  --assets=/path/to/flutter_project/build/flutter_assets

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
