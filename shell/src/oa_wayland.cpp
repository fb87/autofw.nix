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

#include <chrono>
#include <thread>
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
void OnXdgSurfaceConfigure(void* data, xdg_surface* surface,
                           uint32_t serial) {
  xdg_surface_ack_configure(surface, serial);
  if (auto* b = static_cast<WaylandBackend*>(data)) {
    b->OnXdgConfigureAcked();
  }
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
void OnPointerEnter(void* data, wl_pointer* p, uint32_t serial, wl_surface* s,
                    wl_fixed_t sx, wl_fixed_t sy);
void OnPointerLeave(void* data, wl_pointer* p, uint32_t serial,
                    wl_surface* s);
void OnPointerAxisSource(void* data, wl_pointer* p, uint32_t source);
void OnPointerAxisStop(void* data, wl_pointer* p, uint32_t time, uint32_t axis);
void OnPointerAxisDiscrete(void* data, wl_pointer* p, uint32_t axis,
                           int32_t discrete);
void OnPointerFrame(void* data, wl_pointer* p);
const wl_pointer_listener g_pointer_listener = {
    .enter = OnPointerEnter,
    .leave = OnPointerLeave,
    .motion = OnPointerMotion,
    .button = OnPointerButton,
    .axis = OnPointerAxis,
    .frame = OnPointerFrame,
    .axis_source = OnPointerAxisSource,
    .axis_stop = OnPointerAxisStop,
    .axis_discrete = OnPointerAxisDiscrete,
};

// Keyboard.
void OnKeyboardKey(void* data, wl_keyboard* k, uint32_t serial, uint32_t time,
                   uint32_t key, uint32_t state);
void OnKeyboardKeymap(void* data, wl_keyboard* k, uint32_t format, int32_t fd,
                      uint32_t size);
void OnKeyboardEnter(void* data, wl_keyboard* k, uint32_t serial,
                     wl_surface* s, wl_array* keys);
void OnKeyboardLeave(void* data, wl_keyboard* k, uint32_t serial,
                     wl_surface* s);
void OnKeyboardModifiers(void* data, wl_keyboard* k, uint32_t serial,
                         uint32_t md, uint32_t ml, uint32_t mlk,
                         uint32_t group);
void OnKeyboardRepeat(void* data, wl_keyboard* k, int32_t rate, int32_t delay);
const wl_keyboard_listener g_keyboard_listener = {
    .keymap = OnKeyboardKeymap,
    .enter = OnKeyboardEnter,
    .leave = OnKeyboardLeave,
    .key = OnKeyboardKey,
    .modifiers = OnKeyboardModifiers,
    .repeat_info = OnKeyboardRepeat,
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
void OnPointerEnter(void* data, wl_pointer*, uint32_t, wl_surface*,
                    wl_fixed_t sx, wl_fixed_t sy) {
  auto* b = static_cast<WaylandBackend*>(data);
  b->OnPointerEnter();
  b->OnPointerMotion(wl_fixed_to_double(sx), wl_fixed_to_double(sy));
}
void OnPointerLeave(void* data, wl_pointer*, uint32_t, wl_surface*) {
  static_cast<WaylandBackend*>(data)->OnPointerLeave();
}
void OnPointerAxisSource(void*, wl_pointer*, uint32_t) {}
void OnPointerAxisStop(void*, wl_pointer*, uint32_t, uint32_t) {}
void OnPointerAxisDiscrete(void*, wl_pointer*, uint32_t, int32_t) {}
void OnPointerFrame(void*, wl_pointer*) {}
void OnKeyboardKey(void* data, wl_keyboard*, uint32_t, uint32_t, uint32_t key,
                   uint32_t state) {
  static_cast<WaylandBackend*>(data)->OnKey(
      key, state == WL_KEYBOARD_KEY_STATE_PRESSED);
}
void OnKeyboardKeymap(void* data, wl_keyboard*, uint32_t format, int32_t fd,
                      uint32_t size) {
  std::fprintf(stderr,
               "[oa-wayland] wl_keyboard.keymap fd=%d size=%u format=%u\n", fd,
               size, format);
  static_cast<WaylandBackend*>(data)->OnKeymap(format, fd, size);
}
void OnKeyboardEnter(void*, wl_keyboard*, uint32_t, wl_surface*, wl_array*) {}
void OnKeyboardLeave(void*, wl_keyboard*, uint32_t, wl_surface*) {}
void OnKeyboardModifiers(void*, wl_keyboard*, uint32_t, uint32_t, uint32_t,
                         uint32_t, uint32_t) {}
void OnKeyboardRepeat(void*, wl_keyboard*, int32_t, int32_t) {}
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

  // The compositor only sends the initial xdg configure after the first commit;
  // commit a buffer-less frame, then process the configure (and ack it), then
  // wait for the surface to be marked configured before presenting.
  wl_surface_commit(surface_);
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
    std::fprintf(stderr, "[oa-wayland] eglCreateWindowSurface failed egl=0x%x\n",
                 eglGetError());
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
                             std::function<void(const InputEvent&)> on_input,
                             std::function<void(uint32_t, uint32_t, double)>
                                 on_resize,
                             std::function<void(int, uint32_t)> on_keymap) {
  on_vsync_ = std::move(on_vsync);
  on_input_ = std::move(on_input);
  on_resize_ = std::move(on_resize);
  on_keymap_ = std::move(on_keymap);
  running_ = true;

  // Drive rendering with a ~60Hz frame clock while also servicing Wayland
  // socket events. The engine's OnVsync consumes a vblank request only when
  // one is pending (it requests one per completed frame), so over-provisioning
  // ticks is harmless and this avoids the bootstrap deadlock where the first
  // compositor frame callback can never fire because no buffer is presented.
  constexpr auto kFramePeriod = std::chrono::milliseconds(16);
  auto next_tick = std::chrono::steady_clock::now();

  while (running_) {
    auto now = std::chrono::steady_clock::now();
    auto to_tick = next_tick - now;
    if (to_tick < std::chrono::steady_clock::duration::zero())
      to_tick = std::chrono::steady_clock::duration::zero();
    int timeout_ms =
        (int)std::chrono::duration_cast<std::chrono::milliseconds>(to_tick)
            .count();

    // Wait for Wayland events (with a cap so the frame clock stays on pace).
    struct pollfd pfd = {};
    pfd.fd = wl_display_get_fd(display_);
    pfd.events = POLLIN;
    int rc = poll(&pfd, 1, timeout_ms);
    if (rc > 0 && (pfd.revents & POLLIN)) {
      // Dispatch all pending and newly-read events for our proxy objects.
      wl_display_dispatch(display_);
      wl_display_flush(display_);
    }

    // Every frame period, hand the engine a tick (no-op if it isn't waiting).
    // Do not tick ahead of the first xdg configure to avoid attaching a buffer
    // to an unconfigured surface.
    if (on_vsync_ && configured() && now >= next_tick) {
      FrameTick tick;
      tick.present_ns = 0;
      tick.target_ns = 0;
      on_vsync_(tick);
      next_tick += kFramePeriod;
    }

    // Before the surface is configured, avoid busy-waiting.
    if (!configured() && (now < next_tick)) {
      std::this_thread::sleep_for(kFramePeriod);
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
  frames_presented_++;
  uint64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
  if (last_fps_log_ms_ == 0) last_fps_log_ms_ = now_ms;
  uint64_t elapsed = now_ms - last_fps_log_ms_;
  if (elapsed >= 2000) {
    uint64_t frames_this_period = frames_presented_ - last_fps_log_frames_;
    double fps = frames_this_period * 1000.0 / double(elapsed);
    std::fprintf(stderr,
                 "[oa-wayland] presented %llu frames total, %.1f fps over %llums\n",
                 (unsigned long long)frames_presented_, fps,
                 (unsigned long long)elapsed);
    last_fps_log_ms_ = now_ms;
    last_fps_log_frames_ = frames_presented_;
  }
  // Arm the frame callback for the next present; this is what paces vsync.
  CommitSurface();
  return true;
}

void WaylandBackend::OnFrameDone() {
  // Frame pacing is driven by the render loop's own clock in RunLoop; the
  // wl frame callback is used only to let the compositor know we saw the
  // previous buffer, so there is nothing to do here.
  frame_visible_ = true;
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
  std::fprintf(stderr,
               "[oa-wayland] seat caps=0x%x (ptr=%d kbd=%d) has_kbd=%d\n",
               caps, (caps & WL_SEAT_CAPABILITY_POINTER) ? 1 : 0,
               (caps & WL_SEAT_CAPABILITY_KEYBOARD) ? 1 : 0,
               keyboard_ ? 1 : 0);
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

void WaylandBackend::OnPointerEnter() {
  if (!pointer_added_) {
    pointer_added_ = true;
    if (on_input_) {
      InputEvent ev;
      ev.device = InputDeviceKind::Mouse;
      ev.add = true;  // kAdd: pointer entered the surface
      on_input_(ev);
    }
  }
}

void WaylandBackend::OnPointerLeave() {
  if (pointer_added_) {
    pointer_added_ = false;
    buttons_ = 0;
    if (on_input_) {
      InputEvent ev;
      ev.device = InputDeviceKind::Mouse;
      ev.remove = true;  // kRemove: pointer left the surface
      on_input_(ev);
    }
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
    ev.buttons = buttons_;
    on_input_(ev);
  }
}

void WaylandBackend::OnButtonPress(uint32_t button, bool pressed) {
  uint32_t mask = 0;
  switch (button) {
    case BTN_LEFT:   mask = 0x1; break;
    case BTN_RIGHT:  mask = 0x2; break;
    case BTN_MIDDLE: mask = 0x4; break;
    default: return;
  }
  if (pressed) buttons_ |= mask;
  else buttons_ &= ~mask;

  if (on_input_) {
    InputEvent ev;
    ev.device = InputDeviceKind::Mouse;
    ev.x = pointer_x_;
    ev.y = pointer_y_;
    ev.buttons = buttons_;
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
  std::fprintf(stderr, "[oa-wayland] key event code=%u down=%d\n", keycode,
               down);
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
  const uint32_t nw = (w ? w : width_);
  const uint32_t nh = (h ? h : height_);
  const bool changed = (nw != width_ || nh != height_);
  width_ = nw;
  height_ = nh;
  // Notify the Flutter engine so it re-lays-out to the new physical size.
  if (changed && on_resize_) {
    on_resize_(width_, height_, dpi_);
  }
}

void WaylandBackend::OnXdgConfigureAcked() { configured_ = true; }

void WaylandBackend::OnKeymap(uint32_t format, int fd, uint32_t size) {
  // Only the canonical keymap formats are handled; the fd is transferred to
  // the engine which mmaps and owns it.
  if (on_keymap_ && fd >= 0) {
    on_keymap_(fd, size);
  } else if (fd >= 0) {
    close(fd);
  }
  (void)format;
}

// --- Backend factory -----------------------------------------------------------
Backend* CreateWaylandBackend() { return new WaylandBackend(); }

}  // namespace oa
