// oa_texture.h -- External texture registry. Header-only.
//
// Namespace-based functional module (no classes). Flutter's OpenGL renderer
// supports "external textures": native code (GStreamer, Android Auto video
// sink) owns a GL texture and asks Flutter to composite it into the scene (a
// `Texture` widget). This registry tracks those textures and serves the
// engine's frame callback.
//
// Copyright 2026 OpenAuto Framework.
#pragma once

#include <GLES2/gl2.h>

#include <cstdint>
#include <mutex>
#include <unordered_map>

#include "oa_types.h"

namespace oa::texture {

struct entry {
  uint32_t gl_name = 0;
  uint32_t gl_target = GL_TEXTURE_2D;
  uint32_t gl_format = 0x8058;  // GL_RGBA8
  uint32_t width = 0;
  uint32_t height = 0;
  bool ready = false;
};

// Registry of external textures, guarded by a mutex.
struct registry {
  std::mutex mutex;
  std::unordered_map<int64_t, entry> map;
};

inline void register_id(registry* r, int64_t id) {
  std::lock_guard<std::mutex> lock(r->mutex);
  r->map.emplace(id, entry{});
}
inline void unregister_id(registry* r, int64_t id) {
  std::lock_guard<std::mutex> lock(r->mutex);
  r->map.erase(id);
}
inline bool has(registry* r, int64_t id) {
  std::lock_guard<std::mutex> lock(r->mutex);
  return r->map.find(id) != r->map.end();
}

// Create (or fetch) the GL texture for |id|. Must be called with a GL context
// current.
inline bool ensure_texture(registry* r, int64_t id, uint32_t width,
                           uint32_t height) {
  std::lock_guard<std::mutex> lock(r->mutex);
  auto& e = r->map[id];
  if (e.gl_name == 0) {
    GLuint name = 0;
    glGenTextures(1, &name);
    if (name == 0) return false;
    e.gl_name = name;
    e.gl_target = GL_TEXTURE_2D;
    e.gl_format = 0x8058;
    e.width = width;
    e.height = height;
    e.ready = false;
    glBindTexture(GL_TEXTURE_2D, name);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)width, (GLsizei)height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  }
  return true;
}

// Upload raw RGBA8 pixels into the texture for |id|. GL context must be
// current. May be called from any thread.
inline bool upload_frame(registry* r, int64_t id, uint32_t width,
                         uint32_t height, const uint8_t* rgba) {
  if (!ensure_texture(r, id, width, height)) return false;
  std::lock_guard<std::mutex> lock(r->mutex);
  auto it = r->map.find(id);
  if (it == r->map.end()) return false;
  auto& e = it->second;
  glBindTexture(GL_TEXTURE_2D, e.gl_name);
  if (width != e.width || height != e.height) {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)width, (GLsizei)height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    e.width = width;
    e.height = height;
  }
  if (rgba) {
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)width, (GLsizei)height,
                    GL_RGBA, GL_UNSIGNED_BYTE, rgba);
  }
  e.ready = true;
  return true;
}

// Fetch the GL properties for |id| so the engine can sample it. Returns false
// (with a degenerate 1x1) if not yet created.
inline bool get_texture(registry* r, int64_t id, uint32_t* name,
                        uint32_t* target, uint32_t* format, uint32_t* width,
                        uint32_t* height, bool* ready) {
  std::lock_guard<std::mutex> lock(r->mutex);
  auto it = r->map.find(id);
  if (it != r->map.end() && it->second.gl_name != 0) {
    *name = it->second.gl_name;
    *target = it->second.gl_target;
    *format = it->second.gl_format;
    *width = it->second.width ? it->second.width : 1;
    *height = it->second.height ? it->second.height : 1;
    if (ready) *ready = it->second.ready;
    return true;
  }
  *name = 0;
  *target = GL_TEXTURE_2D;
  *format = 0x8058;
  *width = 1;
  *height = 1;
  if (ready) *ready = false;
  return false;
}

// Release GL resources for |id| (GL context must be current).
inline void release_texture(registry* r, int64_t id) {
  std::lock_guard<std::mutex> lock(r->mutex);
  auto it = r->map.find(id);
  if (it != r->map.end() && it->second.gl_name != 0) {
    GLuint name = it->second.gl_name;
    glDeleteTextures(1, &name);
    it->second.gl_name = 0;
    it->second.ready = false;
  }
}

}  // namespace oa::texture
