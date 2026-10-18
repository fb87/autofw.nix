// shell main -- a minimal Flutter embedder shell.
//
// Header-only shell framework: builds backend::wayland/drm + engine::state
// and runs one or more Flutter app instances, one per display.
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <getopt.h>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <shell/engine.hpp>
#include <shell/renderer.hpp>
#include <shell/shell.hpp>
#ifdef SHELL_BACKEND_WAYLAND
#include <shell/backend/wayland.hpp>
#else
#include <shell/backend/drm.hpp>
#include <shell/input_router.hpp>
#endif

namespace
{
struct options
{
    std::string assets;
    std::string icu;
    std::string aot_lib;
    std::string drm_device;     // legacy single-display flag; empty = auto-detect
    std::string renderer{"gl"}; // "gl" | "software"
    bool test_texture{};
    bool test_input{};
    bool test_layers{};
    std::vector<std::string> display_specs;      // raw "--display key=val,..." strings
    std::vector<std::string> input_device_specs;  // raw "--input-device match=idx" strings
};

// One requested display. Populated either from legacy top-level flags (zero
// --display flags given -- preserves every existing single-display
// invocation/test script unchanged) or by parsing a `--display` flag.
struct display_spec
{
    // Wayland:
    std::uint32_t width{1280};
    std::uint32_t height{720};
    // DRM:
    std::string drm_device; // empty = auto-detect
    std::optional<std::uint32_t> connector_id; // empty = auto-assign
    bool primary{false}; // owns --test-texture/--test-input/--test-layers
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
        "                   connected display). Legacy single-display flag;\n"
        "                   ignored if any --display is given.\n"
        "  --renderer R     gl | software (default gl)\n"
        "  --display K=V[,K=V...]\n"
        "                   add a display. May be repeated, once per display.\n"
        "                   Wayland keys: width,height. DRM keys: drm-device,\n"
        "                   connector (numeric connector id). Either backend:\n"
        "                   primary (bare key -- owns --test-texture/\n"
        "                   --test-input/--test-layers; exactly one display\n"
        "                   must be primary, default: the first). Zero\n"
        "                   --display flags synthesizes one display from the\n"
        "                   legacy top-level flags above.\n"
        "  --input-device MATCH=DISPLAY_INDEX\n"
        "                   (drm backend only) route one input device's\n"
        "                   events to one display, by index into the\n"
        "                   --display list (0-based). MATCH is checked as:\n"
        "                   a sysname (\"/dev/input/event3\"), a \"vendor:\n"
        "                   product\" hex pair, or a substring of the\n"
        "                   device's name. May be repeated.\n"
        "  --test-texture   run a demo external-texture producer (primary display)\n"
        "  --test-input     drive a scripted sequence of key events to exercise\n"
        "                   input (primary display)\n"
        "  --test-layers    drive a scripted sequence of layer add/visibility/\n"
        "                   opacity/remove changes to exercise the shell/layers\n"
        "                   channel (primary display)\n"
        "  --help           show this help\n",
        prog);
}

enum
{
    kOptDisplay = 256,
    kOptInputDevice = 257,
};

options parse_args(int argc, char** argv)
{
    static const option kLong[] = {
        {"assets", required_argument, nullptr, 'a'},
        {"icu", required_argument, nullptr, 'i'},
        {"aot-lib", required_argument, nullptr, 'A'},
        {"drm-device", required_argument, nullptr, 'D'},
        {"renderer", required_argument, nullptr, 'r'},
        {"display", required_argument, nullptr, kOptDisplay},
        {"input-device", required_argument, nullptr, kOptInputDevice},
        {"test-texture", no_argument, nullptr, 't'},
        {"test-input", no_argument, nullptr, 'x'},
        {"test-layers", no_argument, nullptr, 'L'},
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
        case kOptDisplay:
            out.display_specs.push_back(optarg);
            break;
        case kOptInputDevice:
            out.input_device_specs.push_back(optarg);
            break;
        case 't':
            out.test_texture = true;
            break;
        case 'x':
            out.test_input = true;
            break;
        case 'L':
            out.test_layers = true;
            break;
        case 'h':
        default:
            print_usage(argv[0]);
            std::exit(0);
        }
    }
    return out;
}

// Splits "k1=v1,k2=v2,bareflag" into [(k1,v1), (k2,v2), (bareflag,"")].
std::vector<std::pair<std::string, std::string>> parse_kv(const std::string& s)
{
    std::vector<std::pair<std::string, std::string>> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ','))
    {
        auto eq = item.find('=');
        if (eq == std::string::npos)
            out.emplace_back(item, std::string{});
        else
            out.emplace_back(item.substr(0, eq), item.substr(eq + 1));
    }
    return out;
}

