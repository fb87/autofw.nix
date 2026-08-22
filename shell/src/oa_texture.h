// oa_texture.h -- External texture registry for the OpenAuto embedder.
//
// Flutter's OpenGL renderer supports "external textures": native code (e.g. a
// GStreamer pipeline or an Android Auto video sink) owns a GL texture and asks
// Flutter to composite it into the scene (via a `Texture` widget). This
// registry tracks those textures and serves the engine's
// `gl_external_texture_frame_callback`.
//
// The producer updates a texture's pixels off-thread and then signals that a
// new frame is available; the engine re-composites on the next frame.
//
// Copyright 2026 OpenAuto Framework.
#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace oa {

// Information about one registered external texture that the engine needs in
// order to sample it while compositing a frame.
struct OaTextureInfo {
  uint32_t name = 0;      // GL texture name
  uint32_t target = 0x0DE1;  // GL_TEXTURE_2D
  uint32_t format = 0x8058;  // GL_RGBA8
  uint32_t width = 0;
  uint32_t height = 0;
  bool ready = false;   // has at least one frame been uploaded
  bool deleted = false; // the GL texture was released
};

// Owns the set of registered external textures and their last-known GL handles.
class TextureRegistry {
 public:
  // Create the GL texture object for |id| if needed. Must be called with a GL
  // context current (the resource/render sharegroup).
  bool EnsureTexture(int64_t id, uint32_t width, uint32_t height);

  // Upload raw RGBA8 pixels into the texture for |id|. GL context must be
  // current on the calling thread.
  bool UploadFrame(int64_t id, uint32_t width, uint32_t height,
                   const uint8_t* rgba);

  // Look up / reserve a texture so the engine can sample it synchronously.
  // Fills |out| with the texture's GL properties. If not ready yet, returns a
  // 1x1 texture so the engine still has a valid sampleable object.
  bool GetTexture(int64_t id, uint32_t* name, uint32_t* target,
                  uint32_t* format, uint32_t* width, uint32_t* height,
                  bool* ready);

  // Release any GL resources for |id| (GL context must be current).
  void ReleaseTexture(int64_t id);

  bool Has(int64_t id) const;
  void Register(int64_t id);
  void Unregister(int64_t id);

 private:
  mutable std::mutex mutex_;
  std::unordered_map<int64_t, OaTextureInfo> textures_;
};

}  // namespace oa
