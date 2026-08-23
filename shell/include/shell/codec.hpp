#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

// Minimal Flutter StandardMessageCodec / StandardMethodCodec.
//
// The Dart `MethodChannel` default codec is binary StandardMethodCodec. This
// helper decodes a method call (method name) and encodes simple responses.

namespace shell::codec
{

// Message types.
enum : std::uint8_t
{
    kBoolTrue = 1,
    kBoolFalse = 2,
    kInt32 = 3,
    kInt64 = 4,
    kDouble = 5,
    kByteArray = 6,
    kIntArray = 7,
    kLongArray = 8,
    kDoubleArray = 9,
    kString = 10,
    kList = 11,
    kMap = 12,
    kNull = 0,
};

inline auto read_u32(std::span<const std::uint8_t> data, std::size_t& at,
                     std::uint32_t& out) -> bool
{
    if (at + 4 > data.size())
        return false;
    out = (std::uint32_t(data[at]) << 24) | (std::uint32_t(data[at + 1]) << 16) |
          (std::uint32_t(data[at + 2]) << 8) | std::uint32_t(data[at + 3]);
    at += 4;
    return true;
}

// Read a StandardMessageCodec String (type byte 0x10), advancing |at|.
inline auto read_string(std::span<const std::uint8_t> data, std::size_t& at,
                        std::string& out) -> bool
{
    if (at >= data.size() || data[at] != kString)
        return false;
    at += 1;
    std::uint32_t len = 0;
    if (!read_u32(data, at, len))
        return false;
    if (at + len > data.size())
        return false;
    out.assign(reinterpret_cast<const char*>(data.data() + at), len);
    at += len;
    return true;
}

// Decode the method name from a StandardMethodCodec-encoded method call.
// The payload begins with the codec version byte (0) then the String method.
inline auto decode_method_call(std::span<const std::uint8_t> data, std::string& method)
    -> bool
{
    if (data.empty() || data[0] != 0x00)
        return false;
    std::size_t at = 1;
    return read_string(data, at, method);
}

// Append a big-endian uint32.
inline auto push_u32(std::vector<std::uint8_t>& out, std::uint32_t v) -> void
{
    out.push_back(std::uint8_t((v >> 24) & 0xff));
    out.push_back(std::uint8_t((v >> 16) & 0xff));
    out.push_back(std::uint8_t((v >> 8) & 0xff));
    out.push_back(std::uint8_t(v & 0xff));
}

inline auto push_string(std::vector<std::uint8_t>& out, const std::string& s) -> void
{
    out.push_back(kString);
    push_u32(out, static_cast<std::uint32_t>(s.size()));
    out.insert(out.end(), s.begin(), s.end());
}

// Encode a bare String as a StandardMessageCodec value.
inline auto encode_string(const std::string& s) -> std::vector<std::uint8_t>
{
    std::vector<std::uint8_t> out;
    push_string(out, s);
    return out;
}

// Encode a { key: value } map with a single String value.
inline auto encode_string_map(const std::string& key, const std::string& value)
    -> std::vector<std::uint8_t>
{
    std::vector<std::uint8_t> out;
    out.push_back(kMap);
    push_u32(out, 1);
    push_string(out, key);
    push_string(out, value);
    return out;
}

} // namespace shell::codec
