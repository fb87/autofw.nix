// engine_impl.hpp -- implementation of the shell Flutter engine wrapper.
// Included at the bottom of engine.hpp; must not include engine.hpp back.

#include <atomic>
#include <cstdio>
#include <functional>
#include <span>

namespace shell::engine
{
namespace detail
{

#ifdef SHELL_RENDERER_GL
bool make_current(void* d)
{
    auto* s = static_cast<state*>(d);
    EGLDisplay dpy = static_cast<EGLDisplay>(s->out->egl_display);
    EGLSurface surf = static_cast<EGLSurface>(s->out->egl_surface);
    if (surf == EGL_NO_SURFACE || dpy == EGL_NO_DISPLAY || s->gl_ctx == EGL_NO_CONTEXT)
        return false;
    eglBindAPI(EGL_OPENGL_ES_API);
    return eglMakeCurrent(dpy, surf, surf, s->gl_ctx) == EGL_TRUE;
}

bool clear_current(void*)
{
    EGLDisplay dpy = eglGetCurrentDisplay();
    if (dpy == EGL_NO_DISPLAY)
        return true;
    return eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) ==
           EGL_TRUE;
}

bool present(void* d)
{
    return static_cast<state*>(d)->out->present(static_cast<state*>(d)->out->state);
}

std::uint32_t fbo(void*)
{
    return 0;
}

bool make_resource_current(void* d)
{
    auto* s = static_cast<state*>(d);
    EGLDisplay dpy = static_cast<EGLDisplay>(s->out->egl_display);
    eglBindAPI(EGL_OPENGL_ES_API);
    return eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, s->resource_ctx) ==
           EGL_TRUE;
}

void* gl_proc_resolver(void*, const char* name)
{
    eglBindAPI(EGL_OPENGL_ES_API);
    return reinterpret_cast<void*>(eglGetProcAddress(name));
}

#endif // SHELL_RENDERER_GL

void vsync_request(void* d, std::intptr_t baton)
{
    auto* s = static_cast<state*>(d);
    std::lock_guard lock(s->vsync_mutex);
    s->batons.push(baton);
}

// Flutter's software renderer presents a completed CPU pixel buffer. We pack
// it into a software::buffer and hand it to the backend's software present path.
bool software_present(void* d, const void* allocation, std::size_t row_bytes,
                      std::size_t height)
{
    auto* s = static_cast<state*>(d);
    if (!s->out->software_present.present)
        return false;
    renderer::software::buffer frame{};
    auto* bytes = static_cast<const std::byte*>(allocation);
    frame.pixels = {const_cast<std::byte*>(bytes), row_bytes * height};
    frame.width = static_cast<std::uint32_t>(row_bytes / 4); // 4 bytes/px (BGRA8888)
    frame.height = static_cast<std::uint32_t>(height);
    frame.stride = static_cast<std::uint32_t>(row_bytes);
    frame.format = renderer::software::pixel_format::bgra8888;
    return s->out->software_present.present(s->out->software_present.output, frame);
}

bool external_texture_frame(void* d, std::int64_t id, std::size_t, std::size_t,
                            FlutterOpenGLTexture* out);

void log_message(const char* tag, const char* message, void*)
{
    std::fprintf(stdout, "[%s] %s\n", (tag ? tag : "flutter"), message);
    std::fflush(stdout);
}

void empty_semantics(const FlutterSemanticsUpdate*, void*)
{
}

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

