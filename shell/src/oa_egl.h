// oa_egl.h -- Shared EGL utility helpers for the OpenAuto embedder backends.
//
// Both the Wayland and the DRM/KMS backends talk to an EGL-conformant platform
// in order to hand Flutter an OpenGLES2 rendering surface. This small helper
// centralizes the boilerplate needed to pick a config and create a context
// suitable for an embedder. Each backend is responsible for obtaining its own
// EGLDisplay handle from the appropriate platform (Wayland / GBM).
//
// Copyright 2026 OpenAuto Framework.
#pragma once

#include <EGL/egl.h>
#include <EGL/eglext.h>

namespace oa::egl {

// Choose a config with 8bpc RGBA, ES2, suitable for a window surface.
// Picks the first matching config.
bool ChooseConfig(EGLDisplay display, EGLConfig* config_out);

// Create an OpenGLES2 context bound to |config|. Returns EGL_NO_CONTEXT on
// failure. The context is obtained on the calling thread.
EGLContext CreateContext(EGLDisplay display, EGLConfig config);

// Query whether |display| supports the named EGL extension string.
bool HasExtension(EGLDisplay display, const char* name);

}  // namespace oa::egl
