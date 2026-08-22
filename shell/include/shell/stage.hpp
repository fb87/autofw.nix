#pragma once

#include <cstdint>

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
    float progress{};
};

struct state
{
    id value{};
    extent size{};
    presentation view{};
    int priority{};
};

} // namespace shell::stage
