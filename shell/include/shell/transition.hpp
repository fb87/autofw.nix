#pragma once

#include <chrono>
#include <cstdint>

#include <shell/result.hpp>

namespace shell::transition
{

using id = std::uint64_t;

enum class target_type
{
    stage,
    layer,
    surface,
};

enum class kind
{
    none,
    fade,
    crossfade,
    slide_left,
    slide_right,
    slide_up,
    slide_down,
    scale,
};

enum class status
{
    pending,
    running,
    completed,
    cancelled,
    interrupted,
};

enum class easing
{
    linear,
    ease_in,
    ease_out,
    ease_in_out,
};

struct spec
{
    kind type{kind::none};
    std::chrono::milliseconds duration{250};
    easing curve{easing::ease_in_out};
};

struct target
{
    target_type type{target_type::stage};
    std::uint64_t id{};
};

struct request
{
    target from{};
    target to{};
    spec animation{};
    int priority{};
};

struct state
{
};

inline auto start(state&, const request&) -> result<id>
{
    return std::unexpected(error{
        .code = error_code::unavailable,
        .message = "transition::start is not implemented",
    });
}

inline auto cancel(state&, id) -> result<void>
{
    return std::unexpected(error{
        .code = error_code::unavailable,
        .message = "transition::cancel is not implemented",
    });
}

} // namespace shell::transition
