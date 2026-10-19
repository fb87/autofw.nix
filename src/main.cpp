// shell main -- a minimal Flutter embedder shell.
//
// Header-only shell framework: builds backend::wayland/drm + engine::state
// and runs one or more Flutter app instances, one per display.
#include <atomic>
#include <csignal>
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

#include <shell/child_renderer.hpp> // self-guards with #ifdef SHELL_RENDERER_GL
#include <shell/engine.hpp>
#include <shell/isolated_app.hpp> // self-guards with #ifdef SHELL_RENDERER_GL
#include <shell/renderer.hpp>
#include <shell/secondary_engine.hpp> // self-guards with #ifdef SHELL_RENDERER_GL
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
    bool test_secondary_engine{};
    bool test_isolated_app{};
    bool test_isolated_app_restart{};
    bool test_secondary_engine_watchdog{};

    // Dashboard HMI (production, not --test-*): when both are non-empty,
    // the primary display spawns a real EV cluster + infotainment app side
    // by side via shell::isolated_app -- see setup_dashboard_apps.
    std::string cluster_assets;
    std::string cluster_icu;
    std::string cluster_aot_lib;
    std::string infotainment_assets;
    std::string infotainment_icu;
    std::string infotainment_aot_lib;
    bool test_dashboard_input{};

    std::vector<std::string> display_specs;      // raw "--display key=val,..." strings
    std::vector<std::string> input_device_specs;  // raw "--input-device match=idx" strings

    // --child-renderer mode (Step 4: process-level isolation) -- this
    // process IS the isolated child, not a display-owning shell at all;
    // see the top-of-main() dispatch. Hidden (not in --help): spawned only
    // by shell::ipc::spawn_child, under the same shell_app binary, never
    // meant to be typed by a human.
    bool child_renderer{};
    std::uint32_t child_width{1280};
    std::uint32_t child_height{720};
    int child_ipc_fd{-1};
    int child_input_fd{-1};
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
        "  --test-secondary-engine\n"
        "                   (GL renderer only) spawn two headless secondary\n"
        "                   Flutter engines and attach them, as live textures,\n"
        "                   to the 'application' and 'overlay' layers from\n"
        "                   --test-layers' scripted sequence (implies\n"
        "                   --test-layers; primary display)\n"
        "  --test-isolated-app\n"
        "                   (GL renderer only) spawn a real OS-process-\n"
        "                   isolated Flutter app (shell::isolated_app) and\n"
        "                   attach it, as a live texture, to the\n"
        "                   'application' layer from --test-layers' scripted\n"
        "                   sequence (implies --test-layers; primary display)\n"
        "  --test-isolated-app-restart\n"
        "                   (GL renderer only) like --test-isolated-app, but\n"
        "                   with auto-restart enabled -- the driver SIGSEGVs\n"
        "                   the child itself and confirms it respawns and\n"
        "                   content resumes (implies --test-layers; primary\n"
        "                   display)\n"
        "  --test-secondary-engine-watchdog\n"
        "                   (GL renderer only) spawn a secondary engine with\n"
        "                   its heartbeat watchdog + auto-restart enabled,\n"
        "                   induce a real stall, and confirm it's detected\n"
        "                   and recovered (implies --test-layers; primary\n"
        "                   display)\n"
        "  --cluster-assets P / --cluster-icu P / --cluster-aot-lib P\n"
        "                   (GL renderer only) EV instrument cluster Flutter\n"
        "                   asset dir. Combined with --infotainment-assets,\n"
        "                   enables the production dashboard layout on the\n"
        "                   primary display: a real OS-process-isolated\n"
        "                   cluster app on the left, auto-restart enabled.\n"
        "                   --cluster-icu/--cluster-aot-lib default the same\n"
        "                   way --icu/--aot-lib do.\n"
        "  --infotainment-assets P / --infotainment-icu P / --infotainment-aot-lib P\n"
        "                   infotainment Flutter asset dir (e.g. the gallery),\n"
        "                   composited on the right under a status bar/dock;\n"
        "                   also a real OS-process-isolated shell::isolated_app\n"
        "                   with auto-restart enabled.\n"
        "  --test-dashboard-input\n"
        "                   (requires --cluster-assets/--infotainment-assets;\n"
        "                   Wayland only) inject synthetic taps + key presses\n"
        "                   at coordinates inside each pane and confirm they\n"
        "                   route to the right child (primary display)\n"
        "  --help           show this help\n",
        prog);
}

