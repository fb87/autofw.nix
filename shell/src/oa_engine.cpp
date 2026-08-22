// oa_engine.cpp -- implementation of the Flutter embedder engine wrapper.
//
// Copyright 2026 OpenAuto Framework.
#include "oa_engine.h"

#include "oa_codec.h"
#include "oa_egl.h"

#include <GLES2/gl2.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <sys/stat.h>

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
  if (surf == EGL_NO_SURFACE || dpy == EGL_NO_DISPLAY)
    return false;
  // eglBindAPI is thread-local; the render thread may default to OpenGL, so
  // re-bind the ES client API before making the ES2 context current.
  eglBindAPI(EGL_OPENGL_ES_API);
  EGLBoolean ok = eglMakeCurrent(dpy, surf, surf, ctx);
  if (ok != EGL_TRUE) {
    std::fprintf(stderr, "[oa-engine] make_current failed egl=0x%x ctx=%p surf=%p\n",
                 eglGetError(), (void*)ctx, (void*)surf);
  }
  return ok == EGL_TRUE;
}

bool OnClearCurrent(void* user_data) {
  auto* e = static_cast<Engine*>(user_data);
  EGLDisplay dpy = reinterpret_cast<EGLDisplay>(e->backend()->egl_display());
  // Release whichever context is current on this thread; EGL must not have a
  // context bound on more than one thread at a time.
  EGLContext cur = eglGetCurrentContext();
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

bool MakeResourceCurrent(void* user_data) {
  // Flutter uploads textures from a worker thread. Because EGL contexts can
  // only be current on one thread at a time, we bind a separate resource
  // context that shares GL state with the main rendering context. A surfaceless
  // bind is sufficient for texture uploads.
  auto* e = static_cast<Engine*>(user_data);
  EGLDisplay dpy = reinterpret_cast<EGLDisplay>(e->backend()->egl_display());
  eglBindAPI(EGL_OPENGL_ES_API);
  return eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE,
                        e->resource_context()) == EGL_TRUE;
}

void* GlProcResolver(void* /*user_data*/, const char* name) {
  // Ensure the ES client API is current on this thread so eglGetProcAddress
  // resolves GLES2 symbols rather than desktop GL.
  eglBindAPI(EGL_OPENGL_ES_API);
  return reinterpret_cast<void*>(eglGetProcAddress(name));
}

void OnVsyncRequest(void* user_data, intptr_t baton) {
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

bool OnGlExternalTextureFrame(void* user_data, int64_t texture_id, size_t w,
                              size_t h, FlutterOpenGLTexture* texture_out) {
  return static_cast<Engine*>(user_data)->GetExternalTexture(
      texture_id, w, h, texture_out);
}

}  // namespace
}  // namespace oa

