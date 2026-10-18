#pragma once

// A headless, off-screen Flutter engine whose rendered content is composited
// into a PRIMARY engine's layer as a GL texture, zero-copy -- Step 3 of the
// automotive-shell roadmap ("per-layer secondary engines", Option C from the
// original feasibility analysis).
//
// Entirely GL-only: a software-renderer primary has no GL texture to share
// into, so this whole module is unavailable without SHELL_RENDERER_GL.
//
// How the zero-copy bridge works:
//   1. One GL texture is created once, up front, from a context already in
//      the primary engine's share group (primary.resource_ctx, briefly made
//      current). Texture *objects* are shared immediately across a share
//      group once created.
//   2. The secondary engine's own gl_ctx is created sharing with the
//      primary's gl_ctx (engine::config::share_context).
//   3. detail::fbo (engine_impl.hpp) lazily creates an FBO *inside the
//      secondary's own context* the first time Flutter asks for one, and
//      attaches the shared texture to it. This has to be lazy and in the
//      secondary's own context because FBOs are *container* objects, not
//      shared objects -- only the texture attached to them is shared.
//   4. After each frame, the secondary's headless "present" callback does
//      not swap anything -- it just calls engine::mark_texture_available on
//      the PRIMARY engine. No pixels move; the texture the primary samples
//      already has the new content because it's the same GL object.
//
// The secondary engine needs a real (if otherwise-unused) EGLSurface to
// satisfy eglMakeCurrent -- a small EGL pbuffer serves that purpose; nothing
// is ever rendered to it (the real target is the lazily-created FBO).
//
// There is no real display to synchronize frame production to, so a plain
// timer thread pumps engine::on_vsync at a fixed rate -- the same role a
// backend's frame-callback/page-flip plays for an on-screen engine, just
// timer-sourced instead of compositor-sourced.
//
// Step 5 (auto-restart): unlike shell::isolated_app, there is no crisp
// "this crashed" signal here -- a Dart exception doesn't kill this engine,
// and a native crash in it takes the whole shell process down with it
// (nothing left to restart). What this module detects instead is its own
// native platform-runner-thread going unresponsive, via a heartbeat lambda
// posted to the engine's own runner_q every `heartbeat_interval`; if
// `last_heartbeat_ms` hasn't advanced within `hang_threshold`, the inner
// engine::state is torn down and reinitialized in place (pbuffer/
// creator_ctx/shared_tex are untouched -- they're not what hung). This is
// explicitly best-effort, not a guarantee: recovery goes through
// engine::destroy -> stop_runner -> runner_thread.join(), so a *truly*
// deadlocked runner thread makes the recovery attempt hang too -- there is
// no safe way to force-kill one native thread from another in C++ without
// risking corruption, and this project does not attempt one. See
// DESIGN.md Step 5 section for the full writeup of this asymmetry against
// shell::isolated_app's OS-process model, which has no such caveat.

#ifdef SHELL_RENDERER_GL

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include <shell/backend.hpp>
#include <shell/egl.hpp>
#include <shell/engine.hpp>
#include <shell/error.hpp>
#include <shell/log.hpp>
#include <shell/renderer.hpp>
#include <shell/restart_policy.hpp>
#include <shell/result.hpp>
#include <shell/surface.hpp>
#include <shell/texture.hpp>

namespace shell::secondary_engine
{

struct config
{
    std::string_view assets;
    std::string_view icu_data;
    std::string_view aot_library;
    std::uint32_t width{1280};
    std::uint32_t height{720};
    double pixel_ratio{1.0};
    double fps{60.0}; // no real display to sync to -- timer-pumped vsync.
    std::string log_tag;

    engine::state* primary{};     // must already be engine::init()'d.
    texture::id target_texture{}; // must already be engine::register_texture()'d on `primary`.
    // Step 5 (auto-restart): must already be engine::attach_surface()'d on
    // `primary` -- only touched if restart.enabled, to blank the layer
    // between a detected hang and a successful restart.
    surface::id target_surface{};
    // Step 5: opt-in. Default-constructed config has enabled=false, which
    // preserves pre-Step-5 behavior exactly -- a hang stays hung, like
    // before this existed.
    restart_policy::config restart{};
    std::chrono::milliseconds heartbeat_interval{2000};
    std::chrono::milliseconds hang_threshold{6000};
};

struct state
{
    config cfg;
    engine::state engine; // this secondary's own full FlutterEngine wrapper -- reset to a fresh instance on every (re)start, see secondary_engine_impl.hpp's detail::start_engine.
    backend::output out{};

    EGLSurface pbuffer{EGL_NO_SURFACE};
    std::uint32_t shared_tex{};
    // Dedicated, short-lived context used only to create/delete shared_tex.
    // Deliberately NOT primary.resource_ctx/upload_ctx: those are Flutter's
    // own internal contexts, used on its own internal threads whenever it
    // needs them -- making one of them current from here too, with no
    // coordination, races Flutter's own use of it (confirmed: this was
    // exactly the failure the de-risking spike caught). A fresh context
    // that shares with primary.gl_ctx is in the same share group (so the
    // texture it creates is visible everywhere) without touching anything
    // Flutter itself uses. Unlike `engine`, this and `pbuffer`/`shared_tex`
    // persist across restarts -- none of them are what a hung runner
    // thread actually affects.
    EGLContext creator_ctx{EGL_NO_CONTEXT};

    std::atomic<bool> vsync_running{false};
    std::thread vsync_thread;
    bool engine_alive{false}; // guards detail::stop_engine against a redundant second call.

    // Step 5 (auto-restart).
    restart_policy::state restart_state;
    std::atomic<std::int64_t> last_heartbeat_ms{0};
    std::atomic<bool> stop_requested{false};
    std::mutex restart_mutex;
    std::condition_variable restart_cv; // paired with restart_mutex; wakes an interruptible wait (heartbeat-interval or backoff).
    std::thread watchdog_thread;
};

inline auto create(state& s, config cfg) -> result<void>;
inline auto destroy(state& s) -> void;

} // namespace shell::secondary_engine

#include <shell/secondary_engine_impl.hpp>

#endif // SHELL_RENDERER_GL
