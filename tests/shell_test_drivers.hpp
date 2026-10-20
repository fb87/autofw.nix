#pragma once

// Generic shell framework self-tests -- these exercise engine/layer/
// texture/secondary_engine/isolated_app primitives directly, not any
// particular demo app, so they're generic shell tests, not app-specific
// examples. Each gets its own standalone binary under tests/test_<name>/
// (see that directory's main.cpp, which spawns the matching function here
// via shell::single_display::run's on_ready hook) instead of a hidden
// --test-* flag on the production shell_app binary.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <signal.h>

#include <shell/engine.hpp>
#include <shell/log.hpp>

#ifdef SHELL_RENDERER_GL
#include <shell/isolated_app.hpp>
#include <shell/secondary_engine.hpp>
#endif

namespace shell_tests
{

constexpr std::int64_t kDemoTextureId = 1001;

inline void run_demo_texture(shell::engine::state* e, bool* running)
{
    constexpr std::uint32_t kW = 640, kH = 360;
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(kW) * kH * 4);
    std::uint64_t frame = 0;
    while (*running)
    {
        double t = double(frame % 120) / 120.0;
        for (std::uint32_t y = 0; y < kH; ++y)
        {
            for (std::uint32_t x = 0; x < kW; ++x)
            {
                auto* p = &rgba[(static_cast<std::size_t>(y) * kW + x) * 4];
                p[0] = static_cast<std::uint8_t>((x * 255) / kW);
                p[1] = static_cast<std::uint8_t>((y * 255) / kH);
                p[2] = static_cast<std::uint8_t>(255 - (x * 255) / kW);
                p[3] = 255;
                int bx = static_cast<int>(t * kW);
                if (static_cast<int>(x) > bx && static_cast<int>(x) < bx + 40)
                {
                    p[0] = p[1] = p[2] = 255;
                }
            }
        }
        shell::engine::push_texture_frame(*e, kDemoTextureId, kW, kH, rgba);
        ++frame;
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }
}

