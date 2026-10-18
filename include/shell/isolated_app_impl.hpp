#pragma once

#include <GLES2/gl2.h>

namespace shell::isolated_app
{
namespace detail
{

// One connection attempt: spawn a fresh child, handshake, import its ring
// buffers, bind the first one. Assumes s.creator_ctx already exists (set
// up once in create(), reused across every attempt -- it's tied to the
// primary's EGL display, not any one child's lifetime). On any failure,
// cleans up fully after itself (terminates/reaps the child it just spawned
// if one was spawned, releases any partially-imported images) so s.child
// and s.images are left exactly as they were before the call -- safe to
// retry immediately.
inline auto try_connect(state& s) -> result<void>
{
    auto& primary = *s.cfg.primary;
    auto dpy = static_cast<EGLDisplay>(primary.out->egl_display);

    ipc::spawn_config spawn_cfg{.exe_path = s.cfg.exe_path,
                               .assets = s.cfg.assets,
                               .icu_data = s.cfg.icu_data,
                               .aot_library = s.cfg.aot_library,
                               .width = s.cfg.width,
                               .height = s.cfg.height};
    auto child = ipc::spawn_child(spawn_cfg);
    if (!child)
        return unexpected(child.error());
    // Close the previous attempt's fds before overwriting s.child -- this is
    // the one place s.child ever gets replaced (both the first connect from
    // create() and every Step-5 restart retry go through here), so it's
    // also the one place that needs to close what it's about to discard.
    if (s.child.parent_fd >= 0)
        close(s.child.parent_fd);
    if (s.child.parent_input_fd >= 0)
        close(s.child.parent_input_fd);
    s.child = *child;

    std::vector<int> recv_fds;
    if (auto err = ipc::recv_handshake(s.child.parent_fd, s.hs, recv_fds, 5000); !err)
    {
        ipc::terminate(s.child);
        int status = 0;
        ipc::wait_blocking(s.child, &status);
        return unexpected(err.error());
    }

    eglBindAPI(EGL_OPENGL_ES_API);
    if (eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, s.creator_ctx) != EGL_TRUE)
    {
        ipc::terminate(s.child);
        int status = 0;
        ipc::wait_blocking(s.child, &status);
        return unexpected(
            error{.code = error_code::egl, .message = "isolated_app: make creator_ctx current"});
    }
    s.images.assign(s.hs.buffer_count, texture::dmabuf_image{});
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

    if (!import_ok)
    {
        for (auto& img : s.images)
            texture::release_image(dpy, img);
        s.images.clear();
        eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        ipc::terminate(s.child);
        int status = 0;
        ipc::wait_blocking(s.child, &status);
        return unexpected(error{.code = error_code::texture, .message = "isolated_app: import_dmabuf failed"});
    }
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    texture::bind_external(primary.textures, s.cfg.target_texture, s.images[0].gl_texture,
                           s.hs.width, s.hs.height);
    return {};
}

} // namespace detail

