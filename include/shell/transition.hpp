#pragma once

#include <chrono>
#include <cstdint>
#include <unordered_map>
#include <vector>

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

// A single active transition with its lifecycle.
struct active
{
    id value{};
    request req{};
    status phase{status::pending};
    std::chrono::steady_clock::time_point start{};
    float progress{}; // 0..1 within the duration
};

// Registry of transitions keyed by id.
struct state
{
    id next{1};
    std::unordered_map<id, active> map;

    // Cancel a running transition in favour of a higher priority one.
    inline static bool is_priority_over(int current, int newer)
    {
        return newer > current;
    }
};

// Map a linear progress t in [0,1] through the easing curve.
inline auto ease(active& a, float t) -> float
{
    auto apply = [t](float) { return t; };
    switch (a.req.animation.curve)
    {
    case easing::linear:
        return t;
    case easing::ease_in:
        return t * t;
    case easing::ease_out:
        return t * (2.0F - t);
    case easing::ease_in_out:
        return t < 0.5F ? 2.0F * t * t : -1.0F + (4.0F - 2.0F * t) * t;
    }
    (void)apply;
    return t;
}

// Begin a transition. A higher-priority request interrupts any running
// transition of the same target (set to interrupted) before it starts.
inline auto start(state& s, const request& req) -> result<id>
{
    // Interrupt lower-priority transitions targeting the same destination.
    for (auto it = s.map.begin(); it != s.map.end();)
    {
        active& a = it->second;
        if ((a.phase == status::running || a.phase == status::pending) &&
            a.req.to.type == req.to.type && a.req.to.id == req.to.id &&
            s.is_priority_over(a.req.priority, req.priority))
        {
            a.phase = status::interrupted;
        }
        ++it;
    }
    id value = s.next++;
    active a{};
    a.value = value;
    a.req = req;
    a.phase = status::pending;
    a.start = std::chrono::steady_clock::now();
    s.map.emplace(value, a);
    return value;
}

// Advance a pending transition to running.
inline auto run(state& s, id tid) -> result<void>
{
    auto it = s.map.find(tid);
    if (it == s.map.end())
        return shell::unexpected(
            error{.code = error_code::invalid_argument, .message = "transition id"});
    it->second.phase = status::running;
    return {};
}

// Advance a transition by the wall clock; completes when its duration passes.
// Applies the easing curve to progress. Returns false once the transition no
// longer needs advancing (completed/cancelled/interrupted).
inline auto advance(state& s, id tid) -> bool
{
    auto it = s.map.find(tid);
    if (it == s.map.end())
        return false;
    active& a = it->second;
    if (a.phase == status::cancelled || a.phase == status::interrupted ||
        a.phase == status::completed)
        return false;
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - a.start);
    std::int64_t total = a.req.animation.duration.count();
    a.phase = status::running;
    if (total <= 0)
    {
        a.progress = 1.0F;
        a.phase = status::completed;
        return false;
    }
    float raw = static_cast<float>(elapsed.count()) / static_cast<float>(total);
    if (raw >= 1.0F)
    {
        a.progress = 1.0F;
        a.phase = status::completed;
        return false;
    }
    a.progress = ease(a, raw);
    return true;
}

// Cancel a transition; its target returns to its previous presentation state.
inline auto cancel(state& s, id tid) -> result<void>
{
    auto it = s.map.find(tid);
    if (it == s.map.end())
        return shell::unexpected(
            error{.code = error_code::invalid_argument, .message = "transition id"});
    it->second.phase = status::cancelled;
    it->second.progress = 0.0F;
    return {};
}

// Deterministically advance a transition by an explicit elapsed time (ms), so
// callers do not depend on the wall clock. Returns false when it is done.
inline auto update(state& s, id tid, std::int64_t elapsed_ms) -> bool
{
    auto it = s.map.find(tid);
    if (it == s.map.end())
        return false;
    active& a = it->second;
    if (a.phase == status::cancelled || a.phase == status::interrupted ||
        a.phase == status::completed)
        return false;
    a.phase = status::running;
    auto total = a.req.animation.duration.count();
    if (total <= 0)
    {
        a.progress = 1.0F;
        a.phase = status::completed;
        return false;
    }
    float raw = static_cast<float>(elapsed_ms) / static_cast<float>(total);
    if (raw >= 1.0F)
    {
        a.progress = 1.0F;
        a.phase = status::completed;
        return false;
    }
    a.progress = ease(a, raw);
    return true;
}

inline auto remove(state& s, id tid) -> bool
{
    return s.map.erase(tid) > 0;
}

inline auto get(state& s, id tid) -> active*
{
    return s.map.contains(tid) ? &s.map[tid] : nullptr;
}

} // namespace shell::transition
