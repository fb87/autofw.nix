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
  - `src/protocol/`          — generated Wayland `xdg-shell` client bindings.
- `flake.nix` — Nix flake: builds the `flutter-engine` and `openauto-shell`
  packages, plus a development shell.

## Backends

| Backend | Display path | Frame pacing | Input |
|---------|--------------|--------------|-------|
| `wayland` | `xdg_surface` + `wl_egl_window` | frame callback | pointer / keyboard |
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
