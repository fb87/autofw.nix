// oa_wayland.h -- Wayland display backend. Header-only.
//
// Namespace-based functional backend (no classes). Connects to a Wayland
// compositor, creates a top-level xdg_surface window backed by an EGL
// `wl_egl_window`, and renders into it. Exposes itself through the `backend_api`
// dispatch table so the common engine code stays backend-agnostic.
//
// Copyright 2026 OpenAuto Framework.
#pragma once

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <sys/errno.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include <wayland-client.h>
#include <wayland-egl.h>

#include "core/oa_backend.h"
#include "core/oa_egl.h"
#include "core/oa_types.h"
#include "protocol/xdg-shell-client-protocol.h"

namespace oa::backend::wayland {

// Opaque state for the Wayland backend.
struct wl_ctx {
  wl_display* display = nullptr;
  wl_registry* registry = nullptr;
  wl_compositor* compositor = nullptr;
  wl_surface* surface = nullptr;
  wl_seat* seat = nullptr;
  wl_pointer* pointer = nullptr;
  wl_keyboard* keyboard = nullptr;
  wl_callback* frame_cb = nullptr;
  wl_egl_window* window = nullptr;
  xdg_wm_base* wm_base = nullptr;
  xdg_surface* xs = nullptr;
  xdg_toplevel* xt = nullptr;

  void* egl_display = nullptr;
  void* egl_config = nullptr;
  void* egl_surface = nullptr;

  uint32_t width = 1280;
  uint32_t height = 720;
  double dpi = 1.0;

  double pointer_x = 0.0;
  double pointer_y = 0.0;
  uint32_t buttons = 0;

  bool running = false;
  bool configured = false;   // xdg configure received and acked
  bool pointer_added = false;

  uint64_t frames_presented = 0;
  uint64_t last_fps_log_ms = 0;
  uint64_t last_fps_log_frames = 0;

  ::oa::backend::vsync_fn on_vsync;
  ::oa::backend::input_fn on_input;
  ::oa::backend::resize_fn on_resize;
  ::oa::backend::keymap_fn on_keymap;
};

namespace detail {

// Forward declarations of the Wayland listener trampolines.
inline void reg_global(void* d, wl_registry* reg, uint32_t name,
                       const char* iface, uint32_t version);
inline void reg_remove(void* d, wl_registry* reg, uint32_t name);
inline void wm_ping(void*, xdg_wm_base* w, uint32_t serial);
inline void xdg_configure(void* d, xdg_surface* s, uint32_t serial);
inline void top_configure(void* d, xdg_toplevel*, int32_t w, int32_t h,
                          wl_array*);
inline void top_close(void* d, xdg_toplevel*);
inline void seat_caps(void* d, wl_seat* seat, uint32_t caps);
inline void seat_name(void*, wl_seat*, const char*);
inline void p_enter(void* d, wl_pointer*, uint32_t, wl_surface*, wl_fixed_t x,
                    wl_fixed_t y);
inline void p_leave(void* d, wl_pointer*, uint32_t, wl_surface*);
inline void p_motion(void* d, wl_pointer*, uint32_t, wl_fixed_t x, wl_fixed_t y);
inline void p_button(void* d, wl_pointer*, uint32_t, uint32_t, uint32_t b,
                     uint32_t state);
inline void p_axis(void* d, wl_pointer*, uint32_t, uint32_t axis, wl_fixed_t v);
inline void p_axis_source(void*, wl_pointer*, uint32_t);
inline void p_axis_stop(void*, wl_pointer*, uint32_t, uint32_t);
inline void p_axis_discrete(void*, wl_pointer*, uint32_t, int32_t);
inline void p_frame(void*, wl_pointer*);
inline void k_keymap(void* d, wl_keyboard*, uint32_t format, int32_t fd,
                     uint32_t size);
inline void k_enter(void*, wl_keyboard*, uint32_t, wl_surface*, wl_array*);
inline void k_leave(void*, wl_keyboard*, uint32_t, wl_surface*);
inline void k_key(void* d, wl_keyboard*, uint32_t, uint32_t, uint32_t key,
                  uint32_t state);
inline void k_mods(void*, wl_keyboard*, uint32_t, uint32_t, uint32_t, uint32_t,
                   uint32_t);
inline void k_repeat(void*, wl_keyboard*, int32_t, int32_t);
inline void frame_done(void* d, wl_callback* cb, uint32_t);

}  // namespace detail

// Listener structs (inline const -> ODR-safe in header-only).
inline const wl_registry_listener kRegistry = {
    .global = detail::reg_global, .global_remove = detail::reg_remove};
inline const xdg_wm_base_listener kWmBase = {.ping = detail::wm_ping};
inline const xdg_surface_listener kXdgSurface = {.configure = detail::xdg_configure};
inline const xdg_toplevel_listener kToplevel = {
    .configure = detail::top_configure, .close = detail::top_close};
inline const wl_seat_listener kSeat = {
    .capabilities = detail::seat_caps, .name = detail::seat_name};
inline const wl_pointer_listener kPointer = {
    .enter = detail::p_enter,
    .leave = detail::p_leave,
    .motion = detail::p_motion,
    .button = detail::p_button,
    .axis = detail::p_axis,
    .frame = detail::p_frame,
    .axis_source = detail::p_axis_source,
    .axis_stop = detail::p_axis_stop,
    .axis_discrete = detail::p_axis_discrete,
};
inline const wl_keyboard_listener kKeyboard = {
    .keymap = detail::k_keymap,
    .enter = detail::k_enter,
    .leave = detail::k_leave,
    .key = detail::k_key,
    .modifiers = detail::k_mods,
    .repeat_info = detail::k_repeat,
};
inline const wl_callback_listener kFrame = {.done = detail::frame_done};

}  // namespace oa::backend::wayland

