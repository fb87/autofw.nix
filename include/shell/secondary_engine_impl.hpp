#pragma once

#include <new>

#include <GLES2/gl2.h>

namespace shell::secondary_engine
{
namespace detail
{

// Headless present: nothing to swap to a window -- the secondary's real
// output is the shared texture, already updated in place by Flutter's own
// rendering into the lazily-created FBO (engine_impl.hpp detail::fbo). Just
// tell the PRIMARY engine a new frame is ready. No pixels move.
bool present(void* user_data)
{
    auto* s = static_cast<state*>(user_data);
    return engine::mark_texture_available(*s->cfg.primary, s->cfg.target_texture);
}

void stop(void* user_data)
{
    static_cast<state*>(user_data)->vsync_running.store(false, std::memory_order_relaxed);
}

inline auto now_ms() -> std::int64_t
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// (Re)initializes just the inner engine::state + its vsync thread, reusing
// the already-set-up pbuffer/creator_ctx/shared_tex (set up once in
// create(), untouched by a restart -- none of them is what a hung runner
// thread actually affects). Always starts from a fresh engine::state:
// engine::destroy() doesn't clear textures/channels/stages/layers/surfaces,
// so reusing the struct in place across a restart would leak stale
// registry state from the crashed instance.
inline auto start_engine(state& s) -> result<void>
{
    // engine::state holds a mutex/condition_variable/thread -- not
    // assignable (deleted operator=), so "fresh instance" means an
    // explicit destroy + placement-new reset, not `s.engine = {}`. Safe
    // here specifically because start_engine is only ever called either on
    // a never-started engine::state (create()'s first call) or
    // immediately after detail::stop_engine has already joined the
    // previous instance's runner_thread (every restart) -- std::thread's
    // destructor only aborts if it's still joinable, and it never is at
    // this point.
    s.engine.~state();
    new (&s.engine) engine::state();
    s.engine.cfg = engine::config{.assets = s.cfg.assets,
                                  .icu_data = s.cfg.icu_data,
                                  .aot_library = s.cfg.aot_library,
                                  .renderer = renderer::type::gl,
                                  .share_context = static_cast<EGLContext>(s.cfg.primary->gl_ctx)};
    s.engine.log_tag = s.cfg.log_tag;
    s.engine.offscreen = true;
    s.engine.offscreen_color_tex = s.shared_tex;
    s.engine.out = &s.out;

    if (auto err = engine::init(s.engine); !err)
        return unexpected(err.error());

    s.vsync_running.store(true, std::memory_order_relaxed);
    s.vsync_thread = std::thread(
        [&s]
        {
            log::set_tag(s.cfg.log_tag);
            auto period = std::chrono::duration<double>(1.0 / s.cfg.fps);
            auto next = std::chrono::steady_clock::now();
            while (s.vsync_running.load(std::memory_order_relaxed))
            {
                next +=
                    std::chrono::duration_cast<std::chrono::steady_clock::duration>(period);
                std::this_thread::sleep_until(next);
                engine::on_vsync(s.engine);
            }
        });
    s.engine_alive = true;
    return {};
}

// Idempotent -- safe to call redundantly (e.g. destroy() after the
// watchdog thread already stopped the engine and never restarted it).
inline auto stop_engine(state& s) -> void
{
    if (!s.engine_alive)
        return;
    s.vsync_running.store(false, std::memory_order_relaxed);
    if (s.vsync_thread.joinable())
        s.vsync_thread.join();
    // Tears down its own gl_ctx/resource_ctx/upload_ctx + runner thread.
    // If that runner thread is truly wedged, this join() hangs too -- see
    // the module doc comment in secondary_engine.hpp for why this project
    // doesn't attempt to work around that.
    engine::destroy(s.engine);
    s.engine_alive = false;
}

} // namespace detail

inline auto create(state& s, config cfg) -> result<void>
{
    s.cfg = cfg;

    auto& primary = *cfg.primary;
    auto dpy = static_cast<EGLDisplay>(primary.out->egl_display);

    // One pbuffer config/surface purely so eglMakeCurrent has a real surface
    // to bind -- engine_impl.hpp's detail::fbo ignores it once it lazily
    // creates this engine's own offscreen FBO; nothing is ever presented
    // to it.
    EGLConfig pbuffer_cfg{};
    if (auto err = egl::choose_pbuffer_config(dpy, &pbuffer_cfg); !err)
        return unexpected(err.error());
    s.pbuffer = egl::create_pbuffer_surface(dpy, pbuffer_cfg, cfg.width, cfg.height);
    if (s.pbuffer == EGL_NO_SURFACE)
        return unexpected(
            error{.code = error_code::egl, .message = "secondary: create pbuffer surface"});

    // Create the shared texture now, using a dedicated context that shares
    // with primary.gl_ctx -- texture *objects* are shared immediately
    // across a share group once created; the FBO that will attach it for
    // rendering is not (see the module doc comment in secondary_engine.hpp),
    // which is why that part is deferred. This context is never touched
    // again outside create()/destroy() (and detail::start_engine/stop_engine's
    // own use of it has nothing to do with Flutter's internal contexts), so
    // it can't race anything.
    auto primary_cfg = static_cast<EGLConfig>(primary.out->egl_config);
    s.creator_ctx = egl::create_context(dpy, primary_cfg,
                                        static_cast<EGLContext>(primary.gl_ctx));
    if (s.creator_ctx == EGL_NO_CONTEXT)
        return unexpected(
            error{.code = error_code::egl, .message = "secondary: create creator_ctx"});
    eglBindAPI(EGL_OPENGL_ES_API);
    if (eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, s.creator_ctx) != EGL_TRUE)
        return unexpected(
            error{.code = error_code::egl, .message = "secondary: make creator_ctx current"});
    glGenTextures(1, &s.shared_tex);
    glBindTexture(GL_TEXTURE_2D, s.shared_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(cfg.width),
                static_cast<GLsizei>(cfg.height), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    if (s.shared_tex == 0)
        return unexpected(
            error{.code = error_code::egl, .message = "secondary: glGenTextures failed"});

    texture::bind_external(primary.textures, cfg.target_texture, s.shared_tex, cfg.width,
                           cfg.height);

    s.out.state = &s;
    s.out.egl_display = dpy; // SAME display as primary -- required for context sharing.
    s.out.egl_config = pbuffer_cfg;
    s.out.egl_surface = s.pbuffer;
    s.out.width = cfg.width;
    s.out.height = cfg.height;
    s.out.pixel_ratio = cfg.pixel_ratio;
    s.out.present = detail::present;
    s.out.stop = detail::stop;

    restart_policy::on_attempt_start(s.restart_state);
    if (auto err = detail::start_engine(s); !err)
    {
        restart_policy::on_attempt_end(s.restart_state, s.cfg.restart); // account for this failed attempt too.
        if (s.shared_tex != 0)
        {
            eglBindAPI(EGL_OPENGL_ES_API);
            if (eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, s.creator_ctx) == EGL_TRUE)
            {
                glDeleteTextures(1, &s.shared_tex);
                eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            }
        }
        eglDestroyContext(dpy, s.creator_ctx);
        s.creator_ctx = EGL_NO_CONTEXT;
        eglDestroySurface(dpy, s.pbuffer);
        s.pbuffer = EGL_NO_SURFACE;
        return unexpected(err.error());
    }
    s.last_heartbeat_ms.store(detail::now_ms(), std::memory_order_relaxed);

