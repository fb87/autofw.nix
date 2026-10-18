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
#include <utility>

#include <flutter_embedder.h>

#include <shell/backend.hpp>
#include <shell/channel.hpp>
#ifdef SHELL_RENDERER_GL
#include <shell/egl.hpp>
#endif
#include <shell/error.hpp>
#include <shell/input.hpp>
#include <shell/keyboard.hpp>
#include <shell/layer.hpp>
#include <shell/log.hpp>
#include <shell/renderer.hpp>
#include <shell/result.hpp>
#include <shell/stage.hpp>
#include <shell/surface.hpp>
#include <shell/textinput.hpp>
#include <shell/texture.hpp>

namespace shell::engine
{

struct config
{
    std::string_view assets;
    std::string_view icu_data;
    // Path to a release-mode AOT snapshot (libapp.so). Empty runs the
    // engine in JIT mode against kernel_blob.bin in `assets` instead.
    std::string_view aot_library;
    renderer::type renderer{renderer::type::automatic};
#ifdef SHELL_RENDERER_GL
    // Share gl_ctx with another engine's already-created context (see
    // shell::secondary_engine). EGL_NO_CONTEXT (default) preserves today's
    // behavior exactly: every on-screen engine gets its own unshared context.
    EGLContext share_context{EGL_NO_CONTEXT};
#endif
};

// The Flutter engine wrapper.
//
// Depends only on the backend-agnostic `shell::backend::output` interface so
// it can run on top of Wayland or DRM/KMS without pulling in backend headers.
struct state
{
    config cfg;
    backend::output* out{};
    // Propagated onto runner_thread (see start_runner in engine_impl.hpp) so
    // platform-thread log lines (IME "[sel]", key results "[kr]", ...) carry
    // the same per-display tag as the thread that constructed this engine --
    // shell::log::g_tag is thread_local, so it doesn't cross threads on its
    // own. Empty by default: single-display processes see no tag, as today.
    std::string log_tag;

    FlutterEngine fl{};
    FlutterEngineAOTData aot_data{};

#ifdef SHELL_RENDERER_GL
    EGLContext gl_ctx{EGL_NO_CONTEXT};
    EGLContext resource_ctx{EGL_NO_CONTEXT};
    EGLContext upload_ctx{EGL_NO_CONTEXT};

    // Headless/offscreen rendering (shell::secondary_engine). `offscreen`
    // defaults false, which keeps detail::fbo's behavior (return 0, render
    // to the window surface) byte-identical for every on-screen engine.
    bool offscreen{};
    std::uint32_t offscreen_color_tex{}; // set by the caller before init()
    std::uint32_t offscreen_fbo{};       // lazily created by detail::fbo
#endif

    texture::state textures;
    keyboard::state kb;
    channel::state channels;
    textinput::state ti;
    stage::registry stages;
    layer::registry layers;
    surface::registry surfaces;

    std::mutex runner_mutex;
    std::condition_variable runner_cv;
    std::deque<std::function<void()>> runner_q;
    std::thread runner_thread;
    std::thread::id runner_id;
    bool runner_started{true};

    std::mutex vsync_mutex;
    std::queue<std::intptr_t> batons;
    std::uint32_t last_buttons{};

    // Injected key events waiting to be drained on the platform thread
    // (used by the --test-input automation hook).
    std::mutex keyq_mutex;
    std::deque<std::pair<std::uint32_t, bool>> keyq;
};

} // namespace shell::engine

#include <shell/engine_impl.hpp>