inline auto create(state& s, config cfg) -> result<void>
{
    s.cfg = cfg;
    auto& primary = *cfg.primary;
    auto dpy = static_cast<EGLDisplay>(primary.out->egl_display);

    // Dedicated, long-lived context sharing with primary.gl_ctx -- NOT
    // primary.gl_ctx/resource_ctx/upload_ctx themselves: those belong to
    // Flutter's own internal threads, which may already be using them at
    // any moment. Making one of them current here too, with no
    // coordination, races Flutter's own use of it (the exact failure this
    // session's child_renderer de-risking spike hit, and the same
    // precedent shell::secondary_engine_impl.hpp already established).
    // Kept alive for this module's whole lifetime -- reused across every
    // connection attempt, not just the first, so it's set up once here and
    // never recreated by detail::try_connect.
    auto primary_cfg = static_cast<EGLConfig>(primary.out->egl_config);
    s.creator_ctx = egl::create_context(dpy, primary_cfg, static_cast<EGLContext>(primary.gl_ctx));
    if (s.creator_ctx == EGL_NO_CONTEXT)
        return unexpected(error{.code = error_code::egl, .message = "isolated_app: create creator_ctx"});

    restart_policy::on_attempt_start(s.restart_state);
    if (auto err = detail::try_connect(s); !err)
    {
        restart_policy::on_attempt_end(s.restart_state, s.cfg.restart); // account for this failed attempt too.
        eglDestroyContext(dpy, s.creator_ctx);
        s.creator_ctx = EGL_NO_CONTEXT;
        return unexpected(err.error());
    }

    s.running.store(true, std::memory_order_relaxed);
    s.receiver = std::thread(
        [&s, &primary, dpy]
        {
            shell::log::set_tag(s.cfg.log_tag);
            eglBindAPI(EGL_OPENGL_ES_API);

            // Every pass through this loop is exactly one "attempt" from
            // restart_policy's point of view: either a connected child's
            // frames get pumped until it dies, or (after a respawn) the
            // reconnect itself fails outright with zero frames pumped.
            // Either way, the loop body ends with exactly one
            // on_attempt_end call.
            while (true)
            {
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
                    // DESIGN.md. Made current transiently -- today's receiver
                    // thread doesn't hold creator_ctx current between
                    // messages (only try_connect/the retry path does).
                    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, s.creator_ctx);
                    int release_fence = egl::create_fence(dpy);
                    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
                    ipc::send_buffer_released(s.child.parent_fd, {.buffer_index = msg.buffer_index},
                                              release_fence);
                }

                // This attempt's connection ended: either the child died
                // (far more likely -- recv_frame_ready returned false on a
                // clean EOF) or we were told to stop mid-pump. Either way,
                // this is the one place that notices a child crash;
                // deliberately does NOT touch `stop_all` -- an isolated
                // app's crash must stay contained to its own layer, not
                // bring down the kiosk.
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
                auto delay = restart_policy::on_attempt_end(s.restart_state, s.cfg.restart);

                if (s.stop_requested.load(std::memory_order_relaxed))
                    break; // explicit shutdown (destroy()).
                if (!s.cfg.restart.enabled)
                    break; // restart disabled -- today's one-shot behavior, unchanged.
                if (restart_policy::should_give_up(s.restart_state, s.cfg.restart))
                {
                    shell::log::info("isolated_app: giving up after " +
                                     std::to_string(s.restart_state.attempt) + " attempts");
                    break;
                }

                // Release the dead attempt's images and blank the layer
                // before waiting out the backoff -- nothing to show while
                // there's no live child, and this frees the GPU resources
                // immediately rather than holding them through a possibly
                // multi-second gap.
                eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, s.creator_ctx);
                for (auto& img : s.images)
                    texture::release_image(dpy, img);
                s.images.clear();
                eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
                engine::set_surface_visible(primary, s.cfg.target_surface, false);
                shell::log::info("isolated_app: blanked, retrying in " +
                                 std::to_string(delay.count()) + "ms");

                {
                    std::unique_lock lock(s.restart_mutex);
                    s.restart_cv.wait_for(lock, delay, [&s]
                                          { return s.stop_requested.load(std::memory_order_relaxed); });
                }
                if (s.stop_requested.load(std::memory_order_relaxed))
                    break;

                restart_policy::on_attempt_start(s.restart_state);
                if (auto err = detail::try_connect(s); !err)
                {
                    shell::log::info("isolated_app: reconnect attempt failed: " + err.error().message);
                    continue; // loop back: on_attempt_end at the top accounts for this failed attempt.
                }

                engine::set_surface_visible(primary, s.cfg.target_surface, true);
                shell::log::info("isolated_app: reconnected, child pid=" +
                                 std::to_string(s.child.pid));
                s.running.store(true, std::memory_order_relaxed);
            }
        });
    return {};
}

inline auto destroy(state& s) -> void
{
    // Parent-initiated clean shutdown -- if the child is already gone
    // (crashed), this is a harmless no-op signal to a dead pid. Also
    // permanently stops the supervisor loop (distinct from `running`,
    // which only gates the current attempt's frame pump) and wakes any
    // blocked interruptible backoff wait immediately rather than making
    // teardown wait out the remaining delay.
    s.stop_requested.store(true, std::memory_order_relaxed);
    ipc::terminate(s.child);
    s.running.store(false, std::memory_order_relaxed);
    s.restart_cv.notify_all();
    if (s.receiver.joinable())
        s.receiver.join();

    // Safe only now that `receiver` (the sole thread that ever reads
    // parent_fd) has been joined -- closing a fd a thread is still blocked
    // reading is racy (see ipc::terminate's doc comment). Nothing ever
    // reads parent_input_fd on this side (it's parent -> child only), so
    // there's no equivalent ordering constraint for it, but closing it here
    // too keeps both fds' lifetimes symmetric and easy to reason about.
    if (s.child.parent_fd >= 0)
        close(s.child.parent_fd);
    if (s.child.parent_input_fd >= 0)
        close(s.child.parent_input_fd);
    s.child.parent_fd = -1;
    s.child.parent_input_fd = -1;

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

inline auto send_pointer(state& s, const input::pointer_event& ev) -> void
{
    if (!s.running.load(std::memory_order_relaxed))
        return;
    ipc::input_msg msg{.kind = ipc::input_kind::pointer,
                       .x = ev.x,
                       .y = ev.y,
                       .phase = static_cast<std::uint32_t>(ev.phase),
                       .buttons = ev.buttons,
                       .device = ev.device,
                       .pointer_device_kind = static_cast<std::uint32_t>(ev.kind)};
    ipc::send_input(s.child.parent_input_fd, msg);
}

inline auto send_key(state& s, std::uint32_t evdev_code, bool down) -> void
{
    if (!s.running.load(std::memory_order_relaxed))
        return;
    ipc::input_msg msg{.kind = ipc::input_kind::key,
                       .evdev_code = evdev_code,
                       .key_down = down ? 1u : 0u};
    ipc::send_input(s.child.parent_input_fd, msg);
}

} // namespace shell::isolated_app
