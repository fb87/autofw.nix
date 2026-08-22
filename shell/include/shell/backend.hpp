// backend.hpp -- Backend-agnostic display output interface.
//
// The engine must not depend on a specific backend (Wayland / DRM). This tiny
// interface is the only contract between the engine and any output backend:
// EGL handles, the native surface geometry, a present function, and a stop
// request. Individual backends implement it (see backend/wayland.hpp,
// backend/drm.hpp).
//
// Copyright 2026 OpenAuto Framework.
#pragma once

#include <cstdint>

#include <shell/result.hpp>

namespace shell::backend
{

// Per-frame present callback. Implemented by a backend; drives eglSwapBuffers
// and any page-flip / frame-commit.
using present_fn = bool (*)(void* user_data);

// Request the backend event loop to stop at the next opportunity.
using stop_fn = void (*)(void* user_data);

// A minimal, backend-agnostic output interface.
//
// `state` is the backend's opaque handle; `egl` carries the EGL display/config
// handles the engine's renderer config needs. `geometry` is the current native
// surface size, and `pixel_ratio` the device pixel ratio.
struct output
{
    void* state{};

    // EGL handles (opaque pointers).
    void* egl_display{};
    void* egl_config{};
    void* egl_surface{};

    std::uint32_t width{};
    std::uint32_t height{};
    double pixel_ratio{1.0};

    present_fn present{};
    stop_fn stop{};
};

}  // namespace shell::backend
