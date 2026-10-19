#pragma once

// One process-wide libinput/udev seat context, shared across every display
// in the process, with per-device routing to a specific display's engine.
//
// Why this exists (Step 2, multi-display): each `shell::backend::drm::state`
// used to create its own libinput context and call
// `libinput_udev_assign_seat(li, "seat0")`, which grabs *every* input device
// on the seat. Two DRM outputs in one process would each independently grab
// every device, so every physical input event would be delivered and
// dispatched twice. Here there is exactly one libinput context for the whole
// process; `device_rule`s decide which display (by index) a given physical
// device's events go to, and decoded events are sent straight to that
// display's `shell::engine::state` via `engine::send_pointer`/`send_key` --
// safe to call from this module's own dedicated thread because those
// functions already marshal the actual `FlutterEngineSend*` call onto the
// *target* engine's own platform-runner thread internally.
//
// DRM-only. Wayland dev-mode windows keep relying on the compositor's own
// per-surface seat/focus model; per-device-to-display assignment isn't a
// meaningful concept there.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <span>
#include <string>
#include <string_view>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

#include <linux/input-event-codes.h>

#include <libinput.h>
#include <libudev.h>

#include <shell/engine.hpp>
#include <shell/error.hpp>
#include <shell/input.hpp>
#include <shell/log.hpp>
#include <shell/result.hpp>

