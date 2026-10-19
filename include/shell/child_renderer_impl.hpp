#pragma once

#include <span>
#include <vector>

#include <GLES2/gl2.h>
#include <unistd.h>

namespace shell::child_renderer
{
namespace detail
{

// Called by Flutter after rendering into render_tex via the engine's
// lazily-created offscreen FBO (engine_impl.hpp's detail::fbo). Blits that
// frame into the next ring buffer slot and announces it to the parent.
bool present(void* user_data)
{
    auto* s = static_cast<state*>(user_data);

    std::uint32_t idx = s->next_index;
    s->next_index = (s->next_index + 1) % kBufferCount;
    ring_buffer& rb = s->ring[idx];

    if (rb.pending_release_fence >= 0)
    {
        // GPU-side wait, cheap even if it occasionally has to actually
        // wait; egl::wait_fence always consumes the fd.
        egl::wait_fence(s->egl_display, rb.pending_release_fence);
        rb.pending_release_fence = -1;
    }

    // GLES2 core has only one framebuffer binding point (no separate
    // read/draw as in GLES3) -- binding GL_FRAMEBUFFER here is also what
    // glCopyTexSubImage2D reads from.
    glBindFramebuffer(GL_FRAMEBUFFER, s->engine.offscreen_fbo);
    glBindTexture(GL_TEXTURE_2D, rb.gl_texture);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, static_cast<GLsizei>(s->cfg.width),
                        static_cast<GLsizei>(s->cfg.height));

    int acquire_fence = egl::create_fence(s->egl_display);
    ipc::frame_ready msg{.buffer_index = idx, .frame_number = ++s->frame_number};
    ipc::send_frame_ready(s->cfg.ipc_fd, msg, acquire_fence);
    return true;
}

void stop(void* user_data)
{
    static_cast<state*>(user_data)->vsync_running.store(false, std::memory_order_relaxed);
}

} // namespace detail

