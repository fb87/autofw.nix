// test_dashboard_input -- exercises the REAL position-based dispatcher
// (single_display::callbacks::on_pointer/on_key, called exactly as a
// genuine backend pointer/keyboard event would be) by injecting synthetic
// events at primary-screen coordinates known to fall inside each pane's
// bounds, then confirming via log output that they landed on the intended
// target. Lives alongside the dashboard example it tests (not under
// generic tests/) since it's entirely dashboard-specific.
//
// Unlike the original shell_app --test-dashboard-input (Wayland only, since
// DRM had no per-position dispatcher at the time), this now exercises
// either backend: shell::single_display abstracts the Wayland-vs-DRM input
// difference away, so the exact same on_pointer/on_key callbacks this test
// calls directly are also what a real backend event would reach.
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include <shell/bundle_paths.hpp>
#include <shell/child_renderer.hpp>
#include <shell/log.hpp>
#include <shell/renderer.hpp>
#include <shell/single_display.hpp>

#include "../dashboard.hpp"

namespace
{
struct options
{
    std::string assets, icu, aot_lib;
    std::string renderer{"gl"};
    dashboard::options dash;
};

bool arg_value(std::string_view arg, std::string_view flag, std::string& out)
{
    if (arg.size() <= flag.size() || arg.substr(0, flag.size()) != flag || arg[flag.size()] != '=')
        return false;
    out = std::string(arg.substr(flag.size() + 1));
    return true;
}

options parse_args(int argc, char** argv)
{
    options o;
    for (int i = 1; i < argc; ++i)
    {
        std::string_view a = argv[i];
        arg_value(a, "--assets", o.assets) || arg_value(a, "--icu", o.icu) ||
            arg_value(a, "--aot-lib", o.aot_lib) || arg_value(a, "--renderer", o.renderer) ||
            arg_value(a, "--cluster-assets", o.dash.cluster_assets) ||
            arg_value(a, "--cluster-icu", o.dash.cluster_icu) ||
            arg_value(a, "--cluster-aot-lib", o.dash.cluster_aot_lib) ||
            arg_value(a, "--infotainment-assets", o.dash.infotainment_assets) ||
            arg_value(a, "--infotainment-icu", o.dash.infotainment_icu) ||
            arg_value(a, "--infotainment-aot-lib", o.dash.infotainment_aot_lib);
    }
    return o;
}

// Give setup()'s children a few seconds to connect and the gallery's Form
// panel to auto-focus its text field, then inject taps/keys exactly as a
// real backend event would arrive.
void run_dashboard_input_test(const dashboard::apps& apps,
                              const shell::single_display::callbacks& cb, bool* running)
{
    std::this_thread::sleep_for(std::chrono::seconds(6));

    auto tap = [&](const shell::layer::geometry& bounds, double y_frac = 0.5)
    {
        double x = bounds.x + bounds.width / 2;
        double y = bounds.y + bounds.height * y_frac;
        cb.on_pointer(shell::input::pointer_event{
            .phase = shell::input::pointer_phase::down, .x = x, .y = y});
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        cb.on_pointer(
            shell::input::pointer_event{.phase = shell::input::pointer_phase::up, .x = x, .y = y});
    };
    auto type = [&](std::initializer_list<std::uint32_t> codes)
    {
        for (auto code : codes)
        {
            cb.on_key(code, true);
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            cb.on_key(code, false);
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
        }
    };

    shell::log::info("[test-dashboard-input] tapping cluster pane, sending a probe key");
    tap(apps.cluster_bounds);
    type({35}); // KEY_H -- cluster has no text field; exercises the dispatch+IPC path only.

    shell::log::info("[test-dashboard-input] tapping infotainment pane, typing 'hi'");
    // ~13% down from the top of the pane -- the gallery Form panel's
    // "Basic text field" row, confirmed against a real captured frame of
    // this exact layout (not a guess); dead-center landed on unrelated
    // content further down the form and defocused the field instead.
    tap(apps.infotainment_bounds, 0.13);
    type({35, 23}); // KEY_H, KEY_I -- lands in the gallery's already-focused Basic text field.

    *running = false;
}
} // namespace

int main(int argc, char** argv)
{
    // See examples/dashboard/main.cpp's identical check -- shell::
    // isolated_app's child re-execs this same binary with --child-renderer.
    if (auto rc = shell::child_renderer::maybe_run_as_child(argc, argv))
        return *rc;

    options opts = parse_args(argc, argv);
    if (opts.assets.empty() || opts.dash.cluster_assets.empty() ||
        opts.dash.infotainment_assets.empty())
    {
        std::fprintf(stderr,
                     "usage: %s --assets P --cluster-assets P --infotainment-assets P\n",
                     argv[0]);
        return 1;
    }
    shell::apply_default_bundle_paths(opts.assets, opts.icu, opts.aot_lib);
    shell::apply_default_bundle_paths(opts.dash.cluster_assets, opts.dash.cluster_icu,
                                      opts.dash.cluster_aot_lib);
    shell::apply_default_bundle_paths(opts.dash.infotainment_assets, opts.dash.infotainment_icu,
                                      opts.dash.infotainment_aot_lib);

    shell::renderer::type rtype = shell::renderer::type::automatic;
    if (opts.renderer == "software")
        rtype = shell::renderer::type::software;
    else if (opts.renderer == "gl")
        rtype = shell::renderer::type::gl;

    shell::single_display::config cfg{};
    cfg.engine_cfg = shell::engine::config{
        .assets = opts.assets, .icu_data = opts.icu, .aot_library = opts.aot_lib,
        .renderer = rtype};

    dashboard::apps apps;
    bool dashboard_active = false;
    dashboard::focus cur_focus = dashboard::focus::primary;
    shell::engine::state* primary = nullptr;

    shell::single_display::callbacks cb{};
    cb.on_pointer = [&](const shell::input::pointer_event& ev)
    {
        if (primary)
            dashboard::dispatch_pointer(apps, dashboard_active, cur_focus, *primary, ev);
    };
    cb.on_key = [&](std::uint32_t code, bool down)
    {
        if (primary)
            dashboard::dispatch_key(apps, cur_focus, *primary, code, down);
    };

    std::thread input_test;
    bool input_test_running = false;
    cb.on_ready = [&](shell::engine::state& engine)
    {
        primary = &engine;
        dashboard_active = dashboard::setup(engine, opts.dash, engine.out->width,
                                            engine.out->height, apps);
        if (dashboard_active)
        {
            input_test_running = true;
            input_test = std::thread(
                [&] { run_dashboard_input_test(apps, cb, &input_test_running); });
        }
    };
    cb.on_before_destroy = [&](shell::engine::state& engine)
    {
        if (input_test.joinable())
        {
            input_test_running = false;
            input_test.join();
        }
        if (dashboard_active)
            dashboard::teardown(engine, apps);
    };

    return shell::single_display::run(cfg, cb);
}
