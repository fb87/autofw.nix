// oa_texture.cpp -- implementation of the external texture registry.
//
// Copyright 2026 OpenAuto Framework.
#include "oa_texture.h"

#include <GLES2/gl2.h>

#include <cstdio>

namespace oa {

void TextureRegistry::Register(int64_t id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (textures_.find(id) == textures_.end()) {
    textures_[id] = OaTextureInfo{};
  }
}

void TextureRegistry::Unregister(int64_t id) {
  std::lock_guard<std::mutex> lock(mutex_);
  textures_.erase(id);
}

bool TextureRegistry::Has(int64_t id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return textures_.find(id) != textures_.end();
}

bool TextureRegistry::EnsureTexture(int64_t id, uint32_t width,
                                    uint32_t height) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = textures_.find(id);
  if (it == textures_.end()) {
    it = textures_.emplace(id, OaTextureInfo{}).first;
  }
  OaTextureInfo& t = it->second;
  if (t.name == 0) {
    GLuint name = 0;
    glGenTextures(1, &name);
    if (name == 0) {
      std::fprintf(stderr, "[oa-texture] glGenTextures failed\n");
      return false;
    }
    t.name = name;
    t.target = GL_TEXTURE_2D;
    t.format = 0x8058;  // GL_RGBA8 internal format for Skia's GrBackendTexture
    t.width = width;
    t.height = height;
    t.ready = false;
    // Reserve the storage so a sampling reference is valid even before pixels.
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

bool TextureRegistry::UploadFrame(int64_t id, uint32_t width, uint32_t height,
                                  const uint8_t* rgba) {
  if (!EnsureTexture(id, width, height)) return false;

  OaTextureInfo* t = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = textures_.find(id);
    if (it == textures_.end()) return false;
    t = &it->second;
    GLuint name = t->name;
    glBindTexture(GL_TEXTURE_2D, name);
    if (width != t->width || height != t->height) {
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)width, (GLsizei)height,
                   0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
      t->width = width;
      t->height = height;
    }
    if (rgba) {
      glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)width, (GLsizei)height,
                      GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    }
    t->ready = true;
  }
  (void)t;
  return true;
}

bool TextureRegistry::GetTexture(int64_t id, uint32_t* name, uint32_t* target,
                                 uint32_t* format, uint32_t* width,
                                 uint32_t* height, bool* ready) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = textures_.find(id);
  if (it != textures_.end() && it->second.name != 0) {
    *name = it->second.name;
    *target = it->second.target;
    *format = it->second.format;
    *width = it->second.width ? it->second.width : 1;
    *height = it->second.height ? it->second.height : 1;
    if (ready) *ready = it->second.ready;
    return true;
  }
  // Not yet created: hand back a degenerate 1x1 texture name so the engine has
  // a valid sampleable target while the producer initializes.
  *name = 0;
  *target = GL_TEXTURE_2D;
  *format = 0x8058;  // GL_RGBA8
  *width = 1;
  *height = 1;
  if (ready) *ready = false;
  return false;
}

void TextureRegistry::ReleaseTexture(int64_t id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = textures_.find(id);
  if (it != textures_.end() && it->second.name != 0) {
    GLuint name = it->second.name;
    glDeleteTextures(1, &name);
    it->second.name = 0;
    it->second.ready = false;
  }
}

}  // namespace oa
