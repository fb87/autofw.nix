#pragma once

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <span>
#include <string>
#include <string_view>
#include <thread>

#include <flutter_embedder.h>

#include <shell/backend.hpp>
#include <shell/channel.hpp>
#ifdef SHELL_RENDERER_GL
#include <shell/egl.hpp>
#endif
#include <shell/error.hpp>
#include <shell/input.hpp>
#include <shell/keyboard.hpp>
#include <shell/log.hpp>
#include <shell/renderer.hpp>
#include <shell/result.hpp>
#include <shell/texture.hpp>
#include <shell/textinput.hpp>

namespace shell::engine
{

struct config
{
    std::string_view assets;
    std::string_view icu_data;
    renderer::type renderer{renderer::type::automatic};
};

// The Flutter engine wrapper.
//
// Depends only on the backend-agnostic `shell::backend::output` interface so
// it can run on top of Wayland or DRM/KMS without pulling in backend headers.
struct state
{
    config cfg;
    backend::output* out{};

    FlutterEngine fl{};

#ifdef SHELL_RENDERER_GL
    EGLContext gl_ctx{EGL_NO_CONTEXT};
    EGLContext resource_ctx{EGL_NO_CONTEXT};
    EGLContext upload_ctx{EGL_NO_CONTEXT};
#endif

    texture::state textures;
    keyboard::state kb;
    channel::state channels;
    textinput::state ti;

    std::mutex runner_mutex;
    std::condition_variable runner_cv;
    std::deque<std::function<void()>> runner_q;
    std::thread runner_thread;
    std::thread::id runner_id;
    bool runner_started{true};

    std::mutex vsync_mutex;
    std::queue<std::intptr_t> batons;
    std::uint32_t last_buttons{};
};

} // namespace shell::engine

#include <shell/engine_impl.hpp>
