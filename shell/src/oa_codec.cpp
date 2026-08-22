// oa_codec.cpp -- StandardMessageCodec helpers.
//
// Copyright 2026 OpenAuto Framework.
#include "oa_codec.h"

namespace oa {

bool ReadUint32BE(const uint8_t* data, size_t size, size_t& at, uint32_t& out) {
  if (at + 4 > size) return false;
  out = (uint32_t(data[at]) << 24) | (uint32_t(data[at + 1]) << 16) |
        (uint32_t(data[at + 2]) << 8) | uint32_t(data[at + 3]);
  at += 4;
  return true;
}

bool ReadStandardString(const uint8_t* data, size_t size, size_t& at,
                        std::string& out) {
  if (at >= size || data[at] != 0x10) return false;  // 0x10 == String
  at += 1;
  uint32_t len = 0;
  if (!ReadUint32BE(data, size, at, len)) return false;
  if (at + len > size) return false;
  out.assign(reinterpret_cast<const char*>(data + at), len);
  at += len;
  return true;
}

std::string DecodeStandardMethodCall(const uint8_t* data, size_t size) {
  // The message begins with the codec version byte (0) followed by the
  // Standard MethodCall encoding: String method, then (optional) args.
  if (size == 0 || data[0] != 0x00) return {};
  size_t at = 1;
  if (at >= size || data[at] != 0x10) return {};  // method must be a String
  std::string method;
  if (!ReadStandardString(data, size, at, method)) return {};
  return method;
}

std::vector<uint8_t> EncodeStandardStringMap(const std::string& key,
                                             const std::string& value) {
  // A bare message payload is the value (map) with no version byte.
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

}  // namespace oa
