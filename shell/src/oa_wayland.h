// oa_wayland.h -- Wayland display backend for the OpenAuto embedder.
//
// The backend connects to a Wayland compositor, creates a top-level xdg_surface
// window backed by an EGL `wl_egl_window`, and renders into it. Frame pacing
// uses the Wayland frame callback; pointer and keyboard input is forwarded to
// the Flutter engine.
//
// Copyright 2026 OpenAuto Framework.
#pragma once

#include "oa_backend.h"
#include <functional>

struct wl_display;
struct wl_registry;
struct wl_compositor;
struct wl_surface;
struct wl_seat;
struct wl_pointer;
struct wl_keyboard;
struct wl_callback;
struct wl_egl_window;
struct xdg_wm_base;
struct xdg_surface;
struct xdg_toplevel;

namespace oa {

class WaylandBackend final : public Backend {
 public:
  WaylandBackend() = default;
  ~WaylandBackend() override;

  bool Initialize() override;
  void RunLoop(std::function<void(const FrameTick&)> on_vsync,
               std::function<void(const InputEvent&)> on_input,
               std::function<void(uint32_t, uint32_t, double)> on_resize = {},
               std::function<void(int, uint32_t)> on_keymap = {}) override;
  void RequestExit() override;

  void* egl_surface() override { return (void*)egl_surface_; }
  void* egl_display() override { return (void*)egl_display_; }
  void* egl_config() override { return (void*)egl_config_; }
  uint32_t width() const override { return width_; }
  uint32_t height() const override { return height_; }
  double dpi() const override { return dpi_; }

  bool FlushFramebuffer() override;
  bool WaitForVBlank(FrameTick* tick) override;

  // --- Wayland event hooks (called by listener trampolines). --------------
  void OnRegistryGlobal(wl_registry* registry, uint32_t name,
                        const char* interface, uint32_t version);
  void OnSeatCapabilities(wl_seat* seat, uint32_t caps);
  void OnPointerEnter();
  void OnPointerLeave();
  void OnPointerMotion(double x, double y);
  void OnButtonPress(uint32_t button, bool pressed);
  void OnAxis(uint32_t axis, double value);
  void OnKey(uint32_t keycode, bool down);
  void OnConfigure(uint32_t w, uint32_t h);
  void OnXdgConfigureAcked();
  void OnKeymap(uint32_t format, int fd, uint32_t size);
  void OnFrameDone();

  // True once the compositor has sent+acked the first xdg configure, i.e. the
  // surface may carry a buffer.
  bool configured() const { return configured_; }

 private:
  bool CreateSurface();
  bool SetupEgl();
  void CommitSurface();

  wl_display* display_ = nullptr;
  wl_registry* registry_ = nullptr;
  wl_compositor* compositor_ = nullptr;
  wl_surface* surface_ = nullptr;
  wl_seat* seat_ = nullptr;
  wl_pointer* pointer_ = nullptr;
  wl_keyboard* keyboard_ = nullptr;
  wl_callback* frame_ = nullptr;
  wl_egl_window* window_ = nullptr;
  xdg_wm_base* wm_base_ = nullptr;
  xdg_surface* xdg_surface_ = nullptr;
  xdg_toplevel* xdg_toplevel_ = nullptr;

  void* egl_display_ = nullptr;
  void* egl_config_ = nullptr;
  void* egl_surface_ = nullptr;

  uint32_t width_ = 1280;
  uint32_t height_ = 720;
  double dpi_ = 1.0;

  double pointer_x_ = 0.0;
  double pointer_y_ = 0.0;
  uint32_t buttons_ = 0;

  bool running_ = false;
  bool frame_visible_ = false;
  bool configured_ = false;  // xdg configure received and acked
  bool pointer_added_ = false;  // kAdd sent to Flutter, awaiting kRemove

  // Frame counters for lightweight present-rate diagnostics.
  uint64_t frames_presented_ = 0;
  uint64_t last_fps_log_ms_ = 0;
  uint64_t last_fps_log_frames_ = 0;

  std::function<void(const InputEvent&)> on_input_;
  std::function<void(const FrameTick&)> on_vsync_;
  std::function<void(uint32_t, uint32_t, double)> on_resize_;  // size/scale change
  std::function<void(int, uint32_t)> on_keymap_;               // wl keymap fd
};

}  // namespace oa