// ---------------------------------------------------------------------------
namespace oa {

Engine::~Engine() {
  // Shut the engine down FIRST so its teardown platform tasks are still
  // serviced by the (still-running) platform task runner. Only after shutdown
  // completes do we stop the runner (its member destructor does that), letting
  // in-flight tasks drain without posting new work to a vanished thread.
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
  // A second context sharing resources is used by make_resource_current so
  // texture uploads can run on a dedicated worker thread.
  resource_context_ = egl::CreateContext(dpy, config, gl_context_);
  if (resource_context_ == EGL_NO_CONTEXT) {
    std::fprintf(stderr,
                 "[oa-engine] failed to create resource GLES2 context\n");
    return false;
  }
  // A dedicated context in the same sharegroup so external-texture producers
  // can upload frames without contending with Flutter's resource thread.
  texture_upload_context_ = egl::CreateContext(dpy, config, gl_context_);
  if (texture_upload_context_ == EGL_NO_CONTEXT) {
    std::fprintf(stderr,
                 "[oa-engine] failed to create texture-upload GLES2 context\n");
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
  gl_config.gl_external_texture_frame_callback = OnGlExternalTextureFrame;

  // 3) Project arguments.
  char* assets = strdup(assets_path.c_str());
  char* icu = strdup(icu_path.c_str());

  // A dedicated platform task runner lets Flutter deliver platform messages
  // (and other platform-thread work) on its own thread while our platform
  // thread is busy in the display backend event loop. We use the
  // Initialize -> set-engine -> RunInitialized sequence so tasks posted during
  // startup can be serviced once the engine handle exists.
  platform_task_runner_ = std::make_unique<TaskRunner>();
  platform_task_runner_->Start();
  FlutterTaskRunnerDescription platform_runner =
      platform_task_runner_->description();
  FlutterCustomTaskRunners custom_runners = {};
  custom_runners.struct_size = sizeof(custom_runners);
  custom_runners.platform_task_runner = &platform_runner;

  FlutterProjectArgs args = {};
  args.struct_size = sizeof(args);
  args.assets_path = assets;
  args.icu_data_path = icu;
  args.platform_message_callback = OnPlatformMessage;
  args.update_semantics_callback = OnSemantics;
  args.vsync_callback = OnVsyncRequest;
  args.log_message_callback = LogMessage;
  args.custom_task_runners = &custom_runners;

  FlutterRendererConfig renderer = {};
  renderer.type = kOpenGL;
  renderer.open_gl = gl_config;

  FlutterEngine engine = nullptr;
  FlutterEngineResult rc =
      FlutterEngineInitialize(FLUTTER_ENGINE_VERSION, &renderer, &args, this,
                              &engine);
  std::free(assets);
  std::free(icu);

  if (rc != kSuccess) {
    std::fprintf(stderr, "[oa-engine] FlutterEngineInitialize failed rc=%d\n",
                 rc);
    platform_task_runner_->RequestStop();
    return false;
  }
  engine_ = engine;
  platform_task_runner_->set_engine(engine_);

  rc = FlutterEngineRunInitialized(engine_);
  if (rc != kSuccess) {
    std::fprintf(stderr, "[oa-engine] FlutterEngineRunInitialized failed rc=%d\n",
                 rc);
    return false;
  }

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
  // Register the output display with the engine; newer Flutter needs this
  // before it will schedule frames.
  FlutterEngineDisplay display = {};
  display.struct_size = sizeof(display);
  display.single_display = true;
  display.refresh_rate = 60.0;
  display.width = backend_->width();
  display.height = backend_->height();
  display.device_pixel_ratio = backend_->dpi();
  FlutterEngineNotifyDisplayUpdate(engine_,
                                   kFlutterEngineDisplaysUpdateTypeStartup,
                                   &display, 1);

  // The implicit (single) view targets the whole display.
  FlutterWindowMetricsEvent metrics = {};
  metrics.struct_size = sizeof(metrics);
  metrics.width = static_cast<size_t>(backend_->width());
  metrics.height = static_cast<size_t>(backend_->height());
  metrics.pixel_ratio = backend_->dpi();
  metrics.view_id = 0;
  metrics.display_id = 0;
  FlutterEngineSendWindowMetricsEvent(engine_, &metrics);

  // Nudge the engine to schedule its first frame.
  FlutterEngineScheduleFrame(engine_);
}

// --- External texture support ------------------------------------------------
bool Engine::RegisterExternalTexture(int64_t texture_id) {
  textures_.Register(texture_id);
  FlutterEngineResult rc =
      FlutterEngineRegisterExternalTexture(engine_, texture_id);
  if (rc != kSuccess) {
    std::fprintf(stderr, "[oa-engine] register ext texture %lld rc=%d\n",
                 (long long)texture_id, rc);
    textures_.Unregister(texture_id);
    return false;
  }
  std::fprintf(stderr, "[oa-engine] registered external texture %lld\n",
               (long long)texture_id);
  return true;
}

bool Engine::UnregisterExternalTexture(int64_t texture_id) {
  FlutterEngineResult rc =
      FlutterEngineUnregisterExternalTexture(engine_, texture_id);
  // Free the GL object under a current context before removing the entry.
  if (MakeResourceCurrent(this)) {
    textures_.ReleaseTexture(texture_id);
    OnClearCurrent(this);
  }
  textures_.Unregister(texture_id);
  std::fprintf(stderr, "[oa-engine] unregistered external texture %lld rc=%d\n",
               (long long)texture_id, rc);
  return rc == kSuccess;
}

bool Engine::IsExternalTextureRegistered(int64_t texture_id) {
  return textures_.Has(texture_id);
}

bool Engine::PushExternalTextureFrame(int64_t texture_id, uint32_t width,
                                      uint32_t height, const uint8_t* rgba) {
  if (!textures_.Has(texture_id)) {
    std::fprintf(stderr,
                 "[oa-engine] push frame: texture %lld not registered\n",
                 (long long)texture_id);
    return false;
  }
  // Upload on a dedicated context (shares the render sharegroup) so we never
  // fight Flutter's own resource thread for the resource context.
  EGLDisplay dpy = reinterpret_cast<EGLDisplay>(backend_->egl_display());
  eglBindAPI(EGL_OPENGL_ES_API);
  bool ok = eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE,
                           texture_upload_context_) == EGL_TRUE;
  if (ok) {
    ok = textures_.UploadFrame(texture_id, width, height, rgba);
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  } else {
    std::fprintf(stderr,
                 "[oa-engine] push frame: cannot make upload ctx current "
                 "(egl=0x%x)\n",
                 eglGetError());
  }
  if (!ok) return false;

  // Tell the engine a new frame is available; it re-composites next vsync.
  FlutterEngineResult rc = FlutterEngineMarkExternalTextureFrameAvailable(
      engine_, texture_id);
  if (rc != kSuccess) {
    std::fprintf(stderr, "[oa-engine] mark ext texture %lld frame rc=%d\n",
                 (long long)texture_id, rc);
    return false;
  }
  return true;
}

bool Engine::GetExternalTexture(int64_t texture_id, size_t /*w*/, size_t /*h*/,
                                FlutterOpenGLTexture* out) {
  uint32_t name = 0, target = 0, format = 0, width = 0, height = 0;
  bool ready = false;
  if (!out) return false;
  // The engine calls this with the render context current; allocate a
  // placeholder texture if the producer hasn't pushed a frame yet so the
  // engine always has a valid, sampleable object.
  if (!textures_.GetTexture(texture_id, &name, &target, &format, &width,
                            &height, &ready)) {
    textures_.EnsureTexture(texture_id, 1, 1);
    textures_.GetTexture(texture_id, &name, &target, &format, &width, &height,
                         &ready);
  }
  out->target = target;
  out->name = name;
  out->format = format;
  out->width = width;
  out->height = height;
  out->user_data = nullptr;
  out->destruction_callback = nullptr;
  return true;
}

void Engine::OnVsync(const FrameTick& tick) {
  intptr_t baton = 0;
  {
    std::lock_guard<std::mutex> lock(vsync_mutex_);
    if (vsync_batons_.empty()) {
      return;
    }
    baton = vsync_batons_.front();
    vsync_batons_.pop();
  }
  uint64_t present_ns = tick.present_ns;
  uint64_t target_ns = tick.target_ns;
  // The engine requires monotonic-nanosecond timestamps; if the backend did
  // not fill them, derive them from the engine clock.
  if (present_ns == 0) present_ns = FlutterEngineGetCurrentTime();
  if (target_ns == 0) target_ns = present_ns + 16666667ull;  // ~60Hz
  FlutterEngineOnVsync(engine_, baton, present_ns, target_ns);
}

void Engine::HandlePlatformMessage(const FlutterPlatformMessage* message) {
  // The Dart `path_provider` plugin (used by e.g. get_storage) asks for a
  // writable directory to store application data. We satisfy it with a stable
  // directory under the embedder's data root so DeferredLibrary / in-Isolate
  // storage works on a bare embedder.
  if (std::strcmp(message->channel,
                  "plugins.flutter.io/path_provider") == 0) {
    HandlePathProviderMessage(message);
    return;
  }

  // Acknowledge every other platform message with an empty payload so the
  // framework does not wait forever on an embedder that has nothing to say
  // (e.g. the EngineRunInfo handshake at start-up).
  FlutterEngineResult rc = FlutterEngineSendPlatformMessageResponse(
      engine_, message->response_handle, nullptr, 0);
  (void)rc;
}

void Engine::HandlePathProviderMessage(const FlutterPlatformMessage* message) {
  // The path_provider channel uses the binary StandardMethodCodec; decode the
  // method name and reply with a StandardMessageCodec { 'path': ... } map.
  std::string method =
      oa::DecodeStandardMethodCall(message->message, message->message_size);

  std::string dir;
  if (const char* home = std::getenv("HOME")) {
    dir = std::string(home) + "/.openauto-data";
  } else {
    dir = "/tmp/openauto-data";
  }
  // Ensure it exists so get_storage can write files into it.
  mkdir(dir.c_str(), 0700);

  std::vector<uint8_t> response = oa::EncodeStandardStringMap("path", dir);
  FlutterEngineResult rc = FlutterEngineSendPlatformMessageResponse(
      engine_, message->response_handle, response.data(), response.size());
  std::fprintf(stderr,
               "[oa-engine] path_provider method '%s' => '%s' (%zu bytes, rc=%d)\n",
               method.c_str(), dir.c_str(), response.size(), rc);
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

  // Pointer lifecycle: kAdd when entering, kRemove when leaving. These must be
  // sent before any button/motion events for the framework to track the cursor.
  if (event.add) {
    pt.phase = kAdd;
    pt.buttons = 0;
    FlutterEngineSendPointerEvent(engine_, &pt, 1);
    return;
  }
  if (event.remove) {
    pt.phase = kRemove;
    pt.buttons = 0;
    FlutterEngineSendPointerEvent(engine_, &pt, 1);
    return;
  }

  // Infer the phase from button-state transitions.
  uint32_t pressed = event.buttons & 0x7;
  uint32_t old_pressed = last_buttons_ & 0x7;
  last_buttons_ = event.buttons;

  if (event.scroll_y != 0.0) {
    pt.phase = kUp;
    pt.signal_kind = kFlutterPointerSignalKindScroll;
    pt.scroll_delta_y = event.scroll_y;
    pt.buttons = event.buttons;
    FlutterEngineSendPointerEvent(engine_, &pt, 1);
    return;
  }

  if (pressed && !old_pressed) {
    pt.phase = kDown;
  } else if (!pressed && old_pressed) {
    pt.phase = kUp;
  } else {
    pt.phase = pressed ? kMove : kHover;
  }
  pt.buttons = event.buttons;
  FlutterEngineSendPointerEvent(engine_, &pt, 1);
}

}  // namespace oa
