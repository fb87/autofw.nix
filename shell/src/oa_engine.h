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

  // --- accessors used by the C callback trampolines -------------------------
  EGLContext gl_context() const { return gl_context_; }
  Backend* backend() const { return backend_; }
  void HandlePlatformMessage(const FlutterPlatformMessage* message);

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

  std::mutex vsync_mutex_;
  std::queue<intptr_t> vsync_batons_;
  uint32_t last_buttons_ = 0;  // cached pointer button mask for phase synthesis
};

}  // namespace oa