    s.watchdog_thread = std::thread(
        [&s, &primary]
        {
            shell::log::set_tag(s.cfg.log_tag + "-wd");

            // Every pass through this loop is exactly one "attempt" from
            // restart_policy's point of view: the inner monitoring loop
            // runs until either a hang is detected or we're told to stop,
            // ending with exactly one on_attempt_end call.
            while (true)
            {
                bool hung = false;
                while (!s.stop_requested.load(std::memory_order_relaxed))
                {
                    {
                        std::unique_lock lock(s.restart_mutex);
                        s.restart_cv.wait_for(
                            lock, s.cfg.heartbeat_interval,
                            [&s] { return s.stop_requested.load(std::memory_order_relaxed); });
                    }
                    if (s.stop_requested.load(std::memory_order_relaxed))
                        break;

                    auto now = detail::now_ms();
                    {
                        std::lock_guard lock(s.engine.runner_mutex);
                        s.engine.runner_q.push_back(
                            [&s, now] { s.last_heartbeat_ms.store(now, std::memory_order_relaxed); });
                        s.engine.runner_cv.notify_one();
                    }
                    auto last = s.last_heartbeat_ms.load(std::memory_order_relaxed);
                    if (now - last > s.cfg.hang_threshold.count())
                    {
                        hung = true;
                        break;
                    }
                }
                if (s.stop_requested.load(std::memory_order_relaxed))
                    break; // explicit shutdown (destroy()).

                shell::log::info("secondary_engine: watchdog detected a hang (no heartbeat in over " +
                                 std::to_string(s.cfg.hang_threshold.count()) + "ms)");
                auto delay = restart_policy::on_attempt_end(s.restart_state, s.cfg.restart);

                if (!s.cfg.restart.enabled)
                {
                    shell::log::info("secondary_engine: restart disabled, giving up");
                    break;
                }
                if (restart_policy::should_give_up(s.restart_state, s.cfg.restart))
                {
                    shell::log::info("secondary_engine: giving up after " +
                                     std::to_string(s.restart_state.attempt) + " attempts");
                    break;
                }

                engine::set_surface_visible(primary, s.cfg.target_surface, false);
                detail::stop_engine(s);
                shell::log::info("secondary_engine: blanked, retrying in " +
                                 std::to_string(delay.count()) + "ms");

                {
                    std::unique_lock lock(s.restart_mutex);
                    s.restart_cv.wait_for(lock, delay, [&s]
                                          { return s.stop_requested.load(std::memory_order_relaxed); });
                }
                if (s.stop_requested.load(std::memory_order_relaxed))
                    break;

                restart_policy::on_attempt_start(s.restart_state);
                if (auto err = detail::start_engine(s); !err)
                {
                    shell::log::info("secondary_engine: restart attempt failed: " + err.error().message);
                    continue; // loop back: the heartbeat-staleness check will detect this and retry again.
                }

                engine::set_surface_visible(primary, s.cfg.target_surface, true);
                s.last_heartbeat_ms.store(detail::now_ms(), std::memory_order_relaxed);
                shell::log::info("secondary_engine: restarted");
            }
        });
    return {};
}