std::vector<display_spec> build_displays(const options& opts)
{
    if (opts.display_specs.empty())
    {
        display_spec d;
        d.drm_device = opts.drm_device;
        d.primary = true;
        return {d};
    }
    std::vector<display_spec> out;
    for (const auto& raw : opts.display_specs)
    {
        display_spec d;
        for (const auto& [k, v] : parse_kv(raw))
        {
            if (k == "width")
                d.width = static_cast<std::uint32_t>(std::stoul(v));
            else if (k == "height")
                d.height = static_cast<std::uint32_t>(std::stoul(v));
            else if (k == "drm-device")
                d.drm_device = v;
            else if (k == "connector")
                d.connector_id = static_cast<std::uint32_t>(std::stoul(v));
            else if (k == "primary")
                d.primary = true;
        }
        out.push_back(std::move(d));
    }
    return out;
}

// Exactly one display must be primary -- defaults to the first if none was
// marked explicitly; a hard error if more than one was, to avoid ambiguous
// --test-texture/--test-input/--test-layers routing.
bool validate_and_default_primary(std::vector<display_spec>& displays)
{
    int primary_count = 0;
    for (const auto& d : displays)
        if (d.primary)
            ++primary_count;
    if (primary_count == 0)
        displays.front().primary = true;
    else if (primary_count > 1)
        return false;
    return true;
}

#ifndef SHELL_BACKEND_WAYLAND
std::vector<shell::input_router::device_rule> build_input_rules(const options& opts)
{
    std::vector<shell::input_router::device_rule> out;
    for (const auto& raw : opts.input_device_specs)
    {
        auto eq = raw.rfind('=');
        if (eq == std::string::npos)
            continue;
        out.push_back({raw.substr(0, eq),
                       static_cast<std::size_t>(std::stoul(raw.substr(eq + 1)))});
    }
    return out;
}

// Resolved DRM wiring for every requested display: a deduped list of opened
// cards (two displays naming the same --display-device path, or both
// auto-detecting onto the same card, share one card entry) plus, per
// display, which card index and which connector id it was bound to.
struct drm_resolution
{
    std::vector<shell::backend::drm::card> cards;
    std::vector<std::size_t> card_index;    // indexed by display
    std::vector<std::uint32_t> connector_id; // indexed by display
};

shell::result<drm_resolution> resolve_drm_displays(const std::vector<display_spec>& displays)
{
    drm_resolution out;
    auto auto_candidates = shell::backend::drm::find_connected_devices();
    std::size_t auto_cursor = 0;
    std::unordered_map<std::string, std::size_t> path_to_card_index;
    std::unordered_map<std::size_t, std::vector<std::uint32_t>> used_connectors; // by card index

    for (const auto& d : displays)
    {
        std::string path = d.drm_device;
        if (path.empty())
        {
            if (auto_candidates.empty())
                return shell::unexpected(shell::error{
                    .code = shell::error_code::drm,
                    .message = "no connected DRM device found for auto-detection"});
            path = auto_candidates[auto_cursor % auto_candidates.size()];
            ++auto_cursor;
            shell::log::info("drm: auto-detected " + path);
        }

        std::size_t card_idx;
        if (auto it = path_to_card_index.find(path); it != path_to_card_index.end())
        {
            card_idx = it->second;
        }
        else
        {
            auto card_result = shell::backend::drm::open_card(path);
            if (!card_result)
                return shell::unexpected(card_result.error());
            card_idx = out.cards.size();
            out.cards.push_back(std::move(*card_result));
            path_to_card_index[path] = card_idx;
        }
        out.card_index.push_back(card_idx);

        std::uint32_t connector_id;
        if (d.connector_id)
        {
            connector_id = *d.connector_id;
        }
        else
        {
            auto& used = used_connectors[card_idx];
            bool found = false;
            for (const auto& ci : shell::backend::drm::list_connectors(out.cards[card_idx]))
            {
                bool already_used =
                    std::find(used.begin(), used.end(), ci.connector_id) != used.end();
                if (ci.connected && ci.mode_count > 0 && !already_used)
                {
                    connector_id = ci.connector_id;
                    found = true;
                    break;
                }
            }
            if (!found)
                return shell::unexpected(shell::error{
                    .code = shell::error_code::drm,
                    .message = "not enough connected connectors on " + path +
                               " for the requested number of displays"});
        }
        used_connectors[card_idx].push_back(connector_id);
        out.connector_id.push_back(connector_id);
    }
    return out;
}
#endif

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
    shell::log::info("[test-input] repeat test: " + std::to_string(kRepeats) +
                      " down-events of 'x'");
    for (int i = 0; i < kRepeats; ++i)
    {
        shell::engine::queue_key(*e, 45, true); // KEY_X down (a repeat)
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    shell::engine::queue_key(*e, 45, false); // release
    shell::log::info("[test-input] queued " +
                      std::to_string(sizeof(kKeys) / sizeof(kKeys[0])) +
                      " key sequences + " + std::to_string(kRepeats) + " repeats");
    *running = false;
}