void destruction_cb(void*)
{
}

} // namespace detail

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
    s.runner_thread = std::thread(
        [&s]
        {
            s.runner_id = std::this_thread::get_id();
            std::unique_lock lock(s.runner_mutex);
            while (s.runner_started)
            {
                while (!s.runner_q.empty())
                {
                    auto fn = s.runner_q.front();
                    s.runner_q.pop_front();
                    lock.unlock();
                    if (fn)
                        fn();
                    lock.lock();
                }
                if (!s.runner_started)
                    break;
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
    if (s.runner_thread.joinable())
        s.runner_thread.join();
}

// Forward declarations (defined below) so the gallery channels in init() can
// drive external textures.
inline auto register_texture(state& s, texture::id id) -> result<void>;
inline auto unregister_texture(state& s, texture::id id) -> result<void>;
inline auto push_texture_frame(state& s, texture::id id, std::uint32_t w,
                               std::uint32_t h, std::span<const std::uint8_t> rgba)
    -> bool;

inline auto init(state& s) -> result<void>
{
    // Renderer selection is independent of the display backend: accelerated
    // (EGL/OpenGL ES) or software (CPU buffer). The GL path is only compiled
    // when SHELL_RENDERER_GL is defined; without it, software is used.
    bool use_gl = false;
#ifdef SHELL_RENDERER_GL
    use_gl = s.cfg.renderer != renderer::type::software;
#endif

    FlutterSoftwareRendererConfig sw = {};
    FlutterRendererConfig rc = {};

#ifdef SHELL_RENDERER_GL
    FlutterOpenGLRendererConfig gl = {};
    if (use_gl)
    {
        EGLDisplay dpy = static_cast<EGLDisplay>(s.out->egl_display);
        EGLConfig cfg = static_cast<EGLConfig>(s.out->egl_config);
        s.gl_ctx = egl::create_context(dpy, cfg, EGL_NO_CONTEXT);
        if (s.gl_ctx == EGL_NO_CONTEXT)
        {
            return unexpected(error{.code = error_code::egl, .message = "gl context"});
        }
        s.resource_ctx = egl::create_context(dpy, cfg, s.gl_ctx);
        s.upload_ctx = egl::create_context(dpy, cfg, s.gl_ctx);

        gl.struct_size = sizeof(gl);
        gl.make_current = detail::make_current;
        gl.clear_current = detail::clear_current;
        gl.present = detail::present;
        gl.fbo_callback = detail::fbo;
        gl.make_resource_current = detail::make_resource_current;
        gl.gl_proc_resolver = detail::gl_proc_resolver;
        gl.gl_external_texture_frame_callback = detail::external_texture_frame;
        rc.type = kOpenGL;
        rc.open_gl = gl;
    }
#endif
    if (!use_gl)
    {
        sw.struct_size = sizeof(sw);
        sw.surface_present_callback = detail::software_present;
        rc.type = kSoftware;
        rc.software = sw;
    }

    start_runner(s);
    FlutterTaskRunnerDescription pr = task_runner_description(s);
    FlutterCustomTaskRunners cr = {};
    cr.struct_size = sizeof(cr);
    cr.platform_task_runner = &pr;

    std::string assets(s.cfg.assets);
    std::string icu(s.cfg.icu_data);
    char* asset_buf = strdup(assets.c_str());
    char* icu_buf = strdup(icu.c_str());

    // A non-empty aot_library switches the engine from JIT (kernel_blob.bin
    // in `assets`) to running a release-mode AOT snapshot.
    std::string aot_path(s.cfg.aot_library);
    if (!aot_path.empty())
    {
        FlutterEngineAOTDataSource aot_source = {};
        aot_source.type = kFlutterEngineAOTDataSourceTypeElfPath;
        aot_source.elf_path = aot_path.c_str();
        if (FlutterEngineCreateAOTData(&aot_source, &s.aot_data) != kSuccess)
        {
            std::free(asset_buf);
            std::free(icu_buf);
            stop_runner(s);
            return unexpected(
                error{.code = error_code::flutter,
                      .message = "failed to load AOT snapshot: " + aot_path});
        }
        log::info("AOT mode: " + aot_path);
    }
    else
    {
        log::info("JIT mode");
    }

    FlutterProjectArgs args = {};
    args.struct_size = sizeof(args);
    args.assets_path = asset_buf;
    args.icu_data_path = icu_buf;
    args.aot_data = s.aot_data;
    args.vsync_callback = detail::vsync_request;
    args.log_message_callback = detail::log_message;
    args.platform_message_callback = detail::on_platform_message;
    args.update_semantics_callback = detail::empty_semantics;
    args.custom_task_runners = &cr;

    FlutterEngine fl = nullptr;
    if (FlutterEngineInitialize(FLUTTER_ENGINE_VERSION, &rc, &args, &s, &fl) !=
        kSuccess)
    {
        std::free(asset_buf);
        std::free(icu_buf);
        if (s.aot_data)
            FlutterEngineCollectAOTData(s.aot_data);
        stop_runner(s);
        return unexpected(error{.code = error_code::flutter, .message = "init failed"});
    }
    s.fl = fl;
    std::free(asset_buf);
    std::free(icu_buf);
    if (FlutterEngineRunInitialized(fl) != kSuccess)
    {
        stop_runner(s);
        return unexpected(error{.code = error_code::flutter, .message = "run failed"});
    }

    // Gallery channels.
    //
    // 'shell/demo'  — canned replies for the echo and ping round-trips.
    // 'shell/calc'  — decodes [a, b] int args and returns add/mul results,
    //                 demonstrating argument passing through the codec.
    // 'shell/texture' — external-texture lifecycle: register/unregister ids and
    //                 push CPU RGBA frames the engine uploads to a GL texture
    //                 that Flutter composites (GL builds only).
    channel::register_channel(
        s.channels, "shell/demo",
        [](const std::string& method,
           std::span<const std::uint8_t>) -> result<std::vector<std::uint8_t>>
        {
            if (method == "echo")
                return codec::encode_string("pong");
            if (method == "ping")
                return codec::encode_string("pong");
            return unexpected(
                error{.code = error_code::plugin, .message = "unknown demo method"});
        });

    channel::register_channel(
        s.channels, "shell/calc",
        [](const std::string& method,
           std::span<const std::uint8_t> args) -> result<std::vector<std::uint8_t>>
        {
            std::int64_t a = 0, b = 0;
            int idx = 0;
            bool ok = codec::foreach_list_item(args,
                                               [&](std::span<const std::uint8_t> item)
                                               {
                                                   std::int64_t v = 0;
                                                   if (codec::decode_int64(item, v))
                                                   {
                                                       if (idx == 0)
                                                           a = v;
                                                       else if (idx == 1)
                                                           b = v;
                                                       ++idx;
                                                   }
                                               });
            if (!ok || idx < 2)
                return unexpected(
                    error{.code = error_code::plugin, .message = "calc: need [a, b]"});
            if (method == "add")
                return codec::encode_int64(a + b);
            if (method == "mul")
                return codec::encode_int64(a * b);
            return unexpected(
                error{.code = error_code::plugin, .message = "calc: unknown method"});
        });

    channel::register_channel(
        s.channels, "shell/texture",
        [&s](const std::string& method,
             std::span<const std::uint8_t> args) -> result<std::vector<std::uint8_t>>
        {
            std::int64_t id = 0;
            if (method == "register")
            {
                if (!codec::decode_int64(args, id))
                    return unexpected(error{.code = error_code::plugin,
                                            .message = "texture: register needs id"});
                auto r = register_texture(s, static_cast<texture::id>(id));
                if (!r)
                    return unexpected(r.error());
                return codec::encode_bool(true);
            }
            if (method == "unregister")
            {
                if (codec::decode_int64(args, id))
                    unregister_texture(s, static_cast<texture::id>(id));
                return codec::encode_bool(true);
            }
            if (method == "pushFrame")
            {
                // args: list [id, width, height, rgbaByteArray]
                std::int64_t w = 0, h = 0;
                std::span<const std::uint8_t> rgba;
                int idx = 0;
                codec::foreach_list_item(
                    args,
                    [&](std::span<const std::uint8_t> item)
                    {
                        std::int64_t v = 0;
                        if (codec::decode_int64(item, v))
                        {
                            if (idx == 0)
                                id = v;
                            else if (idx == 1)
                                w = v;
                            else if (idx == 2)
                                h = v;
                            ++idx;
                        }
                        else if (codec::decode_byte_array(item, rgba))
                        {
                            ++idx;
                        }
                    });
                if (idx < 4 || w <= 0 || h <= 0 || rgba.size() < 4)
                    return unexpected(error{.code = error_code::plugin,
                                            .message = "texture: bad pushFrame args"});
                bool ok = push_texture_frame(s, static_cast<texture::id>(id),
                                             static_cast<std::uint32_t>(w),
                                             static_cast<std::uint32_t>(h), rgba);
                if (!ok)
                    return unexpected(
                        error{.code = error_code::texture,
                              .message = "texture: push failed (GL only)"});
                return codec::encode_bool(true);
            }
            return unexpected(error{.code = error_code::plugin,
                                    .message = "texture: unknown method"});
        });

    FlutterEngineDisplay display = {};
    display.struct_size = sizeof(display);
    display.single_display = true;
    display.refresh_rate = 60.0;
    display.width = s.out->width;
    display.height = s.out->height;
    display.device_pixel_ratio = s.out->pixel_ratio;
    FlutterEngineNotifyDisplayUpdate(fl, kFlutterEngineDisplaysUpdateTypeStartup,
                                     &display, 1);

    FlutterWindowMetricsEvent m = {};
    m.struct_size = sizeof(m);
    m.width = s.out->width;
    m.height = s.out->height;
    m.pixel_ratio = s.out->pixel_ratio;
    m.view_id = 0;
    FlutterEngineSendWindowMetricsEvent(fl, &m);
    FlutterEngineScheduleFrame(fl);
    return {};
}

inline auto destroy(state& s) -> void
{
    if (s.fl)
        FlutterEngineShutdown(s.fl);
    if (s.aot_data)
        FlutterEngineCollectAOTData(s.aot_data);
    stop_runner(s);
}

// --- vsync / input -----------------------------------------------------------
inline void on_vsync(state& s)
{
    std::intptr_t baton = 0;
    {
        std::lock_guard lock(s.vsync_mutex);
        if (s.batons.empty())
            return;
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
    pt.device = ev.device;
    pt.device_kind = kFlutterPointerDeviceKindMouse;
    pt.signal_kind = kFlutterPointerSignalKindNone;
    pt.view_id = 0;
    switch (ev.phase)
    {
    case input::pointer_phase::add:
        pt.phase = kAdd;
        break;
    case input::pointer_phase::remove:
        pt.phase = kRemove;
        break;
    case input::pointer_phase::down:
        pt.phase = kDown;
        break;
    case input::pointer_phase::up:
        pt.phase = kUp;
        break;
    case input::pointer_phase::move:
        pt.phase = kMove;
        break;
    default:
        pt.phase = kHover;
        break;
    }
    pt.buttons = ev.buttons;
    s.last_buttons = ev.buttons;
    FlutterEngineSendPointerEvent(s.fl, &pt, 1);
}

// Forward declaration (defined below).
inline void send_key(state& s, std::uint32_t evdev_code, bool down);
// Queue a key event for delivery on the platform thread (--test-input hook).
inline void queue_key(state& s, std::uint32_t code, bool down)
{
    std::lock_guard lock(s.keyq_mutex);
    s.keyq.emplace_back(code, down);
}

// Drain queued key events into send_key; must run on the platform thread.
inline auto drain_keys(state& s) -> void
{
    std::deque<std::pair<std::uint32_t, bool>> batch;
    {
        std::lock_guard lock(s.keyq_mutex);
        batch.swap(s.keyq);
    }
    for (const auto& [code, down] : batch)
        send_key(s, code, down);
}

inline void send_key(state& s, std::uint32_t evdev_code, bool down)
{
    keyboard::ensure_default_keymap(s.kb);
    keyboard::key_result r = keyboard::translate(s.kb, evdev_code, down);
    std::uint32_t ev = evdev_code;

    // Translate on the caller's thread, then deliver on the RUNNER thread.
    // Flutter's `flutter/keydata` channel (used by key events) must be serviced
    // on the platform thread, which for this shell is the custom platform task
    // runner (runner_thread). Sending from the Wayland dispatch thread makes the
    // engine warn and drop the events, so shortcuts/arrows/home/end never fire.
    auto payload = std::make_shared<keyboard::key_result>(r);
    auto deliver = [&s, payload, ev, down]
    {
        FlutterKeyEvent key = {};
        key.struct_size = sizeof(key);
        key.timestamp = FlutterEngineGetCurrentTime();
        key.type = down ? kFlutterKeyEventTypeDown : kFlutterKeyEventTypeUp;
        key.physical = 0x00070000ull | payload->physical_hid; // PhysicalKeyboardKey
        key.logical = payload->logical;
        key.character = (down && !payload->character.empty())
                            ? payload->character.c_str()
                            : nullptr;
        key.synthesized = false;
        key.device_type = kFlutterKeyEventDeviceTypeKeyboard;
        FlutterEngineResult kr =
            FlutterEngineSendKeyEvent(s.fl, &key, nullptr, nullptr);
        std::fprintf(stderr, "[kr] rc=%d\n", (int)kr);

        // Drive host-side IME under the state lock, building host->framework
        // JSON; send it after the lock is released.
        std::string update_json, action_json;
        {
            std::lock_guard lock(s.ti.mutex);
            if (s.ti.connected && down)
            {
                if (ev == 14 /* KEY_BACKSPACE */)
                {
                    textinput::backspace(s.ti);
                }
                else if (ev == 111 /* KEY_DELETE */)
                {
                    textinput::forward_delete(s.ti);
                }
                else if (ev == 105 /* KEY_LEFT */)
                {
                    textinput::move_caret(s.ti, -1, false);
                }
                else if (ev == 106 /* KEY_RIGHT */)
                {
                    textinput::move_caret(s.ti, +1, false);
                }
                else if (ev == 102 /* KEY_HOME */)
                {
                    textinput::move_caret(s.ti, 0, true);
                }
                else if (ev == 107 /* KEY_END */)
                {
                    textinput::move_caret(s.ti, -2, true); // sentinel: go to end
                }
                else if (!payload->character.empty() && payload->character[0] < 0x20)
                {
                    if (ev == 28 /* KEY_ENTER */ && s.ti.multiline)
                    {
                        textinput::insert_characters(s.ti, "\n");
                    }
                    else
                    {
                        textinput::request_action(s.ti);
                    }
                }
                else if (!payload->character.empty())
                {
                    textinput::insert_characters(s.ti, payload->character);
                }
            }
            if (s.ti.action_pending && s.ti.connected)
            {
                action_json = textinput::perform_action_json(s.ti);
                s.ti.action_pending = false;
            }
            if (s.ti.dirty && s.ti.connected)
            {
                update_json = textinput::update_editing_state(s.ti);
                s.ti.dirty = false;
            }
        }
        const std::string kChan = "flutter/textinput";
        auto send_pm = [&](const std::string& json)
        {
            if (json.empty())
                return;
            FlutterPlatformMessage msg = {};
            msg.struct_size = sizeof(msg);
            msg.channel = kChan.c_str();
            msg.message = reinterpret_cast<const std::uint8_t*>(json.data());
            msg.message_size = json.size();
            FlutterEngineSendPlatformMessage(s.fl, &msg);
        };
        send_pm(action_json);
        send_pm(update_json);
    };
    {
        std::lock_guard lock(s.runner_mutex);
        s.runner_q.push_back(deliver);
        s.runner_cv.notify_one();
    }
}

inline void on_resize(state& s, std::uint32_t width, std::uint32_t height)
{
    s.out->width = width;
    s.out->height = height;
    FlutterWindowMetricsEvent m = {};
    m.struct_size = sizeof(m);
    m.width = width;
    m.height = height;
    m.pixel_ratio = s.out->pixel_ratio;
    m.view_id = 0;
    FlutterEngineSendWindowMetricsEvent(s.fl, &m);
}

inline void on_keymap(state& s, int fd, std::uint32_t size)
{
    keyboard::set_keymap(s.kb, fd, size);
}

inline void on_close(state& s)
{
    s.out->stop(s.out->state);
}

// --- external textures ---------------------------------------------------------
inline auto register_texture(state& s, texture::id id) -> result<void>
{
    texture::register_id(s.textures, id);
    if (FlutterEngineRegisterExternalTexture(s.fl, id) != kSuccess)
    {
        texture::unregister_id(s.textures, id);
        return unexpected(error{.code = error_code::texture, .message = "register"});
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
                               std::uint32_t h, std::span<const std::uint8_t> rgba)
    -> bool
{
    if (!texture::has(s.textures, id))
        return false;
#ifdef SHELL_RENDERER_GL
    EGLDisplay dpy = static_cast<EGLDisplay>(s.out->egl_display);
    eglBindAPI(EGL_OPENGL_ES_API);
    bool ok =
        eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, s.upload_ctx) == EGL_TRUE;
    if (ok)
    {
        ok = texture::upload_frame(s.textures, id, w, h, rgba);
        eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    if (!ok)
        return false;
    return FlutterEngineMarkExternalTextureFrameAvailable(s.fl, id) == kSuccess;
#else
    // Software mode has no GL upload context; GL-backed external textures are
    // unsupported and must fail capability negotiation cleanly.
    (void)w;
    (void)h;
    (void)rgba;
    return false;
#endif
}

namespace detail
{
#ifdef SHELL_RENDERER_GL
bool external_texture_frame(void* d, std::int64_t id, std::size_t, std::size_t,
                            FlutterOpenGLTexture* out)
{
    auto* s = static_cast<state*>(d);
    texture::gl t{};
    if (!texture::get_texture(s->textures, id, t))
        return false; // no uploaded frame yet; Flutter retries on next tick.
    out->target = GL_TEXTURE_2D;
    out->name = t.name;
    out->format = 0x8058;
    out->width = t.width;
    out->height = t.height;
    out->user_data = nullptr;
    out->destruction_callback = nullptr;
    return true;
}
#else
bool external_texture_frame(void*, std::int64_t, std::size_t, std::size_t,
                            FlutterOpenGLTexture*)
{
    return false;
}
#endif

void on_platform_message(const FlutterPlatformMessage* m, void* d)
{
    auto* e = static_cast<state*>(d);
    if (m == nullptr || e->fl == nullptr)
        return;

    if (std::string_view(m->channel).find("textinput") != std::string_view::npos)
    {
        // Flutter drives the IME over the JSON-method-coded `flutter/textinput`
        // channel. Parse it and ACK (or update editing state) so a focused
        // TextField actually accepts typed characters.
        std::string payload(reinterpret_cast<const char*>(m->message), m->message_size);
        std::vector<std::uint8_t> resp;
        {
            std::lock_guard lock(e->ti.mutex);
            resp = textinput::handle(e->ti, payload);
            std::fprintf(stderr, "[sel] client=%ld text='%.48s' sel=%ld..%ld\n",
                         (long)e->ti.client, e->ti.text.c_str(),
                         (long)e->ti.selection_base, (long)e->ti.selection_extent);
        }
        // We must always answer (even a bare ACK keeps the connection alive).
        if (resp.empty())
            resp = textinput::ack();
        FlutterEngineSendPlatformMessageResponse(e->fl, m->response_handle, resp.data(),
                                                 resp.size());
        return;
    }
    // Route named platform-message channels through the channel registry. The
    // returned payload is an encoded value; wrap it in a success envelope so the
    // Dart StandardMethodCodec.decodeEnvelope can read it.
    std::vector<std::uint8_t> value = channel::dispatch(
        e->channels, m->channel, std::span(m->message, m->message_size));
    std::vector<std::uint8_t> response;
    if (!value.empty())
        response = codec::success_envelope(value);
    FlutterEngineSendPlatformMessageResponse(
        e->fl, m->response_handle, response.empty() ? nullptr : response.data(),
        response.size());
}
} // namespace detail

} // namespace shell::engine
