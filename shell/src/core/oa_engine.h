// oa_engine.h -- Flutter embedder engine wrapper. Header-only.
//
// Namespace-based functional module (no classes). Bridges a `backend` dispatch
// table to the Flutter C embedder API using Flutter's OpenGL renderer config.
// All renderer callbacks are wired to the active platform backend so frame
// work happens on the same GL context the backend manages.
//
// Copyright 2026 OpenAuto Framework.
#pragma once

#include <EGL/egl.h>
#include "flutter_embedder.h"

#include <GLES2/gl2.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <sys/stat.h>

#include "oa_backend.h"
#include "oa_codec.h"
#include "oa_egl.h"
#include "oa_keyboard.h"
#include "oa_task_runner.h"
#include "oa_texture.h"
#include "oa_types.h"

namespace oa::engine {

// Handle to a live Flutter engine. Opaque to callers.
struct engine {
  FlutterEngine fl = nullptr;
  backend::backend* b = nullptr;

  EGLContext gl_ctx = EGL_NO_CONTEXT;
  EGLContext resource_ctx = EGL_NO_CONTEXT;
  EGLContext upload_ctx = EGL_NO_CONTEXT;

  task_runner::ctx runner;

  std::mutex vsync_mutex;
  std::queue<intptr_t> vsync_batons;
  uint32_t last_buttons = 0;  // cached pointer button mask for phase synthesis

  texture::registry textures;
  keyboard::ctx kb;
};

// ---- embedder C callback trampolines (user_data == engine handle) ----------
namespace detail {
inline bool make_current(void* user_data) {
  auto* e = static_cast<engine*>(user_data);
  EGLDisplay dpy = reinterpret_cast<EGLDisplay>(e->b ? backend_egl_display(e->b) : nullptr);
  EGLSurface surf = reinterpret_cast<EGLSurface>(e->b ? backend_egl_surface(e->b) : nullptr);
  if (surf == EGL_NO_SURFACE || dpy == EGL_NO_DISPLAY || e->gl_ctx == EGL_NO_CONTEXT)
    return false;
  eglBindAPI(EGL_OPENGL_ES_API);
  EGLBoolean ok = eglMakeCurrent(dpy, surf, surf, e->gl_ctx);
  if (ok != EGL_TRUE)
    std::fprintf(stderr, "[oa-engine] make_current failed egl=0x%x\n", eglGetError());
  return ok == EGL_TRUE;
}
inline bool clear_current(void*) {
  EGLDisplay dpy = eglGetCurrentDisplay();
  if (dpy == EGL_NO_DISPLAY) return true;
  return eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) == EGL_TRUE;
}
inline bool present(void* user_data) {
  return backend_flush(static_cast<engine*>(user_data)->b);
}
inline uint32_t fbo(void*) {
  return 0;  // render into the default FBO of the shared GL context
}
inline bool make_resource_current(void* user_data) {
  auto* e = static_cast<engine*>(user_data);
  EGLDisplay dpy = reinterpret_cast<EGLDisplay>(backend_egl_display(e->b));
  eglBindAPI(EGL_OPENGL_ES_API);
  return eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, e->resource_ctx) == EGL_TRUE;
}
inline void* gl_proc_resolver(void*, const char* name) {
  eglBindAPI(EGL_OPENGL_ES_API);
  return reinterpret_cast<void*>(eglGetProcAddress(name));
}
inline void vsync_request(void* user_data, intptr_t baton) {
  auto* e = static_cast<engine*>(user_data);
  std::lock_guard<std::mutex> lock(e->vsync_mutex);
  e->vsync_batons.push(baton);
}
inline void on_platform_message(const FlutterPlatformMessage* msg, void* user_data);
inline void on_semantics(const FlutterSemanticsUpdate*, void*) {}
inline void log_message(const char* tag, const char* message, void*) {
  std::fprintf(stdout, "[%s] %s\n", (tag ? tag : "flutter"), message);
  std::fflush(stdout);
}
bool external_texture_frame(void* user_data, int64_t id, size_t w, size_t h,
                            FlutterOpenGLTexture* out);
}  // namespace detail

// ---- public API ------------------------------------------------------------

// forward decls (definitions may appear later in the header)
inline void send_metrics(engine* e);

