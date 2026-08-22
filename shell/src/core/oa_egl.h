// oa_egl.h -- Shared EGL utilities. Header-only.
//
// Namespace-based functional helpers (no classes). Both the Wayland and the
// DRM/KMS backends talk to an EGL-conformant platform in order to hand Flutter
// an OpenGLES2 rendering surface.
//
// Copyright 2026 OpenAuto Framework.
#pragma once

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <cstdio>
#include <string>

namespace oa::egl {

// Choose a config with 8bpc RGBA, ES2, suitable for a window surface. Picks
// the first matching config. Returns false on failure.
inline bool choose_config(EGLDisplay display, EGLConfig* config_out) {
  static const EGLint kAttribs[] = {
      EGL_SURFACE_TYPE,      EGL_WINDOW_BIT,    EGL_RENDERABLE_TYPE,
      EGL_OPENGL_ES2_BIT,    EGL_RED_SIZE,      8,
      EGL_GREEN_SIZE,        8,                 EGL_BLUE_SIZE,
      8,                     EGL_ALPHA_SIZE,    8,
      EGL_DEPTH_SIZE,        16,                EGL_NONE};

  EGLint num_configs = 0;
  EGLConfig configs[8] = {};
  if (!eglChooseConfig(display, kAttribs, configs, 8, &num_configs) ||
      num_configs == 0) {
    return false;
  }
  *config_out = configs[0];
  EGLint st0 = 0, rt0 = 0, red = 0;
  eglGetConfigAttrib(display, configs[0], EGL_SURFACE_TYPE, &st0);
  eglGetConfigAttrib(display, configs[0], EGL_RENDERABLE_TYPE, &rt0);
  eglGetConfigAttrib(display, configs[0], EGL_RED_SIZE, &red);
  return true;
}

// Create an OpenGLES2 context bound to |config|. Returns EGL_NO_CONTEXT on
// failure. If |share_with| is non-NULL the new context shares GL resources
// with it (so the render/texture contexts share a group).
inline EGLContext create_context(EGLDisplay display, EGLConfig config,
                                 EGLContext share_with) {
  eglBindAPI(EGL_OPENGL_ES_API);
  static const EGLint kContextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2,
                                           EGL_NONE};
  return eglCreateContext(display, config, share_with, kContextAttribs);
}

// Query whether |display| supports the named EGL extension string.
inline bool has_extension(EGLDisplay display, const char* name) {
  const char* extensions = eglQueryString(display, EGL_EXTENSIONS);
  if (!extensions) return false;
  std::string haystack(extensions);
  return haystack.find(name) != std::string::npos;
}

}  // namespace oa::egl
