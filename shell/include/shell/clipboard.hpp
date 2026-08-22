#pragma once

#include <cstddef>
#include <span>
#include <string>

#include <shell/result.hpp>

namespace shell::shared::clipboard
{

enum class format
{
    text,
    uri,
    binary,
};

struct item
{
    format type{format::text};
    std::span<const std::byte> data;
};

struct state
{
};

inline auto clear(state&) -> result<void>
{
    return {};
}

} // namespace shell::shared::clipboard
