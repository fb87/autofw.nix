// test_demo_texture -- standalone binary for shell_tests::run_demo_texture
// (see ../shell_test_drivers.hpp), using shell::single_display instead of
// a hidden --test-texture flag on the production shell_app binary.
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

    std::thread driver;
    bool driver_running = false;

    shell::single_display::callbacks cb{};
    cb.on_ready = [&](shell::engine::state& engine)
    {
        if (shell::engine::register_texture(engine, shell_tests::kDemoTextureId))
        {
            driver_running = true;
            driver = std::thread([&] { shell_tests::run_demo_texture(&engine, &driver_running); });
        }
    };
    cb.on_before_destroy = [&](shell::engine::state& engine)
    {
        if (driver.joinable())
        {
            driver_running = false;
            driver.join();
            shell::engine::unregister_texture(engine, shell_tests::kDemoTextureId);
        }
    };

    return shell::single_display::run(cfg, cb);
}
