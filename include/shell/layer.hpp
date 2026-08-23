#pragma once

#include <cstdint>
#include <unordered_map>

#include <shell/stage.hpp>

namespace shell::layer
{

using id = std::uint64_t;

enum class role
{
    background,
    application,
    system_ui,
    overlay,
    notification,
    keyboard,
    emergency,
};

enum class input_mode
{
    passthrough,
    normal,
    exclusive,
};

struct geometry
{
    float x{};
    float y{};
    float width{};
    float height{};
};

struct state
{
    id value{};
    stage::id stage{};
    role kind{role::application};
    geometry bounds{};
    float opacity{1.0F};
    bool visible{true};
    input_mode input{input_mode::normal};
};

// Recommended default z-order per semantic role (DESIGN.md).
inline auto z_of(role kind) -> int
{
    switch (kind)
    {
    case role::background:
        return 0;
    case role::application:
        return 100;
    case role::system_ui:
        return 200;
    case role::overlay:
        return 300;
    case role::notification:
        return 350;
    case role::keyboard:
        return 400;
    case role::emergency:
        return 1000;
    }
    return 100;
}

// A registry of layers keyed by id.
using registry = std::unordered_map<id, state>;

inline auto add(registry& r, const state& s) -> bool
{
    return r.emplace(s.value, s).second;
}

inline auto remove(registry& r, id lid) -> bool
{
    return r.erase(lid) > 0;
}

inline auto get(registry& r, id lid) -> state*
{
    auto it = r.find(lid);
    return it == r.end() ? nullptr : &it->second;
}

inline auto has(const registry& r, id lid) -> bool
{
    return r.contains(lid);
}

inline auto set_visible(state& s, bool visible) -> void
{
    s.visible = visible;
}

inline auto set_opacity(state& s, float opacity) -> void
{
    s.opacity = opacity < 0.0F ? 0.0F : (opacity > 1.0F ? 1.0F : opacity);
}

inline auto set_bounds(state& s, geometry bounds) -> void
{
    s.bounds = bounds;
}

inline auto set_role(state& s, role kind) -> void
{
    s.kind = kind;
}

inline auto set_input(state& s, input_mode mode) -> void
{
    s.input = mode;
}

// A layer accepts input only when visible and not in passthrough mode.
inline auto accepts_input(const state& s) -> bool
{
    return s.visible && s.input != input_mode::passthrough;
}

} // namespace shell::layer
