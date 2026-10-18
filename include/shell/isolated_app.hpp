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
// receiver thread notices via a clean socket EOF, logs it, and (Step 5)
// either stops for good (restart.enabled == false, the pre-Step-5 default)
// or blanks the layer and respawns a fresh child after a backoff delay. It
// deliberately never touches the kiosk-wide `stop_all` flag every backend's
// on_poll checks, restarting or not. That is the entire point of
// process-level isolation.

#ifdef SHELL_RENDERER_GL

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <shell/egl.hpp>
#include <shell/engine.hpp>
#include <shell/error.hpp>
#include <shell/ipc.hpp>
#include <shell/log.hpp>
#include <shell/restart_policy.hpp>
#include <shell/result.hpp>
#include <shell/surface.hpp>
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
    // Step 5 (auto-restart): must already be engine::attach_surface()'d on
    // `primary` (same precondition as target_texture) -- only touched if
    // restart.enabled, to blank the layer between a crash and a respawn
    // (engine::set_surface_visible). Unused, may be left default, when
    // restart.enabled is false.
    surface::id target_surface{};
    std::string exe_path{"/proc/self/exe"};
    // Step 5: opt-in. Default-constructed config has enabled=false, which
    // preserves pre-Step-5 behavior exactly -- a crash stays dead, exactly
    // like before this existed.
    restart_policy::config restart{};
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
    std::atomic<bool> running{false}; // true only while a connected child's frames are being pumped.
    std::thread receiver;

    // Step 5 (auto-restart).
    restart_policy::state restart_state;
    // Distinct from `running`: sends the supervisor loop itself (not just
    // the current attempt's frame pump) a permanent "stop for good" signal
    // -- set only by destroy(). `running` alone can't carry that meaning
    // since it's also legitimately false during any backoff gap between
    // live attempts.
    std::atomic<bool> stop_requested{false};
    std::mutex restart_mutex;
    std::condition_variable restart_cv; // paired with restart_mutex; wakes an interruptible backoff wait.
};

inline auto create(state& s, config cfg) -> result<void>;
inline auto destroy(state& s) -> void;

} // namespace shell::isolated_app

#include <shell/isolated_app_impl.hpp>

#endif // SHELL_RENDERER_GL
