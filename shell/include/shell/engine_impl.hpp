// engine_impl.hpp -- implementation of the shell Flutter engine wrapper.
// Included at the bottom of engine.hpp; must not include engine.hpp back.

#include <functional>
#include <span>

namespace shell::engine
{
namespace detail
{

bool make_current(void* d)
{
    auto* s = static_cast<state*>(d);
    EGLDisplay dpy = static_cast<EGLDisplay>(s->wl->egl.display);
    EGLSurface surf = static_cast<EGLSurface>(s->wl->egl.surface);
    if (surf == EGL_NO_SURFACE || dpy == EGL_NO_DISPLAY || s->gl_ctx == EGL_NO_CONTEXT)
        return false;
    eglBindAPI(EGL_OPENGL_ES_API);
    return eglMakeCurrent(dpy, surf, surf, s->gl_ctx) == EGL_TRUE;
}

bool clear_current(void*)
{
    EGLDisplay dpy = eglGetCurrentDisplay();
    if (dpy == EGL_NO_DISPLAY) return true;
    return eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) == EGL_TRUE;
}

bool present(void* d) { return backend::wayland::swap(*static_cast<state*>(d)->wl); }

std::uint32_t fbo(void*) { return 0; }

bool make_resource_current(void* d)
{
    auto* s = static_cast<state*>(d);
    EGLDisplay dpy = static_cast<EGLDisplay>(s->wl->egl.display);
    eglBindAPI(EGL_OPENGL_ES_API);
    return eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, s->resource_ctx) == EGL_TRUE;
}

void* gl_proc_resolver(void*, const char* name)
{
    eglBindAPI(EGL_OPENGL_ES_API);
    return reinterpret_cast<void*>(eglGetProcAddress(name));
}

void vsync_request(void* d, std::intptr_t baton)
{
    auto* s = static_cast<state*>(d);
    std::lock_guard lock(s->vsync_mutex);
    s->batons.push(baton);
}

bool external_texture_frame(void* d, std::int64_t id, std::size_t, std::size_t,
                            FlutterOpenGLTexture* out);

void log_message(const char* tag, const char* message, void*)
{
    std::fprintf(stdout, "[%s] %s\n", (tag ? tag : "flutter"), message);
    std::fflush(stdout);
}

void empty_semantics(const FlutterSemanticsUpdate*, void*) {}

void on_platform_message(const FlutterPlatformMessage* m, void* d);

// ---- platform task runner trampolines --------------------------------------
bool runs_on_this_thread(void* d)
{
    auto* s = static_cast<state*>(d);
    return std::this_thread::get_id() == s->runner_id;
}

void post_task(FlutterTask task, std::uint64_t, void* d)
{
    auto* s = static_cast<state*>(d);
    std::lock_guard lock(s->runner_mutex);
    s->runner_q.push_back([s, task] { FlutterEngineRunTask(s->fl, &task); });
    s->runner_cv.notify_one();
}

void destruction_cb(void*) {}

}  // namespace detail

// ---- public API ---------------------------------------------------------------
inline auto task_runner_description(state& s) -> FlutterTaskRunnerDescription
{
    FlutterTaskRunnerDescription desc = {};
    desc.struct_size = sizeof(desc);
    desc.user_data = &s;
    desc.runs_task_on_current_thread_callback = detail::runs_on_this_thread;
    desc.post_task_callback = detail::post_task;
    desc.identifier = 0;
    desc.destruction_callback = detail::destruction_cb;
    return desc;
}

inline auto start_runner(state& s) -> void
{
    s.runner_thread = std::thread([&s] {
        s.runner_id = std::this_thread::get_id();
        std::unique_lock lock(s.runner_mutex);
        while (s.runner_started) {
            while (!s.runner_q.empty()) {
                auto fn = s.runner_q.front();
                s.runner_q.pop_front();
                lock.unlock();
                if (fn) fn();
                lock.lock();
            }
            if (!s.runner_started) break;
            s.runner_cv.wait(lock);
        }
    });
}

inline auto stop_runner(state& s) -> void
{
    {
        std::lock_guard lock(s.runner_mutex);
        s.runner_started = false;
    }
    s.runner_cv.notify_all();
    if (s.runner_thread.joinable()) s.runner_thread.join();
}

