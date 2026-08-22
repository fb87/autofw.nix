// oa_codec.h -- Minimal Flutter StandardMessageCodec / StandardMethodCodec.
// Header-only, namespace-based free functions.
//
// The Dart `path_provider` plugin communicates over a MethodChannel using the
// binary StandardMethodCodec (not JSON). This helper decodes the method name
// from an incoming method call and encodes a simple path map response.
//
// Copyright 2026 OpenAuto Framework.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace oa::codec {

// Read a size-prefixed (big-endian uint32) byte slice.
inline bool read_u32be(const uint8_t* data, size_t size, size_t& at,
                       uint32_t& out) {
  if (at + 4 > size) return false;
  out = (uint32_t(data[at]) << 24) | (uint32_t(data[at + 1]) << 16) |
        (uint32_t(data[at + 2]) << 8) | uint32_t(data[at + 3]);
  at += 4;
  return true;
}

// Read a StandardMessageCodec String (type byte 0x10), advancing |at|.
inline bool read_string(const uint8_t* data, size_t size, size_t& at,
                        std::string& out) {
  if (at >= size || data[at] != 0x10) return false;  // 0x10 == String
  at += 1;
  uint32_t len = 0;
  if (!read_u32be(data, size, at, len)) return false;
  if (at + len > size) return false;
  out.assign(reinterpret_cast<const char*>(data + at), len);
  at += len;
  return true;
}

// Decode the method name from a StandardMethodCodec-encoded method call.
// Returns the method name, or empty on failure.
inline std::string decode_method_call(const uint8_t* data, size_t size) {
  if (size == 0 || data[0] != 0x00) return {};
  size_t at = 1;
  if (at >= size || data[at] != 0x10) return {};  // method must be a String
  std::string method;
  if (!read_string(data, size, at, method)) return {};
  return method;
}

// Encode a StandardMessageCodec map with a single string value, e.g.
// { "path": "/tmp/..." }.
inline std::vector<uint8_t> encode_string_map(const std::string& key,
                                              const std::string& value) {
  std::vector<uint8_t> out;
  auto push_u32 = [&out](uint32_t v) {
    out.push_back(uint8_t((v >> 24) & 0xff));
    out.push_back(uint8_t((v >> 16) & 0xff));
    out.push_back(uint8_t((v >> 8) & 0xff));
    out.push_back(uint8_t(v & 0xff));
  };
  auto push_string = [&](const std::string& s) {
    out.push_back(0x10);  // String
    push_u32(uint32_t(s.size()));
    out.insert(out.end(), s.begin(), s.end());
  };

  out.push_back(0x0C);  // Map
  push_u32(1);          // 1 key/value pair
  push_string(key);
  push_string(value);
  return out;
}

}  // namespace oa::codec
