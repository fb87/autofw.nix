// oa_codec.h -- Minimal Flutter StandardMessageCodec / StandardMethodCodec
// support.
//
// The Dart `path_provider` plugin communicates over a MethodChannel using the
// binary StandardMethodCodec (not JSON). This small helper decodes the method
// name from an incoming method call and encodes a simple `{String: String}`
// map response holding a path.
//
// Copyright 2026 OpenAuto Framework.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace oa {

// Read a size-prefixed (big-endian uint32) byte slice.
bool ReadUint32BE(const uint8_t* data, size_t size, size_t& at,
                  uint32_t& out);

// Read a StandardMessageCodec String (type byte 0x10), advancing |at|.
// Returns true on success.
bool ReadStandardString(const uint8_t* data, size_t size, size_t& at,
                        std::string& out);

// Decode the method name from a StandardMethodCodec encoded method call.
// Returns the method name, or empty on failure.
std::string DecodeStandardMethodCall(const uint8_t* data, size_t size);

// Encode a StandardMessageCodec map with a single string value:
// e.g. { "path": "/tmp/..." }. The result is a bare message-payload byte
// sequence suitable for FlutterEngineSendPlatformMessageResponse.
std::vector<uint8_t> EncodeStandardStringMap(const std::string& key,
                                             const std::string& value);

}  // namespace oa