inline auto init(state& s) -> result<void>
{
    EGLDisplay dpy = static_cast<EGLDisplay>(s.wl->egl.display);
    EGLConfig cfg = static_cast<EGLConfig>(s.wl->egl.config);
    s.gl_ctx = egl::create_context(dpy, cfg, EGL_NO_CONTEXT);
    if (s.gl_ctx == EGL_NO_CONTEXT) {
        return std::unexpected(error{.code = error_code::egl, .message = "gl context"});
    }
    s.resource_ctx = egl::create_context(dpy, cfg, s.gl_ctx);
    s.upload_ctx = egl::create_context(dpy, cfg, s.gl_ctx);

    FlutterOpenGLRendererConfig gl = {};
    gl.struct_size = sizeof(gl);
    gl.make_current = detail::make_current;
    gl.clear_current = detail::clear_current;
    gl.present = detail::present;
    gl.fbo_callback = detail::fbo;
    gl.make_resource_current = detail::make_resource_current;
    gl.gl_proc_resolver = detail::gl_proc_resolver;
    gl.gl_external_texture_frame_callback = detail::external_texture_frame;

    start_runner(s);
    FlutterTaskRunnerDescription pr = task_runner_description(s);
    FlutterCustomTaskRunners cr = {};
    cr.struct_size = sizeof(cr);
    cr.platform_task_runner = &pr;

    std::string assets(s.cfg.assets);
    std::string icu(s.cfg.icu_data);
    char* asset_buf = strdup(assets.c_str());
    char* icu_buf = strdup(icu.c_str());

    FlutterProjectArgs args = {};
    args.struct_size = sizeof(args);
    args.assets_path = asset_buf;
    args.icu_data_path = icu_buf;
    args.vsync_callback = detail::vsync_request;
    args.log_message_callback = detail::log_message;
    args.platform_message_callback = detail::on_platform_message;
    args.update_semantics_callback = detail::empty_semantics;
    args.custom_task_runners = &cr;

    FlutterRendererConfig rc = {};
    rc.type = kOpenGL;
    rc.open_gl = gl;

    FlutterEngine fl = nullptr;
    if (FlutterEngineInitialize(FLUTTER_ENGINE_VERSION, &rc, &args, &s, &fl) != kSuccess) {
        std::free(asset_buf);
        std::free(icu_buf);
        stop_runner(s);
        return std::unexpected(error{.code = error_code::flutter, .message = "init failed"});
    }
    s.fl = fl;
    std::free(asset_buf);
    std::free(icu_buf);
    if (FlutterEngineRunInitialized(fl) != kSuccess) {
        stop_runner(s);
        return std::unexpected(error{.code = error_code::flutter, .message = "run failed"});
    }

    FlutterEngineDisplay display = {};
    display.struct_size = sizeof(display);
    display.single_display = true;
    display.refresh_rate = 60.0;
    display.width = s.wl->width;
    display.height = s.wl->height;
    display.device_pixel_ratio = s.wl->dpi;
    FlutterEngineNotifyDisplayUpdate(fl, kFlutterEngineDisplaysUpdateTypeStartup, &display, 1);

    FlutterWindowMetricsEvent m = {};
    m.struct_size = sizeof(m);
    m.width = s.wl->width;
    m.height = s.wl->height;
    m.pixel_ratio = s.wl->dpi;
    m.view_id = 0;
    FlutterEngineSendWindowMetricsEvent(fl, &m);
    FlutterEngineScheduleFrame(fl);
    return {};
}

inline auto destroy(state& s) -> void
{
    if (s.fl) FlutterEngineShutdown(s.fl);
    stop_runner(s);
}

// --- vsync / input -----------------------------------------------------------
inline void on_vsync(state& s)
{
    std::intptr_t baton = 0;
    {
        std::lock_guard lock(s.vsync_mutex);
        if (s.batons.empty()) return;
        baton = s.batons.front();
        s.batons.pop();
    }
    std::uint64_t now = FlutterEngineGetCurrentTime();
    FlutterEngineOnVsync(s.fl, baton, now, now + 16666667ull);
}

inline void send_pointer(state& s, const input::pointer_event& ev)
{
    FlutterPointerEvent pt = {};
    pt.struct_size = sizeof(pt);
    pt.timestamp = FlutterEngineGetCurrentTime();
    pt.x = ev.x;
    pt.y = ev.y;
    pt.device = 0;
    pt.device_kind = kFlutterPointerDeviceKindMouse;
    pt.signal_kind = kFlutterPointerSignalKindNone;
    pt.view_id = 0;
    switch (ev.phase) {
        case input::pointer_phase::add: pt.phase = kAdd; break;
        case input::pointer_phase::remove: pt.phase = kRemove; break;
        case input::pointer_phase::down: pt.phase = kDown; break;
        case input::pointer_phase::up: pt.phase = kUp; break;
        case input::pointer_phase::move: pt.phase = kMove; break;
        default: pt.phase = kHover; break;
    }
    pt.buttons = s.last_buttons;
    FlutterEngineSendPointerEvent(s.fl, &pt, 1);
}

