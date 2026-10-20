// shell main -- a minimal Flutter embedder shell.
//
// Header-only shell framework: builds backend::wayland/drm + engine::state
// and runs one or more Flutter app instances, one per display. Generic
// only: framework self-tests live in tests/test_*/ (shell::single_display
// + tests/shell_test_drivers.hpp), demo apps live in examples/ (e.g.
// examples/dashboard) -- neither is wired into this binary's CLI anymore.
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

#include <shell/bundle_paths.hpp>
#include <shell/child_renderer.hpp> // self-guards with #ifdef SHELL_RENDERER_GL
#include <shell/ctrl.hpp>
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

    std::vector<std::string> display_specs;      // raw "--display key=val,..." strings
    std::vector<std::string> input_device_specs;  // raw "--input-device match=idx" strings

    // shell::ctrl: a running shell_app listens on this socket for shell_ctrl
    // (or anything else) to connect and issue runtime commands.
    std::string ctrl_socket{"/tmp/shell_app-ctrl.sock"};

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
// invocation unchanged) or by parsing a `--display` flag.
struct display_spec
{
    // Wayland:
    std::uint32_t width{1280};
    std::uint32_t height{720};
    // DRM:
    std::string drm_device; // empty = auto-detect
    std::optional<std::uint32_t> connector_id; // empty = auto-assign
    bool primary{false};
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
        "                   primary (bare key; exactly one display must be\n"
        "                   primary, default: the first). Zero --display\n"
        "                   flags synthesizes one display from the legacy\n"
        "                   top-level flags above.\n"
        "  --input-device MATCH=DISPLAY_INDEX\n"
        "                   (drm backend only) route one input device's\n"
        "                   events to one display, by index into the\n"
        "                   --display list (0-based). MATCH is checked as:\n"
        "                   a sysname (\"/dev/input/event3\"), a \"vendor:\n"
        "                   product\" hex pair, or a substring of the\n"
        "                   device's name. May be repeated.\n"
        "  --ctrl-socket P  shell::ctrl listen path for shell_ctrl (default\n"
        "                   /tmp/shell_app-ctrl.sock)\n"
        "  --help           show this help\n",
        prog);
}

enum
{
    kOptDisplay = 256,
    kOptInputDevice = 257,
    kOptChildRenderer = 260,
    kOptChildWidth = 261,
    kOptChildHeight = 262,
    kOptChildIpcFd = 263,
    kOptChildInputFd = 272,
    kOptCtrlSocket = 274,
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
        {"ctrl-socket", required_argument, nullptr, kOptCtrlSocket},
        // Hidden child-renderer-mode flags -- not listed in print_usage(),
        // see the options::child_renderer doc comment.
        {"child-renderer", no_argument, nullptr, kOptChildRenderer},
        {"width", required_argument, nullptr, kOptChildWidth},
        {"height", required_argument, nullptr, kOptChildHeight},
        {"ipc-fd", required_argument, nullptr, kOptChildIpcFd},
        {"input-fd", required_argument, nullptr, kOptChildInputFd},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0},
    };
    options out;
    int c;
    while ((c = getopt_long(argc, argv, "a:i:r:h", kLong, nullptr)) != -1)
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
        case kOptCtrlSocket:
            out.ctrl_socket = optarg;
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
// marked explicitly; a hard error if more than one was.
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

// shell::ctrl's only runtime target right now -- published by
// run_one_display for whichever display is primary, right after its
// engine::init() succeeds (cleared before engine::destroy()). Same
// publish/clear pattern input_router already uses for its own per-display
// engine::state* targets, just unconditional on backend since shell_ctrl
// needs it regardless of Wayland/DRM.
std::atomic<shell::engine::state*> g_primary_engine{nullptr};

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
    if (spec.primary)
        g_primary_engine.store(&engine, std::memory_order_release);

#ifdef SHELL_BACKEND_WAYLAND
    shell::backend::wayland::run(backend);
#else
    shell::backend::drm::run(backend);
#endif

    // Kiosk shutdown: this display's run() loop returned (naturally closed,
    // or a backend-internal error) -- tear every other display down too.
    stop_all.store(true, std::memory_order_relaxed);

    if (spec.primary)
        g_primary_engine.store(nullptr, std::memory_order_release);
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

// No SIGTERM/SIGINT handler existed before this -- confirmed directly
// while testing backend/wayland.hpp's destroy() fix: a plain `kill` on
// this process never reached any of the shell::backend::*/engine::destroy
// cleanup paths, because the OS default action for SIGTERM is immediate
// termination. In production that matters: systemd sends SIGTERM on every
// `systemctl stop`/`restart` of the unit. Just setting `stop_all` here and
// letting the existing shutdown path run is enough -- that's the exact
// same mechanism one display's own connect/setup failure already uses to
// wind every display down cleanly (see run_one_display's `fail` lambda),
// so this doesn't introduce a new shutdown path, just a new trigger for
// the one that already exists.
//
// `g_stop_all` is a raw pointer, not a reference, because a signal handler
// is a plain C-style function with no way to capture state -- set once in
// main(), right where the real std::atomic<bool> it points to is
// constructed, before any thread that could receive a signal is spawned.
// The store is relaxed and lock-free (std::atomic<bool> always is on every
// platform this project targets) -- the only async-signal-safe operation
// this handler needs.
std::atomic<bool>* g_stop_all = nullptr;

void handle_shutdown_signal(int)
{
    if (g_stop_all)
        g_stop_all->store(true, std::memory_order_relaxed);
}

namespace
{
// shell::ctrl command dispatch: a running shell_app's only runtime control
// surface right now.
std::string dispatch_ctrl_command(const std::string& line)
{
    std::istringstream iss(line);
    std::string verb;
    iss >> verb;
    if (verb == "set-layer-visible")
    {
        long id = 0;
        std::string vis;
        iss >> id >> vis;
        if (vis != "true" && vis != "false")
            return "error: usage: set-layer-visible <id> <true|false>";
        auto* e = g_primary_engine.load(std::memory_order_acquire);
        if (!e)
            return "error: no primary display up yet";
        bool ok = shell::engine::set_layer_visible(*e, static_cast<shell::layer::id>(id),
                                                   vis == "true");
        return ok ? "ok" : "error: unknown layer id";
    }
    return "error: unknown command";
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
    shell::apply_default_bundle_paths(opts.assets, opts.icu, opts.aot_lib);

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
    g_stop_all = &stop_all;
    struct sigaction sa{};
    sa.sa_handler = handle_shutdown_signal;
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);

    auto ctrl_listener = shell::ctrl::listen(opts.ctrl_socket);
    std::thread ctrl_thread;
    if (ctrl_listener)
    {
        ctrl_thread = std::thread(
            [&]
            {
                while (!stop_all.load(std::memory_order_relaxed))
                {
                    auto cmd = shell::ctrl::accept_command(*ctrl_listener, 200);
                    if (!cmd)
                        continue;
                    auto resp = dispatch_ctrl_command(cmd->line);
                    shell::ctrl::respond(*cmd, resp);
                }
            });
    }
    else
    {
        shell::log::info(std::string("ctrl listen failed: ") + ctrl_listener.error().message);
    }

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

    if (ctrl_thread.joinable())
    {
        ctrl_thread.join();
        shell::ctrl::close(*ctrl_listener);
    }

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
