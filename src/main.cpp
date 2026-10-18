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
        {"test-secondary-engine", no_argument, nullptr, kOptTestSecondaryEngine},
        {"test-isolated-app", no_argument, nullptr, kOptTestIsolatedApp},
        {"test-isolated-app-restart", no_argument, nullptr, kOptTestIsolatedAppRestart},
        {"test-secondary-engine-watchdog", no_argument, nullptr, kOptTestSecondaryEngineWatchdog},
        // Hidden child-renderer-mode flags -- not listed in print_usage(),
        // see the options::child_renderer doc comment.
        {"child-renderer", no_argument, nullptr, kOptChildRenderer},
        {"width", required_argument, nullptr, kOptChildWidth},
        {"height", required_argument, nullptr, kOptChildHeight},
        {"ipc-fd", required_argument, nullptr, kOptChildIpcFd},
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
        case kOptChildRenderer:
            out.child_renderer = true;
            break;
        case kOptChildWidth:
            out.child_width = static_cast<std::uint32_t>(std::stoul(optarg));
            break;
        case kOptChildHeight:
            out.child_height = static_cast<std::uint32_t>(std::stoul(optarg));
            break;
        case kOptChildIpcFd:
            out.child_ipc_fd = std::stoi(optarg);
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
#endif // SHELL_RENDERER_GL

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
#endif

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
                                          .ipc_fd = opts.child_ipc_fd};
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