inline auto run(config cfg) -> int
{
    state s;
    s.cfg = cfg;

    auto dev = gbm::open_render_node();
    if (!dev)
    {
        log::info("child_renderer: open_render_node failed: " + dev.error().message);
        return 1;
    }
    s.gbm_device = std::move(*dev);

    s.egl_display = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, s.gbm_device.dev, nullptr);
    if (s.egl_display == EGL_NO_DISPLAY ||
        eglInitialize(s.egl_display, nullptr, nullptr) != EGL_TRUE)
    {
        log::info("child_renderer: eglInitialize failed");
        return 1;
    }
    if (!egl::has_extension(s.egl_display, "EGL_ANDROID_native_fence_sync"))
    {
        log::info("child_renderer: EGL_ANDROID_native_fence_sync not supported");
        return 1;
    }
    if (!egl::has_extension(s.egl_display, "EGL_KHR_surfaceless_context"))
    {
        log::info("child_renderer: EGL_KHR_surfaceless_context not supported");
        return 1;
    }

    // No EGL_PBUFFER_BIT configs exist on this display (confirmed
    // empirically) -- a WINDOW-type config is fine, since it's only used
    // for context creation; no window surface is ever created (see
    // engine_impl.hpp detail::make_current's offscreen relaxation).
    EGLint num_configs = 0;
    const EGLint attribs[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE,
                              EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                              EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    if (!eglChooseConfig(s.egl_display, attribs, &s.egl_config, 1, &num_configs) ||
        num_configs == 0)
    {
        log::info("child_renderer: eglChooseConfig failed");
        return 1;
    }

    s.creator_ctx = egl::create_context(s.egl_display, s.egl_config, EGL_NO_CONTEXT);
    if (s.creator_ctx == EGL_NO_CONTEXT)
    {
        log::info("child_renderer: create creator_ctx failed");
        return 1;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    eglMakeCurrent(s.egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, s.creator_ctx);
    glGenTextures(1, &s.render_tex);
    glBindTexture(GL_TEXTURE_2D, s.render_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(cfg.width),
                static_cast<GLsizei>(cfg.height), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    eglMakeCurrent(s.egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (s.render_tex == 0)
    {
        log::info("child_renderer: render_tex creation failed");
        return 1;
    }

    s.out.state = &s;
    s.out.egl_display = s.egl_display;
    s.out.egl_config = s.egl_config;
    s.out.egl_surface = EGL_NO_SURFACE;
    s.out.width = cfg.width;
    s.out.height = cfg.height;
    s.out.pixel_ratio = cfg.pixel_ratio;
    s.out.present = detail::present;
    s.out.stop = detail::stop;

    s.engine.cfg = engine::config{.assets = cfg.assets,
                                  .icu_data = cfg.icu_data,
                                  .aot_library = cfg.aot_library,
                                  .renderer = renderer::type::gl,
                                  .share_context = s.creator_ctx};
    s.engine.log_tag = cfg.log_tag;
    s.engine.offscreen = true;
    s.engine.offscreen_color_tex = s.render_tex;
    s.engine.out = &s.out;

    if (auto err = engine::init(s.engine); !err)
    {
        log::info("child_renderer: engine::init failed: " + err.error().message);
        return 1;
    }

    // Self-import the ring buffers now. Uses creator_ctx -- NOT
    // engine.gl_ctx -- because engine::init() may already have handed
    // gl_ctx to Flutter's own internal threads by this point (an EGL
    // context can only be current on one thread at a time; grabbing it
    // here too would silently fail eglMakeCurrent, leaving every GL call
    // below a no-op against no current context). creator_ctx is idle and
    // shares the same group, so textures created here are visible from
    // gl_ctx too -- the same cross-context-sharing precedent
    // secondary_engine_impl.hpp already established.
    eglMakeCurrent(s.egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, s.creator_ctx);
    std::vector<int> handshake_fds;
    handshake_fds.reserve(kBufferCount);
    for (auto& rb : s.ring)
    {
        auto buf = gbm::create_buffer(s.gbm_device, cfg.width, cfg.height);
        if (!buf)
        {
            log::info("child_renderer: create_buffer failed: " + buf.error().message);
            return 1;
        }
        rb.gbm = std::move(*buf);
        texture::dmabuf_plane plane = gbm::as_dmabuf_plane(rb.gbm);
        texture::dmabuf db{.width = cfg.width, .height = cfg.height, .format = rb.gbm.format,
                           .modifier = 0, .planes = std::span(&plane, 1)};
        rb.image = texture::import_dmabuf(s.egl_display, db, cfg.width, cfg.height,
                                          &rb.gl_texture);
        if (rb.gl_texture == 0)
        {
            log::info("child_renderer: self-import_dmabuf failed");
            return 1;
        }
        handshake_fds.push_back(rb.gbm.fd);
    }
    eglMakeCurrent(s.egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    ipc::handshake hs{.width = cfg.width,
                      .height = cfg.height,
                      .format = s.ring[0].gbm.format,
                      .stride = s.ring[0].gbm.stride,
                      .buffer_count = kBufferCount};
    if (auto err = ipc::send_handshake(cfg.ipc_fd, hs, handshake_fds); !err)
    {
        log::info("child_renderer: send_handshake failed: " + err.error().message);
        return 1;
    }

    s.release_listener_running.store(true, std::memory_order_relaxed);
    s.release_listener_thread = std::thread(
        [&s]
        {
            log::set_tag(s.cfg.log_tag + "-rel");
            ipc::buffer_released msg{};
            int fence_fd = -1;
            while (s.release_listener_running.load(std::memory_order_relaxed) &&
                  ipc::recv_buffer_released(s.cfg.ipc_fd, msg, fence_fd))
            {
                if (msg.buffer_index < kBufferCount)
                {
                    auto& rb = s.ring[msg.buffer_index];
                    if (rb.pending_release_fence >= 0)
                        close(rb.pending_release_fence); // previous one never consumed
                    rb.pending_release_fence = fence_fd;
                }
                else if (fence_fd >= 0)
                {
                    close(fence_fd);
                }
            }
            // Loop exited: either explicitly stopped, or (far more likely)
            // the parent is gone (EOF) -- either way, this is the signal
            // to begin shutdown.
            s.release_listener_running.store(false, std::memory_order_relaxed);
        });

    s.input_listener_running.store(true, std::memory_order_relaxed);
    s.input_listener_thread = std::thread(
        [&s]
        {
            log::set_tag(s.cfg.log_tag + "-in");
            ipc::input_msg msg{};
            while (s.input_listener_running.load(std::memory_order_relaxed) &&
                  ipc::recv_input(s.cfg.input_fd, msg))
            {
                if (msg.kind == ipc::input_kind::pointer)
                {
                    input::pointer_event ev{
                        .device = msg.device,
                        .phase = static_cast<input::pointer_phase>(msg.phase),
                        .x = msg.x,
                        .y = msg.y,
                        .kind = static_cast<input::device_type>(msg.pointer_device_kind),
                        .buttons = msg.buttons,
                    };
                    engine::send_pointer(s.engine, ev);
                }
                else if (msg.kind == ipc::input_kind::key)
                {
                    engine::send_key(s.engine, msg.evdev_code, msg.key_down != 0);
                }
                // Any other value is a malformed/corrupted message -- ignore
                // it rather than guessing (previously this fell into the
                // key-event branch by default, silently misinterpreting
                // garbage as a key event).
            }
            // Same shutdown signal shape as release_listener_thread: EOF
            // here means the parent is gone (both its fds into this child
            // die together when the parent process exits), not a protocol
            // error worth distinguishing from the ipc_fd side.
            s.input_listener_running.store(false, std::memory_order_relaxed);
        });

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

    // Block until the parent goes away: the release listener's blocking
    // read loop is tied to the parent's lifetime (EOF once it closes/dies),
    // making it the natural "time to shut down" signal.
    if (s.release_listener_thread.joinable())
        s.release_listener_thread.join();

    // input_listener_thread's own fd (input_fd) dies alongside ipc_fd when
    // the parent process goes away -- it will have already unblocked via
    // its own EOF by now in practice, this join just ensures it has.
    s.input_listener_running.store(false, std::memory_order_relaxed);
    if (s.input_listener_thread.joinable())
        s.input_listener_thread.join();

    s.vsync_running.store(false, std::memory_order_relaxed);
    if (s.vsync_thread.joinable())
        s.vsync_thread.join();

    engine::destroy(s.engine);
    for (auto& rb : s.ring)
    {
        if (rb.pending_release_fence >= 0)
            close(rb.pending_release_fence);
        texture::release_image(s.egl_display, rb.image);
        gbm::destroy_buffer(rb.gbm);
    }
    eglMakeCurrent(s.egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (s.creator_ctx != EGL_NO_CONTEXT)
        eglDestroyContext(s.egl_display, s.creator_ctx);
    eglTerminate(s.egl_display);
    gbm::close_device(s.gbm_device);
    return 0;
}

} // namespace shell::child_renderer