// Drive a scripted sequence of layer add/visibility/opacity/remove changes
// straight through engine::add_layer/set_layer_*/remove_layer -- the shell/
// layers channel Dart side; used as `--test-layers` for the GUI layer
// compositing test.
void run_layer_test(shell::engine::state* e, bool* running)
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

// Runs one display's full engine+backend lifecycle on the calling thread;
// blocks on that backend's run() loop. Intended to be invoked once per
// display_spec, each on its own std::thread. Any failure (startup or
// runtime) sets *exit_code and flips `stop_all`, which every display's
// backend.cb.on_poll checks each loop iteration to self-stop -- this is the
// kiosk all-or-nothing shutdown: one display going down tears down every
// other display in the process too.
#ifdef SHELL_BACKEND_WAYLAND
void run_one_display(const options& opts, const display_spec& spec, std::size_t display_index,
                     std::atomic<bool>& stop_all, int* exit_code)
#else
void run_one_display(const options& opts, const display_spec& spec, std::size_t display_index,
                     std::atomic<bool>& stop_all, shell::backend::drm::card* card,
                     std::uint32_t connector_id, shell::input_router::state* router,
                     int* exit_code)
#endif
{
    std::string tag = "d" + std::to_string(display_index) + (spec.primary ? "*" : "");
    shell::log::set_tag(tag);

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
    // Propagated onto the engine's own runner_thread (start_runner in
    // engine_impl.hpp) -- thread_local tags don't cross threads on their
    // own, and platform-thread work (IME "[sel]", key results "[kr]", ...)
    // runs there, not on this thread.
    engine.log_tag = tag;
    engine.channels.data_root =
        shell::channel::default_data_root() + "/display" + std::to_string(display_index);
    std::filesystem::create_directories(engine.channels.data_root);

    auto fail = [&](std::string_view what, const shell::error& err)
    {
        shell::log::info(std::string(what) + ": " + err.message);
        *exit_code = 1;
        stop_all.store(true, std::memory_order_relaxed);
    };

    shell::backend::output out{};

#ifdef SHELL_BACKEND_WAYLAND
    shell::backend::wayland::config cfg{
        .display = {}, .width = spec.width, .height = spec.height};
    shell::backend::wayland::state backend = shell::backend::wayland::create(cfg);
    if (auto err = shell::backend::wayland::connect(backend); !err)
    {
        shell::backend::wayland::destroy(backend);
        fail("wayland connect failed", err.error());
        return;
    }
    if (auto err = shell::backend::wayland::setup_egl(backend); !err)
    {
        shell::backend::wayland::destroy(backend);
        fail("wayland setup_egl failed", err.error());
        return;
    }
    if (auto err = shell::backend::wayland::setup_surface(backend); !err)
    {
        shell::backend::wayland::destroy(backend);
        fail("wayland setup_surface failed", err.error());
        return;
    }
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
    backend.cb.on_poll = [&]
    {
        shell::engine::drain_keys(engine);
        if (stop_all.load(std::memory_order_relaxed))
            shell::backend::wayland::stop(backend);
    };
#else
    auto output_result = shell::backend::drm::open_output(*card, connector_id);
    if (!output_result)
    {
        fail("drm open_output failed", output_result.error());
        return;
    }
    shell::backend::drm::state backend = std::move(*output_result);
    out = shell::backend::drm::output::take(backend);
    backend.cb.on_vsync = [&] { shell::engine::on_vsync(engine); };
    backend.cb.on_resize = [&](std::uint32_t w, std::uint32_t h)
    { shell::engine::on_resize(engine, w, h); };
    backend.cb.on_close = [&] { shell::engine::on_close(engine); };
    backend.cb.on_poll = [&]
    {
        if (stop_all.load(std::memory_order_relaxed))
            shell::backend::drm::stop(backend);
    };
    // Pointer/keyboard/touch input is routed in via `router` below, straight
    // to engine::send_pointer/send_key -- not through backend.cb here.
#endif

    engine.out = &out;

    if (auto err = shell::engine::init(engine); !err)
    {
#ifdef SHELL_BACKEND_WAYLAND
        shell::backend::wayland::destroy(backend);
#else
        shell::backend::drm::close_output(backend);
#endif
        fail("engine init failed", err.error());
        return;
    }

#ifndef SHELL_BACKEND_WAYLAND
    if (router)
        router->targets[display_index].store(&engine, std::memory_order_release);
#endif

    std::thread producer;
    bool producer_running = false;
    if (spec.primary && opts.test_texture &&
        shell::engine::register_texture(engine, kDemoTextureId))
    {
        producer_running = true;
        producer = std::thread(
            [&engine, &producer_running, tag]
            {
                shell::log::set_tag(tag);
                run_demo_texture(&engine, &producer_running);
            });
    }

    std::thread keytest;
    bool keytest_running = false;
    if (spec.primary && opts.test_input)
    {
        keytest_running = true;
        keytest = std::thread(
            [&engine, &keytest_running, tag]
            {
                shell::log::set_tag(tag);
                run_input_test(&engine, &keytest_running);
            });
    }

    std::thread layertest;
    bool layertest_running = false;
    if (spec.primary && opts.test_layers)
    {
        layertest_running = true;
        layertest = std::thread(
            [&engine, &layertest_running, tag]
            {
                shell::log::set_tag(tag);
                run_layer_test(&engine, &layertest_running);
            });
    }

#ifdef SHELL_BACKEND_WAYLAND
    shell::backend::wayland::run(backend);
#else
    shell::backend::drm::run(backend);
#endif

    // Kiosk shutdown: this display's run() loop returned (naturally closed,
    // or a backend-internal error) -- tear every other display down too.
    stop_all.store(true, std::memory_order_relaxed);

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
    if (layertest.joinable())
    {
        layertest_running = false;
        layertest.join();
    }

#ifndef SHELL_BACKEND_WAYLAND
    if (router)
        router->targets[display_index].store(nullptr, std::memory_order_release);
#endif

    shell::engine::destroy(engine);
#ifdef SHELL_BACKEND_WAYLAND
    shell::backend::wayland::destroy(backend);
#else
    shell::backend::drm::close_output(backend); // also closes `card` once refcount hits 0
#endif
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

    std::vector<display_spec> displays = build_displays(opts);
    if (!validate_and_default_primary(displays))
    {
        std::fprintf(stderr, "error: at most one --display may be marked 'primary'\n");
        return 1;
    }

#ifndef SHELL_BACKEND_WAYLAND
    auto drm_resolved = resolve_drm_displays(displays);
    if (!drm_resolved)
    {
        shell::log::info(std::string("drm resolve failed: ") + drm_resolved.error().message);
        return 1;
    }

    shell::input_router::state router;
    auto input_rules = build_input_rules(opts);
    if (auto err = shell::input_router::create(router, input_rules, displays.size()); !err)
    {
        shell::log::info(std::string("input_router create failed: ") + err.error().message);
        return 1;
    }
    std::thread input_thread(shell::input_router::run, std::ref(router));
#endif

    std::atomic<bool> stop_all{false};
    std::vector<std::thread> threads;
    std::vector<int> exit_codes(displays.size(), 0);
    for (std::size_t i = 0; i < displays.size(); ++i)
    {
#ifdef SHELL_BACKEND_WAYLAND
        threads.emplace_back(run_one_display, std::cref(opts), std::cref(displays[i]), i,
                             std::ref(stop_all), &exit_codes[i]);
#else
        threads.emplace_back(run_one_display, std::cref(opts), std::cref(displays[i]), i,
                             std::ref(stop_all),
                             &drm_resolved->cards[drm_resolved->card_index[i]],
                             drm_resolved->connector_id[i], &router, &exit_codes[i]);
#endif
    }
    for (auto& t : threads)
        t.join();

#ifndef SHELL_BACKEND_WAYLAND
    shell::input_router::stop(router);
    input_thread.join();
    shell::input_router::destroy(router);
#endif

    int rc = 0;
    for (int c : exit_codes)
        rc = rc ? rc : c;
    return rc;
}
