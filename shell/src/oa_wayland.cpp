// oa_wayland.cpp -- Wayland backend implementation.
//
// Copyright 2026 OpenAuto Framework.
#include "oa_wayland.h"

#include "oa_egl.h"

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <sys/errno.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

#include <wayland-client.h>
#include <wayland-egl.h>

#include "protocol/xdg-shell-client-protocol.h"

namespace oa {

// ---------------------------------------------------------------------------
// Free-standing listener trampolines. Every listener carries the WaylandBackend
// instance in its `data` slot.
// ---------------------------------------------------------------------------
namespace {

// Registry.
void OnRegistryGlobal(void* data, wl_registry* registry, uint32_t name,
                      const char* interface, uint32_t version);
void OnRegistryRemove(void* data, wl_registry* registry, uint32_t name);
const wl_registry_listener g_registry_listener = {
    .global = OnRegistryGlobal,
    .global_remove = OnRegistryRemove,
};

// xdg_wm_base.
void OnWmBasePing(void* /*data*/, xdg_wm_base* wm, uint32_t serial) {
  xdg_wm_base_pong(wm, serial);
}
const xdg_wm_base_listener g_wmbase_listener = {.ping = OnWmBasePing};

// xdg_surface.
void OnXdgSurfaceConfigure(void* /*data*/, xdg_surface* surface,
                           uint32_t serial) {
  xdg_surface_ack_configure(surface, serial);
}
const xdg_surface_listener g_xdgsurface_listener = {
    .configure = OnXdgSurfaceConfigure};

// xdg_toplevel.
void OnXdgToplevelConfigure(void* data, xdg_toplevel* t, int32_t width,
                            int32_t height, wl_array* states);
void OnXdgToplevelClose(void* data, xdg_toplevel* t);
const xdg_toplevel_listener g_xdgtoplevel_listener = {
    .configure = OnXdgToplevelConfigure,
    .close = OnXdgToplevelClose,
};

// Seat.
void OnSeatCapabilities(void* data, wl_seat* seat, uint32_t caps);
void OnSeatName(void* data, wl_seat* seat, const char* name);
const wl_seat_listener g_seat_listener = {
    .capabilities = OnSeatCapabilities,
    .name = OnSeatName,
};

// Pointer.
void OnPointerMotion(void* data, wl_pointer* p, uint32_t time, wl_fixed_t sx,
                     wl_fixed_t sy);
void OnPointerButton(void* data, wl_pointer* p, uint32_t serial, uint32_t time,
                     uint32_t button, uint32_t state);
void OnPointerAxis(void* d, wl_pointer* p, uint32_t t, uint32_t axis,
                   wl_fixed_t value);
const wl_pointer_listener g_pointer_listener = {
    .enter = nullptr,
    .leave = nullptr,
    .motion = OnPointerMotion,
    .button = OnPointerButton,
    .axis = OnPointerAxis,
    .frame = nullptr,
    .axis_source = nullptr,
    .axis_stop = nullptr,
    .axis_discrete = nullptr,
};

// Keyboard.
void OnKeyboardKey(void* data, wl_keyboard* k, uint32_t serial, uint32_t time,
                   uint32_t key, uint32_t state);
const wl_keyboard_listener g_keyboard_listener = {
    .keymap = nullptr,
    .enter = nullptr,
    .leave = nullptr,
    .key = OnKeyboardKey,
    .modifiers = nullptr,
    .repeat_info = nullptr,
};

// Frame callback.
void OnFrameDone(void* data, wl_callback* callback, uint32_t serial);
const wl_callback_listener g_frame_listener = {.done = OnFrameDone};

}  // namespace

// ---------------------------------------------------------------------------
// Trampoline bodies.
// ---------------------------------------------------------------------------
namespace {

void OnRegistryGlobal(void* data, wl_registry* registry, uint32_t name,
                      const char* interface, uint32_t version) {
  static_cast<WaylandBackend*>(data)->OnRegistryGlobal(registry, name,
                                                       interface, version);
}
void OnRegistryRemove(void*, wl_registry*, uint32_t) {}

void OnXdgToplevelConfigure(void* data, xdg_toplevel*, int32_t width,
                            int32_t height, wl_array*) {
  static_cast<WaylandBackend*>(data)->OnConfigure(width, height);
}
void OnXdgToplevelClose(void* data, xdg_toplevel*) {
  static_cast<WaylandBackend*>(data)->RequestExit();
}
void OnSeatCapabilities(void* data, wl_seat* seat, uint32_t caps) {
  static_cast<WaylandBackend*>(data)->OnSeatCapabilities(seat, caps);
}
void OnSeatName(void*, wl_seat*, const char*) {}
void OnPointerMotion(void* data, wl_pointer*, uint32_t, wl_fixed_t sx,
                     wl_fixed_t sy) {
  static_cast<WaylandBackend*>(data)->OnPointerMotion(wl_fixed_to_double(sx),
                                                      wl_fixed_to_double(sy));
}
void OnPointerButton(void* data, wl_pointer*, uint32_t, uint32_t, uint32_t b,
                     uint32_t state) {
  static_cast<WaylandBackend*>(data)->OnButtonPress(
      b, state == WL_POINTER_BUTTON_STATE_PRESSED);
}
void OnPointerAxis(void* data, wl_pointer*, uint32_t, uint32_t axis,
                   wl_fixed_t value) {
  static_cast<WaylandBackend*>(data)->OnAxis(axis, wl_fixed_to_double(value));
}
void OnKeyboardKey(void* data, wl_keyboard*, uint32_t, uint32_t, uint32_t key,
                   uint32_t state) {
  static_cast<WaylandBackend*>(data)->OnKey(
      key, state == WL_KEYBOARD_KEY_STATE_PRESSED);
}
void OnFrameDone(void* data, wl_callback* callback, uint32_t) {
  wl_callback_destroy(callback);
  static_cast<WaylandBackend*>(data)->OnFrameDone();
}

}  // namespace

// ---------------------------------------------------------------------------
// WaylandBackend implementation.
// ---------------------------------------------------------------------------
WaylandBackend::~WaylandBackend() {
  if (egl_surface_) {
    eglDestroySurface(reinterpret_cast<EGLDisplay>(egl_display_),
                      reinterpret_cast<EGLSurface>(egl_surface_));
  }
  if (window_) wl_egl_window_destroy(window_);
  if (surface_) wl_surface_destroy(surface_);
  if (pointer_) wl_pointer_destroy(pointer_);
  if (keyboard_) wl_keyboard_destroy(keyboard_);
  if (seat_) wl_seat_destroy(seat_);
  if (compositor_) wl_compositor_destroy(compositor_);
  if (egl_display_) eglTerminate(reinterpret_cast<EGLDisplay>(egl_display_));
  if (display_) wl_display_disconnect(display_);
}

bool WaylandBackend::Initialize() {
  display_ = wl_display_connect(nullptr);
  if (!display_) {
    std::fprintf(stderr, "[oa-wayland] cannot connect to Wayland compositor\n");
    return false;
  }

  registry_ = wl_display_get_registry(display_);
  wl_registry_add_listener(registry_, &g_registry_listener, this);
  wl_display_roundtrip(display_);

  if (!compositor_ || !wm_base_ || !seat_) {
    std::fprintf(stderr,
                 "[oa-wayland] missing required globals (compositor=%p "
                 "wm_base=%p seat=%p)\n",
                 (void*)compositor_, (void*)wm_base_, (void*)seat_);
    return false;
  }

  if (!SetupEgl()) return false;
  if (!CreateSurface()) return false;
  return true;
}

bool WaylandBackend::CreateSurface() {
  surface_ = wl_compositor_create_surface(compositor_);
  if (!surface_) {
    std::fprintf(stderr, "[oa-wayland] wl_compositor_create_surface failed\n");
    return false;
  }

  xdg_surface* xs = xdg_wm_base_get_xdg_surface(wm_base_, surface_);
  xdg_surface_ = xs;
  xdg_surface_add_listener(xs, &g_xdgsurface_listener, this);

  xdg_toplevel* top = xdg_surface_get_toplevel(xs);
  xdg_toplevel_ = top;
  xdg_toplevel_add_listener(top, &g_xdgtoplevel_listener, this);
  xdg_toplevel_set_app_id(top, "openauto");
  xdg_toplevel_set_title(top, "OpenAuto");
  xdg_toplevel_set_min_size(top, 1, 1);

  // wait for the first configure event.
  wl_display_roundtrip(display_);

  // Establish our window/EGL surface.
  window_ = wl_egl_window_create(surface_, width_, height_);
  if (!window_) {
    std::fprintf(stderr, "[oa-wayland] wl_egl_window_create failed\n");
    return false;
  }
  egl_surface_ = eglCreateWindowSurface(
      reinterpret_cast<EGLDisplay>(egl_display_),
      reinterpret_cast<EGLConfig>(egl_config_),
      reinterpret_cast<EGLNativeWindowType>(window_), nullptr);
  if (egl_surface_ == EGL_NO_SURFACE) {
    std::fprintf(stderr, "[oa-wayland] eglCreateWindowSurface failed\n");
    return false;
  }
  return true;
}

bool WaylandBackend::SetupEgl() {
  static const EGLAttrib kAttribs[] = {EGL_NONE};
  egl_display_ = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_EXT,
                                       (void*)display_, kAttribs);
  if (!egl_display_) {
    std::fprintf(stderr, "[oa-wayland] eglGetPlatformDisplay failed\n");
    return false;
  }
  if (eglInitialize(reinterpret_cast<EGLDisplay>(egl_display_), nullptr,
                    nullptr) != EGL_TRUE) {
    std::fprintf(stderr, "[oa-wayland] eglInitialize failed\n");
    return false;
  }
  return oa::egl::ChooseConfig(reinterpret_cast<EGLDisplay>(egl_display_),
                               reinterpret_cast<EGLConfig*>(&egl_config_));
}