inline auto destroy(state& s) -> void
{
    s.stop_requested.store(true, std::memory_order_relaxed);
    s.restart_cv.notify_all();
    if (s.watchdog_thread.joinable())
        s.watchdog_thread.join();

    detail::stop_engine(s); // idempotent -- a no-op if the watchdog already stopped it and never restarted.

    auto dpy = static_cast<EGLDisplay>(s.out.egl_display);
    if (s.pbuffer != EGL_NO_SURFACE)
        eglDestroySurface(dpy, s.pbuffer);

    // Shared texture is owned conceptually by this module even though it's
    // registered on `primary` -- delete it using the same dedicated
    // creator_ctx it was created with (never touched by anything else, so
    // this can't race Flutter's own use of primary's real contexts). The
    // caller is still responsible for engine::unregister_texture(primary,
    // target_texture) afterward (same two-step lifecycle the --test-texture
    // demo already uses: producer.join(); unregister_texture(...)).
    if (s.shared_tex != 0 && s.creator_ctx != EGL_NO_CONTEXT)
    {
        eglBindAPI(EGL_OPENGL_ES_API);
        if (eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, s.creator_ctx) == EGL_TRUE)
        {
            glDeleteTextures(1, &s.shared_tex);
            eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        }
    }
    if (s.creator_ctx != EGL_NO_CONTEXT)
        eglDestroyContext(dpy, s.creator_ctx);
}

} // namespace shell::secondary_engine
