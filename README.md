# shell

`shell` is a small, header-only C++23 framework for building extensible
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
- A demo channel `shell/demo` echoes a canned reply so a Dart app can verify the
  two-way bridge.

Flutter-side: `flutter/` holds the framework widgets; a runnable example
lives in `examples/channel_demo/` (custom MethodChannel + path_provider).

```sh
# build the Flutter demo assets, then run
(cd examples/channel_demo && flutter build bundle --target-platform linux-x64)
./result-shell/bin/shell_app --assets=examples/channel_demo/build/flutter_assets ...
```
