#pragma once

#include <cstdint>
#include <optional>

#include <shell/shared.hpp>

namespace shell::shared::selection
{

// Transient selected/shared content; future drag/drop or selection ownership.
enum class type
{
    text,
    uri,
    widget,
};

struct state
{
    type kind{type::text};
    std::uint64_t source{};
    std::optional<std::uint64_t> widget_id;
    std::optional<int64_t> flutter_texture_id;
    bool active{};
};

inline auto select(state& s, std::uint64_t source, type kind) -> void
{
    s.source = source;
    s.kind = kind;
    s.active = true;
}

inline auto clear(state& s) -> void
{
    s.active = false;
    s.source = 0;
    s.widget_id.reset();
    s.flutter_texture_id.reset();
}

} // namespace shell::shared::selection