// Drive a scripted, deterministic sequence of key events straight through
// engine::send_key (the same code path compositor key events use). The Dart
// app mirrors each back via a method channel so a test can assert the round
// trip.
inline void run_input_test(shell::engine::state* e, bool* running)
{
    // (evdev code, label): cover letters, digits, punctuation, editing, nav.
    static const std::pair<std::uint32_t, const char*> kKeys[] = {
        {30, "a"}, {48, "b"}, {46, "c"}, {32, "d"}, {18, "e"}, {33, "f"}, {34, "g"},
        {35, "h"}, {23, "i"}, {36, "j"}, {37, "k"}, {38, "l"}, {50, "m"}, {49, "n"},
        {24, "o"}, {25, "p"}, {16, "q"}, {19, "r"}, {31, "s"}, {20, "t"}, {22, "u"},
        {47, "v"}, {17, "w"}, {45, "x"}, {21, "y"}, {44, "z"}, {2, "1"}, {3, "2"},
        {4, "3"}, {5, "4"}, {6, "5"}, {7, "6"}, {8, "7"}, {9, "8"}, {10, "9"}, {11, "0"},
        // Navigation tested while the field is still connected (before Enter
        // performs the field action and unfocuses it).
        {103, "up"}, {108, "down"}, {105, "left"}, {106, "right"}, {102, "home"},
        {107, "end"}, {104, "pageup"}, {109, "pagedown"}, {110, "insert"}, {111, "delete"},
        {57, "space"}, {14, "backspace"}, {28, "enter"}, {15, "tab"},
    };
    // Let the app finish booting and the text field focus.
    std::this_thread::sleep_for(std::chrono::seconds(6));
    for (const auto& [code, label] : kKeys)
    {
        // Queue on the event loop (platform) thread via the pending queue; the
        // queued events are drained by backend.cb.on_poll each loop iteration.
        shell::engine::queue_key(*e, code, true);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        shell::engine::queue_key(*e, code, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
    }
    // Key-repeat test: hold 'x' (code 45) so the compositor would fire many
    // back-to-back key-downs. Each down of a printable key must insert one
    // character, so 10 downs -> 10 'x' in the text.
    constexpr int kRepeats = 10;
    shell::log::info("[test-input] repeat test: " + std::to_string(kRepeats) +
                     " down-events of 'x'");
    for (int i = 0; i < kRepeats; ++i)
    {
        shell::engine::queue_key(*e, 45, true); // KEY_X down (a repeat)
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    shell::engine::queue_key(*e, 45, false); // release
    shell::log::info("[test-input] queued " + std::to_string(sizeof(kKeys) / sizeof(kKeys[0])) +
                     " key sequences + " + std::to_string(kRepeats) + " repeats");
    *running = false;
}

// Drive a scripted sequence of layer add/visibility/opacity/remove changes
// straight through engine::add_layer/set_layer_*/remove_layer -- the shell/
// layers channel Dart side.
inline void run_layer_test(shell::engine::state* e, bool* running)
{
    // Let the app finish booting (its Dart side fetches the initial
    // snapshot via shell/layers "list" in its own initState).
    std::this_thread::sleep_for(std::chrono::seconds(3));

    constexpr shell::stage::id kStageId = 1;
    constexpr shell::layer::id kBackgroundId = 1;
    constexpr shell::layer::id kApplicationId = 2;
    constexpr shell::layer::id kOverlayId = 3;

    shell::engine::add_layer(
        *e, shell::layer::state{.value = kBackgroundId,
                                .stage = kStageId,
                                .kind = shell::layer::role::background,
                                .bounds = {0, 0, 1280, 720}});
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    shell::engine::add_layer(
        *e, shell::layer::state{.value = kApplicationId,
                                .stage = kStageId,
                                .kind = shell::layer::role::application,
                                .bounds = {40, 40, 400, 300}});
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    shell::engine::add_layer(
        *e, shell::layer::state{.value = kOverlayId,
                                .stage = kStageId,
                                .kind = shell::layer::role::overlay,
                                .bounds = {480, 40, 300, 200}});
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    shell::engine::set_layer_visible(*e, kApplicationId, false);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    shell::engine::set_layer_visible(*e, kApplicationId, true);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    for (float opacity : {0.25F, 0.5F, 0.75F, 1.0F})
    {
        shell::engine::set_layer_opacity(*e, kOverlayId, opacity);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    shell::engine::remove_layer(*e, kBackgroundId);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    shell::log::info("[test-layers] add/visibility/opacity/remove sequence done");
    *running = false;
}

#ifdef SHELL_RENDERER_GL

// Spawns TWO headless secondary engines (zero-copy GL texture sharing, see
// shell/secondary_engine.hpp) and attaches each as a live texture to one of
// run_layer_test's scripted layers -- "application" (id 2) and "overlay"
// (id 3). Two, not one: this is the only way to actually exercise "per-layer"
// (plural) rather than resting on architecture-by-inspection, and it proves
// two independent engines each get their own lazily-created FBO in the same
// share group without interfering.
inline void run_secondary_engine_test(shell::engine::state* primary, const std::string& assets,
                                      const std::string& icu, const std::string& aot_lib,
                                      bool* running)
{
    // Run after run_layer_test's add sequence (background/application/
    // overlay are all added within the first ~1s of its own 3s initial
    // sleep) but before it removes the background layer, so both target
    // layers are guaranteed to exist by the time we attach to them.
    std::this_thread::sleep_for(std::chrono::seconds(4));

    constexpr shell::layer::id kApplicationId = 2;
    constexpr shell::layer::id kOverlayId = 3;
    constexpr shell::texture::id kAppTextureId = 900;
    constexpr shell::texture::id kOverlayTextureId = 901;
    constexpr shell::surface::id kAppSurfaceId = 1;
    constexpr shell::surface::id kOverlaySurfaceId = 2;

    struct slot
    {
        const char* label;
        shell::layer::id layer;
        shell::texture::id texture;
        shell::surface::id surface;
        std::uint32_t width, height;
        shell::secondary_engine::state engine; // contains std::atomic/mutex -- not
        bool spawned{};                        // copyable or movable, so this can
    };                                          // never live in a std::vector<slot>
                                                 // (reserve()/emplace_back() both
                                                 // require a well-formed move ctor
                                                 // even when never actually called).
    slot app_slot{"app", kApplicationId, kAppTextureId, kAppSurfaceId, 400, 300};
    slot overlay_slot{"overlay", kOverlayId, kOverlayTextureId, kOverlaySurfaceId, 300, 200};
    slot* slots[] = {&app_slot, &overlay_slot};

    for (slot* s : slots)
    {
        if (!shell::engine::register_texture(*primary, s->texture))
        {
            shell::log::info(std::string("[test-secondary-engine] register_texture(") +
                             s->label + ") failed");
            continue;
        }
        shell::secondary_engine::config cfg{
            .assets = assets, .icu_data = icu, .aot_library = aot_lib,
            .width = s->width, .height = s->height, .pixel_ratio = 1.0, .fps = 60.0,
            .log_tag = std::string("sec-") + s->label, .primary = primary,
            .target_texture = s->texture,
        };
        if (auto err = shell::secondary_engine::create(s->engine, cfg); !err)
        {
            shell::log::info(std::string("[test-secondary-engine] create(") + s->label +
                             ") failed: " + err.error().message);
            shell::engine::unregister_texture(*primary, s->texture);
            continue;
        }
        shell::engine::attach_surface(
            *primary, shell::surface::state{.value = s->surface, .layer = s->layer,
                                            .size = {s->width, s->height},
                                            .texture = s->texture, .visible = true});
        s->spawned = true;
        shell::log::info(std::string("[test-secondary-engine] ") + s->label + " attached");
    }

    std::this_thread::sleep_for(std::chrono::seconds(6));

    for (slot* s : slots)
    {
        if (!s->spawned)
            continue;
        shell::engine::detach_surface(*primary, s->surface);
        shell::secondary_engine::destroy(s->engine);
        shell::engine::unregister_texture(*primary, s->texture);
    }
    shell::log::info("[test-secondary-engine] torn down");
    *running = false;
}

// Spawns ONE real OS-process-isolated Flutter app (shell::isolated_app --
// a genuinely separate child process, not an in-process secondary engine)
// and attaches it, as a live texture, to run_layer_test's "application"
// layer (id 2).
inline void run_isolated_app_test(shell::engine::state* primary, const std::string& assets,
                                  const std::string& icu, const std::string& aot_lib,
                                  bool* running)
{
    std::this_thread::sleep_for(std::chrono::seconds(4));

    constexpr shell::layer::id kLayerId = 2; // "application"
    constexpr shell::texture::id kTextureId = 950;
    constexpr shell::surface::id kSurfaceId = 3;
    constexpr std::uint32_t kWidth = 400;
    constexpr std::uint32_t kHeight = 300;

    if (!shell::engine::register_texture(*primary, kTextureId))
    {
        shell::log::info("[test-isolated-app] register_texture failed");
        *running = false;
        return;
    }

    shell::isolated_app::state app;
    shell::isolated_app::config cfg{
        .assets = assets, .icu_data = icu, .aot_library = aot_lib,
        .width = kWidth, .height = kHeight, .log_tag = "iso", .primary = primary,
        .target_texture = kTextureId,
    };
    if (auto err = shell::isolated_app::create(app, cfg); !err)
    {
        shell::log::info("[test-isolated-app] create failed: " + err.error().message);
        shell::engine::unregister_texture(*primary, kTextureId);
        *running = false;
        return;
    }
    // Logged so an external test script can target this exact process for
    // the kill -SEGV containment test -- the whole payoff of Step 4.
    shell::log::info("[test-isolated-app] child pid=" + std::to_string(app.child.pid));
    shell::engine::attach_surface(
        *primary, shell::surface::state{.value = kSurfaceId, .layer = kLayerId,
                                        .size = {kWidth, kHeight}, .texture = kTextureId,
                                        .visible = true});
    shell::log::info("[test-isolated-app] attached");

    std::this_thread::sleep_for(std::chrono::seconds(8));

    shell::engine::detach_surface(*primary, kSurfaceId);
    shell::isolated_app::destroy(app); // safe no-op if the child already died.
    shell::engine::unregister_texture(*primary, kTextureId);
    shell::log::info("[test-isolated-app] torn down");
    *running = false;
}

// Step 5: same shape as run_isolated_app_test, but with restart enabled --
// proves *recovery*, not just containment. The test driver itself sends
// the SIGSEGV directly (kill(app.child.pid, ...), no external bash
// choreography needed) and asserts a second, different child pid shows up
// and content resumes afterward.
inline void run_isolated_app_restart_test(shell::engine::state* primary,
                                          const std::string& assets, const std::string& icu,
                                          const std::string& aot_lib, bool* running)
{
    std::this_thread::sleep_for(std::chrono::seconds(4));

    constexpr shell::layer::id kLayerId = 2; // "application"
    constexpr shell::texture::id kTextureId = 960;
    constexpr shell::surface::id kSurfaceId = 4;
    constexpr std::uint32_t kWidth = 400;
    constexpr std::uint32_t kHeight = 300;

    if (!shell::engine::register_texture(*primary, kTextureId))
    {
        shell::log::info("[test-isolated-app-restart] register_texture failed");
        *running = false;
        return;
    }

    shell::isolated_app::state app;
    shell::isolated_app::config cfg{
        .assets = assets, .icu_data = icu, .aot_library = aot_lib,
        .width = kWidth, .height = kHeight, .log_tag = "iso-r", .primary = primary,
        .target_texture = kTextureId, .target_surface = kSurfaceId,
    };
    cfg.restart.enabled = true;
    cfg.restart.initial_delay = std::chrono::milliseconds(500);
    cfg.restart.max_delay = std::chrono::milliseconds(2000);
    if (auto err = shell::isolated_app::create(app, cfg); !err)
    {
        shell::log::info("[test-isolated-app-restart] create failed: " + err.error().message);
        shell::engine::unregister_texture(*primary, kTextureId);
        *running = false;
        return;
    }
    pid_t first_pid = app.child.pid;
    shell::log::info("[test-isolated-app-restart] child pid=" + std::to_string(first_pid));
    shell::engine::attach_surface(
        *primary, shell::surface::state{.value = kSurfaceId, .layer = kLayerId,
                                        .size = {kWidth, kHeight}, .texture = kTextureId,
                                        .visible = true});
    shell::log::info("[test-isolated-app-restart] attached");

    std::this_thread::sleep_for(std::chrono::seconds(2));

    shell::log::info("[test-isolated-app-restart] sending SIGSEGV to " +
                     std::to_string(first_pid));
    kill(first_pid, SIGSEGV);

    std::this_thread::sleep_for(std::chrono::seconds(5));

    shell::log::info("[test-isolated-app-restart] second child pid=" +
                     std::to_string(app.child.pid) +
                     (app.child.pid > 0 && app.child.pid != first_pid ? " (respawned)"
                                                                      : " (NOT respawned)"));

    std::this_thread::sleep_for(std::chrono::seconds(2));

    shell::engine::detach_surface(*primary, kSurfaceId);
    shell::isolated_app::destroy(app);
    shell::engine::unregister_texture(*primary, kTextureId);
    shell::log::info("[test-isolated-app-restart] torn down");
    *running = false;
}

// Step 5: proves shell::secondary_engine's watchdog + restart. There's no
// safe way to externally kill just one thread of this process the way
// run_isolated_app_restart_test kills a whole child process, so this
// induces a controlled, real stall instead: the test driver itself locks
// app.engine.runner_mutex for longer than hang_threshold, which blocks the
// watchdog thread's own attempt to post a heartbeat into that same
// engine's runner_q -- exercising the exact detection path (a heartbeat
// that doesn't get processed in time) without needing any test-only hook
// inside the shipped header.
inline void run_secondary_engine_watchdog_test(shell::engine::state* primary,
                                               const std::string& assets, const std::string& icu,
                                               const std::string& aot_lib, bool* running)
{
    std::this_thread::sleep_for(std::chrono::seconds(4));

    constexpr shell::layer::id kLayerId = 3; // "overlay"
    constexpr shell::texture::id kTextureId = 970;
    constexpr shell::surface::id kSurfaceId = 5;
    constexpr std::uint32_t kWidth = 300;
    constexpr std::uint32_t kHeight = 200;

    if (!shell::engine::register_texture(*primary, kTextureId))
    {
        shell::log::info("[test-secondary-engine-watchdog] register_texture failed");
        *running = false;
        return;
    }

    shell::secondary_engine::state app;
    shell::secondary_engine::config cfg{
        .assets = assets, .icu_data = icu, .aot_library = aot_lib,
        .width = kWidth, .height = kHeight, .pixel_ratio = 1.0, .fps = 60.0,
        .log_tag = "sec-wd", .primary = primary, .target_texture = kTextureId,
        .target_surface = kSurfaceId,
    };
    cfg.restart.enabled = true;
    cfg.restart.initial_delay = std::chrono::milliseconds(500);
    cfg.restart.max_delay = std::chrono::milliseconds(2000);
    cfg.heartbeat_interval = std::chrono::milliseconds(500);
    cfg.hang_threshold = std::chrono::milliseconds(1500);
    if (auto err = shell::secondary_engine::create(app, cfg); !err)
    {
        shell::log::info("[test-secondary-engine-watchdog] create failed: " + err.error().message);
        shell::engine::unregister_texture(*primary, kTextureId);
        *running = false;
        return;
    }
    shell::engine::attach_surface(
        *primary, shell::surface::state{.value = kSurfaceId, .layer = kLayerId,
                                        .size = {kWidth, kHeight}, .texture = kTextureId,
                                        .visible = true});
    shell::log::info("[test-secondary-engine-watchdog] attached");

    std::this_thread::sleep_for(std::chrono::seconds(2));

    shell::log::info("[test-secondary-engine-watchdog] inducing a stall (holding runner_mutex)");
    {
        std::lock_guard lock(app.engine.runner_mutex);
        // Comfortably longer than hang_threshold -- the watchdog's
        // heartbeat post can't even enqueue while this lock is held, so it
        // sees no progress and declares a hang.
        std::this_thread::sleep_for(cfg.hang_threshold * 3);
    }
    shell::log::info("[test-secondary-engine-watchdog] stall released");

    std::this_thread::sleep_for(std::chrono::seconds(5));

    shell::engine::detach_surface(*primary, kSurfaceId);
    shell::secondary_engine::destroy(app);
    shell::engine::unregister_texture(*primary, kTextureId);
    shell::log::info("[test-secondary-engine-watchdog] torn down");
    *running = false;
}

#endif // SHELL_RENDERER_GL

} // namespace shell_tests
