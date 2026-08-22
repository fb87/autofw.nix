// oa_engine.cpp -- implementation of the Flutter embedder engine wrapper.
//
// Copyright 2026 OpenAuto Framework.
#include "oa_engine.h"

#include "oa_egl.h"

#include <GLES2/gl2.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

// ---------------------------------------------------------------------------
// C callback trampolines. Flutter invokes these on its render thread; the
// `user_data` passed to FlutterEngineRun is always our `oa::Engine*`.
// ---------------------------------------------------------------------------
namespace oa {
namespace {

bool OnMakeCurrent(void* user_data) {
  auto* e = static_cast<Engine*>(user_data);
  EGLDisplay dpy = reinterpret_cast<EGLDisplay>(e->backend()->egl_display());
  EGLSurface surf = reinterpret_cast<EGLSurface>(e->backend()->egl_surface());
  EGLContext ctx = e->gl_context();
  return eglMakeCurrent(dpy, surf, surf, ctx) == EGL_TRUE;
}

bool OnClearCurrent(void* user_data) {
  auto* e = static_cast<Engine*>(user_data);
  EGLDisplay dpy = reinterpret_cast<EGLDisplay>(e->backend()->egl_display());
  return eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) ==
         EGL_TRUE;
}

bool OnPresent(void* user_data) {
  auto* e = static_cast<Engine*>(user_data);
  return e->backend()->FlushFramebuffer();
}

uint32_t OnFbo(void* user_data) {
  // Flutter renders into the default framebuffer bound to the shared GL
  // context; returning 0 lets the engine use that directly.
  (void)user_data;
  return 0;
}

bool MakeResourceCurrent(void* user_data) { return OnMakeCurrent(user_data); }

void* GlProcResolver(void* /*user_data*/, const char* name) {
  return reinterpret_cast<void*>(eglGetProcAddress(name));
}

void OnVsyncRequest(void* user_data, intptr_t baton) {
  // The engine asks for the next frame time; queue the baton and the render
  // loop will answer it on the next vblank via Engine::OnVsync.
  auto* e = static_cast<Engine*>(user_data);
  e->QueueVsyncBaton(baton);
}

void OnPlatformMessage(const FlutterPlatformMessage* message, void* user_data) {
  static_cast<Engine*>(user_data)->HandlePlatformMessage(message);
}

// Semantics updates are forwarded to the platform accessibility tooling. We
// acknowledge and drop them; the shell is not tied to a specific a11y stack.
void OnSemantics(const FlutterSemanticsUpdate* /*update*/,
                 void* /*user_data*/) {}

void LogMessage(const char* tag, const char* message, void* /*user_data*/) {
  std::fprintf(stdout, "[%s] %s\n", (tag ? tag : "flutter"), message);
  std::fflush(stdout);
}

}  // namespace
}  // namespace oa

