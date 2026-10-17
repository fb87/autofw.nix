// shell main -- a minimal Flutter embedder shell (Wayland backend).
//
// Header-only shell framework: builds backend::wayland + engine::state and
// runs a Flutter app.
#include <cstdio>
#include <filesystem>
#include <getopt.h>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <shell/engine.hpp>
#include <shell/renderer.hpp>
#include <shell/shell.hpp>
#ifdef SHELL_BACKEND_WAYLAND
#include <shell/backend/wayland.hpp>
#else
#include <shell/backend/drm.hpp>
#endif

namespace
{
struct options
{
    std::string assets;
    std::string icu;
    std::string aot_lib;
    std::string drm_device;     // drm backend only; empty = auto-detect
    std::string renderer{"gl"}; // "gl" | "software"
    bool test_texture{};
    bool test_input{};
};

void print_usage(const char* prog)
{
    std::fprintf(
        stderr,
        "usage: %s [options]\n"
        "  --assets P       Flutter asset directory (required)\n"
        "  --icu P          icudtl.dat path (default <assets>/icudtl.dat)\n"
        "  --aot-lib P      release AOT snapshot (libapp.so); runs JIT against\n"
        "                   <assets>/kernel_blob.bin if omitted and no sibling\n"
        "                   lib/libapp.so is found next to <assets>\n"
        "  --drm-device P   DRM device node (drm backend only; default:\n"
        "                   auto-detect the first /dev/dri/card* with a\n"
        "                   connected display)\n"
        "  --renderer R     gl | software (default gl)\n"
        "  --test-texture   run a demo external-texture producer\n"
        "  --test-input     drive a scripted sequence of key events to exercise input\n"
        "  --help           show this help\n",
        prog);
}

options parse_args(int argc, char** argv)
{
    static const option kLong[] = {
        {"assets", required_argument, nullptr, 'a'},
        {"icu", required_argument, nullptr, 'i'},
        {"aot-lib", required_argument, nullptr, 'A'},
        {"drm-device", required_argument, nullptr, 'D'},
        {"renderer", required_argument, nullptr, 'r'},
        {"test-texture", no_argument, nullptr, 't'},
        {"test-input", no_argument, nullptr, 'x'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0},
    };
    options out;
    int c;
    while ((c = getopt_long(argc, argv, "a:i:r:th", kLong, nullptr)) != -1)
    {
        switch (c)
        {
        case 'a':
            out.assets = optarg;
            break;
        case 'i':
            out.icu = optarg;
            break;
        case 'A':
            out.aot_lib = optarg;
            break;
        case 'D':
            out.drm_device = optarg;
            break;
        case 'r':
            out.renderer = optarg;
            break;
        case 't':
            out.test_texture = true;
            break;
        case 'x':
            out.test_input = true;
            break;
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
// trip. Used as `--test-input` for the GUI input automation test.
void run_input_test(shell::engine::state* e, bool* running)
{
    // (evdev code, label): cover letters, digits, punctuation, editing, nav.
    static const std::pair<std::uint32_t, const char*> kKeys[] = {
        {30, "a"},
        {48, "b"},
        {46, "c"},
        {32, "d"},
        {18, "e"},
        {33, "f"},
        {34, "g"},
        {35, "h"},
        {23, "i"},
        {36, "j"},
        {37, "k"},
        {38, "l"},
        {50, "m"},
        {49, "n"},
        {24, "o"},
        {25, "p"},
        {16, "q"},
        {19, "r"},
        {31, "s"},
        {20, "t"},
        {22, "u"},
        {47, "v"},
        {17, "w"},
        {45, "x"},
        {21, "y"},
        {44, "z"},
        {2, "1"},
        {3, "2"},
        {4, "3"},
        {5, "4"},
        {6, "5"},
        {7, "6"},
        {8, "7"},
        {9, "8"},
        {10, "9"},
        {11, "0"},
        // Navigation tested while the field is still connected (before Enter
        // performs the field action and unfocuses it).
        {103, "up"},
        {108, "down"},
        {105, "left"},
        {106, "right"},
        {102, "home"},
        {107, "end"},
        {104, "pageup"},
        {109, "pagedown"},
        {110, "insert"},
        {111, "delete"},
        {57, "space"},
        {14, "backspace"},
        {28, "enter"},
        {15, "tab"},
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
    std::fprintf(stderr, "[test-input] repeat test: %d down-events of 'x'\n", kRepeats);
    for (int i = 0; i < kRepeats; ++i)
    {
        shell::engine::queue_key(*e, 45, true); // KEY_X down (a repeat)
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    shell::engine::queue_key(*e, 45, false); // release
    std::fprintf(stderr, "[test-input] queued %zu key sequences + %d repeats\n",
                 sizeof(kKeys) / sizeof(kKeys[0]), kRepeats);
    *running = false;
}
} // namespace

int main(int argc, char** argv)
{
    options opts = parse_args(argc, argv);
    if (opts.assets.empty())
    {
        print_usage(argv[0]);
        return 1;
    }
    if (opts.icu.empty())
        opts.icu = opts.assets + "/icudtl.dat";
    if (opts.aot_lib.empty())
    {
        // Two conventions in the wild: flutter-pi/ardera-style gen_snapshot
        // writes app.so straight into the assets dir; Flutter's own
        // `release_bundle_linux-<arch>_assets` target instead lays out
        // <out>/flutter_assets/ next to a sibling <out>/lib/libapp.so.
        auto assets_path = std::filesystem::path(opts.assets);
        auto candidates = {
            assets_path / "app.so",
            assets_path.parent_path() / "lib" / "libapp.so",
        };
        for (const auto& candidate : candidates)
        {
            if (!std::filesystem::exists(candidate))
                continue;
            if (opts.aot_lib.empty())
            {
                opts.aot_lib = candidate.string();
            }
            else
            {
                // Both conventions present, almost always a stale leftover
                // from an earlier build with the other toolchain. Make the
                // ignored one visible instead of silently picking one.
                std::fprintf(stderr, "warning: ignoring %s (using %s instead)\n",
                             candidate.string().c_str(), opts.aot_lib.c_str());
            }
        }
    }

    // The engine (declared first so backend callbacks can capture it).
    shell::engine::state engine;
    shell::renderer::type rtype = shell::renderer::type::automatic;
    if (opts.renderer == "software")
        rtype = shell::renderer::type::software;
    else if (opts.renderer == "gl")
        rtype = shell::renderer::type::gl;
    engine.cfg = shell::engine::config{.assets = opts.assets,
                                       .icu_data = opts.icu,
                                       .aot_library = opts.aot_lib,
                                       .renderer = rtype};

    // Every early-exit below reports *why*: a bare `return 1` with no log
    // line previously left failures (e.g. no Wayland compositor, DRM device
    // already owned, AOT snapshot mismatch) completely silent.
    auto fail = [](std::string_view what, const shell::error& err) -> int
    {
        shell::log::info(std::string(what) + ": " + err.message);
        return 1;
    };

    shell::backend::output out{};

#ifdef SHELL_BACKEND_WAYLAND
    shell::backend::wayland::config cfg{.display = {}, .width = 1280, .height = 720};
    shell::backend::wayland::state backend = shell::backend::wayland::create(cfg);
    if (auto err = shell::backend::wayland::connect(backend); !err)
        return fail("wayland connect failed", err.error());
    if (auto err = shell::backend::wayland::setup_egl(backend); !err)
        return fail("wayland setup_egl failed", err.error());
    if (auto err = shell::backend::wayland::setup_surface(backend); !err)
        return fail("wayland setup_surface failed", err.error());
    out = shell::backend::wayland::output::take(backend);
    backend.cb.on_vsync = [&] { shell::engine::on_vsync(engine); };
    backend.cb.on_pointer = [&](const shell::input::pointer_event& ev)
    { shell::engine::send_pointer(engine, ev); };
    backend.cb.on_key = [&](std::uint32_t code, bool down)
    { shell::engine::send_key(engine, code, down); };
    backend.cb.on_keymap = [&](int fd, std::uint32_t size)
    { shell::engine::on_keymap(engine, fd, size); };
    backend.cb.on_resize = [&](std::uint32_t w, std::uint32_t h)
    { shell::engine::on_resize(engine, w, h); };
    backend.cb.on_close = [&] { shell::engine::on_close(engine); };
    backend.cb.on_poll = [&] { shell::engine::drain_keys(engine); };
#else
    std::string drm_device = opts.drm_device;
    if (drm_device.empty())
    {
        auto found = shell::backend::drm::find_connected_device();
        if (!found)
            return fail("drm device auto-detect failed", found.error());
        drm_device = *found;
        shell::log::info("drm: auto-detected " + drm_device);
    }
    shell::backend::drm::config cfg{.device = drm_device};
    shell::backend::drm::state backend = shell::backend::drm::create(cfg);
    if (auto err = shell::backend::drm::connect(backend); !err)
        return fail("drm connect failed (device=" + drm_device + ")", err.error());
    out = shell::backend::drm::output::take(backend);
    backend.cb.on_vsync = [&] { shell::engine::on_vsync(engine); };
    backend.cb.on_resize = [&](std::uint32_t w, std::uint32_t h)
    { shell::engine::on_resize(engine, w, h); };
    backend.cb.on_close = [&] { shell::engine::on_close(engine); };
#endif

    engine.out = &out;

    if (auto err = shell::engine::init(engine); !err)
        return fail("engine init failed", err.error());

    std::thread producer;
    bool producer_running = false;
    if (opts.test_texture && shell::engine::register_texture(engine, kDemoTextureId))
    {
        producer_running = true;
        producer = std::thread(run_demo_texture, &engine, &producer_running);
    }

    std::thread keytest;
    bool keytest_running = false;
    if (opts.test_input)
    {
        keytest_running = true;
        keytest = std::thread(run_input_test, &engine, &keytest_running);
    }

#ifdef SHELL_BACKEND_WAYLAND
    shell::backend::wayland::run(backend);
#else
    shell::backend::drm::run(backend);
#endif

    if (producer.joinable())
    {
        producer_running = false;
        producer.join();
        shell::engine::unregister_texture(engine, kDemoTextureId);
    }
    if (keytest.joinable())
    {
        keytest_running = false;
        keytest.join();
    }
    shell::engine::destroy(engine);
#ifdef SHELL_BACKEND_WAYLAND
    shell::backend::wayland::destroy(backend);
#else
    shell::backend::drm::destroy(backend);
#endif
    return 0;
}