// ---- listener bodies -------------------------------------------------------
namespace oa::backend::wayland {
namespace detail {

inline void wm_ping(void*, xdg_wm_base* w, uint32_t serial) {
  xdg_wm_base_pong(w, serial);
}
inline void xdg_configure(void* d, xdg_surface* s, uint32_t serial) {
  xdg_surface_ack_configure(s, serial);
  static_cast<wl_ctx*>(d)->configured = true;
}
inline void top_configure(void* d, xdg_toplevel*, int32_t w, int32_t h,
                          wl_array*) {
  auto* c = static_cast<wl_ctx*>(d);
  if (c->window) wl_egl_window_resize(c->window, (int)w, (int)h, 0, 0);
  uint32_t nw = (w ? (uint32_t)w : c->width);
  uint32_t nh = (h ? (uint32_t)h : c->height);
  bool changed = (nw != c->width || nh != c->height);
  c->width = nw;
  c->height = nh;
  if (changed && c->on_resize) c->on_resize(c->width, c->height, c->dpi);
}
inline void top_close(void* d, xdg_toplevel*) {
  static_cast<wl_ctx*>(d)->running = false;
}
inline void seat_caps(void* d, wl_seat* seat, uint32_t caps) {
  auto* c = static_cast<wl_ctx*>(d);
  if ((caps & WL_SEAT_CAPABILITY_POINTER) && !c->pointer) {
    c->pointer = wl_seat_get_pointer(seat);
    wl_pointer_add_listener(c->pointer, &kPointer, c);
  } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && c->pointer) {
    wl_pointer_destroy(c->pointer);
    c->pointer = nullptr;
  }
  if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !c->keyboard) {
    c->keyboard = wl_seat_get_keyboard(seat);
    wl_keyboard_add_listener(c->keyboard, &kKeyboard, c);
  } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && c->keyboard) {
    wl_keyboard_destroy(c->keyboard);
    c->keyboard = nullptr;
  }
}
inline void seat_name(void*, wl_seat*, const char*) {}
inline void p_enter(void* d, wl_pointer*, uint32_t, wl_surface*,
                    wl_fixed_t sx, wl_fixed_t sy) {
  auto* c = static_cast<wl_ctx*>(d);
  if (!c->pointer_added && c->on_input) {
    c->pointer_added = true;
    input_event ev;
    ev.device = input_device_kind::mouse;
    ev.add = true;
    c->on_input(ev);
  }
  c->pointer_x = wl_fixed_to_double(sx);
  c->pointer_y = wl_fixed_to_double(sy);
}
inline void p_leave(void* d, wl_pointer*, uint32_t, wl_surface*) {
  auto* c = static_cast<wl_ctx*>(d);
  if (c->pointer_added) {
    c->pointer_added = false;
    c->buttons = 0;
    if (c->on_input) {
      input_event ev;
      ev.device = input_device_kind::mouse;
      ev.remove = true;
      c->on_input(ev);
    }
  }
}
inline void p_motion(void* d, wl_pointer*, uint32_t, wl_fixed_t sx, wl_fixed_t sy) {
  auto* c = static_cast<wl_ctx*>(d);
  c->pointer_x = wl_fixed_to_double(sx);
  c->pointer_y = wl_fixed_to_double(sy);
  if (c->on_input) {
    input_event ev;
    ev.device = input_device_kind::mouse;
    ev.x = c->pointer_x;
    ev.y = c->pointer_y;
    ev.buttons = c->buttons;
    c->on_input(ev);
  }
}
inline void p_button(void* d, wl_pointer*, uint32_t, uint32_t, uint32_t button,
                     uint32_t state) {
  auto* c = static_cast<wl_ctx*>(d);
  uint32_t mask = 0;
  if (button == BTN_LEFT) mask = 0x1;
  else if (button == BTN_RIGHT) mask = 0x2;
  else if (button == BTN_MIDDLE) mask = 0x4;
  else return;
  if (state == WL_POINTER_BUTTON_STATE_PRESSED) c->buttons |= mask;
  else c->buttons &= ~mask;
  if (c->on_input) {
    input_event ev;
    ev.device = input_device_kind::mouse;
    ev.x = c->pointer_x;
    ev.y = c->pointer_y;
    ev.buttons = c->buttons;
    c->on_input(ev);
  }
}
inline void p_axis(void* d, wl_pointer*, uint32_t, uint32_t axis, wl_fixed_t v) {
  if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL) return;
  auto* c = static_cast<wl_ctx*>(d);
  if (c->on_input) {
    input_event ev;
    ev.device = input_device_kind::mouse;
    ev.scroll_y = wl_fixed_to_double(v);
    ev.x = c->pointer_x;
    ev.y = c->pointer_y;
    c->on_input(ev);
  }
}
inline void p_axis_source(void*, wl_pointer*, uint32_t) {}
inline void p_axis_stop(void*, wl_pointer*, uint32_t, uint32_t) {}
inline void p_axis_discrete(void*, wl_pointer*, uint32_t, int32_t) {}
inline void p_frame(void*, wl_pointer*) {}
inline void k_keymap(void* d, wl_keyboard*, uint32_t format, int32_t fd,
                     uint32_t size) {
  auto* c = static_cast<wl_ctx*>(d);
  if (c->on_keymap && fd >= 0) c->on_keymap(fd, size);
  else if (fd >= 0) close(fd);
  (void)format;
}
inline void k_enter(void*, wl_keyboard*, uint32_t, wl_surface*, wl_array*) {}
inline void k_leave(void*, wl_keyboard*, uint32_t, wl_surface*) {}
inline void k_key(void* d, wl_keyboard*, uint32_t, uint32_t, uint32_t key,
                  uint32_t state) {
  auto* c = static_cast<wl_ctx*>(d);
  if (c->on_input) {
    input_event ev;
    ev.device = input_device_kind::keyboard;
    ev.key_down = (state == WL_KEYBOARD_KEY_STATE_PRESSED);
    ev.keycode = key;
    c->on_input(ev);
  }
}
inline void k_mods(void*, wl_keyboard*, uint32_t, uint32_t, uint32_t, uint32_t,
                   uint32_t) {}
