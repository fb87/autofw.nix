// shell main -- a minimal Flutter embedder shell (Wayland backend).
//
// Header-only shell framework: builds backend::wayland + engine::state and
// runs a Flutter app.
#include <cstdio>
#include <getopt.h>
#include <string>
#include <thread>
#include <vector>

#include <shell/shell.hpp>
#include <shell/engine.hpp>
#include <shell/renderer.hpp>
#ifdef SHELL_BACKEND_WAYLAND
#include <shell/backend/wayland.hpp>
#endif

namespace
{
struct options
{
    std::string assets;
    std::string icu;
    std::string renderer{"gl"};  // "gl" | "software"
    bool test_texture{};
};

void print_usage(const char* prog)
{
    std::fprintf(stderr,
                 "usage: %s [options]\n"
                 "  --assets P       Flutter asset directory (required)\n"
                 "  --icu P          icudtl.dat path (default <assets>/icudtl.dat)\n"
                 "  --renderer R     gl | software (default gl)\n"
                 "  --test-texture   run a demo external-texture producer\n"
                 "  --help           show this help\n",
                 prog);
}

options parse_args(int argc, char** argv)
{
    static const option kLong[] = {
        {"assets", required_argument, nullptr, 'a'},
        {"icu", required_argument, nullptr, 'i'},
        {"renderer", required_argument, nullptr, 'r'},
        {"test-texture", no_argument, nullptr, 't'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0},
    };
    options out;
    int c;
    while ((c = getopt_long(argc, argv, "a:i:r:th", kLong, nullptr)) != -1) {
        switch (c) {
            case 'a': out.assets = optarg; break;
            case 'i': out.icu = optarg; break;
            case 'r': out.renderer = optarg; break;
            case 't': out.test_texture = true; break;
            case 'h':
            default:
                print_usage(argv[0]);
                std::exit(0);
        }
    }
    return out;
}

constexpr std::int64_t kDemoTextureId = 1001;

void run_demo_texture(shell::engine::state* e, bool* running)
{
    constexpr std::uint32_t kW = 640, kH = 360;
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(kW) * kH * 4);
    std::uint64_t frame = 0;
    while (*running) {
        double t = double(frame % 120) / 120.0;
        for (std::uint32_t y = 0; y < kH; ++y) {
            for (std::uint32_t x = 0; x < kW; ++x) {
                auto* p = &rgba[(static_cast<std::size_t>(y) * kW + x) * 4];
                p[0] = static_cast<std::uint8_t>((x * 255) / kW);
                p[1] = static_cast<std::uint8_t>((y * 255) / kH);
                p[2] = static_cast<std::uint8_t>(255 - (x * 255) / kW);
                p[3] = 255;
                int bx = static_cast<int>(t * kW);
                if (static_cast<int>(x) > bx && static_cast<int>(x) < bx + 40) {
                    p[0] = p[1] = p[2] = 255;
                }
            }
        }
        shell::engine::push_texture_frame(*e, kDemoTextureId, kW, kH, rgba);
        ++frame;
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }
}
}  // namespace

int main(int argc, char** argv)
{
    options opts = parse_args(argc, argv);
    if (opts.assets.empty()) {
        print_usage(argv[0]);
        return 1;
    }
    if (opts.icu.empty()) opts.icu = opts.assets + "/icudtl.dat";

    // Build the display backend selected at build time (SHELL_BACKEND_WAYLAND
    // or SHELL_BACKEND_DRM) and expose it through the backend-agnostic
    // `shell::backend::output` interface. The engine never sees backend types.
#ifdef SHELL_BACKEND_WAYLAND
    shell::backend::wayland::config cfg{.display = {}, .width = 1280, .height = 720};
    shell::backend::wayland::state backend = shell::backend::wayland::create(cfg);

    if (auto err = shell::backend::wayland::connect(backend); !err) {
        shell::log::info("wayland connect failed");
        return 1;
    }
    if (auto err = shell::backend::wayland::setup_egl(backend); !err) {
        shell::log::info("wayland egl setup failed");
        return 1;
    }
    if (auto err = shell::backend::wayland::setup_surface(backend); !err) {
        shell::log::info("wayland surface setup failed");
        return 1;
    }
    shell::backend::output out = shell::backend::wayland::output::take(backend);
    shell::engine::state engine;
    shell::renderer::type rtype = shell::renderer::type::automatic;
    if (opts.renderer == "software") rtype = shell::renderer::type::software;
    else if (opts.renderer == "gl") rtype = shell::renderer::type::gl;
    engine.cfg = shell::engine::config{.assets = opts.assets, .icu_data = opts.icu,
                                       .renderer = rtype};
    engine.out = &out;

    if (auto err = shell::engine::init(engine); !err) {
        shell::log::info("engine init failed");
        return 1;
    }

    // Wire the backend callbacks to the engine.
    backend.cb.on_vsync = [&engine] { shell::engine::on_vsync(engine); };
    backend.cb.on_pointer = [&engine](const shell::input::pointer_event& ev) {
        shell::engine::send_pointer(engine, ev);
    };
    backend.cb.on_key = [&engine](std::uint32_t code, bool down) {
        shell::engine::send_key(engine, code, down);
    };
    backend.cb.on_keymap = [&engine](int fd, std::uint32_t size) {
        shell::engine::on_keymap(engine, fd, size);
    };
    backend.cb.on_resize = [&engine](std::uint32_t w, std::uint32_t h) {
        shell::engine::on_resize(engine, w, h);
    };
    backend.cb.on_close = [&engine] { shell::engine::on_close(engine); };

    std::thread producer;
    bool producer_running = false;
    if (opts.test_texture && shell::engine::register_texture(engine, kDemoTextureId)) {
        producer_running = true;
        producer = std::thread(run_demo_texture, &engine, &producer_running);
    }

    shell::backend::wayland::run(backend);

    if (producer.joinable()) {
        producer_running = false;
        producer.join();
        shell::engine::unregister_texture(engine, kDemoTextureId);
    }
    shell::engine::destroy(engine);
    shell::backend::wayland::destroy(backend);
    return 0;
#else
    // DRM/KMS backend not yet implemented; the build config selects it but the
    // runtime path is pending.
    shell::log::info("DRM/KMS backend is not implemented yet");
    return 1;
#endif
}
