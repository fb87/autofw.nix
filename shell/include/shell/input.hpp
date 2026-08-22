#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <utility>

namespace shell::input
{

enum class pointer_phase
{
    add,
    remove,
    hover,
    down,
    move,
    up,
    cancel,
};

enum class device_type
{
    touch,
    pointer,
    keyboard,
};

struct pointer_event
{
    std::uint32_t device{};
    pointer_phase phase{};
    double x{};
    double y{};
    std::uint64_t timestamp{};
    device_type kind{device_type::pointer};
};

// A normalized input source. It owns a file descriptor (Wayland, libinput,
// eventfd, timerfd, ...) and dispatches events. Driver-specific translations
// (libinput -> pointer_event, etc.) live in the driver adapter, not here.
struct source
{
    int fd{-1};
    std::function<void(const pointer_event&)> on_event;

    std::uint32_t width{};
    std::uint32_t height{};
};

// Normalize a driver event to shell input coordinates. |event| is expected to
// already carry logical coordinates when libinput does the mapping; for raw
// absolute devices (touch), x/y are in units/per-mm and may pass through.
inline auto normalize(const pointer_event& ev, std::uint32_t surface_w,
                      std::uint32_t surface_h) -> pointer_event
{
    pointer_event out = ev;
    (void)surface_w;
    (void)surface_h;
    return out;
}

// Map an absolute device coordinate (0..range) into a surface coordinate and
// produce a pointer event at that position.
inline auto from_absolute(std::uint32_t device, pointer_phase phase, std::uint64_t ts,
                          double ax, double ay, double amax_x, double amax_y,
                          std::uint32_t surface_w, std::uint32_t surface_h)
    -> std::optional<pointer_event>
{
    if (amax_x <= 0.0 || amax_y <= 0.0 || surface_w == 0 || surface_h == 0)
        return std::nullopt;
    pointer_event ev{};
    ev.device = device;
    ev.phase = phase;
    ev.timestamp = ts;
    ev.kind = device_type::touch;
    ev.x = ax / amax_x * surface_w;
    ev.y = ay / amax_y * surface_h;
    return ev;
}

// A running input state (holds any active sources).
struct state
{
    std::optional<source> active;
};

inline auto set_source(state& s, source src) -> void
{
    s.active = std::move(src);
}

inline auto fd(const state& s) -> int
{
    return s.active ? s.active->fd : -1;
}

} // namespace shell::input