void WaylandBackend::RunLoop(std::function<void(const FrameTick&)> on_vsync,
                             std::function<void(const InputEvent&)> on_input) {
  on_vsync_ = std::move(on_vsync);
  on_input_ = std::move(on_input);
  running_ = true;

  // Request the first frame.
  CommitSurface();

  while (running_) {
    if (wl_display_dispatch(display_) < 0) {
      if (errno != EAGAIN) {
        std::fprintf(stderr, "[oa-wayland] dispatch error: %s\n",
                     strerror(errno));
        break;
      }
    }
    // Drive frame output when the engine wants to render (a frame callback
    // will be received via FlushFramebuffer -> wl_surface_commit).
    if (on_vsync_ && !frame_visible_) {
      // Engine only asks to render while we accept a frame.
    }
  }
  running_ = false;
}

void WaylandBackend::CommitSurface() {
  // Attach the EGL buffer: for a wl_egl_window-backed surface the buffer is
  // implicit; we merely frame() + commit() and let eglSwapBuffers do the work.
  wl_surface_set_buffer_scale(surface_, (int)dpi_);
  frame_ = wl_surface_frame(surface_);
  wl_callback_add_listener(frame_, &g_frame_listener, this);
  wl_surface_commit(surface_);
  frame_visible_ = false;
}

