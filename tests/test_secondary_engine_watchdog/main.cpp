// test_secondary_engine_watchdog -- standalone binary for
// shell_tests::run_secondary_engine_watchdog_test (see
// ../shell_test_drivers.hpp). Also runs run_layer_test, same as the
// original --test-secondary-engine-watchdog flag implied --test-layers.
#include <cstdio>
#include <string>
#include <string_view>
#include <thread>

#include <shell/args.hpp>
#include <shell/bundle_paths.hpp>
#include <shell/renderer.hpp>
#include <shell/single_display.hpp>

#include "../shell_test_drivers.hpp"

int main(int argc, char** argv)
{
    std::string assets, icu, aot_lib, renderer = "gl";
    for (int i = 1; i < argc; ++i)
    {
        std::string_view a = argv[i];
        shell::args::match(a, "--assets", assets) || shell::args::match(a, "--icu", icu) ||
            shell::args::match(a, "--aot-lib", aot_lib) ||
            shell::args::match(a, "--renderer", renderer);
    }
    if (assets.empty())
    {
        std::fprintf(stderr,
                     "usage: %s --assets P [--icu P] [--aot-lib P] [--renderer gl|software]\n",
                     argv[0]);
        return 1;
    }
    shell::apply_default_bundle_paths(assets, icu, aot_lib);

    shell::renderer::type rtype = shell::renderer::type::automatic;
    if (renderer == "software")
        rtype = shell::renderer::type::software;
    else if (renderer == "gl")
        rtype = shell::renderer::type::gl;

    shell::single_display::config cfg{};
    cfg.engine_cfg = shell::engine::config{
        .assets = assets, .icu_data = icu, .aot_library = aot_lib, .renderer = rtype};

    std::thread layer_driver, wd_driver;
    bool layer_running = false, wd_running = false;

    shell::single_display::callbacks cb{};
    cb.on_ready = [&](shell::engine::state& engine)
    {
        layer_running = true;
        layer_driver = std::thread([&] { shell_tests::run_layer_test(&engine, &layer_running); });
        wd_running = true;
        wd_driver = std::thread([&]
                                {
                                    shell_tests::run_secondary_engine_watchdog_test(
                                        &engine, assets, icu, aot_lib, &wd_running);
                                });
    };
    cb.on_before_destroy = [&](shell::engine::state&)
    {
        if (wd_driver.joinable())
        {
            wd_running = false;
            wd_driver.join();
        }
        if (layer_driver.joinable())
        {
            layer_running = false;
            layer_driver.join();
        }
    };

    return shell::single_display::run(cfg, cb);
}
