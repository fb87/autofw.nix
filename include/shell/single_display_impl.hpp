#pragma once

#include <csignal>
#include <cstdio>
#include <string_view>
#include <thread>
#include <vector>

#include <shell/log.hpp>

#ifdef SHELL_BACKEND_WAYLAND
#include <shell/backend/wayland.hpp>
#else
#include <shell/backend/drm.hpp>
#include <shell/input_router.hpp>
#endif

namespace shell::single_display
{
namespace detail
{

// Same pattern as src/main.cpp's g_stop_all/handle_shutdown_signal,
// generalized here so every standalone binary built on run() gets
// graceful shutdown without repeating this.
inline std::atomic<bool>* g_stop_all = nullptr;

inline void handle_shutdown_signal(int)
{
    if (g_stop_all)
        g_stop_all->store(true, std::memory_order_relaxed);
}

inline void install_signal_handlers(std::atomic<bool>& stop_all)
{
    g_stop_all = &stop_all;
    struct sigaction sa{};
    sa.sa_handler = handle_shutdown_signal;
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);
}

} // namespace detail

inline auto run(const config& cfg, const callbacks& cb) -> int
{
    shell::engine::state engine;
    engine.cfg = cfg.engine_cfg;

    std::atomic<bool> stop_all{false};
    detail::install_signal_handlers(stop_all);

    auto fail = [&](std::string_view what, const shell::error& err) -> int
    {
        shell::log::info(std::string(what) + ": " + err.message);
        return 1;
    };

    shell::backend::output out{};

#ifdef SHELL_BACKEND_WAYLAND
    shell::backend::wayland::config wcfg{.display = {}, .width = cfg.width, .height = cfg.height};
    shell::backend::wayland::state backend = shell::backend::wayland::create(wcfg);
    if (auto err = shell::backend::wayland::connect(backend); !err)
    {
        shell::backend::wayland::destroy(backend);
        return fail("wayland connect failed", err.error());
    }
    if (auto err = shell::backend::wayland::setup_egl(backend); !err)
    {
        shell::backend::wayland::destroy(backend);
        return fail("wayland setup_egl failed", err.error());
    }
    if (auto err = shell::backend::wayland::setup_surface(backend); !err)
    {
        shell::backend::wayland::destroy(backend);
        return fail("wayland setup_surface failed", err.error());
    }
    out = shell::backend::wayland::output::take(backend);
    backend.cb.on_vsync = [&] { shell::engine::on_vsync(engine); };
    backend.cb.on_pointer = [&](const shell::input::pointer_event& ev)
    {
        if (cb.on_pointer)
            cb.on_pointer(ev);
        else
            shell::engine::send_pointer(engine, ev);
    };
    backend.cb.on_key = [&](std::uint32_t code, bool down)
    {
        if (cb.on_key)
            cb.on_key(code, down);
        else
            shell::engine::send_key(engine, code, down);
    };
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
    auto device = cfg.drm_device;
    if (device.empty())
    {
        auto found = shell::backend::drm::find_connected_device();
        if (!found)
            return fail("drm auto-detect failed", found.error());
        device = *found;
    }
    auto card_result = shell::backend::drm::open_card(device);
    if (!card_result)
        return fail("drm open_card failed", card_result.error());
    shell::backend::drm::card card = std::move(*card_result);

    auto connectors = shell::backend::drm::list_connectors(card);
    std::uint32_t connector_id = 0;
    bool found_connector = false;
    for (const auto& c : connectors)
    {
        if (c.connected && c.mode_count > 0)
        {
            connector_id = c.connector_id;
            found_connector = true;
            break;
        }
    }
    if (!found_connector)
    {
        shell::backend::drm::close_card(card);
        return fail("drm: no connected connector",
                    shell::error{.code = shell::error_code::drm,
                                 .message = "no connected connector with a mode"});
    }

    auto output_result = shell::backend::drm::open_output(card, connector_id);
    if (!output_result)
    {
        shell::backend::drm::close_card(card);
        return fail("drm open_output failed", output_result.error());
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

    // Private, single-display input routing: one catch-all rule ("" is a
    // substring of every device name) sends every device straight to this
    // one engine -- no need for shell_app's multi-device-to-many-displays
    // input_router::create(rules, display_count) complexity here.
    shell::input_router::state router;
    std::vector<shell::input_router::device_rule> rules{
        {.match = "", .display_index = 0}};
    if (auto err = shell::input_router::create(router, rules, 1); !err)
    {
        shell::backend::drm::close_output(backend);
        return fail("input_router create failed", err.error());
    }
#endif

    engine.out = &out;

    if (auto err = shell::engine::init(engine); !err)
    {
#ifdef SHELL_BACKEND_WAYLAND
        shell::backend::wayland::destroy(backend);
#else
        shell::input_router::destroy(router);
        shell::backend::drm::close_output(backend);
#endif
        return fail("engine init failed", err.error());
    }

#ifndef SHELL_BACKEND_WAYLAND
    router.targets[0].store(&engine, std::memory_order_release);
    shell::input_router::dispatch_hook hook{};
    struct hook_ctx
    {
        const callbacks* cb;
        shell::engine::state* engine;
    } ctx{&cb, &engine};
    if (cb.on_pointer || cb.on_key)
    {
        hook.ctx = &ctx;
        hook.on_pointer = [](void* c, const shell::input::pointer_event& ev)
        {
            auto* h = static_cast<hook_ctx*>(c);
            if (h->cb->on_pointer)
                h->cb->on_pointer(ev);
            else
                shell::engine::send_pointer(*h->engine, ev);
        };
        hook.on_key = [](void* c, std::uint32_t code, bool down)
        {
            auto* h = static_cast<hook_ctx*>(c);
            if (h->cb->on_key)
                h->cb->on_key(code, down);
            else
                shell::engine::send_key(*h->engine, code, down);
        };
        router.dispatch_hooks[0].store(&hook, std::memory_order_release);
    }
    std::thread input_thread(shell::input_router::run, std::ref(router));
#endif

    if (cb.on_ready)
        cb.on_ready(engine);

#ifdef SHELL_BACKEND_WAYLAND
    shell::backend::wayland::run(backend);
#else
    shell::backend::drm::run(backend);
#endif

    stop_all.store(true, std::memory_order_relaxed);

#ifndef SHELL_BACKEND_WAYLAND
    router.targets[0].store(nullptr, std::memory_order_release);
    router.dispatch_hooks[0].store(nullptr, std::memory_order_release);
    shell::input_router::stop(router);
    input_thread.join();
    shell::input_router::destroy(router);
#endif

    if (cb.on_before_destroy)
        cb.on_before_destroy(engine);

    shell::engine::destroy(engine);
#ifdef SHELL_BACKEND_WAYLAND
    shell::backend::wayland::destroy(backend);
#else
    shell::backend::drm::close_output(backend);
#endif
    return 0;
}

} // namespace shell::single_display
