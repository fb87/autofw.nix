#pragma once

// Shared exponential-backoff primitive for Step 5 (auto-restart of
// isolated/secondary apps). Pure and dependency-free -- no threading, no
// knowledge of what's being restarted. shell::isolated_app and
// shell::secondary_engine each drive their own supervisor loop and call
// into this only for the backoff math.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>

namespace shell::restart_policy
{

struct config
{
    bool enabled{false}; // opt-in -- false preserves pre-Step-5 behavior exactly.
    std::chrono::milliseconds initial_delay{1000};
    std::chrono::milliseconds max_delay{30000};
    double multiplier{2.0};
    std::uint32_t max_restarts{0};                     // 0 = unlimited.
    std::chrono::milliseconds stable_duration{60000}; // run this long -> backoff resets.
};

struct state
{
    std::uint32_t attempt{0};
    std::chrono::steady_clock::time_point attempt_started{};
};

inline auto on_attempt_start(state& s) -> void
{
    s.attempt_started = std::chrono::steady_clock::now();
}

// Call once an attempt has ended (the child/engine stopped running).
// Resets the attempt counter first if this attempt ran long enough to
// count as "stable" (an isolated crash after a long healthy run shouldn't
// inherit a maxed-out delay from earlier flapping -- same idea as
// Kubernetes' CrashLoopBackOff), then returns the delay to wait before the
// next attempt and advances the counter for next time.
inline auto on_attempt_end(state& s, const config& cfg) -> std::chrono::milliseconds
{
    auto now = std::chrono::steady_clock::now();
    if (now - s.attempt_started >= cfg.stable_duration)
        s.attempt = 0;

    double scaled = static_cast<double>(cfg.initial_delay.count()) *
                    std::pow(cfg.multiplier, static_cast<double>(s.attempt));
    auto delay = std::chrono::milliseconds(static_cast<std::int64_t>(scaled));
    delay = std::min(delay, cfg.max_delay);
    ++s.attempt;
    return delay;
}

inline auto should_give_up(const state& s, const config& cfg) -> bool
{
    return cfg.max_restarts != 0 && s.attempt >= cfg.max_restarts;
}

} // namespace shell::restart_policy
