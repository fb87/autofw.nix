#pragma once

#include <cstdint>
#include <unordered_map>

#include <shell/layer.hpp>
#include <shell/texture.hpp>

namespace shell::surface
{

using id = std::uint64_t;

struct extent
{
    std::uint32_t width{};
    std::uint32_t height{};
};

// A native visual content surface attached to a layer and backed by a texture.
struct state
{
    id value{};
    layer::id layer{};
    extent size{};
    texture::id texture{};
    bool visible{};

    float x{};
    float y{};
};

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

inline auto attach_texture(state& s, texture::id tid) -> void
{
    s.texture = tid;
}

inline auto detach_texture(state& s) -> void
{
    s.texture = {};
}

inline auto set_visible(state& s, bool visible) -> void
{
    s.visible = visible;
}

inline auto set_size(state& s, extent size) -> void
{
    s.size = size;
}

inline auto set_position(state& s, float x, float y) -> void
{
    s.x = x;
    s.y = y;
}

inline auto move_to(state& s, layer::state* layer, float x, float y) -> void
{
    if (layer)
    {
        s.layer = layer->value;
        s.x = x;
        s.y = y;
    }
}

} // namespace shell::surface