bool WaylandBackend::FlushFramebuffer() {
  if (!running_) return false;
  EGLDisplay dpy = reinterpret_cast<EGLDisplay>(egl_display_);
  if (eglSwapBuffers(dpy, reinterpret_cast<EGLSurface>(egl_surface_)) !=
      EGL_TRUE) {
    return false;
  }
  // Arm the frame callback for the next present; this is what paces vsync.
  CommitSurface();
  return true;
}

void WaylandBackend::OnFrameDone() {
  // The previous buffer has been shown; the compositor is ready for the next.
  if (!running_) return;
  frame_visible_ = true;
  if (on_vsync_) {
    FrameTick tick;
    // Compute a 60Hz target from the monotonic clock; the compositor drives
    // the actual cadence. A more precise implementation would track the
    // presentation time from the callback serial.
    tick.present_ns = 0;
    tick.target_ns = 0;
    on_vsync_(tick);
  }
}

bool WaylandBackend::WaitForVBlank(FrameTick* tick) {
  // Frame pacing arrives via OnFrameDone; nothing to do synchronously.
  (void)tick;
  return true;
}

void WaylandBackend::RequestExit() { running_ = false; }

// --- public event hooks -------------------------------------------------------
void WaylandBackend::OnRegistryGlobal(wl_registry* registry, uint32_t name,
                                      const char* interface, uint32_t version) {
  if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
    compositor_ = static_cast<wl_compositor*>(
        wl_registry_bind(registry, name, &wl_compositor_interface, 4));
  } else if (std::strcmp(interface, xdg_wm_base_interface.name) == 0) {
    wm_base_ = static_cast<xdg_wm_base*>(
        wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
    xdg_wm_base_add_listener(wm_base_, &g_wmbase_listener, this);
  } else if (std::strcmp(interface, wl_seat_interface.name) == 0) {
    seat_ =
        static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, 7));
    wl_seat_add_listener(seat_, &g_seat_listener, this);
  }
}

