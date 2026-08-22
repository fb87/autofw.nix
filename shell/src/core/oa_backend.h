// oa_backend.h -- Display backend dispatch table.
//
// Header-only. This is the seam that separates the common embedder logic from
// a concrete display backend (Wayland or DRM/KMS). A backend is described by a
// `backend_api` (a vtable of plain function pointers) plus an opaque
// `void* state`. The engine drives a backend purely through these pointers, so
// the core never depends on a specific platform.
//
// No classes: the dispatch is a plain struct of function pointers.
//
// Copyright 2026 OpenAuto Framework.
#pragma once

#include <functional>

#include "oa_types.h"

namespace oa::backend {

// Function pointer typedefs.
using vsync_fn = std::function<void(const frame_tick&)>;
using input_fn = std::function<void(const input_event&)>;
using resize_fn = std::function<void(uint32_t, uint32_t, double)>;
using keymap_fn = std::function<void(int, uint32_t)>;

struct backend_api {
  // Perform one-time initialization (display connection, window/plane
  // creation, EGL/GL context setup). Returns false on failure.
  bool (*initialize)(void* state) = nullptr;

  // The render-loop entry point. The backend owns the event loop and invokes
  // `on_vsync` each frame tick, `on_input` for input, `on_resize` on size
  // change, and `on_keymap` when a Wayland keymap fd arrives. Blocks until
  // `request_exit` is called on the platform thread.
  void (*run_loop)(void* state, vsync_fn on_vsync, input_fn on_input,
                   resize_fn on_resize, keymap_fn on_keymap) = nullptr;

  // Ask the backend to stop at the next opportunity.
  void (*request_exit)(void* state) = nullptr;

  // GL surface / EGL handles.
  void* (*egl_surface)(void* state) = nullptr;
  void* (*egl_display)(void* state) = nullptr;
  void* (*egl_config)(void* state) = nullptr;

  uint32_t (*width)(const void* state) = nullptr;
  uint32_t (*height)(const void* state) = nullptr;
  double (*dpi)(const void* state) = nullptr;

  // Present the current back buffer and wait for vblank.
  bool (*flush_framebuffer)(void* state) = nullptr;
  bool (*wait_for_vblank)(void* state, frame_tick* out) = nullptr;

  // Release the backend's state.
  void (*destroy)(void* state) = nullptr;
};

// A concrete backend instance: dispatch table + opaque state.
struct backend {
  const backend_api* api = nullptr;
  void* state = nullptr;
};

inline bool backend_initialize(backend* b) {
  return b && b->api && b->api->initialize && b->api->initialize(b->state);
}
inline void backend_run_loop(backend* b, vsync_fn on_vsync, input_fn on_input,
                             resize_fn on_resize, keymap_fn on_keymap) {
  if (b && b->api && b->api->run_loop)
    b->api->run_loop(b->state, std::move(on_vsync), std::move(on_input),
                     std::move(on_resize), std::move(on_keymap));
}
inline void backend_request_exit(backend* b) {
  if (b && b->api && b->api->request_exit) b->api->request_exit(b->state);
}
inline void* backend_egl_surface(backend* b) {
  return b && b->api && b->api->egl_surface ? b->api->egl_surface(b->state)
                                           : nullptr;
}
inline void* backend_egl_display(backend* b) {
  return b && b->api && b->api->egl_display ? b->api->egl_display(b->state)
                                           : nullptr;
}
inline void* backend_egl_config(backend* b) {
  return b && b->api && b->api->egl_config ? b->api->egl_config(b->state)
                                          : nullptr;
}
inline uint32_t backend_width(const backend* b) {
  return b && b->api && b->api->width ? b->api->width(b->state) : 0u;
}
inline uint32_t backend_height(const backend* b) {
  return b && b->api && b->api->height ? b->api->height(b->state) : 0u;
}
inline double backend_dpi(const backend* b) {
  return b && b->api && b->api->dpi ? b->api->dpi(b->state) : 1.0;
}
inline bool backend_flush(backend* b) {
  return b && b->api && b->api->flush_framebuffer
             ? b->api->flush_framebuffer(b->state)
             : false;
}
inline bool backend_wait_vblank(backend* b, frame_tick* out) {
  return b && b->api && b->api->wait_for_vblank
             ? b->api->wait_for_vblank(b->state, out)
             : true;
}
inline void backend_destroy(backend* b) {
  if (b) {
    if (b->api && b->api->destroy) b->api->destroy(b->state);
    delete b;
  }
}

}  // namespace oa::backend
