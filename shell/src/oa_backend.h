// oa_backend.h -- Abstract display backend interface for the OpenAuto embedder.
//
// The embedder shell is renderer-agnostic at the Flutter level. It relies on
// an EGL/OpenGLES2 pipeline which is exposed through this interface so that
// both the Wayland and the DRM/KMS backends provide the same guarantees to the
// Flutter engine wrapper.
//
// Copyright 2026 OpenAuto Framework.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace oa {

// One (or zero) vsync/frame-tick requests outstanding right now. The backend
// fills in the target frame time so the engine can schedule its frame work.
struct FrameTick {
  // Microseconds (monotonic) when the frame is presented to the display.
  // The engine uses this as `frame_target_time_nanos`.
  uint64_t target_ns = 0;
  // Microseconds (monotonic) when the frame actually starts. This is normally
  // the target minus one refresh interval.
  uint64_t present_ns = 0;
};

// A single flattened input event delivered to the render loop.
enum class InputDeviceKind { Mouse, Touch, Stylus, Trackpad, Keyboard };

struct InputEvent {
  InputDeviceKind device{InputDeviceKind::Mouse};
  // Raw X11/wayland-style button mask (bits for primary/secondary/middle).
  uint32_t buttons = 0;
  // Touch/pen slot index, -1 for mouse/keyboard.
  int32_t slot = -1;
  double x = 0.0;  // physical pixels, window-relative
  double y = 0.0;
  double scroll_x = 0.0;  // physical pixels per notch
  double scroll_y = 0.0;
  // True marks a key (de)press; only used when device == Keyboard.
  bool key_down = false;
  bool key_repeat = false;
  uint32_t keycode = 0;      // Linux evdev keycode
  uint32_t keysym = 0;       // X11 keysym if remapped, else 0
  // Pointer lifecycle: the cursor entered (kAdd) or left (kRemove) the view.
  bool add = false;
  bool remove = false;
};

// --- Backend -----------------------------------------------------------------
//
// Minimal surface needed to drive Flutter's OpenGL renderer config.
class Backend {
 public:
  virtual ~Backend() = default;

  // Perform all one-time initialization (display connection, window/plane
  // creation, EGL/GL context setup). Returns false on failure.
  virtual bool Initialize() = 0;

  // The render thread entry point. The backend owns the event loop and invokes
  // on_vsync() for every frame tick, on_input() for input events,
  // on_resize(w,h,scale) when the display size/scale changes, and
  // on_keymap(fd,size) when a Wayland keyboard keymap is supplied. This call
  // blocks until Shutdown().
  virtual void RunLoop(
      std::function<void(const FrameTick&)> on_vsync,
      std::function<void(const InputEvent&)> on_input,
      std::function<void(uint32_t, uint32_t, double)> on_resize = {},
      std::function<void(int, uint32_t)> on_keymap = {}) = 0;

  // Ask the backend (and therefore the engine) to stop at the very next
  // opportunity. Called from the platform thread.
  virtual void RequestExit() = 0;

  // GL surface (EGLSurface); set/reset current before each present.
  virtual void* egl_surface() = 0;

  // EGLDisplay/EGLConfig used by the shared EGL helper.
  virtual void* egl_display() = 0;
  virtual void* egl_config() = 0;

  virtual uint32_t width() const = 0;
  virtual uint32_t height() const = 0;
  // Physical pixels per logical pixel (scale).
  virtual double dpi() const = 0;

  // Present the current back buffer to the display.
  virtual bool FlushFramebuffer() = 0;
  // Block until the display is ready for the next frame (vblank wait),
  // returning the tick that the engine should attach to. Called only while
  // the current frame is not presented yet.
  virtual bool WaitForVBlank(FrameTick* tick) = 0;
};

Backend* CreateWaylandBackend();
Backend* CreateKmsBackend();

}  // namespace oa
