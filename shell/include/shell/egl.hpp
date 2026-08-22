#pragma once

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <cstdint>
#include <string_view>

#include <shell/error.hpp>
#include <shell/log.hpp>
#include <shell/result.hpp>

namespace shell::egl
{

struct state
{
    void* display{};
    void* config{};
    void* surface{};
    void* context{};
    void* upload_context{};
};

inline auto choose_config(EGLDisplay display, EGLConfig* config_out)
    -> result<void>
{
    static const EGLint kAttribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE,
        EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE,
        8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 16, EGL_NONE};

    EGLint num_configs = 0;
    EGLConfig configs[8] = {};
    if (!eglChooseConfig(display, kAttribs, configs, 8, &num_configs) ||
        num_configs == 0) {
        return std::unexpected(error{.code = error_code::egl,
                                     .message = "no suitable EGL config"});
    }
    *config_out = configs[0];
    return {};
}

inline auto create_context(EGLDisplay display, EGLConfig config,
                           EGLContext share_with) -> EGLContext
{
    eglBindAPI(EGL_OPENGL_ES_API);
    static const EGLint kContextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2,
                                             EGL_NONE};
    return eglCreateContext(display, config, share_with, kContextAttribs);
}

inline auto has_extension(EGLDisplay display, const char* name) -> bool
{
    const char* extensions = eglQueryString(display, EGL_EXTENSIONS);
    return extensions != nullptr &&
           std::string_view(extensions).find(name) != std::string_view::npos;
}

}  // namespace shell::egl