enum
{
    kOptDisplay = 256,
    kOptInputDevice = 257,
    kOptTestSecondaryEngine = 258,
    kOptTestIsolatedApp = 259,
    kOptChildRenderer = 260,
    kOptChildWidth = 261,
    kOptChildHeight = 262,
    kOptChildIpcFd = 263,
    kOptTestIsolatedAppRestart = 264,
    kOptTestSecondaryEngineWatchdog = 265,
    kOptClusterAssets = 266,
    kOptClusterIcu = 267,
    kOptClusterAotLib = 268,
    kOptInfotainmentAssets = 269,
    kOptInfotainmentIcu = 270,
    kOptInfotainmentAotLib = 271,
    kOptChildInputFd = 272,
    kOptTestDashboardInput = 273,
};

// std::stoul/std::stoi throw on malformed input (non-numeric, empty, or
// out of range) -- an uncaught exception from inside parse_args would
// crash the whole process instead of reporting a clean usage error.
// --width/--height/--ipc-fd/--input-fd are normally parent-generated
// internal flags (see spawn_config's own --child-renderer argv), but this
// binary accepts them from argv unconditionally, so malformed input here
// is a real, reachable case to handle cleanly, not just a theoretical one.
long parse_long_arg(const char* prog, const char* flag, const char* value)
{
    try
    {
        return std::stol(value);
    }
    catch (const std::exception&)
    {
        std::fprintf(stderr, "%s: bad value for %s: '%s'\n", prog, flag, value);
        print_usage(prog);
        std::exit(1);
    }
}

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
        {"test-secondary-engine", no_argument, nullptr, kOptTestSecondaryEngine},
        {"test-isolated-app", no_argument, nullptr, kOptTestIsolatedApp},
        {"test-isolated-app-restart", no_argument, nullptr, kOptTestIsolatedAppRestart},
        {"test-secondary-engine-watchdog", no_argument, nullptr, kOptTestSecondaryEngineWatchdog},
        {"cluster-assets", required_argument, nullptr, kOptClusterAssets},
        {"cluster-icu", required_argument, nullptr, kOptClusterIcu},
        {"cluster-aot-lib", required_argument, nullptr, kOptClusterAotLib},
        {"infotainment-assets", required_argument, nullptr, kOptInfotainmentAssets},
        {"infotainment-icu", required_argument, nullptr, kOptInfotainmentIcu},
        {"infotainment-aot-lib", required_argument, nullptr, kOptInfotainmentAotLib},
        // Hidden child-renderer-mode flags -- not listed in print_usage(),
        // see the options::child_renderer doc comment.
        {"child-renderer", no_argument, nullptr, kOptChildRenderer},
        {"width", required_argument, nullptr, kOptChildWidth},
        {"height", required_argument, nullptr, kOptChildHeight},
        {"ipc-fd", required_argument, nullptr, kOptChildIpcFd},
        {"input-fd", required_argument, nullptr, kOptChildInputFd},
        {"test-dashboard-input", no_argument, nullptr, kOptTestDashboardInput},
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
        case kOptTestSecondaryEngine:
            out.test_secondary_engine = true;
            out.test_layers = true; // the layers it attaches to must exist.
            break;
        case kOptTestIsolatedApp:
            out.test_isolated_app = true;
            out.test_layers = true; // the layer it attaches to must exist.
            break;
        case kOptTestIsolatedAppRestart:
            out.test_isolated_app_restart = true;
            out.test_layers = true; // the layer it attaches to must exist.
            break;
        case kOptTestSecondaryEngineWatchdog:
            out.test_secondary_engine_watchdog = true;
            out.test_layers = true; // the layer it attaches to must exist.
            break;
        case kOptClusterAssets:
            out.cluster_assets = optarg;
            break;
        case kOptClusterIcu:
            out.cluster_icu = optarg;
            break;
        case kOptClusterAotLib:
            out.cluster_aot_lib = optarg;
            break;
        case kOptInfotainmentAssets:
            out.infotainment_assets = optarg;
            break;
        case kOptInfotainmentIcu:
            out.infotainment_icu = optarg;
            break;
        case kOptInfotainmentAotLib:
            out.infotainment_aot_lib = optarg;
            break;
        case kOptChildRenderer:
            out.child_renderer = true;
            break;
        case kOptChildWidth:
            out.child_width =
                static_cast<std::uint32_t>(parse_long_arg(argv[0], "--width", optarg));
            break;
        case kOptChildHeight:
            out.child_height =
                static_cast<std::uint32_t>(parse_long_arg(argv[0], "--height", optarg));
            break;
        case kOptChildIpcFd:
            out.child_ipc_fd = static_cast<int>(parse_long_arg(argv[0], "--ipc-fd", optarg));
            break;
        case kOptChildInputFd:
            out.child_input_fd =
                static_cast<int>(parse_long_arg(argv[0], "--input-fd", optarg));
            break;
        case kOptTestDashboardInput:
            out.test_dashboard_input = true;
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

