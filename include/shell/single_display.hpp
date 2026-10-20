#pragma once

// The generic "run one display" primitive: backend connect -> engine::init
// -> blocking run loop -> teardown, parameterized just enough for a
// standalone test/example binary to use instead of duplicating shell_app's
// own backend boilerplate.
//
// Deliberately simpler than shell_app's own run_one_display (src/main.cpp):
// this only ever serves ONE display per process, so there's no DRM card-
// sharing refcount across sibling outputs and no input_router shared across
// many displays -- for DRM it still reuses backend::drm's existing
// find_connected_device/open_card/open_output (already correct, already
// audited this session), just owns them privately, and creates a private
// input_router with one catch-all rule (every input device routed to this
// one display). shell_app's own multi-display core is NOT rewritten in
// terms of this -- it stays as its own, already-proven implementation; some
// connect/init/run/teardown boilerplate necessarily exists in both places.
//
// Installs its own SIGTERM/SIGINT handler (same g_stop_all-pointer pattern
// proven in src/main.cpp, generalized here) so every standalone binary that
// uses this gets graceful shutdown for free.

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

#include <shell/engine.hpp>
#include <shell/input.hpp>
#include <shell/result.hpp>

namespace shell::single_display
{

struct config
{
    engine::config engine_cfg;
#ifdef SHELL_BACKEND_WAYLAND
    std::uint32_t width{1280};
    std::uint32_t height{720};
#else
    std::string drm_device; // empty = auto-detect the first connected card.
#endif
};

struct callbacks
{
    // Called once engine::init() succeeds, on run()'s own calling thread,
    // before the blocking event loop starts. Use this to attach layers,
    // spawn a test driver thread, call an app's own setup function, etc.
    std::function<void(engine::state&)> on_ready;
    // Position-agnostic pointer/key hooks. Left unset (the default),
    // events are forwarded straight to engine::send_pointer/send_key --
    // set these for app-specific routing (e.g. a multi-pane dashboard
    // hit-testing a tap before deciding where it goes).
    std::function<void(const input::pointer_event&)> on_pointer;
    std::function<void(std::uint32_t, bool)> on_key;
    // Called once the backend's run loop has ended (shutdown is already
    // underway), after any driver thread the caller spawned in on_ready
    // should itself have been joined/stopped, but BEFORE engine::destroy()
    // -- the mirror image of on_ready. Use this to tear down whatever
    // on_ready set up (e.g. shell::isolated_app children).
    std::function<void(engine::state&)> on_before_destroy;
};

// Blocks until the backend's run loop ends (SIGTERM/SIGINT, a compositor-
// initiated close, or a DRM backend error) and tears everything down.
// Returns a process-ready exit code: 0 on a clean run, 1 on any setup
// failure (logged via shell::log before returning).
inline auto run(const config& cfg, const callbacks& cb) -> int;

} // namespace shell::single_display

#include <shell/single_display_impl.hpp>
