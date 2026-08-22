// oa_egl.cpp -- implementation of the shared EGL helpers.
//
// Copyright 2026 OpenAuto Framework.
#include "oa_egl.h"

#include <array>
#include <cstdio>
#include <string>

namespace oa::egl {

namespace {
void LogError(const char* what) { std::fprintf(stderr, "[oa-egl] %s\n", what); }
}  // namespace

bool ChooseConfig(EGLDisplay display, EGLConfig* config_out) {
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
    LogError("no suitable EGL config found");
    return false;
  }
  *config_out = configs[0];
  return true;
}

EGLContext CreateContext(EGLDisplay display, EGLConfig config) {
  static const EGLint kContextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2,
                                           EGL_NONE};
  EGLContext context =
      eglCreateContext(display, config, EGL_NO_CONTEXT, kContextAttribs);
  if (context == EGL_NO_CONTEXT) {
    LogError("failed to create OpenGLES2 context");
    return EGL_NO_CONTEXT;
  }
  return context;
}

bool HasExtension(EGLDisplay display, const char* name) {
  const char* extensions = eglQueryString(display, EGL_EXTENSIONS);
  if (!extensions) {
    return false;
  }
  const std::string haystack(extensions);
  return haystack.find(name) != std::string::npos;
}

}  // namespace oa::egl