namespace shell::input_router
{

// A device-to-display assignment rule. `match` is checked in this order:
//   1. "/dev/input/eventN"  -- exact match against the device's sysname.
//   2. "VVVV:PPPP" (hex)    -- exact match against vendor:product id.
//   3. anything else        -- substring match against the device's name
//                              (e.g. "DURGOD Taurus K320").
struct device_rule
{
    std::string match;
    std::size_t display_index{};
};

struct per_display_pointer_state
{
    double x{};
    double y{};
    std::uint32_t buttons{};
    // Last known position per touch seat slot (libinput seat slots are
    // small, reused non-negative integers), for LIBINPUT_EVENT_TOUCH_UP,
    // which carries no coordinates of its own.
    static constexpr std::size_t kMaxTouchSlots = 10;
    std::array<std::pair<double, double>, kMaxTouchSlots> touch_pos{};
};

// Lets a caller intercept a display's pointer/key events instead of having
// them go straight to its `engine::state*` -- e.g. the dashboard's
// position-based dispatcher (hit-test a tap against on-screen panes, route
// to whichever one it landed in) needs the same interception under DRM
// that it already gets for free under Wayland via backend.cb.on_pointer/
// on_key. Deliberately a plain ctx + function-pointer pair, not a
// std::function: this module knows nothing about dashboards/isolated apps
// and shouldn't have to -- the caller owns `ctx` and both functions for as
// long as the hook is installed (same lifetime as the display itself).
struct dispatch_hook
{
    void* ctx{};
    void (*on_pointer)(void* ctx, const input::pointer_event& ev){};
    void (*on_key)(void* ctx, std::uint32_t code, bool down){};
};

struct state
{
    udev* udev_ctx{};
    libinput* li{};
    std::vector<device_rule> rules;
    std::unordered_map<libinput_device*, std::size_t> assigned;
    // Indexed by display_index. Published by each display's own thread
    // (store the live engine after a successful engine::init(), clear back
    // to nullptr before engine::destroy()); read by this module's run()
    // thread. Lock-free, since engine::send_pointer/send_key are themselves
    // safe to call from any thread.
    std::vector<std::atomic<shell::engine::state*>> targets;
    // Indexed the same as `targets`; nullptr (the default) means "send
    // straight to targets[idx]", today's exact behavior -- zero change for
    // any display that never installs a hook.
    std::vector<std::atomic<const dispatch_hook*>> dispatch_hooks;
    std::vector<per_display_pointer_state> pointer_state;
    bool running{};
};

namespace detail
{
inline int li_open_restricted(const char* path, int flags, void*)
{
    return open(path, flags);
}
inline void li_close_restricted(int fd, void*)
{
    close(fd);
}
inline const libinput_interface kLibinputInterface = {
    .open_restricted = li_open_restricted,
    .close_restricted = li_close_restricted,
};

inline auto button_mask(std::uint32_t button) -> std::uint32_t
{
    if (button == BTN_LEFT)
        return 0x1;
    if (button == BTN_RIGHT)
        return 0x2;
    if (button == BTN_MIDDLE)
        return 0x4;
    return 0;
}

// Routes libinput's own diagnostics through shell::log instead of its
// default raw-stderr handler.
inline void li_log_handler(libinput*, libinput_log_priority priority,
                           const char* format, va_list args)
{
    char buf[512];
    std::vsnprintf(buf, sizeof(buf), format, args);
    const char* level = priority >= LIBINPUT_LOG_PRIORITY_ERROR  ? "error"
                        : priority >= LIBINPUT_LOG_PRIORITY_INFO ? "info"
                                                                 : "debug";
    log::info(std::string("libinput ") + level + ": " + buf);
}

// Checked in the order documented on device_rule::match.
inline auto match_device(libinput_device* dev, const std::string& match) -> bool
{
    static constexpr std::string_view kDevPrefix = "/dev/input/";
    if (match.rfind(kDevPrefix.data(), 0, kDevPrefix.size()) == 0)
    {
        const char* sysname = libinput_device_get_sysname(dev);
        return sysname && match.substr(kDevPrefix.size()) == sysname;
    }
    auto colon = match.find(':');
    if (colon != std::string::npos)
    {
        char* end_vendor = nullptr;
        char* end_product = nullptr;
        unsigned long want_vendor =
            std::strtoul(match.substr(0, colon).c_str(), &end_vendor, 16);
        unsigned long want_product =
            std::strtoul(match.substr(colon + 1).c_str(), &end_product, 16);
        if (*end_vendor == '\0' && *end_product == '\0' && !match.substr(0, colon).empty())
        {
            return libinput_device_get_id_vendor(dev) == want_vendor &&
                   libinput_device_get_id_product(dev) == want_product;
        }
    }
    const char* name = libinput_device_get_name(dev);
    return name && std::string(name).find(match) != std::string::npos;
}
} // namespace detail

// Creates the one process-wide libinput/udev seat context. Call exactly
// once, before any display thread starts (so device-added events enumerate
// deterministically early), and before spawning run()'s thread.
inline auto create(state& s, std::span<const device_rule> rules,
                   std::size_t display_count) -> result<void>
{
    s.rules.assign(rules.begin(), rules.end());
    s.targets = std::vector<std::atomic<shell::engine::state*>>(display_count);
    for (auto& t : s.targets)
        t.store(nullptr, std::memory_order_relaxed);
    s.dispatch_hooks = std::vector<std::atomic<const dispatch_hook*>>(display_count);
    for (auto& h : s.dispatch_hooks)
        h.store(nullptr, std::memory_order_relaxed);
    s.pointer_state.assign(display_count, per_display_pointer_state{});

    s.udev_ctx = udev_new();
    if (!s.udev_ctx)
        return unexpected(error{.code = error_code::drm, .message = "udev_new"});
    s.li = libinput_udev_create_context(&detail::kLibinputInterface, &s, s.udev_ctx);
    if (!s.li)
        return unexpected(
            error{.code = error_code::drm, .message = "libinput_udev_create_context"});
    libinput_log_set_handler(s.li, detail::li_log_handler);
    if (libinput_udev_assign_seat(s.li, "seat0") != 0)
        return unexpected(
            error{.code = error_code::drm, .message = "libinput_udev_assign_seat"});
    return {};
}

namespace detail
{
// Routes through the display's hook if one is installed, otherwise straight
// to its engine target -- the one place that decision is made, so
// dispatch_one's event-decoding switch below doesn't need to care which
// path a given display uses.
inline void send_pointer(state& s, std::size_t idx, shell::engine::state& target,
                         const input::pointer_event& ev)
{
    const dispatch_hook* hook = s.dispatch_hooks[idx].load(std::memory_order_acquire);
    if (hook && hook->on_pointer)
        hook->on_pointer(hook->ctx, ev);
    else
        shell::engine::send_pointer(target, ev);
}

inline void send_key(state& s, std::size_t idx, shell::engine::state& target,
                     std::uint32_t code, bool down)
{
    const dispatch_hook* hook = s.dispatch_hooks[idx].load(std::memory_order_acquire);
    if (hook && hook->on_key)
        hook->on_key(hook->ctx, code, down);
    else
        shell::engine::send_key(target, code, down);
}

inline void dispatch_one(state& s, libinput_event* ev)
{
    libinput_event_type type = libinput_event_get_type(ev);
    libinput_device* dev = libinput_event_get_device(ev);

    if (type == LIBINPUT_EVENT_DEVICE_ADDED)
    {
        const char* name = libinput_device_get_name(dev);
        for (const auto& rule : s.rules)
        {
            if (match_device(dev, rule.match))
            {
                s.assigned[dev] = rule.display_index;
                log::info(std::string("input_router: assigned '") + (name ? name : "?") +
                          "' to display " + std::to_string(rule.display_index));
                return;
            }
        }
        log::info(std::string("input_router: '") + (name ? name : "?") +
                  "' matched no --input-device rule; ignored");
        return;
    }
    if (type == LIBINPUT_EVENT_DEVICE_REMOVED)
    {
        s.assigned.erase(dev);
        return;
    }

    auto it = s.assigned.find(dev);
    if (it == s.assigned.end())
        return; // device not assigned to any display -- ignore its events
    std::size_t idx = it->second;
    if (idx >= s.targets.size())
        return;
    shell::engine::state* target = s.targets[idx].load(std::memory_order_acquire);
    if (!target)
        return; // display not up yet (or already torn down)
    per_display_pointer_state& ps = s.pointer_state[idx];
    std::uint32_t w = target->out ? target->out->width : 0;
    std::uint32_t h = target->out ? target->out->height : 0;

    switch (type)
    {
    case LIBINPUT_EVENT_KEYBOARD_KEY:
    {
        auto* kev = libinput_event_get_keyboard_event(ev);
        std::uint32_t code = libinput_event_keyboard_get_key(kev);
        bool pressed =
            libinput_event_keyboard_get_key_state(kev) == LIBINPUT_KEY_STATE_PRESSED;
        send_key(s, idx, *target, code, pressed);
        break;
    }
    case LIBINPUT_EVENT_POINTER_MOTION:
    {
        auto* pev = libinput_event_get_pointer_event(ev);
        ps.x = std::clamp(ps.x + libinput_event_pointer_get_dx(pev), 0.0,
                          static_cast<double>(w));
        ps.y = std::clamp(ps.y + libinput_event_pointer_get_dy(pev), 0.0,
                          static_cast<double>(h));
        send_pointer(
            s, idx, *target,
            input::pointer_event{.phase = ps.buttons ? input::pointer_phase::move
                                                     : input::pointer_phase::hover,
                                 .x = ps.x,
                                 .y = ps.y,
                                 .buttons = ps.buttons});
        break;
    }
    case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE:
    {
        auto* pev = libinput_event_get_pointer_event(ev);
        ps.x = libinput_event_pointer_get_absolute_x_transformed(pev, w);
        ps.y = libinput_event_pointer_get_absolute_y_transformed(pev, h);
        send_pointer(
            s, idx, *target,
            input::pointer_event{.phase = ps.buttons ? input::pointer_phase::move
                                                     : input::pointer_phase::hover,
                                 .x = ps.x,
                                 .y = ps.y,
                                 .buttons = ps.buttons});
        break;
    }
    case LIBINPUT_EVENT_POINTER_BUTTON:
    {
        auto* pev = libinput_event_get_pointer_event(ev);
        std::uint32_t mask = button_mask(libinput_event_pointer_get_button(pev));
        if (mask)
        {
            bool pressed = libinput_event_pointer_get_button_state(pev) ==
                           LIBINPUT_BUTTON_STATE_PRESSED;
            if (pressed)
                ps.buttons |= mask;
            else
                ps.buttons &= ~mask;
            send_pointer(
                s, idx, *target,
                input::pointer_event{.phase = pressed ? input::pointer_phase::down
                                                       : input::pointer_phase::up,
                                     .x = ps.x,
                                     .y = ps.y,
                                     .buttons = ps.buttons});
        }
        break;
    }
    case LIBINPUT_EVENT_TOUCH_DOWN:
    case LIBINPUT_EVENT_TOUCH_MOTION:
    {
        auto* tev = libinput_event_get_touch_event(ev);
        std::int32_t slot = libinput_event_touch_get_seat_slot(tev);
        double x = libinput_event_touch_get_x_transformed(tev, w);
        double y = libinput_event_touch_get_y_transformed(tev, h);
        if (slot >= 0 &&
            static_cast<std::size_t>(slot) < per_display_pointer_state::kMaxTouchSlots)
            ps.touch_pos[static_cast<std::size_t>(slot)] = {x, y};
        send_pointer(
            s, idx, *target,
            input::pointer_event{
                .device = static_cast<std::uint32_t>(slot),
                .phase = type == LIBINPUT_EVENT_TOUCH_DOWN ? input::pointer_phase::down
                                                           : input::pointer_phase::move,
                .x = x,
                .y = y,
                .kind = input::device_type::touch,
                .buttons = 0x1});
        break;
    }
    case LIBINPUT_EVENT_TOUCH_UP:
    {
        auto* tev = libinput_event_get_touch_event(ev);
        std::int32_t slot = libinput_event_touch_get_seat_slot(tev);
        auto [x, y] =
            (slot >= 0 &&
             static_cast<std::size_t>(slot) < per_display_pointer_state::kMaxTouchSlots)
                ? ps.touch_pos[static_cast<std::size_t>(slot)]
                : std::pair<double, double>{};
        send_pointer(
            s, idx, *target,
            input::pointer_event{.device = static_cast<std::uint32_t>(slot),
                                 .phase = input::pointer_phase::up,
                                 .x = x,
                                 .y = y,
                                 .kind = input::device_type::touch});
        break;
    }
    default:
        break;
    }
}
} // namespace detail

// Blocking dispatch loop: wait on the libinput fd (bounded by a timeout so
// the loop can observe `running` going false promptly), then drain and
// route events. Call on its own dedicated thread.
//
// Deliberately does NOT gate libinput_dispatch()/get_event() behind
// poll()'s readiness bit: on at least one real target (Rockchip/Panfrost
// GBM), poll() on libinput_get_fd() can report "no data" (timeout)
// indefinitely for a device's already-enumerated initial event batch even
// though libinput_dispatch()+get_event() reliably drain it when called
// directly -- the kernel-level epoll readiness for that fd appears to get
// consumed internally by libinput's own udev enumeration before this
// thread ever gets to poll() it. Calling dispatch unconditionally every
// tick sidesteps that: poll() here is purely a sleep, not a gate.
inline auto run(state& s) -> void
{
    int fd = libinput_get_fd(s.li);
    s.running = true;
    while (s.running)
    {
        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        poll(&pfd, 1, 100);
        libinput_dispatch(s.li);
        libinput_event* ev;
        while ((ev = libinput_get_event(s.li)) != nullptr)
        {
            detail::dispatch_one(s, ev);
            libinput_event_destroy(ev);
        }
    }
}

inline auto stop(state& s) -> void
{
    s.running = false;
}

inline auto destroy(state& s) -> void
{
    if (s.li)
        libinput_unref(s.li);
    if (s.udev_ctx)
        udev_unref(s.udev_ctx);
    s.li = nullptr;
    s.udev_ctx = nullptr;
}

} // namespace shell::input_router
