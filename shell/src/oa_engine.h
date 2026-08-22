// oa_engine.h -- Thin wrapper around the Flutter embedder engine.
//
// This component bridges a `oa::Backend` to the Flutter C embedder API using
// Flutter's OpenGL renderer configuration. All callbacks set on
// `FlutterOpenGLRendererConfig` are wired to the active platform backend so
// that Flutter frame work happens on the same GL context that the backend
// manages, and presents are synchronized to the platform's vblank.
//
// Copyright 2026 OpenAuto Framework.
#pragma once

#include <EGL/egl.h>
#include "flutter_embedder.h"
#include "oa_backend.h"
#include "oa_keyboard.h"
#include "oa_task_runner.h"
#include "oa_texture.h"

#include <memory>
#include <mutex>
#include <queue>
#include <string>

namespace oa {

class Backend;

// RAII handle to a live Flutter engine.
class Engine {
 public:
  ~Engine();

  static std::unique_ptr<Engine> Create(Backend* backend,
                                        const std::string& assets_path,
                                        const std::string& icu_path);

  // Called by the backend render loop for every completed frame tick; fulfills
  // any pending vsync request with the given timestamps.
  void OnVsync(const FrameTick& tick);

  // Forward a platform input event (pointer motion/button or keyboard) to the
  // Flutter framework on the platform thread.
  void SendInputEvent(const InputEvent& event);

  // Supply the Wayland keyboard keymap (fd from wl_keyboard.keymap) so the
  // engine can translate keycodes to Flutter logical keys / characters.
  void SetKeyboardKeymap(int fd, uint32_t size);

  // Notify the engine that the display/window size (or scale) changed so it
  // re-lays-out the view. Called from the backend event loop on the platform
  // thread.
  void OnResize(uint32_t width, uint32_t height, double pixel_ratio);

  // --- External texture support --------------------------------------------
  // Register/unregister a texture with Flutter. The framework references it
  // from Dart via a `Texture(textureId: ...)` widget.
  bool RegisterExternalTexture(int64_t texture_id);
  bool UnregisterExternalTexture(int64_t texture_id);
  bool IsExternalTextureRegistered(int64_t texture_id);

  // Push a new RGBA8 frame into an external texture and mark it available so
  // the engine re-composites on the next vsync. May be called from any thread;
  // performs the GL upload on the resource context (shared with the render
  // context). |texture_id| must already be registered.
  bool PushExternalTextureFrame(int64_t texture_id, uint32_t width,
                                uint32_t height, const uint8_t* rgba);

  // internal: serve the engine's gl_external_texture_frame_callback.
  bool GetExternalTexture(int64_t texture_id, size_t /*w*/, size_t /*h*/,
                          FlutterOpenGLTexture* out);

  // --- accessors used by the C callback trampolines -------------------------
  EGLContext gl_context() const { return gl_context_; }
  EGLContext resource_context() const { return resource_context_; }
  Backend* backend() const { return backend_; }
  void HandlePlatformMessage(const FlutterPlatformMessage* message);
  void HandlePathProviderMessage(const FlutterPlatformMessage* message);

  // Store a baton from a vsync request so the next OnVsync can return it.
  void QueueVsyncBaton(intptr_t baton) {
    std::lock_guard<std::mutex> lock(vsync_mutex_);
    vsync_batons_.push(baton);
  }
 private:
  Engine() = default;
  bool Init(Backend* backend, const std::string& assets,
            const std::string& icu);
  void SendMetrics();

  FlutterEngine engine_ = nullptr;
  Backend* backend_ = nullptr;
  EGLContext gl_context_ = EGL_NO_CONTEXT;
  // A second context sharing GL resources with |gl_context_|, used for
  // asynchronous texture uploads on a background thread. Each Flutter thread
  // must bind *its own* context to avoid EGL_BAD_ATTRIBUTE conflicts.
  EGLContext resource_context_ = EGL_NO_CONTEXT;
  // A third context in the same sharegroup used by external-texture producers
  // (e.g. video sinks) so they never fight with Flutter's own resource thread
  // over |resource_context_|.
  EGLContext texture_upload_context_ = EGL_NO_CONTEXT;

  // A dedicated platform task runner so Flutter can deliver platform-message
  // callbacks while our platform thread is busy in the display event loop.
  std::unique_ptr<TaskRunner> platform_task_runner_;

  std::mutex vsync_mutex_;
  std::queue<intptr_t> vsync_batons_;
  uint32_t last_buttons_ = 0;  // cached pointer button mask for phase synthesis

  TextureRegistry textures_;  // registered external textures
  Keyboard keyboard_;          // keymap translation for key events
};

}  // namespace oa