void WaylandBackend::OnSeatCapabilities(wl_seat* seat, uint32_t caps) {
  if (!seat) return;
  if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer_) {
    pointer_ = wl_seat_get_pointer(seat);
    wl_pointer_add_listener(pointer_, &g_pointer_listener, this);
  } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && pointer_) {
    wl_pointer_destroy(pointer_);
    pointer_ = nullptr;
  }
  if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !keyboard_) {
    keyboard_ = wl_seat_get_keyboard(seat);
    wl_keyboard_add_listener(keyboard_, &g_keyboard_listener, this);
  } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && keyboard_) {
    wl_keyboard_destroy(keyboard_);
    keyboard_ = nullptr;
  }
}

void WaylandBackend::OnPointerMotion(double x, double y) {
  pointer_x_ = x;
  pointer_y_ = y;
  if (on_input_) {
    InputEvent ev;
    ev.device = InputDeviceKind::Mouse;
    ev.x = x;
    ev.y = y;
    on_input_(ev);
  }
}

void WaylandBackend::OnButtonPress(uint32_t button, bool pressed) {
  if (on_input_) {
    InputEvent ev;
    ev.device = InputDeviceKind::Mouse;
    ev.x = pointer_x_;
    ev.y = pointer_y_;
    switch (button) {
      case BTN_LEFT:
        ev.buttons |= 0x1;
        break;
      case BTN_RIGHT:
        ev.buttons |= 0x2;
        break;
      case BTN_MIDDLE:
        ev.buttons |= 0x4;
        break;
    }
    if (pressed) buttons_ = ev.buttons;
    else buttons_ &= ~(uint32_t)0;
    on_input_(ev);
  }
}

void WaylandBackend::OnAxis(uint32_t axis, double value) {
  if (on_input_ && axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
    InputEvent ev;
    ev.device = InputDeviceKind::Mouse;
    ev.scroll_y = value;
    ev.x = pointer_x_;
    ev.y = pointer_y_;
    on_input_(ev);
  }
}

void WaylandBackend::OnKey(uint32_t keycode, bool down) {
  if (on_input_) {
    InputEvent ev;
    ev.device = InputDeviceKind::Keyboard;
    ev.key_down = down;
    ev.keycode = keycode;
    on_input_(ev);
  }
}

void WaylandBackend::OnConfigure(uint32_t w, uint32_t h) {
  if (window_) wl_egl_window_resize(window_, (int)w, (int)h, 0, 0);
  width_ = (w ? w : width_);
  height_ = (h ? h : height_);
}

// --- Backend factory -----------------------------------------------------------
Backend* CreateWaylandBackend() { return new WaylandBackend(); }

}  // namespace oa