inline void send_key(state& s, std::uint32_t evdev_code, bool down)
{
    keyboard::ensure_default_keymap(s.kb);
    keyboard::key_result r = keyboard::translate(s.kb, evdev_code, down);
    auto captured = std::make_shared<keyboard::key_result>(r);
    auto deliver = [&s, captured, down] {
        FlutterKeyEvent key = {};
        key.struct_size = sizeof(key);
        key.timestamp = FlutterEngineGetCurrentTime();
        key.type = down ? kFlutterKeyEventTypeDown : kFlutterKeyEventTypeUp;
        key.physical = captured->physical_hid;
        key.logical = captured->logical;
        key.character = (down && !captured->character.empty()) ? captured->character.c_str() : nullptr;
        key.synthesized = false;
        key.device_type = kFlutterKeyEventDeviceTypeKeyboard;
        FlutterEngineSendKeyEvent(s.fl, &key, nullptr, nullptr);
    };
    std::lock_guard lock(s.runner_mutex);
    s.runner_q.push_back(deliver);
    s.runner_cv.notify_one();
}

inline void on_resize(state& s, std::uint32_t width, std::uint32_t height)
{
    s.wl->width = width;
    s.wl->height = height;
    FlutterWindowMetricsEvent m = {};
    m.struct_size = sizeof(m);
    m.width = width;
    m.height = height;
    m.pixel_ratio = s.wl->dpi;
    m.view_id = 0;
    FlutterEngineSendWindowMetricsEvent(s.fl, &m);
}

inline void on_keymap(state& s, int fd, std::uint32_t size)
{
    keyboard::set_keymap(s.kb, fd, size);
}

inline void on_close(state& s) { backend::wayland::stop(*s.wl); }

// --- external textures ---------------------------------------------------------
inline auto register_texture(state& s, texture::id id) -> result<void>
{
    texture::register_id(s.textures, id);
    if (FlutterEngineRegisterExternalTexture(s.fl, id) != kSuccess) {
        texture::unregister_id(s.textures, id);
        return std::unexpected(error{.code = error_code::texture, .message = "register"});
    }
    return {};
}

inline auto unregister_texture(state& s, texture::id id) -> result<void>
{
    FlutterEngineUnregisterExternalTexture(s.fl, id);
    texture::unregister_id(s.textures, id);
    return {};
}

inline auto push_texture_frame(state& s, texture::id id, std::uint32_t w,
                               std::uint32_t h, std::span<const std::uint8_t> rgba) -> bool
{
    if (!texture::has(s.textures, id)) return false;
    EGLDisplay dpy = static_cast<EGLDisplay>(s.wl->egl.display);
    eglBindAPI(EGL_OPENGL_ES_API);
    bool ok = eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, s.upload_ctx) == EGL_TRUE;
    if (ok) {
        ok = texture::upload_frame(s.textures, id, w, h, rgba);
        eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    if (!ok) return false;
    return FlutterEngineMarkExternalTextureFrameAvailable(s.fl, id) == kSuccess;
}

namespace detail
{
bool external_texture_frame(void* d, std::int64_t id, std::size_t, std::size_t,
                            FlutterOpenGLTexture* out)
{
    auto* s = static_cast<state*>(d);
    texture::gl t{};
    if (!texture::get_texture(s->textures, id, t)) {
        texture::ensure_texture(s->textures, id, 1, 1);
        texture::get_texture(s->textures, id, t);
    }
    out->target = GL_TEXTURE_2D;
    out->name = t.name;
    out->format = 0x8058;
    out->width = t.width;
    out->height = t.height;
    out->user_data = nullptr;
    out->destruction_callback = nullptr;
    return true;
}

void on_platform_message(const FlutterPlatformMessage* m, void* d)
{
    FlutterEngineSendPlatformMessageResponse(static_cast<state*>(d)->fl,
                                             m->response_handle, nullptr, 0);
}
}  // namespace detail

}  // namespace shell::engine