// Create and run a Flutter engine bound to |b|. |assets| is the asset dir
// (kernel_blob.bin etc.), |icu| the icudtl.dat path. Returns nullptr on failure.
inline engine* create(backend::backend* b, const std::string& assets_path,
                      const std::string& icu_path) {
  auto* e = new engine();
  e->b = b;

  EGLDisplay dpy = reinterpret_cast<EGLDisplay>(backend_egl_display(b));
  EGLConfig config = reinterpret_cast<EGLConfig>(backend_egl_config(b));

  e->gl_ctx = oa::egl::create_context(dpy, config, EGL_NO_CONTEXT);
  if (e->gl_ctx == EGL_NO_CONTEXT) { std::fprintf(stderr, "[oa-engine] GL ctx failed\n"); delete e; return nullptr; }
  e->resource_ctx = oa::egl::create_context(dpy, config, e->gl_ctx);
  if (e->resource_ctx == EGL_NO_CONTEXT) { std::fprintf(stderr, "[oa-engine] resource ctx failed\n"); delete e; return nullptr; }
  e->upload_ctx = oa::egl::create_context(dpy, config, e->gl_ctx);
  if (e->upload_ctx == EGL_NO_CONTEXT) { std::fprintf(stderr, "[oa-engine] upload ctx failed\n"); delete e; return nullptr; }

  // Renderer config.
  FlutterOpenGLRendererConfig gl = {};
  gl.struct_size = sizeof(gl);
  gl.make_current = detail::make_current;
  gl.clear_current = detail::clear_current;
  gl.present = detail::present;
  gl.fbo_callback = detail::fbo;
  gl.make_resource_current = detail::make_resource_current;
  gl.gl_proc_resolver = detail::gl_proc_resolver;
  gl.gl_external_texture_frame_callback = detail::external_texture_frame;

  // Platform task runner (Initialize -> set engine -> RunInitialized).
  task_runner::start(&e->runner);
  FlutterTaskRunnerDescription pr = task_runner::description(&e->runner);
  FlutterCustomTaskRunners cr = {};
  cr.struct_size = sizeof(cr);
  cr.platform_task_runner = &pr;

  char* assets = strdup(assets_path.c_str());
  char* icu = strdup(icu_path.c_str());
  FlutterProjectArgs args = {};
  args.struct_size = sizeof(args);
  args.assets_path = assets;
  args.icu_data_path = icu;
  args.platform_message_callback = detail::on_platform_message;
  args.update_semantics_callback = detail::on_semantics;
  args.vsync_callback = detail::vsync_request;
  args.log_message_callback = detail::log_message;
  args.custom_task_runners = &cr;

  FlutterRendererConfig renderer = {};
  renderer.type = kOpenGL;
  renderer.open_gl = gl;

  FlutterEngine fl = nullptr;
  if (FlutterEngineInitialize(FLUTTER_ENGINE_VERSION, &renderer, &args, e,
                              &fl) != kSuccess) {
    std::fprintf(stderr, "[oa-engine] FlutterEngineInitialize failed\n");
    task_runner::stop(&e->runner);
    std::free(assets); std::free(icu);
    delete e;
    return nullptr;
  }
  e->fl = fl;
  task_runner::set_engine(&e->runner, fl);
  std::free(assets); std::free(icu);

  if (FlutterEngineRunInitialized(fl) != kSuccess) {
    std::fprintf(stderr, "[oa-engine] FlutterEngineRunInitialized failed\n");
    task_runner::stop(&e->runner);
    delete e;
    return nullptr;
  }

  // Send initial metrics + display registration, and schedule first frame.
  send_metrics(e);
  return e;
}

inline void destroy(engine* e) {
  if (!e) return;
  // Shut the engine down FIRST while the runner is still alive, then stop it.
  if (e->fl) FlutterEngineShutdown(e->fl);
  task_runner::stop(&e->runner);
  delete e;
}

inline bool is_valid(const engine* e) { return e && e->fl; }

// Called by the backend render loop each frame tick; fulfills a pending vsync
// request.
inline void on_vsync(engine* e, const frame_tick& tick) {
  intptr_t baton = 0;
  {
    std::lock_guard<std::mutex> lock(e->vsync_mutex);
    if (e->vsync_batons.empty()) return;
    baton = e->vsync_batons.front();
    e->vsync_batons.pop();
  }
  uint64_t present_ns = tick.present_ns;
  uint64_t target_ns = tick.target_ns;
  if (present_ns == 0) present_ns = FlutterEngineGetCurrentTime();
  if (target_ns == 0) target_ns = present_ns + 16666667ull;
  FlutterEngineOnVsync(e->fl, baton, present_ns, target_ns);
}

