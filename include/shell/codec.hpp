#pragma once

#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

// Flutter StandardMessageCodec / StandardMethodCodec.
//
// This implements the actual wire format emitted by Dart's StandardMessageCodec
// (see packages/flutter/lib/src/services/message_codecs.dart):
//   * Message values use a leading type byte.
//   * Numbers are host (little) endian. int32 -> 4-byte two's complement;
//     int64 -> 8-byte two's complement; double -> IEEE 754 binary64.
//   * Strings and byte lists prefix their length with an "expanding" size:
//         value < 254        -> one byte (the value)
//         value <= 0xffff    -> byte 0xfe then a uint16 (LE)
//         otherwise          -> byte 0xff then a uint32 (LE)
//   * Method calls are encoded as [String method][value args] with NO version
//     byte. Success envelopes are [0x00][value]; errors are [0x01][code][msg]...

namespace shell::codec
{

// Message type codes (must match StandardMessageCodec).
enum : std::uint8_t
{
    kNull        = 0,
    kBoolTrue    = 1,
    kBoolFalse   = 2,
    kInt32       = 3,
    kInt64       = 4,
    kLargeInt    = 5,
    kDouble      = 6,
    kString      = 7,
    kByteArray   = 8,
    kInt32Array  = 9,
    kInt64Array  = 10,
    kFloat64Arr  = 11,
    kList        = 12,
    kMap         = 13,
    kFloat32Arr  = 14,
};

inline auto read_u8(std::span<const std::uint8_t> data, std::size_t& at,
                    std::uint8_t& out) -> bool
{
    if (at >= data.size())
        return false;
    out = data[at++];
    return true;
}

inline auto read_u16(std::span<const std::uint8_t> data, std::size_t& at,
                     std::uint16_t& out) -> bool
{
    if (at + 2 > data.size())
        return false;
    out = std::uint16_t(data[at]) | std::uint16_t(std::uint16_t(data[at + 1]) << 8);
    at += 2;
    return true;
}

inline auto read_u32(std::span<const std::uint8_t> data, std::size_t& at,
                     std::uint32_t& out) -> bool
{
    if (at + 4 > data.size())
        return false;
    out = std::uint32_t(data[at]) | (std::uint32_t(data[at + 1]) << 8) |
          (std::uint32_t(data[at + 2]) << 16) | (std::uint32_t(data[at + 3]) << 24);
    at += 4;
    return true;
}

// Read the expanding size prefix (see header comment).
inline auto read_size(std::span<const std::uint8_t> data, std::size_t& at,
                      std::uint32_t& out) -> bool
{
    std::uint8_t first = 0;
    if (!read_u8(data, at, first))
        return false;
    if (first < 254)
    {
        out = first;
        return true;
    }
    if (first == 254)
    {
        std::uint16_t v = 0;
        if (!read_u16(data, at, v))
            return false;
        out = v;
        return true;
    }
    return read_u32(data, at, out);
}

// Read a String value (type byte 0x07), advancing |at|.
inline auto read_string(std::span<const std::uint8_t> data, std::size_t& at,
                        std::string& out) -> bool
{
    std::uint8_t type = 0;
    if (!read_u8(data, at, type) || type != kString)
        return false;
    std::uint32_t len = 0;
    if (!read_size(data, at, len))
        return false;
    if (at + len > data.size())
        return false;
    out.assign(reinterpret_cast<const char*>(data.data() + at), len);
    at += len;
    return true;
}

// Read a Uint8List value (type byte 0x08).
inline auto read_byte_list(std::span<const std::uint8_t> data, std::size_t& at,
                           std::span<const std::uint8_t>& out) -> bool
{
    std::uint8_t type = 0;
    if (!read_u8(data, at, type) || type != kByteArray)
        return false;
    std::uint32_t len = 0;
    if (!read_size(data, at, len))
        return false;
    if (at + len > data.size())
        return false;
    out = data.subspan(at, len);
    at += len;
    return true;
}

// Read an integer value (int32 / int64, both big-endian-independent: int32 and
// int64 are stored in host endianness here, both little-endian on our targets).
inline auto read_int(std::span<const std::uint8_t> data, std::size_t& at,
                     std::int64_t& out) -> bool
{
    std::uint8_t type = 0;
    if (!read_u8(data, at, type))
        return false;
    std::size_t bytes = 0;
    switch (type)
    {
    case kInt32:
        bytes = 4;
        break;
    case kInt64:
        bytes = 8;
        break;
    default:
        return false;
    }
    if (at + bytes > data.size())
        return false;
    std::uint64_t u = 0;
    for (std::size_t i = 0; i < bytes; ++i)
        u |= std::uint64_t(data[at + i]) << (8 * i);
    // Sign-extend a 32-bit value to 64 bits.
    if (bytes == 4 && (u & 0x80000000ull))
        u |= 0xffffffff00000000ull;
    out = static_cast<std::int64_t>(u);
    at += bytes;
    return true;
}

// Decode the method name from a StandardMethodCodec method call. The payload is
// exactly [String method][value args]: there is no version byte.
inline auto decode_method_call(std::span<const std::uint8_t> data, std::string& method)
    -> bool
{
    std::size_t at = 0;
    return read_string(data, at, method);
}

// Decode the method name plus the raw encoded argument value (a sub-span of
// |data| after the method string; empty when the call carries no argument).
inline auto decode_method_call_args(std::span<const std::uint8_t> data,
                                    std::string& method,
                                    std::span<const std::uint8_t>& args) -> bool
{
    std::size_t at = 0;
    if (!read_string(data, at, method))
        return false;
    args = data.last(data.size() - at);
    return true;
}

// --- convenience decoders for a single argument value ----------------------
inline auto decode_int64(std::span<const std::uint8_t> args, std::int64_t& out)
    -> bool
{
    std::size_t at = 0;
    return read_int(args, at, out);
}

inline auto decode_string(std::span<const std::uint8_t> args, std::string& out)
    -> bool
{
    std::size_t at = 0;
    return read_string(args, at, out);
}

inline auto decode_double(std::span<const std::uint8_t> args, double& out) -> bool
{
    if (args.empty() || args[0] != kDouble || args.size() < 9)
        return false;
    std::uint64_t u = 0;
    for (std::size_t i = 0; i < 8; ++i)
        u |= std::uint64_t(args[1 + i]) << (8 * i);
    std::memcpy(&out, &u, sizeof(out));
    return true;
}

inline auto decode_byte_array(std::span<const std::uint8_t> args,
                              std::span<const std::uint8_t>& out) -> bool
{
    std::size_t at = 0;
    return read_byte_list(args, at, out);
}

inline auto value_type(std::span<const std::uint8_t> args) -> std::uint8_t
{
    return args.empty() ? kNull : args[0];
}

// Return the encoded byte length of a single top-level codec value, or 0 if it
// cannot be measured (nested list/map). Used to walk lists.
inline auto value_item_length(std::span<const std::uint8_t> v) -> std::size_t
{
    if (v.empty())
        return 0;
    switch (v[0])
    {
    case kNull:
    case kBoolTrue:
    case kBoolFalse:
        return 1;
    case kInt32:
        return 1 + 4;
    case kInt64:
    case kDouble:
        return 1 + 8;
    case kString:
    case kByteArray:
    {
        std::uint32_t len = 0;
        std::size_t c = 1;
        if (!read_size(v, c, len))
            return 0;
        return 1 + (c - 1) + len; // type byte + size bytes + payload
    }
    default:
        return 0; // nested list/map / float arrays not measurable by length
    }
}

// Enumerate the items of a StandardMessageCodec List (kList), calling |visit|
// with each item's raw encoded span.
template <typename Fn>
inline auto foreach_list_item(std::span<const std::uint8_t> args, Fn visit) -> bool
{
    if (args.empty() || args[0] != kList)
        return false;
    std::uint32_t count = 0;
    std::size_t at = 1;
    if (!read_size(args, at, count))
        return false;
    for (std::uint32_t i = 0; i < count; ++i)
    {
        if (at >= args.size())
            return true;
        std::size_t item_len = value_item_length(args.subspan(at));
        if (item_len == 0 || at + item_len > args.size())
            return true; // unable to measure; stop gracefully
        visit(args.subspan(at, item_len));
        at += item_len;
    }
    return true;
}

// --- encoders --------------------------------------------------------------
inline auto put_u8(std::vector<std::uint8_t>& out, std::uint8_t v) -> void
{
    out.push_back(v);
}
inline auto put_u16(std::vector<std::uint8_t>& out, std::uint16_t v) -> void
{
    out.push_back(std::uint8_t(v & 0xff));
    out.push_back(std::uint8_t((v >> 8) & 0xff));
}
inline auto put_u32(std::vector<std::uint8_t>& out, std::uint32_t v) -> void
{
    out.push_back(std::uint8_t(v & 0xff));
    out.push_back(std::uint8_t((v >> 8) & 0xff));
    out.push_back(std::uint8_t((v >> 16) & 0xff));
    out.push_back(std::uint8_t((v >> 24) & 0xff));
}

// Write the expanding size prefix.
inline auto put_size(std::vector<std::uint8_t>& out, std::uint32_t v) -> void
{
    if (v < 254)
    {
        put_u8(out, std::uint8_t(v));
    }
    else if (v <= 0xffff)
    {
        put_u8(out, 254);
        put_u16(out, std::uint16_t(v));
    }
    else
    {
        put_u8(out, 255);
        put_u32(out, v);
    }
}

inline auto encode_string(const std::string& s) -> std::vector<std::uint8_t>
{
    std::vector<std::uint8_t> out;
    put_u8(out, kString);
    put_size(out, static_cast<std::uint32_t>(s.size()));
    out.insert(out.end(), s.begin(), s.end());
    return out;
}

inline auto encode_int64(std::int64_t v) -> std::vector<std::uint8_t>
{
    // int64 type byte + 8-byte little-endian two's complement.
    std::vector<std::uint8_t> out;
    put_u8(out, kInt64);
    std::uint64_t u = static_cast<std::uint64_t>(v);
    for (int i = 0; i < 8; ++i)
        out.push_back(std::uint8_t((u >> (8 * i)) & 0xff));
    return out;
}

inline auto encode_bool(bool v) -> std::vector<std::uint8_t>
{
    return {std::uint8_t(v ? kBoolTrue : kBoolFalse)};
}

inline auto encode_byte_array(std::span<const std::uint8_t> data)
    -> std::vector<std::uint8_t>
{
    std::vector<std::uint8_t> out;
    put_u8(out, kByteArray);
    put_size(out, static_cast<std::uint32_t>(data.size()));
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

inline auto encode_int32_list(std::span<const std::int64_t> items)
    -> std::vector<std::uint8_t>
{
    std::vector<std::uint8_t> out;
    put_u8(out, kInt32Array);
    put_size(out, static_cast<std::uint32_t>(items.size()));
    out.reserve(out.size() + items.size() * 4);
    for (std::int64_t v : items)
    {
        std::uint32_t u = static_cast<std::uint32_t>(static_cast<std::int32_t>(v));
        for (int i = 0; i < 4; ++i)
            out.push_back(std::uint8_t((u >> (8 * i)) & 0xff));
    }
    return out;
}

inline auto encode_string_map(const std::string& key, const std::string& value)
    -> std::vector<std::uint8_t>
{
    std::vector<std::uint8_t> out;
    put_u8(out, kMap);
    put_size(out, 1);
    auto k = encode_string(key);
    auto v = encode_string(value);
    out.insert(out.end(), k.begin(), k.end());
    out.insert(out.end(), v.begin(), v.end());
    return out;
}

// Wrap an already-encoded value in a success envelope ([0x00][value]).
inline auto success_envelope(std::span<const std::uint8_t> value)
    -> std::vector<std::uint8_t>
{
    std::vector<std::uint8_t> out;
    out.reserve(value.size() + 1);
    out.push_back(0x00);
    out.insert(out.end(), value.begin(), value.end());
    return out;
}

inline auto success_envelope(const std::vector<std::uint8_t>& value)
    -> std::vector<std::uint8_t>
{
    return success_envelope(std::span<const std::uint8_t>(value));
}

inline auto encode_success(bool v) -> std::vector<std::uint8_t>
{
    return success_envelope(encode_bool(v));
}

} // namespace shell::codec
