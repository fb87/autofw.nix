#pragma once

// The dashboard HMI: a real digital EV instrument cluster on the left, the
// gallery as infotainment content on the right, both genuinely OS-process-
// isolated via shell::isolated_app with Step 5 auto-restart enabled. This
// is app-specific (demo) code, not part of the generic shell:: framework --
// it lives under examples/ and is built as its own standalone binary
// (see main.cpp) on top of shell::single_display, same as any other
// shell:: consumer. The dashboard app itself (--assets) owns the top
// status bar and bottom dock as plain Flutter chrome in its own widget
// tree -- those are not shell::layer entries and need no C++ wiring here.

#include <chrono>
#include <cstdint>
#include <string>

#include <shell/engine.hpp>
#include <shell/input.hpp>
#include <shell/isolated_app.hpp> // self-guards with #ifdef SHELL_RENDERER_GL

// The dashboard needs shell::isolated_app, which only exists under
// SHELL_RENDERER_GL -- this whole header is meaningless without it, same
// as isolated_app.hpp itself.
#ifdef SHELL_RENDERER_GL

namespace dashboard
{

constexpr shell::stage::id kStageId = 10;
constexpr shell::layer::id kClusterLayerId = 10;       // examples/dashboard/lib/layer_ids.dart must match.
constexpr shell::layer::id kInfotainmentLayerId = 11; // examples/dashboard/lib/layer_ids.dart must match.
constexpr shell::texture::id kClusterTextureId = 2000;
constexpr shell::texture::id kInfotainmentTextureId = 2001;
constexpr shell::surface::id kClusterSurfaceId = 10;
constexpr shell::surface::id kInfotainmentSurfaceId = 11;
// Kept in sync BY CONVENTION with examples/dashboard's StatusBar/Dock
// SizedBox heights -- used only so the shell::layer bounds pushed over
// 'shell/layers' accurately describe the infotainment layer's visible
// rectangle for introspection. The dashboard's actual Dart layout derives
// its own split live from LayoutBuilder, not from these constants.
constexpr float kStatusBarHeight = 56.0F;
constexpr float kDockHeight = 96.0F;

struct options
{
    std::string cluster_assets;
    std::string cluster_icu;
    std::string cluster_aot_lib;
    std::string infotainment_assets;
    std::string infotainment_icu;
    std::string infotainment_aot_lib;
};

struct apps
{
    shell::isolated_app::state cluster;
    shell::isolated_app::state infotainment;
    bool cluster_spawned{};
    bool infotainment_spawned{};
    // On-screen rectangles, same primary-window pixel space pointer_event
    // coordinates already arrive in -- populated by setup(), consulted by
    // dispatch_pointer/dispatch_key to hit-test a tap and translate it
    // into each pane's own local coordinate space before forwarding
    // (shell::isolated_app itself is position-agnostic).
    shell::layer::geometry cluster_bounds{};
    shell::layer::geometry infotainment_bounds{};
};

// Which pane (if any) a pointer/key event should go to.
enum class focus
{
    primary,
    cluster,
    infotainment,
};

inline bool hit_inside(const shell::layer::geometry& b, double x, double y)
{
    return x >= b.x && x < b.x + b.width && y >= b.y && y < b.y + b.height;
}

inline focus hit_test(const apps& a, bool active, double x, double y)
{
    if (active)
    {
        if (hit_inside(a.cluster_bounds, x, y))
            return focus::cluster;
        if (hit_inside(a.infotainment_bounds, x, y))
            return focus::infotainment;
    }
    return focus::primary;
}

inline shell::input::pointer_event translate(const shell::input::pointer_event& ev,
                                              const shell::layer::geometry& bounds)
{
    shell::input::pointer_event local = ev;
    local.x -= bounds.x;
    local.y -= bounds.y;
    return local;
}

inline bool setup(shell::engine::state& primary, const options& opts, std::uint32_t display_w,
                  std::uint32_t display_h, apps& a)
{
    const std::uint32_t left_w = display_w / 2;
    const std::uint32_t right_w = display_w - left_w;
    const std::uint32_t chrome_h = static_cast<std::uint32_t>(kStatusBarHeight + kDockHeight);
    const std::uint32_t right_h = display_h > chrome_h ? display_h - chrome_h : display_h;

    a.cluster_bounds = {0, 0, static_cast<float>(left_w), static_cast<float>(display_h)};
    a.infotainment_bounds = {static_cast<float>(left_w), kStatusBarHeight,
                             static_cast<float>(right_w), static_cast<float>(right_h)};

    shell::engine::add_layer(
        primary, shell::layer::state{.value = kClusterLayerId, .stage = kStageId,
                                     .kind = shell::layer::role::application,
                                     .bounds = a.cluster_bounds});
    shell::engine::add_layer(
        primary, shell::layer::state{.value = kInfotainmentLayerId, .stage = kStageId,
                                     .kind = shell::layer::role::application,
                                     .bounds = a.infotainment_bounds});

    if (!shell::engine::register_texture(primary, kClusterTextureId))
    {
        shell::log::info("[dashboard] register_texture(cluster) failed");
        return false;
    }
    shell::isolated_app::config cluster_cfg{
        .assets = opts.cluster_assets, .icu_data = opts.cluster_icu,
        .aot_library = opts.cluster_aot_lib, .width = left_w, .height = display_h,
        .log_tag = "cluster", .primary = &primary, .target_texture = kClusterTextureId,
        .target_surface = kClusterSurfaceId};
    cluster_cfg.restart.enabled = true;
    cluster_cfg.restart.initial_delay = std::chrono::milliseconds(500);
    cluster_cfg.restart.max_delay = std::chrono::milliseconds(5000);
    if (auto err = shell::isolated_app::create(a.cluster, cluster_cfg); !err)
    {
        shell::log::info("[dashboard] create(cluster) failed: " + err.error().message);
        shell::engine::unregister_texture(primary, kClusterTextureId);
        return false;
    }
    shell::log::info("[dashboard] cluster child pid=" + std::to_string(a.cluster.child.pid));
    shell::engine::attach_surface(
        primary, shell::surface::state{.value = kClusterSurfaceId, .layer = kClusterLayerId,
                                       .size = {left_w, display_h}, .texture = kClusterTextureId,
                                       .visible = true});
    a.cluster_spawned = true;

    if (!shell::engine::register_texture(primary, kInfotainmentTextureId))
    {
        shell::log::info("[dashboard] register_texture(infotainment) failed");
        return a.cluster_spawned;
    }
    shell::isolated_app::config info_cfg{
        .assets = opts.infotainment_assets, .icu_data = opts.infotainment_icu,
        .aot_library = opts.infotainment_aot_lib, .width = right_w, .height = right_h,
        .log_tag = "infotainment", .primary = &primary,
        .target_texture = kInfotainmentTextureId, .target_surface = kInfotainmentSurfaceId};
    info_cfg.restart.enabled = true;
    info_cfg.restart.initial_delay = std::chrono::milliseconds(500);
    info_cfg.restart.max_delay = std::chrono::milliseconds(5000);
    if (auto err = shell::isolated_app::create(a.infotainment, info_cfg); !err)
    {
        shell::log::info("[dashboard] create(infotainment) failed: " + err.error().message);
        shell::engine::unregister_texture(primary, kInfotainmentTextureId);
        return a.cluster_spawned;
    }
    shell::log::info("[dashboard] infotainment child pid=" +
                     std::to_string(a.infotainment.child.pid));
    shell::engine::attach_surface(
        primary, shell::surface::state{.value = kInfotainmentSurfaceId,
                                       .layer = kInfotainmentLayerId,
                                       .size = {right_w, right_h},
                                       .texture = kInfotainmentTextureId, .visible = true});
    a.infotainment_spawned = true;
    return true;
}

inline void teardown(shell::engine::state& primary, apps& a)
{
    if (a.infotainment_spawned)
    {
        shell::engine::detach_surface(primary, kInfotainmentSurfaceId);
        shell::isolated_app::destroy(a.infotainment);
        shell::engine::unregister_texture(primary, kInfotainmentTextureId);
    }
    if (a.cluster_spawned)
    {
        shell::engine::detach_surface(primary, kClusterSurfaceId);
        shell::isolated_app::destroy(a.cluster);
        shell::engine::unregister_texture(primary, kClusterTextureId);
    }
    shell::engine::remove_layer(primary, kInfotainmentLayerId);
    shell::engine::remove_layer(primary, kClusterLayerId);
}

// Position-based dispatch: hit-test a fresh pointer-down against the two
// panes' bounds, remember the result in `cur_focus` (a drag that leaves the
// pane it started on stays targeted there -- standard UI behavior, not
// re-hit-tested on every move/up), and route accordingly. Keyboard has no
// inherent position, so it just follows whichever pane the most recent
// pointer-down landed on ("tap to focus"). Used identically by both
// backends via shell::single_display::callbacks -- unlike shell_app's own
// Wayland-vs-DRM split (backend.cb.on_pointer directly vs. input_router's
// dispatch_hook), single_display::run already abstracts that difference
// away, so this dashboard code only ever deals with plain pointer/key
// callbacks.
inline void dispatch_pointer(apps& a, bool active, focus& cur_focus, shell::engine::state& primary,
                             const shell::input::pointer_event& ev)
{
    if (ev.phase == shell::input::pointer_phase::down)
        cur_focus = hit_test(a, active, ev.x, ev.y);
    switch (cur_focus)
    {
    case focus::cluster:
        shell::isolated_app::send_pointer(a.cluster, translate(ev, a.cluster_bounds));
        break;
    case focus::infotainment:
        shell::isolated_app::send_pointer(a.infotainment, translate(ev, a.infotainment_bounds));
        break;
    case focus::primary:
        shell::engine::send_pointer(primary, ev);
        break;
    }
}

inline void dispatch_key(apps& a, focus cur_focus, shell::engine::state& primary,
                         std::uint32_t code, bool down)
{
    switch (cur_focus)
    {
    case focus::cluster:
        shell::isolated_app::send_key(a.cluster, code, down);
        break;
    case focus::infotainment:
        shell::isolated_app::send_key(a.infotainment, code, down);
        break;
    case focus::primary:
        shell::engine::send_key(primary, code, down);
        break;
    }
}

} // namespace dashboard

#endif // SHELL_RENDERER_GL
