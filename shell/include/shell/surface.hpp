#pragma once

#include <cstdint>

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

struct state
{
    id value{};
    layer::id layer{};
    extent size{};
    texture::id texture{};
    bool visible{};
};

} // namespace shell::surface