// Send the initial display registration + window metrics.
inline void send_metrics(engine* e) {
  FlutterEngineDisplay display = {};
  display.struct_size = sizeof(display);
  display.single_display = true;
  display.refresh_rate = 60.0;
  display.width = backend_width(e->b);
  display.height = backend_height(e->b);
  display.device_pixel_ratio = backend_dpi(e->b);
  FlutterEngineNotifyDisplayUpdate(e->fl, kFlutterEngineDisplaysUpdateTypeStartup,
                                   &display, 1);

  FlutterWindowMetricsEvent metrics = {};
  metrics.struct_size = sizeof(metrics);
  metrics.width = backend_width(e->b);
  metrics.height = backend_height(e->b);
  metrics.pixel_ratio = backend_dpi(e->b);
  metrics.view_id = 0;
  metrics.display_id = 0;
  FlutterEngineSendWindowMetricsEvent(e->fl, &metrics);
  FlutterEngineScheduleFrame(e->fl);
}

// Notify the engine that the window size/scale changed.
inline void on_resize(engine* e, uint32_t width, uint32_t height,
                      double pixel_ratio) {
  FlutterWindowMetricsEvent metrics = {};
  metrics.struct_size = sizeof(metrics);
  metrics.width = width ? width : backend_width(e->b);
  metrics.height = height ? height : backend_height(e->b);
  metrics.pixel_ratio = pixel_ratio > 0.0 ? pixel_ratio : backend_dpi(e->b);
  metrics.view_id = 0;
  metrics.display_id = 0;
  FlutterEngineSendWindowMetricsEvent(e->fl, &metrics);

  FlutterEngineDisplay display = {};
  display.struct_size = sizeof(display);
  display.single_display = true;
  display.refresh_rate = 60.0;
  display.width = metrics.width;
  display.height = metrics.height;
  display.device_pixel_ratio = metrics.pixel_ratio;
  FlutterEngineNotifyDisplayUpdate(e->fl, kFlutterEngineDisplaysUpdateTypeStartup,
                                   &display, 1);
}

// Supply the Wayland keyboard keymap fd (transferred/owned).
inline void set_keymap(engine* e, int fd, uint32_t size) {
  keyboard::set_keymap(&e->kb, fd, size);
}

// --- External texture support ------------------------------------------------
inline bool register_external_texture(engine* e, int64_t id) {
  texture::register_id(&e->textures, id);
  FlutterEngineResult rc = FlutterEngineRegisterExternalTexture(e->fl, id);
  if (rc != kSuccess) {
    std::fprintf(stderr, "[oa-engine] register ext texture %lld rc=%d\n",
                 (long long)id, rc);
    texture::unregister_id(&e->textures, id);
    return false;
  }
  return true;
}
inline bool unregister_external_texture(engine* e, int64_t id) {
  FlutterEngineResult rc = FlutterEngineUnregisterExternalTexture(e->fl, id);
  if (detail::make_resource_current(e)) {
    texture::release_texture(&e->textures, id);
    detail::clear_current(e);
  }
  texture::unregister_id(&e->textures, id);
  return rc == kSuccess;
}
inline bool has_external_texture(engine* e, int64_t id) {
  return texture::has(&e->textures, id);
}
// Push a new RGBA8 frame into an external texture and mark it available. May be
// called from any thread (uploads on the dedicated sharegroup context).
inline bool push_external_texture_frame(engine* e, int64_t id, uint32_t w,
                                        uint32_t h, const uint8_t* rgba) {
  if (!texture::has(&e->textures, id)) return false;
  EGLDisplay dpy = reinterpret_cast<EGLDisplay>(backend_egl_display(e->b));
  eglBindAPI(EGL_OPENGL_ES_API);
  bool ok = eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, e->upload_ctx) == EGL_TRUE;
  if (ok) {
    ok = texture::upload_frame(&e->textures, id, w, h, rgba);
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  }
  if (!ok) return false;
  return FlutterEngineMarkExternalTextureFrameAvailable(e->fl, id) == kSuccess;
}

// internal: serve the engine's gl_external_texture_frame_callback.
inline bool get_external_texture(engine* e, int64_t id, FlutterOpenGLTexture* out) {
  uint32_t name=0, target=0, format=0, w=0, h=0; bool ready=false;
  if (!out) return false;
  if (!texture::get_texture(&e->textures, id, &name, &target, &format, &w, &h, &ready)) {
    texture::ensure_texture(&e->textures, id, 1, 1);
    texture::get_texture(&e->textures, id, &name, &target, &format, &w, &h, &ready);
  }
  out->target = target;
  out->name = name;
  out->format = format;
  out->width = w;
  out->height = h;
  out->user_data = nullptr;
  out->destruction_callback = nullptr;
  return true;
}

// --- input -------------------------------------------------------------------
inline void handle_platform_message(engine* e, const FlutterPlatformMessage* m);
inline void handle_path_provider(engine* e, const FlutterPlatformMessage* m);

