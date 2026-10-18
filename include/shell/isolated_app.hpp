#pragma once

// Parent-side supervisor for a Step-4 isolated app: spawns the SAME
// shell_app binary in --child-renderer mode (shell::ipc::spawn_child),
// imports its GBM/dma-buf-backed ring buffers as GL textures in a context
// sharing with the primary engine's, and feeds whichever one is newest into
// a layer via the exact same texture::bind_external + engine::
// mark_texture_available integration point shell::secondary_engine already
// uses -- this module only changes how a GL texture name gets populated
// (dma-buf import across a real OS process boundary, instead of a shared GL
// context within one process), not what happens downstream of that.
//
// The key behavioral difference from shell::secondary_engine: a crash in
// the child -- Dart or native -- only ends the child process. This module's
// receiver thread notices via a clean socket EOF, logs it, and stops; it
// deliberately never touches the kiosk-wide `stop_all` flag every backend's
// on_poll checks. That is the entire point of process-level isolation.

#ifdef SHELL_RENDERER_GL

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include <shell/egl.hpp>
#include <shell/engine.hpp>
#include <shell/error.hpp>
#include <shell/ipc.hpp>
#include <shell/log.hpp>
#include <shell/result.hpp>
#include <shell/texture.hpp>

namespace shell::isolated_app
{

struct config
{
    std::string assets;
    std::string icu_data;
    std::string aot_library;
    std::uint32_t width{1280};
    std::uint32_t height{720};
    std::string log_tag;

    engine::state* primary{};     // must already be engine::init()'d.
    texture::id target_texture{}; // must already be engine::register_texture()'d on `primary`.
    std::string exe_path{"/proc/self/exe"};
};

struct state
{
    config cfg;
    ipc::child_process child;
    ipc::handshake hs;
    // Dedicated, long-lived context sharing with primary.gl_ctx -- used to
    // import every ring buffer once at startup, then reused by the
    // receiver thread to create each frame's release fence. Deliberately
    // NOT primary.gl_ctx/resource_ctx/upload_ctx themselves (Flutter's own
    // internal threads use those); see isolated_app_impl.hpp. Bound
    // surfaceless (EGL_NO_SURFACE) -- confirmed supported on primary's
    // Wayland EGLDisplay (shell::secondary_engine's creator_ctx already
    // relies on the same thing); no pbuffer needed since this context
    // never renders anything, only creates/deletes GL objects.
    EGLContext creator_ctx{EGL_NO_CONTEXT};
    std::vector<texture::dmabuf_image> images; // imported in PRIMARY's share group.
    std::atomic<bool> running{false};
    std::thread receiver;
};

inline auto create(state& s, config cfg) -> result<void>;
inline auto destroy(state& s) -> void;

} // namespace shell::isolated_app

#include <shell/isolated_app_impl.hpp>

#endif // SHELL_RENDERER_GL
