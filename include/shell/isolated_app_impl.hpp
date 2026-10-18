#pragma once

#include <GLES2/gl2.h>

namespace shell::isolated_app
{

inline auto create(state& s, config cfg) -> result<void>
{
    s.cfg = cfg;
    auto& primary = *cfg.primary;
    auto dpy = static_cast<EGLDisplay>(primary.out->egl_display);

    ipc::spawn_config spawn_cfg{.exe_path = cfg.exe_path,
                               .assets = cfg.assets,
                               .icu_data = cfg.icu_data,
                               .aot_library = cfg.aot_library,
                               .width = cfg.width,
                               .height = cfg.height};
    auto child = ipc::spawn_child(spawn_cfg);
    if (!child)
        return unexpected(child.error());
    s.child = *child;

    std::vector<int> recv_fds;
    if (auto err = ipc::recv_handshake(s.child.parent_fd, s.hs, recv_fds, 5000); !err)
    {
        ipc::terminate(s.child);
        int status = 0;
        ipc::wait_blocking(s.child, &status);
        return unexpected(err.error());
    }

    // Dedicated, long-lived context sharing with primary.gl_ctx -- NOT
    // primary.gl_ctx/resource_ctx/upload_ctx themselves: those belong to
    // Flutter's own internal threads, which may already be using them at
    // any moment. Making one of them current here too, with no
    // coordination, races Flutter's own use of it (the exact failure this
    // session's child_renderer de-risking spike hit, and the same
    // precedent shell::secondary_engine_impl.hpp already established).
    // Kept alive for this module's whole lifetime (not just create()): the
    // receiver thread below reuses it every frame to create each release
    // fence.
    auto primary_cfg = static_cast<EGLConfig>(primary.out->egl_config);
    s.creator_ctx = egl::create_context(dpy, primary_cfg, static_cast<EGLContext>(primary.gl_ctx));
    if (s.creator_ctx == EGL_NO_CONTEXT)
    {
        ipc::terminate(s.child);
        int status = 0;
        ipc::wait_blocking(s.child, &status);
        return unexpected(error{.code = error_code::egl, .message = "isolated_app: create creator_ctx"});
    }

    eglBindAPI(EGL_OPENGL_ES_API);
    if (eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, s.creator_ctx) != EGL_TRUE)
    {
        eglDestroyContext(dpy, s.creator_ctx);
        s.creator_ctx = EGL_NO_CONTEXT;
        ipc::terminate(s.child);
        int status = 0;
        ipc::wait_blocking(s.child, &status);
        return unexpected(
            error{.code = error_code::egl, .message = "isolated_app: make creator_ctx current"});
    }
    s.images.resize(s.hs.buffer_count);
    bool import_ok = true;
    for (std::uint32_t i = 0; i < s.hs.buffer_count; ++i)
    {
        texture::dmabuf_plane plane{.fd = recv_fds[i], .offset = 0, .stride = s.hs.stride};
        texture::dmabuf db{.width = s.hs.width, .height = s.hs.height, .format = s.hs.format,
                           .modifier = 0, .planes = std::span(&plane, 1)};
        std::uint32_t gl_name = 0;
        s.images[i] = texture::import_dmabuf(dpy, db, static_cast<EGLint>(s.hs.width),
                                             static_cast<EGLint>(s.hs.height), &gl_name);
        close(recv_fds[i]); // EGLImage now holds its own reference; the raw fd isn't needed.
        if (gl_name == 0)
            import_ok = false;
    }
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    if (!import_ok)
    {
        eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, s.creator_ctx);
        for (auto& img : s.images)
            texture::release_image(dpy, img);
        eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(dpy, s.creator_ctx);
        s.creator_ctx = EGL_NO_CONTEXT;
        ipc::terminate(s.child);
        int status = 0;
        ipc::wait_blocking(s.child, &status);
        return unexpected(error{.code = error_code::texture, .message = "isolated_app: import_dmabuf failed"});
    }

    texture::bind_external(primary.textures, cfg.target_texture, s.images[0].gl_texture,
                           s.hs.width, s.hs.height);

    s.running.store(true, std::memory_order_relaxed);
    s.receiver = std::thread(
        [&s, &primary, dpy]
        {
            shell::log::set_tag(s.cfg.log_tag);
            eglBindAPI(EGL_OPENGL_ES_API);
            eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, s.creator_ctx);

            ipc::frame_ready msg{};
            int acquire_fence = -1;
            while (s.running.load(std::memory_order_relaxed) &&
                  ipc::recv_frame_ready(s.child.parent_fd, msg, acquire_fence))
            {
                if (msg.buffer_index >= s.images.size())
                {
                    if (acquire_fence >= 0)
                        close(acquire_fence);
                    continue;
                }
                // CPU-blocking wait: this thread has no actively-rendering
                // GL context of its own -- the real GPU read happens later,
                // asynchronously, on Flutter's own rasterizer thread. See
                // egl::wait_fence_cpu's doc comment.
                egl::wait_fence_cpu(acquire_fence);

                texture::bind_external(primary.textures, s.cfg.target_texture,
                                       s.images[msg.buffer_index].gl_texture, s.hs.width,
                                       s.hs.height);
                engine::mark_texture_available(primary, s.cfg.target_texture);

                // Approximate release-fence timing: created immediately
                // after mark_texture_available, on this module's own
                // creator_ctx -- not the context that actually samples the
                // texture (Flutter's rasterizer, on its own thread, does
                // that asynchronously; there is no per-external-texture
                // completion hook exposed to key a precise fence off of).
                // Documented, accepted open risk for this first cut -- see
                // DESIGN.md.
                int release_fence = egl::create_fence(dpy);
                ipc::send_buffer_released(s.child.parent_fd, {.buffer_index = msg.buffer_index},
                                          release_fence);
            }

            // Loop exited: either we were told to stop, or (far more
            // likely) the child is gone -- recv_frame_ready returned false
            // on a clean EOF. Either way, this is the one place that
            // notices a child crash; deliberately does NOT touch
            // `stop_all` -- an isolated app's crash must stay contained to
            // its own layer, not bring down the kiosk.
            if (s.child.pid > 0)
            {
                int status = 0;
                ipc::wait_blocking(s.child, &status);
                if (WIFSIGNALED(status))
                    shell::log::info("isolated_app: child terminated by signal " +
                                     std::to_string(WTERMSIG(status)));
                else if (WIFEXITED(status))
                    shell::log::info("isolated_app: child exited, code " +
                                     std::to_string(WEXITSTATUS(status)));
            }
            s.running.store(false, std::memory_order_relaxed);
            eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        });
    return {};
}

inline auto destroy(state& s) -> void
{
    // Parent-initiated clean shutdown -- if the child is already gone
    // (crashed), this is a harmless no-op signal to a dead pid.
    ipc::terminate(s.child);
    s.running.store(false, std::memory_order_relaxed);
    if (s.receiver.joinable())
        s.receiver.join();

    auto dpy = static_cast<EGLDisplay>(s.cfg.primary->out->egl_display);
    if (s.creator_ctx != EGL_NO_CONTEXT)
    {
        eglBindAPI(EGL_OPENGL_ES_API);
        if (eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, s.creator_ctx) == EGL_TRUE)
        {
            for (auto& img : s.images)
                texture::release_image(dpy, img);
            eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        }
        eglDestroyContext(dpy, s.creator_ctx);
    }
}

} // namespace shell::isolated_app
