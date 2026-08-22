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
