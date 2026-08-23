#pragma once

#include <cstdint>
#include <cstdlib>
#include <functional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include <shell/codec.hpp>
#include <shell/result.hpp>

namespace shell::channel
{

// A named platform-message handler. It receives a decoded StandardMethodCodec
// method call (method name plus the raw encoded argument value) and returns the
// response payload (or an error result for which an empty ack is sent).
//
// |args| is the StandardMessageCodec-encoded single argument (empty when the
// call carries none). Decode it with codec::decode_int64 / decode_string /
// decode_byte_array.
using handler = std::function<
    result<std::vector<std::uint8_t>>(const std::string& method,
                                      std::span<const std::uint8_t> args)>;

// Registry mapping channel names to method handlers.
struct state
{
    std::unordered_map<std::string, handler, std::hash<std::string>> map;

    std::function<const void*()> host; // optional host context (e.g. path root)
    std::string data_root;
};

inline auto register_channel(state& s, const std::string& name, handler fn) -> void
{
    s.map[name] = std::move(fn);
}

// The built-in plugin channel used by get_storage/path_provider.
inline const char* kPathProvider = "plugins.flutter.io/path_provider";

inline auto default_data_root() -> std::string
{
    const char* home = std::getenv("HOME");
    return (home ? std::string(home) : std::string("/tmp")) + "/.shell-data";
}

// Route an inbound platform message to its registered handler. Returns the
// response payload, or empty (meaning "ack with no data") when unhandled.
inline auto dispatch(state& s, const char* channel,
                     std::span<const std::uint8_t> message) -> std::vector<std::uint8_t>
{
    if (channel == nullptr)
        return {};
    auto it = s.map.find(channel);
    if (it == s.map.end())
    {
        // Built-in path_provider: return a documents path.
        if (std::string(channel) == kPathProvider)
        {
            if (s.data_root.empty())
                s.data_root = default_data_root();
            std::string method;
            if (codec::decode_method_call(message, method))
                return codec::encode_string_map("path", s.data_root);
        }
        return {};
    }
    std::string method;
    std::span<const std::uint8_t> args;
    if (!codec::decode_method_call_args(message, method, args))
        return {};
    auto res = it->second(method, args);
    if (!res)
        return {};
    return *res;
}

} // namespace shell::channel