// ---------------------------------------------------------------------------
namespace oa {

Engine::~Engine() {
  if (engine_) {
    FlutterEngineShutdown(engine_);
    engine_ = nullptr;
  }
}

bool Engine::Init(Backend* backend, const std::string& assets_path,
                  const std::string& icu_path) {
  backend_ = backend;

  // 1) Create a shared GLES2 context for the config the backend selected.
  EGLDisplay dpy = reinterpret_cast<EGLDisplay>(backend->egl_display());
  EGLConfig config = reinterpret_cast<EGLConfig>(backend->egl_config());
  gl_context_ = egl::CreateContext(dpy, config);
  if (gl_context_ == EGL_NO_CONTEXT) {
    std::fprintf(stderr, "[oa-engine] failed to create GLES2 context\n");
    return false;
  }

  // 2) OpenGL renderer configuration.
  FlutterOpenGLRendererConfig gl_config = {};
  gl_config.struct_size = sizeof(gl_config);
  gl_config.make_current = OnMakeCurrent;
  gl_config.clear_current = OnClearCurrent;
  gl_config.present = OnPresent;
  gl_config.fbo_callback = OnFbo;
  gl_config.make_resource_current = MakeResourceCurrent;
  gl_config.gl_proc_resolver = GlProcResolver;

  // 3) Project arguments.
  char* assets = strdup(assets_path.c_str());
  char* icu = strdup(icu_path.c_str());

  FlutterProjectArgs args = {};
  args.struct_size = sizeof(args);
  args.assets_path = assets;
  args.icu_data_path = icu;
  args.platform_message_callback = OnPlatformMessage;
  args.update_semantics_callback = OnSemantics;
  args.vsync_callback = OnVsyncRequest;
  args.log_message_callback = LogMessage;

  FlutterRendererConfig renderer = {};
  renderer.type = kOpenGL;
  renderer.open_gl = gl_config;

  FlutterEngine engine = nullptr;
  FlutterEngineResult rc =
      FlutterEngineRun(FLUTTER_ENGINE_VERSION, &renderer, &args, this, &engine);
  std::free(assets);
  std::free(icu);

  if (rc != kSuccess) {
    std::fprintf(stderr, "[oa-engine] FlutterEngineRun failed rc=%d\n", rc);
    return false;
  }
  engine_ = engine;

  SendMetrics();
  return true;
}

std::unique_ptr<Engine> Engine::Create(Backend* backend,
                                       const std::string& assets_path,
                                       const std::string& icu_path) {
  auto engine = std::unique_ptr<Engine>(new Engine());
  if (!engine->Init(backend, assets_path, icu_path)) {
    return nullptr;
  }
  return engine;
}

void Engine::SendMetrics() {
  // The implicit (single) view targets the whole display.
  FlutterWindowMetricsEvent metrics = {};
  metrics.struct_size = sizeof(metrics);
  metrics.width = static_cast<size_t>(backend_->width());
  metrics.height = static_cast<size_t>(backend_->height());
  metrics.pixel_ratio = backend_->dpi();
  metrics.view_id = 0;
  FlutterEngineSendWindowMetricsEvent(engine_, &metrics);
}

void Engine::OnVsync(const FrameTick& tick) {
  std::lock_guard<std::mutex> lock(vsync_mutex_);
  if (vsync_batons_.empty()) {
    return;
  }
  intptr_t baton = vsync_batons_.front();
  vsync_batons_.pop();
  FlutterEngineOnVsync(engine_, baton, tick.present_ns, tick.target_ns);
}

void Engine::HandlePlatformMessage(const FlutterPlatformMessage* message) {
  // Acknowledge every platform message by responding with an empty payload so
  // the framework does not wait forever on an embedder that has nothing to say
  // (e.g. the EngineRunInfo handshake at start-up).
  FlutterEngineResult rc = FlutterEngineSendPlatformMessageResponse(
      engine_, message->response_handle, nullptr, 0);
  std::fprintf(stderr,
               "[oa-engine] platform message '%s' (%zu bytes), ack rc=%d\n",
               message->channel, message->message_size, rc);
}

void Engine::SendInputEvent(const InputEvent& event) {
  if (event.device == InputDeviceKind::Keyboard) {
    // Forward the raw Linux keycode as a Flutter key event. Flutter maps the
    // physical key from the keycode; a full implementation would translate to
    // USB HID codes and logical keys via the keymap.
    FlutterKeyEvent key = {};
    key.struct_size = sizeof(key);
    key.timestamp = FlutterEngineGetCurrentTime();
    key.type = event.key_repeat
                   ? kFlutterKeyEventTypeRepeat
                   : (event.key_down ? kFlutterKeyEventTypeDown
                                     : kFlutterKeyEventTypeUp);
    key.physical = key.logical = static_cast<uint64_t>(event.keycode);
    key.device_type = kFlutterKeyEventDeviceTypeKeyboard;
    FlutterEngineSendKeyEvent(engine_, &key, nullptr, nullptr);
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

  // Infer the phase from button state transitions.
  uint32_t pressed = event.buttons & 0x7;
  uint32_t old_pressed = last_buttons_ & 0x7;
  last_buttons_ = event.buttons;

  if (event.scroll_y != 0.0) {
    pt.phase = kUp;
    pt.signal_kind = kFlutterPointerSignalKindScroll;
    pt.scroll_delta_y = event.scroll_y;
    pt.buttons = pressed;
    FlutterEngineSendPointerEvent(engine_, &pt, 1);
    return;
  }

  if (pressed && !old_pressed) {
    pt.phase = kDown;
  } else if (!pressed && old_pressed) {
    pt.phase = kUp;
  } else {
    pt.phase = (pressed) ? kMove : (event.buttons & kFlutterPointerButtonMouseBack
                                     ? kMove
                                     : kHover);
  }
  pt.buttons = pressed ? event.buttons : 0;
  FlutterEngineSendPointerEvent(engine_, &pt, 1);
}

}  // namespace oa
