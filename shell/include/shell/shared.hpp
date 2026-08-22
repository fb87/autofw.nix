#pragma once

#include <string>
#include <unordered_map>

namespace shell::shared
{

enum class scope
{
    system,
    stage,
    plugin,
};

// Small runtime values shared across stages (current media source, navigation
// destination, vehicle profile, locale, theme, active source). Not a database.
struct state
{
    std::unordered_map<std::string, std::string> values;

    // Values may be scoped; keys are typically "scope/key".
    scope key_scope{scope::system};
};

inline auto set(state& s, const std::string& key, const std::string& value) -> void
{
    s.values[key] = value;
}

inline auto get(const state& s, const std::string& key) -> const std::string*
{
    auto it = s.values.find(key);
    return it == s.values.end() ? nullptr : &it->second;
}

inline auto erase(state& s, const std::string& key) -> bool
{
    return s.values.erase(key) > 0;
}

inline auto clear(state& s) -> void
{
    s.values.clear();
}

} // namespace shell::shared