// Forward a platform input event to the Flutter framework (platform thread).
inline void send_input(engine* e, const input_event& event) {
  if (event.device == input_device_kind::keyboard) {
    key_result r = keyboard::translate(&e->kb, event.keycode, event.key_down);
    struct captured_key {
      FlutterEngine fl;
      FlutterKeyEventType type;
      uint64_t timestamp;
      uint64_t physical, logical;
      std::string character;
      bool has_char;
    };
    auto cap = std::make_shared<captured_key>();
    cap->fl = e->fl;
    cap->timestamp = FlutterEngineGetCurrentTime();
    cap->type = event.key_repeat ? kFlutterKeyEventTypeRepeat
                : (event.key_down ? kFlutterKeyEventTypeDown : kFlutterKeyEventTypeUp);
    cap->physical = r.physical_hid;
    cap->logical = r.logical;
    cap->character = r.character;
    cap->has_char = event.key_down && !r.character.empty();

    auto deliver = [cap]() {
      FlutterKeyEvent key = {};
      key.struct_size = sizeof(key);
      key.timestamp = cap->timestamp;
      key.type = cap->type;
      key.physical = cap->physical;
      key.logical = cap->logical;
      key.character = cap->has_char ? cap->character.c_str() : nullptr;
      key.synthesized = false;
      key.device_type = kFlutterKeyEventDeviceTypeKeyboard;
      FlutterEngineSendKeyEvent(cap->fl, &key, nullptr, nullptr);
    };
    if (e->runner.started) task_runner::post_closure(&e->runner, deliver);
    else deliver();
    return;
  }

  // Pointer event (mouse).
  FlutterPointerEvent pt = {};
  pt.struct_size = sizeof(pt);
  pt.timestamp = FlutterEngineGetCurrentTime();
  pt.x = event.x;
  pt.y = event.y;
  pt.device = 0;
  pt.device_kind = kFlutterPointerDeviceKindMouse;
  pt.signal_kind = kFlutterPointerSignalKindNone;
  pt.view_id = 0;

  if (event.add) { pt.phase = kAdd; pt.buttons = 0; FlutterEngineSendPointerEvent(e->fl, &pt, 1); return; }
  if (event.remove) { pt.phase = kRemove; pt.buttons = 0; FlutterEngineSendPointerEvent(e->fl, &pt, 1); return; }

  uint32_t pressed = event.buttons & 0x7;
  uint32_t old_pressed = e->last_buttons & 0x7;
  e->last_buttons = event.buttons;

  if (event.scroll_y != 0.0) {
    pt.phase = kUp; pt.signal_kind = kFlutterPointerSignalKindScroll;
    pt.scroll_delta_y = event.scroll_y; pt.buttons = event.buttons;
    FlutterEngineSendPointerEvent(e->fl, &pt, 1); return;
  }
  if (pressed && !old_pressed) pt.phase = kDown;
  else if (!pressed && old_pressed) pt.phase = kUp;
  else pt.phase = pressed ? kMove : kHover;
  pt.buttons = event.buttons;
  FlutterEngineSendPointerEvent(e->fl, &pt, 1);
}

// ---- platform message handling (defined after send_input) ------------------
inline void handle_path_provider(engine* e, const FlutterPlatformMessage* m) {
  std::string method = oa::codec::decode_method_call(m->message, m->message_size);
  std::string dir = std::getenv("HOME") ? std::string(std::getenv("HOME")) + "/.openauto-data"
                                        : "/tmp/openauto-data";
  mkdir(dir.c_str(), 0700);
  std::vector<uint8_t> resp = oa::codec::encode_string_map("path", dir);
  FlutterEngineSendPlatformMessageResponse(e->fl, m->response_handle,
                                          resp.data(), resp.size());
}
inline void handle_platform_message(engine* e, const FlutterPlatformMessage* m) {
  if (std::strcmp(m->channel, "plugins.flutter.io/path_provider") == 0) {
    handle_path_provider(e, m);
    return;
  }
  FlutterEngineSendPlatformMessageResponse(e->fl, m->response_handle, nullptr, 0);
}

namespace detail {
inline void on_platform_message(const FlutterPlatformMessage* m, void* user_data) {
  handle_platform_message(static_cast<engine*>(user_data), m);
}
inline bool external_texture_frame(void* user_data, int64_t id, size_t, size_t,
                                   FlutterOpenGLTexture* out) {
  return get_external_texture(static_cast<engine*>(user_data), id, out);
}
}  // namespace detail

}  // namespace oa::engine
