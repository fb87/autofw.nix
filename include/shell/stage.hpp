#pragma once

#include <cstdint>
#include <unordered_map>

#include <shell/result.hpp>

namespace shell::stage
{

using id = std::uint64_t;

struct extent
{
    std::uint32_t width{};
    std::uint32_t height{};
};

enum class visibility
{
    hidden,
    appearing,
    visible,
    disappearing,
};

struct presentation
{
    visibility state{visibility::hidden};
    float progress{}; // 0..1 fade/animate progress
};

// A logical scene container. Its size, presentation and priority describe how
// it is presented relative to other visible stages.
struct state
{
    id value{};
    extent size{};
    presentation view{};
    int priority{};
};

// A registry of stages keyed by id.
using registry = std::unordered_map<id, state>;

inline auto add(registry& r, const state& s) -> bool
{
    return r.emplace(s.value, s).second;
}

inline auto remove(registry& r, id sid) -> bool
{
    return r.erase(sid) > 0;
}

inline auto get(registry& r, id sid) -> state*
{
    auto it = r.find(sid);
    return it == r.end() ? nullptr : &it->second;
}

inline auto has(const registry& r, id sid) -> bool
{
    return r.contains(sid);
}

inline auto is_visible(const state& s) -> bool
{
    return s.view.state == visibility::visible || s.view.state == visibility::appearing;
}

// Update visibility while preserving a progress for transitions.
inline auto set_visible(state& s, bool visible) -> void
{
    s.view.state = visible ? visibility::visible : visibility::hidden;
    s.view.progress = visible ? 1.0F : 0.0F;
}

inline auto begin_appear(state& s) -> void
{
    s.view.state = visibility::appearing;
    s.view.progress = 0.0F;
}

inline auto begin_disappear(state& s) -> void
{
    s.view.state = visibility::disappearing;
    s.view.progress = 1.0F;
}

} // namespace shell::stage