inline void k_repeat(void*, wl_keyboard*, int32_t, int32_t) {}
inline void frame_done(void* d, wl_callback* cb, uint32_t) {
  wl_callback_destroy(cb);
  static_cast<wl_ctx*>(d)->frame_cb = nullptr;
}

// ---- registry --------------------------------------------------------------
inline void reg_global(void* d, wl_registry* reg, uint32_t name,
                       const char* iface, uint32_t version) {
  auto* c = static_cast<wl_ctx*>(d);
  if (std::strcmp(iface, wl_compositor_interface.name) == 0) {
    c->compositor = static_cast<wl_compositor*>(
        wl_registry_bind(reg, name, &wl_compositor_interface, 4));
  } else if (std::strcmp(iface, xdg_wm_base_interface.name) == 0) {
    c->wm_base = static_cast<xdg_wm_base*>(
        wl_registry_bind(reg, name, &xdg_wm_base_interface, 1));
    xdg_wm_base_add_listener(c->wm_base, &kWmBase, c);
  } else if (std::strcmp(iface, wl_seat_interface.name) == 0) {
    c->seat = static_cast<wl_seat*>(
        wl_registry_bind(reg, name, &wl_seat_interface, 7));
    wl_seat_add_listener(c->seat, &kSeat, c);
  }
  (void)version;
}
inline void reg_remove(void*, wl_registry*, uint32_t) {}

}  // namespace detail

