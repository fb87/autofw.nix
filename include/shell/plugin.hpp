#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <shell/native.hpp>
#include <shell/plugin_abi.h>
#include <shell/result.hpp>

namespace shell::plugin
{

using id = std::uint64_t;

enum class type
{
    flutter,
    native,
    flutter_engine,
};

struct manifest
{
    std::string id;
    std::string name;
    std::string version;
    type kind{type::flutter};

    // Native rendering capabilities (M9).
    std::vector<native::capability> capabilities;
};

struct entry
{
    id value{};
    manifest m{};
    void* handle{}; // dlopen handle (native) or framework ref
};

// Registry of loaded plugins (M13: feature plugin selection).
struct state
{
    id next{1};
    std::unordered_map<id, entry> by_id;
    std::unordered_map<std::string, id> by_name;
};

inline auto add(state& s, const manifest& m) -> result<id>
{
    if (s.by_name.contains(m.id))
        return unexpected(error{.code = error_code::plugin,
                                .message = "plugin id already registered"});
    id value = s.next++;
    entry e{};
    e.value = value;
    e.m = m;
    s.by_id.emplace(value, e);
    s.by_name.emplace(m.id, value);
    return value;
}

inline auto remove(state& s, id pid) -> bool
{
    auto it = s.by_id.find(pid);
    if (it == s.by_id.end())
        return false;
    s.by_name.erase(it->second.m.id);
    s.by_id.erase(it);
    return true;
}

inline auto get(state& s, id pid) -> entry*
{
    return s.by_id.contains(pid) ? &s.by_id[pid] : nullptr;
}

inline auto find(state& s, const std::string& name) -> entry*
{
    auto it = s.by_name.find(name);
    if (it == s.by_name.end())
        return nullptr;
    return &s.by_id[it->second];
}

// M12: validate a plugin descriptor's ABI version before use.
inline auto validate_abi(const shell_plugin_descriptor* desc) -> result<void>
{
    if (desc == nullptr)
        return unexpected(
            error{.code = error_code::plugin, .message = "null descriptor"});
    if (desc->abi_version != SHELL_PLUGIN_ABI_VERSION)
        return unexpected(
            error{.code = error_code::plugin, .message = "ABI version mismatch"});
    return {};
}

// M9/M13: a plugin is usable with the active renderer if its declared native
// capabilities negotiate (for source-less plugins this always succeeds).
inline auto check_renderer_ok(const manifest& m, const renderer::type rtype)
    -> result<void>
{
    native::capabilities caps;
    caps.supported = m.capabilities;
    return native::negotiate(rtype, caps);
}

} // namespace shell::plugin
