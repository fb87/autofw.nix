#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <vector>

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
    std::vector<std::byte> data;
};

// System-wide clipboard by default; future scopes may add stage/plugin.
enum class scope
{
    system,
    stage,
    plugin,
};

struct state
{
    item current{};
    bool has{};
    scope owner{scope::system};
};

inline auto set(state& s, const std::vector<std::byte>& data,
                format type = format::text) -> result<void>
{
    s.current.type = type;
    s.current.data = data;
    s.has = true;
    return {};
}

inline auto set_text(state& s, const std::string& text) -> result<void>
{
    s.current.type = format::text;
    s.current.data.clear();
    s.current.data.reserve(text.size());
    for (char c : text)
        s.current.data.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    s.has = true;
    return {};
}

inline auto get(const state& s) -> const item*
{
    return s.has ? &s.current : nullptr;
}

inline auto clear(state& s) -> result<void>
{
    s.has = false;
    s.current = {};
    return {};
}

} // namespace shell::shared::clipboard