#ifdef SHELL_RENDERER_GL
// Spawns TWO headless secondary engines (zero-copy GL texture sharing, see
// shell/secondary_engine.hpp) and attaches each as a live texture to one of
// run_layer_test's scripted layers -- "application" (id 2) and "overlay"
// (id 3). Two, not one: this is the only way to actually exercise "per-layer"
// (plural) rather than resting on architecture-by-inspection, and it proves
// two independent engines each get their own lazily-created FBO in the same
// share group without interfering. Used as `--test-secondary-engine`
// (implies --test-layers; primary display only).
void run_secondary_engine_test(shell::engine::state* primary, const options& opts,
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
            .assets = opts.assets, .icu_data = opts.icu, .aot_library = opts.aot_lib,
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
// layer (id 2) -- distinct texture/surface ids from
// run_secondary_engine_test's so the two tests can run back to back
// without colliding. Used as `--test-isolated-app` (implies --test-layers;
// primary display only).
void run_isolated_app_test(shell::engine::state* primary, const options& opts, bool* running)
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
        .assets = opts.assets, .icu_data = opts.icu, .aot_library = opts.aot_lib,
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
// choreography needed, unlike this session's original manual
// verification) and asserts a second, different child pid shows up and
// content resumes afterward. Distinct texture/surface/layer ids from both
// other tests so all three could in principle run back to back. Used as
// `--test-isolated-app-restart` (implies --test-layers; primary display).
void run_isolated_app_restart_test(shell::engine::state* primary, const options& opts,
                                   bool* running)
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
        .assets = opts.assets, .icu_data = opts.icu, .aot_library = opts.aot_lib,
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
// --test-isolated-app-restart kills a whole child process, so this induces
// a controlled, real stall instead: the test driver itself locks
// app.engine.runner_mutex for longer than hang_threshold, which blocks the
// watchdog thread's own attempt to post a heartbeat into that same
// engine's runner_q -- exercising the exact detection path (a heartbeat
// that doesn't get processed in time) without needing any test-only hook
// inside the shipped header. Used as `--test-secondary-engine-watchdog`
// (implies --test-layers; primary display).
void run_secondary_engine_watchdog_test(shell::engine::state* primary, const options& opts,
                                        bool* running)
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
        .assets = opts.assets, .icu_data = opts.icu, .aot_library = opts.aot_lib,
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

// --- Dashboard HMI (production, not --test-*) ---------------------------------
//
// The first non-test use of shell::isolated_app in this codebase: a real
// digital EV instrument cluster on the left, the gallery as infotainment
// content on the right, both genuinely OS-process-isolated with Step 5
// auto-restart enabled. The dashboard app itself (--assets) owns the top
// status bar and bottom dock as plain Flutter chrome in its own widget
// tree -- those are not shell::layer entries and need no C++ wiring here.
constexpr shell::stage::id kDashboardStageId = 10;
constexpr shell::layer::id kClusterLayerId = 10;        // examples/dashboard/lib/layer_ids.dart must match.
constexpr shell::layer::id kInfotainmentLayerId = 11;    // examples/dashboard/lib/layer_ids.dart must match.
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

struct dashboard_apps
{
    shell::isolated_app::state cluster;
    shell::isolated_app::state infotainment;
    bool cluster_spawned{};
    bool infotainment_spawned{};
    // On-screen rectangles, same primary-window pixel space pointer_event
    // coordinates already arrive in -- populated by setup_dashboard_apps,
    // consulted by run_one_display's input dispatcher to hit-test a tap
    // and to translate it into each pane's own local coordinate space
    // before forwarding (shell::isolated_app itself is position-agnostic).
    shell::layer::geometry cluster_bounds{};
    shell::layer::geometry infotainment_bounds{};
};

// Which pane (if any) a pointer/key event should go to. Shared by both
// backends' input dispatch: Wayland wires it straight into backend.cb.on_
// pointer/on_key (see run_one_display); DRM wires the identical logic into
// shell::input_router's per-display dispatch_hook (dashboard_dispatch_*
// below) since input_router routes per-display, not per-position, on its
// own.
enum class dashboard_focus
{
    primary,
    cluster,
    infotainment,
};

inline bool dashboard_hit_inside(const shell::layer::geometry& b, double x, double y)
{
    return x >= b.x && x < b.x + b.width && y >= b.y && y < b.y + b.height;
}

inline dashboard_focus dashboard_hit_test(const dashboard_apps& apps, bool dashboard_active,
                                          double x, double y)
{
    if (dashboard_active)
    {
        if (dashboard_hit_inside(apps.cluster_bounds, x, y))
            return dashboard_focus::cluster;
        if (dashboard_hit_inside(apps.infotainment_bounds, x, y))
            return dashboard_focus::infotainment;
    }
    return dashboard_focus::primary;
}

inline shell::input::pointer_event dashboard_translate(const shell::input::pointer_event& ev,
                                                        const shell::layer::geometry& bounds)
{
    shell::input::pointer_event local = ev;
    local.x -= bounds.x;
    local.y -= bounds.y;
    return local;
}

bool setup_dashboard_apps(shell::engine::state& primary, const options& opts,
                          std::uint32_t display_w, std::uint32_t display_h,
                          dashboard_apps& apps)
{
    const std::uint32_t left_w = display_w / 2;
    const std::uint32_t right_w = display_w - left_w;
    const std::uint32_t chrome_h = static_cast<std::uint32_t>(kStatusBarHeight + kDockHeight);
    const std::uint32_t right_h = display_h > chrome_h ? display_h - chrome_h : display_h;

    apps.cluster_bounds = {0, 0, static_cast<float>(left_w), static_cast<float>(display_h)};
    apps.infotainment_bounds = {static_cast<float>(left_w), kStatusBarHeight,
                                static_cast<float>(right_w), static_cast<float>(right_h)};

    shell::engine::add_layer(
        primary, shell::layer::state{.value = kClusterLayerId, .stage = kDashboardStageId,
                                     .kind = shell::layer::role::application,
                                     .bounds = apps.cluster_bounds});
    shell::engine::add_layer(
        primary, shell::layer::state{.value = kInfotainmentLayerId, .stage = kDashboardStageId,
                                     .kind = shell::layer::role::application,
                                     .bounds = apps.infotainment_bounds});

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
    if (auto err = shell::isolated_app::create(apps.cluster, cluster_cfg); !err)
    {
        shell::log::info("[dashboard] create(cluster) failed: " + err.error().message);
        shell::engine::unregister_texture(primary, kClusterTextureId);
        return false;
    }
    shell::log::info("[dashboard] cluster child pid=" + std::to_string(apps.cluster.child.pid));
    shell::engine::attach_surface(
        primary, shell::surface::state{.value = kClusterSurfaceId, .layer = kClusterLayerId,
                                       .size = {left_w, display_h}, .texture = kClusterTextureId,
                                       .visible = true});
    apps.cluster_spawned = true;

    if (!shell::engine::register_texture(primary, kInfotainmentTextureId))
    {
        shell::log::info("[dashboard] register_texture(infotainment) failed");
        return apps.cluster_spawned;
    }
    shell::isolated_app::config info_cfg{
        .assets = opts.infotainment_assets, .icu_data = opts.infotainment_icu,
        .aot_library = opts.infotainment_aot_lib, .width = right_w, .height = right_h,
        .log_tag = "infotainment", .primary = &primary,
        .target_texture = kInfotainmentTextureId, .target_surface = kInfotainmentSurfaceId};
    info_cfg.restart.enabled = true;
    info_cfg.restart.initial_delay = std::chrono::milliseconds(500);
    info_cfg.restart.max_delay = std::chrono::milliseconds(5000);
    if (auto err = shell::isolated_app::create(apps.infotainment, info_cfg); !err)
    {
        shell::log::info("[dashboard] create(infotainment) failed: " + err.error().message);
        shell::engine::unregister_texture(primary, kInfotainmentTextureId);
        return apps.cluster_spawned;
    }
    shell::log::info("[dashboard] infotainment child pid=" +
                     std::to_string(apps.infotainment.child.pid));
    shell::engine::attach_surface(
        primary, shell::surface::state{.value = kInfotainmentSurfaceId,
                                       .layer = kInfotainmentLayerId,
                                       .size = {right_w, right_h},
                                       .texture = kInfotainmentTextureId, .visible = true});
    apps.infotainment_spawned = true;
    return true;
}

void teardown_dashboard_apps(shell::engine::state& primary, dashboard_apps& apps)
{
    if (apps.infotainment_spawned)
    {
        shell::engine::detach_surface(primary, kInfotainmentSurfaceId);
        shell::isolated_app::destroy(apps.infotainment);
        shell::engine::unregister_texture(primary, kInfotainmentTextureId);
    }
    if (apps.cluster_spawned)
    {
        shell::engine::detach_surface(primary, kClusterSurfaceId);
        shell::isolated_app::destroy(apps.cluster);
        shell::engine::unregister_texture(primary, kClusterTextureId);
    }
    shell::engine::remove_layer(primary, kInfotainmentLayerId);
    shell::engine::remove_layer(primary, kClusterLayerId);
}

#ifndef SHELL_BACKEND_WAYLAND
// DRM parity for the dashboard dispatcher: under Wayland this logic lives
// directly in backend.cb.on_pointer/on_key (run_one_display, below), since
// Wayland delivers events straight to whichever display they're already
// tied to. DRM instead routes every display's input through one shared
// shell::input_router thread that otherwise sends straight to a display's
// engine::state* with no notion of position within it (see
// input_router.hpp's dispatch_one) -- this ctx + the two trampolines below
// plug into its per-display dispatch_hook to get the exact same hit-test-
// and-translate behavior there too, reusing dashboard_hit_test/
// dashboard_translate rather than duplicating them.
struct dashboard_dispatch_ctx
{
    dashboard_apps* apps{};
    bool* dashboard_active{};
    dashboard_focus* focus{};
    shell::engine::state* primary{};
};

inline void dashboard_dispatch_pointer(void* ctx_raw, const shell::input::pointer_event& ev)
{
    auto* ctx = static_cast<dashboard_dispatch_ctx*>(ctx_raw);
    if (ev.phase == shell::input::pointer_phase::down)
        *ctx->focus = dashboard_hit_test(*ctx->apps, *ctx->dashboard_active, ev.x, ev.y);
    switch (*ctx->focus)
    {
    case dashboard_focus::cluster:
        shell::isolated_app::send_pointer(ctx->apps->cluster,
                                          dashboard_translate(ev, ctx->apps->cluster_bounds));
        break;
    case dashboard_focus::infotainment:
        shell::isolated_app::send_pointer(
            ctx->apps->infotainment, dashboard_translate(ev, ctx->apps->infotainment_bounds));
        break;
    case dashboard_focus::primary:
        shell::engine::send_pointer(*ctx->primary, ev);
        break;
    }
}

inline void dashboard_dispatch_key(void* ctx_raw, std::uint32_t code, bool down)
{
    auto* ctx = static_cast<dashboard_dispatch_ctx*>(ctx_raw);
    switch (*ctx->focus)
    {
    case dashboard_focus::cluster:
        shell::isolated_app::send_key(ctx->apps->cluster, code, down);
        break;
    case dashboard_focus::infotainment:
        shell::isolated_app::send_key(ctx->apps->infotainment, code, down);
        break;
    case dashboard_focus::primary:
        shell::engine::send_key(*ctx->primary, code, down);
        break;
    }
}
#endif // !SHELL_BACKEND_WAYLAND
#endif // SHELL_RENDERER_GL

#if defined(SHELL_RENDERER_GL) && defined(SHELL_BACKEND_WAYLAND)
// Exercises the REAL position-based dispatcher (backend.cb.on_pointer/
// on_key, called exactly as a genuine Wayland pointer/keyboard event
// would be) -- not just shell::isolated_app's IPC plumbing directly -- by
// injecting synthetic events at primary-screen coordinates known to fall
// inside each pane's bounds, then confirming via log output that they
// landed on the intended target. Used as `--test-dashboard-input`
// (requires --cluster-assets/--infotainment-assets; primary display;
// Wayland only -- DRM has no equivalent per-position dispatcher wired up).
void run_dashboard_input_test(const dashboard_apps& apps,
                              const shell::backend::wayland::callbacks& cb, bool* running)
{
    // Give setup_dashboard_apps' children a few seconds to connect and the
    // gallery's Form panel to auto-focus its text field -- same boot
    // window run_input_test already waits out for the primary case.
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
#endif

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

#ifdef SHELL_RENDERER_GL
    // Declared up here (not at the dashboard setup call site below) because
    // the pointer/key dispatcher wired into backend.cb (Wayland) or the
    // dashboard_dispatch_ctx (DRM) just below needs to capture/point to
    // them by reference; setup_dashboard_apps (called later, after
    // engine::init) just populates what's already declared.
    dashboard_apps dash_apps;
    bool dashboard_active = false;
    dashboard_focus focus = dashboard_focus::primary;
#ifndef SHELL_BACKEND_WAYLAND
    dashboard_dispatch_ctx dash_dispatch_ctx{.apps = &dash_apps,
                                             .dashboard_active = &dashboard_active,
                                             .focus = &focus,
                                             .primary = &engine};
    shell::input_router::dispatch_hook dash_dispatch_hook{
        .ctx = &dash_dispatch_ctx,
        .on_pointer = dashboard_dispatch_pointer,
        .on_key = dashboard_dispatch_key};
#endif
#endif

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
#ifdef SHELL_RENDERER_GL
    backend.cb.on_pointer = [&](const shell::input::pointer_event& ev)
    {
        // Only a fresh down re-evaluates focus -- a drag that leaves the
        // pane it started on stays targeted there, standard UI behavior
        // (not re-hit-tested on every move/up).
        if (ev.phase == shell::input::pointer_phase::down)
            focus = dashboard_hit_test(dash_apps, dashboard_active, ev.x, ev.y);
        switch (focus)
        {
        case dashboard_focus::cluster:
            shell::isolated_app::send_pointer(dash_apps.cluster,
                                              dashboard_translate(ev, dash_apps.cluster_bounds));
            break;
        case dashboard_focus::infotainment:
            shell::isolated_app::send_pointer(
                dash_apps.infotainment, dashboard_translate(ev, dash_apps.infotainment_bounds));
            break;
        case dashboard_focus::primary:
            shell::engine::send_pointer(engine, ev);
            break;
        }
    };
    backend.cb.on_key = [&](std::uint32_t code, bool down)
    {
        // Keyboard has no inherent position -- it follows whichever pane
        // the most recent pointer-down landed on ("tap to focus").
        switch (focus)
        {
        case dashboard_focus::cluster:
            shell::isolated_app::send_key(dash_apps.cluster, code, down);
            break;
        case dashboard_focus::infotainment:
            shell::isolated_app::send_key(dash_apps.infotainment, code, down);
            break;
        case dashboard_focus::primary:
            shell::engine::send_key(engine, code, down);
            break;
        }
    };
#else
    backend.cb.on_pointer = [&](const shell::input::pointer_event& ev)
    { shell::engine::send_pointer(engine, ev); };
    backend.cb.on_key = [&](std::uint32_t code, bool down)
    { shell::engine::send_key(engine, code, down); };
#endif
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
    {
        router->targets[display_index].store(&engine, std::memory_order_release);
#ifdef SHELL_RENDERER_GL
        // Gives this display's input_router dispatch the same position-based
        // dashboard routing Wayland gets via backend.cb.on_pointer/on_key
        // above -- harmless/no-op for a non-primary or non-dashboard display,
        // since dashboard_active there stays false and dashboard_hit_test
        // always resolves to dashboard_focus::primary (today's exact
        // behavior).
        router->dispatch_hooks[display_index].store(&dash_dispatch_hook,
                                                     std::memory_order_release);
#endif
    }
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

#ifdef SHELL_RENDERER_GL
    std::thread sectest;
    bool sectest_running = false;
    if (spec.primary && opts.test_secondary_engine)
    {
        sectest_running = true;
        sectest = std::thread(
            [&engine, &opts, &sectest_running, tag]
            {
                shell::log::set_tag(tag);
                run_secondary_engine_test(&engine, opts, &sectest_running);
            });
    }

    std::thread isotest;
    bool isotest_running = false;
    if (spec.primary && opts.test_isolated_app)
    {
        isotest_running = true;
        isotest = std::thread(
            [&engine, &opts, &isotest_running, tag]
            {
                shell::log::set_tag(tag);
                run_isolated_app_test(&engine, opts, &isotest_running);
            });
    }

    std::thread isorestarttest;
    bool isorestarttest_running = false;
    if (spec.primary && opts.test_isolated_app_restart)
    {
        isorestarttest_running = true;
        isorestarttest = std::thread(
            [&engine, &opts, &isorestarttest_running, tag]
            {
                shell::log::set_tag(tag);
                run_isolated_app_restart_test(&engine, opts, &isorestarttest_running);
            });
    }

    std::thread secwdtest;
    bool secwdtest_running = false;
    if (spec.primary && opts.test_secondary_engine_watchdog)
    {
        secwdtest_running = true;
        secwdtest = std::thread(
            [&engine, &opts, &secwdtest_running, tag]
            {
                shell::log::set_tag(tag);
                run_secondary_engine_watchdog_test(&engine, opts, &secwdtest_running);
            });
    }

    if (spec.primary && !opts.cluster_assets.empty() && !opts.infotainment_assets.empty())
    {
        dashboard_active = setup_dashboard_apps(engine, opts, out.width, out.height, dash_apps);
    }
    else if (spec.primary && (opts.cluster_assets.empty() != opts.infotainment_assets.empty()))
    {
        shell::log::info("[dashboard] both --cluster-assets and --infotainment-assets "
                         "are required to enable the dashboard layout; ignoring");
    }

#ifdef SHELL_BACKEND_WAYLAND
    std::thread dashinputtest;
    bool dashinputtest_running = false;
    if (dashboard_active && opts.test_dashboard_input)
    {
        dashinputtest_running = true;
        dashinputtest = std::thread(
            [&dash_apps, &backend, &dashinputtest_running, tag]
            {
                shell::log::set_tag(tag);
                run_dashboard_input_test(dash_apps, backend.cb, &dashinputtest_running);
            });
    }
#endif
#endif

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
#ifdef SHELL_RENDERER_GL
    if (sectest.joinable())
    {
        sectest_running = false;
        sectest.join();
    }
    if (isotest.joinable())
    {
        isotest_running = false;
        isotest.join();
    }
    if (isorestarttest.joinable())
    {
        isorestarttest_running = false;
        isorestarttest.join();
    }
    if (secwdtest.joinable())
    {
        secwdtest_running = false;
        secwdtest.join();
    }
#ifdef SHELL_BACKEND_WAYLAND
    if (dashinputtest.joinable())
    {
        dashinputtest_running = false;
        dashinputtest.join();
    }
#endif
    if (dashboard_active)
        teardown_dashboard_apps(engine, dash_apps);
#endif

#ifndef SHELL_BACKEND_WAYLAND
    if (router)
    {
#ifdef SHELL_RENDERER_GL
        // Clear before the target itself so input_router's dispatch_one
        // (which loads the target first, then the hook) never observes a
        // non-null target with a hook that's about to point at a
        // dash_dispatch_ctx/engine this function is tearing down.
        router->dispatch_hooks[display_index].store(nullptr, std::memory_order_release);
#endif
        router->targets[display_index].store(nullptr, std::memory_order_release);
    }
#endif

    shell::engine::destroy(engine);
#ifdef SHELL_BACKEND_WAYLAND
    shell::backend::wayland::destroy(backend);
#else
    shell::backend::drm::close_output(backend); // also closes `card` once refcount hits 0
#endif
}

// Defaults icu/aot_lib for one asset bundle, same convention as the
// original single-bundle --icu/--aot-lib logic: icu defaults to
// <assets>/icudtl.dat; aot_lib auto-detects either flutter-pi/ardera-style
// app.so straight in the assets dir, or Flutter's own
// release_bundle_linux-<arch>_assets layout (sibling lib/libapp.so). Used
// for the primary bundle and, for the dashboard layout, the cluster and
// infotainment bundles too -- each is a fully independent Flutter engine
// instance with its own assets, so each needs its own defaulting pass.
void apply_default_bundle_paths(const std::string& assets, std::string& icu,
                                std::string& aot_lib)
{
    if (icu.empty())
        icu = assets + "/icudtl.dat";
    if (aot_lib.empty())
    {
        auto assets_path = std::filesystem::path(assets);
        auto candidates = {
            assets_path / "app.so",
            assets_path.parent_path() / "lib" / "libapp.so",
        };
        for (const auto& candidate : candidates)
        {
            if (!std::filesystem::exists(candidate))
                continue;
            if (aot_lib.empty())
            {
                aot_lib = candidate.string();
            }
            else
            {
                // Both conventions present, almost always a stale leftover
                // from an earlier build with the other toolchain. Make the
                // ignored one visible instead of silently picking one.
                std::fprintf(stderr, "warning: ignoring %s (using %s instead)\n",
                             candidate.string().c_str(), aot_lib.c_str());
            }
        }
    }
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
    apply_default_bundle_paths(opts.assets, opts.icu, opts.aot_lib);
    if (!opts.cluster_assets.empty())
        apply_default_bundle_paths(opts.cluster_assets, opts.cluster_icu, opts.cluster_aot_lib);
    if (!opts.infotainment_assets.empty())
        apply_default_bundle_paths(opts.infotainment_assets, opts.infotainment_icu,
                                   opts.infotainment_aot_lib);

#ifdef SHELL_RENDERER_GL
    // This process IS the isolated child (Step 4), not a display-owning
    // shell -- bypasses all Wayland/DRM display logic entirely. Spawned
    // only by shell::ipc::spawn_child (see shell::isolated_app), never a
    // normal top-level invocation.
    if (opts.child_renderer)
    {
        shell::child_renderer::config cfg{.assets = opts.assets,
                                          .icu_data = opts.icu,
                                          .aot_library = opts.aot_lib,
                                          .width = opts.child_width,
                                          .height = opts.child_height,
                                          .log_tag = "child",
                                          .ipc_fd = opts.child_ipc_fd,
                                          .input_fd = opts.child_input_fd};
        return shell::child_renderer::run(cfg);
    }
#endif

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