// ---- backend API implementation --------------------------------------------
inline bool init(void* state) {
  auto* c = static_cast<wl_ctx*>(state);
  c->display = wl_display_connect(nullptr);
  if (!c->display) {
    std::fprintf(stderr, "[oa-wayland] cannot connect to Wayland compositor\n");
    return false;
  }
  c->registry = wl_display_get_registry(c->display);
  wl_registry_add_listener(c->registry, &kRegistry, c);
  wl_display_roundtrip(c->display);
  if (!c->compositor || !c->wm_base || !c->seat) {
    std::fprintf(stderr, "[oa-wayland] missing required globals\n");
    return false;
  }
  // EGL platform.
  static const EGLAttrib kAttribs[] = {EGL_NONE};
  c->egl_display = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_EXT,
                                         (void*)c->display, kAttribs);
  if (!c->egl_display || eglInitialize((EGLDisplay)c->egl_display, nullptr,
                                       nullptr) != EGL_TRUE)
    return false;
  if (!egl::choose_config((EGLDisplay)c->egl_display,
                          (EGLConfig*)&c->egl_config))
    return false;

  // Surface + window.
  c->surface = wl_compositor_create_surface(c->compositor);
  if (!c->surface) return false;
  xdg_surface* xs = xdg_wm_base_get_xdg_surface(c->wm_base, c->surface);
  c->xs = xs;
  xdg_surface_add_listener(xs, &kXdgSurface, c);
  xdg_toplevel* top = xdg_surface_get_toplevel(xs);
  c->xt = top;
  xdg_toplevel_add_listener(top, &kToplevel, c);
  xdg_toplevel_set_app_id(top, "openauto");
  xdg_toplevel_set_title(top, "OpenAuto");
  xdg_toplevel_set_min_size(top, 1, 1);
  wl_surface_commit(c->surface);
  wl_display_roundtrip(c->display);

  c->window = wl_egl_window_create(c->surface, (int)c->width, (int)c->height);
  if (!c->window) return false;
  c->egl_surface = eglCreateWindowSurface((EGLDisplay)c->egl_display,
                                          (EGLConfig)c->egl_config,
                                          (EGLNativeWindowType)c->window, nullptr);
  if (c->egl_surface == EGL_NO_SURFACE) return false;
  return true;
}

inline void run_loop(void* state, ::oa::backend::vsync_fn on_vsync,
                     ::oa::backend::input_fn on_input, ::oa::backend::resize_fn on_resize,
                     ::oa::backend::keymap_fn on_keymap) {
  auto* c = static_cast<wl_ctx*>(state);
  c->on_vsync = std::move(on_vsync);
  c->on_input = std::move(on_input);
  c->on_resize = std::move(on_resize);
  c->on_keymap = std::move(on_keymap);
  c->running = true;

  constexpr auto kPeriod = std::chrono::milliseconds(16);
  auto next_tick = std::chrono::steady_clock::now();
  while (c->running) {
    auto now = std::chrono::steady_clock::now();
    auto to = next_tick - now;
    if (to < std::chrono::steady_clock::duration::zero()) to = {};
    int to_ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(to).count();
    pollfd pfd{};
    pfd.fd = wl_display_get_fd(c->display);
    pfd.events = POLLIN;
    int rc = poll(&pfd, 1, to_ms);
    if (rc > 0 && (pfd.revents & POLLIN)) {
      wl_display_dispatch(c->display);
      wl_display_flush(c->display);
    }
    if (c->on_vsync && c->configured && now >= next_tick) {
      frame_tick tick;
      tick.present_ns = 0;
      tick.target_ns = 0;
      c->on_vsync(tick);
      next_tick += kPeriod;
    }
    if (!c->configured && now < next_tick) {
      std::this_thread::sleep_for(kPeriod);
    }
  }
}

