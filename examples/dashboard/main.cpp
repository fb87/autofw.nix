// shell_dashboard -- standalone dashboard HMI example: a real digital EV
// instrument cluster + the gallery as infotainment, both genuinely
// OS-process-isolated, composited side by side on one display via
// shell::single_display. Replaces what used to be shell_app's hidden
// --cluster-assets/--infotainment-assets flags -- this is app-specific
// demo code, so it lives in its own binary under examples/, built on top
// of the same generic shell::single_display primitive any other
// standalone consumer uses.
#include <cstdio>
#include <cstring>
#include <string>

#include <shell/bundle_paths.hpp>
#include <shell/child_renderer.hpp>
#include <shell/renderer.hpp>
#include <shell/single_display.hpp>

#include "dashboard.hpp"

namespace
{
struct options
{
    std::string assets, icu, aot_lib;
    std::string renderer{"gl"};
    dashboard::options dash;
};

void print_usage(const char* prog)
{
    std::fprintf(stderr,
                 "usage: %s --assets P --cluster-assets P --infotainment-assets P [options]\n"
                 "  --assets P                 dashboard chrome Flutter asset dir (required)\n"
                 "  --icu P / --aot-lib P       defaults: <assets>/icudtl.dat, auto-detected app.so\n"
                 "  --cluster-assets P / --cluster-icu P / --cluster-aot-lib P\n"
                 "  --infotainment-assets P / --infotainment-icu P / --infotainment-aot-lib P\n"
                 "  --renderer R                gl | software (default gl; dashboard needs gl)\n",
                 prog);
}

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
        if (arg_value(a, "--assets", o.assets) || arg_value(a, "--icu", o.icu) ||
            arg_value(a, "--aot-lib", o.aot_lib) || arg_value(a, "--renderer", o.renderer) ||
            arg_value(a, "--cluster-assets", o.dash.cluster_assets) ||
            arg_value(a, "--cluster-icu", o.dash.cluster_icu) ||
            arg_value(a, "--cluster-aot-lib", o.dash.cluster_aot_lib) ||
            arg_value(a, "--infotainment-assets", o.dash.infotainment_assets) ||
            arg_value(a, "--infotainment-icu", o.dash.infotainment_icu) ||
            arg_value(a, "--infotainment-aot-lib", o.dash.infotainment_aot_lib))
        {
            continue;
        }
        if (a == "--help" || a == "-h")
        {
            print_usage(argv[0]);
            std::exit(0);
        }
    }
    return o;
}
} // namespace

int main(int argc, char** argv)
{
    // shell::isolated_app's child re-execs THIS SAME binary with
    // --child-renderer -- must dispatch into shell::child_renderer::run()
    // instead of our own dashboard CLI handling below, or the re-exec'd
    // process sees "--assets" but no "--cluster-assets"/"--infotainment-
    // assets" and fails our own validation instead of becoming the
    // headless child it's supposed to be.
    if (auto rc = shell::child_renderer::maybe_run_as_child(argc, argv))
        return *rc;

    options opts = parse_args(argc, argv);
    if (opts.assets.empty() || opts.dash.cluster_assets.empty() ||
        opts.dash.infotainment_assets.empty())
    {
        print_usage(argv[0]);
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
    cb.on_ready = [&](shell::engine::state& engine)
    {
        primary = &engine;
        dashboard_active = dashboard::setup(engine, opts.dash, engine.out->width,
                                            engine.out->height, apps);
    };
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
    cb.on_before_destroy = [&](shell::engine::state& engine)
    {
        if (dashboard_active)
            dashboard::teardown(engine, apps);
    };

    return shell::single_display::run(cfg, cb);
}
