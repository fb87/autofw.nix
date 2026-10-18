#pragma once

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
    // again outside create()/destroy(), so it can't race anything.
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

    s.engine.cfg = engine::config{.assets = cfg.assets,
                                  .icu_data = cfg.icu_data,
                                  .aot_library = cfg.aot_library,
                                  .renderer = renderer::type::gl,
                                  .share_context = static_cast<EGLContext>(primary.gl_ctx)};
    s.engine.log_tag = cfg.log_tag;
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
    return {};
}

inline auto destroy(state& s) -> void
{
    s.vsync_running.store(false, std::memory_order_relaxed);
    if (s.vsync_thread.joinable())
        s.vsync_thread.join();

    engine::destroy(s.engine); // tears down its own gl_ctx/resource_ctx/upload_ctx + runner thread.

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