inline void request_exit(void* state) {
  static_cast<wl_ctx*>(state)->running = false;
}

inline void* egl_surface(void* state) { return static_cast<wl_ctx*>(state)->egl_surface; }
inline void* egl_display(void* state) { return static_cast<wl_ctx*>(state)->egl_display; }
inline void* egl_config(void* state) { return static_cast<wl_ctx*>(state)->egl_config; }
inline uint32_t width(const void* state) { return static_cast<const wl_ctx*>(state)->width; }
inline uint32_t height(const void* state) { return static_cast<const wl_ctx*>(state)->height; }
inline double dpi(const void* state) { return static_cast<const wl_ctx*>(state)->dpi; }

inline bool flush_framebuffer(void* state) {
  auto* c = static_cast<wl_ctx*>(state);
  if (!c->running) return false;
  if (eglSwapBuffers((EGLDisplay)c->egl_display, (EGLSurface)c->egl_surface) != EGL_TRUE)
    return false;
  c->frames_presented++;
  auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count();
  if (c->last_fps_log_ms == 0) c->last_fps_log_ms = now_ms;
  uint64_t elapsed = now_ms - c->last_fps_log_ms;
  if (elapsed >= 2000) {
    uint64_t n = c->frames_presented - c->last_fps_log_frames;
    // arm frame callback
    if (c->frame_cb) wl_callback_destroy(c->frame_cb);
    c->frame_cb = wl_surface_frame(c->surface);
    wl_callback_add_listener(c->frame_cb, &kFrame, c);
    wl_surface_set_buffer_scale(c->surface, (int)c->dpi);
    wl_surface_commit(c->surface);
    std::fprintf(stderr, "[oa-wayland] %.1f fps over %llums\n",
                 n * 1000.0 / double(elapsed), (unsigned long long)elapsed);
    c->last_fps_log_ms = now_ms;
    c->last_fps_log_frames = c->frames_presented;
  }
  return true;
}
inline bool wait_for_vblank(void* /*state*/, frame_tick*) { return true; }

inline void destroy(void* state) {
  auto* c = static_cast<wl_ctx*>(state);
  if (c->egl_surface) eglDestroySurface((EGLDisplay)c->egl_display, (EGLSurface)c->egl_surface);
  if (c->window) wl_egl_window_destroy(c->window);
  if (c->surface) wl_surface_destroy(c->surface);
  if (c->pointer) wl_pointer_destroy(c->pointer);
  if (c->keyboard) wl_keyboard_destroy(c->keyboard);
  if (c->seat) wl_seat_destroy(c->seat);
  if (c->compositor) wl_compositor_destroy(c->compositor);
  if (c->egl_display) eglTerminate((EGLDisplay)c->egl_display);
  if (c->display) wl_display_disconnect(c->display);
  delete c;
}

// ---- factory ---------------------------------------------------------------
inline ::oa::backend::backend* create_wayland() {
  auto* b = new ::oa::backend::backend();
  static const backend_api api = {
      .initialize = init,
      .run_loop = run_loop,
      .request_exit = request_exit,
      .egl_surface = egl_surface,
      .egl_display = egl_display,
      .egl_config = egl_config,
      .width = width,
      .height = height,
      .dpi = dpi,
      .flush_framebuffer = flush_framebuffer,
      .wait_for_vblank = wait_for_vblank,
      .destroy = destroy,
  };
  b->api = &api;
  b->state = new wl_ctx();
  return b;
}

}  // namespace oa::backend::wayland
