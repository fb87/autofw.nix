#pragma once

// The isolated child process's entire post-exec program (Step 4: process-
// level isolation). Headless: renders Flutter content with no window
// surface at all (EGL_KHR_surfaceless_context against a GBM render node's
// EGL_PLATFORM_GBM_KHR display -- confirmed empirically that this display
// exposes zero EGL_PBUFFER_BIT configs, so the pbuffer trick
// shell::secondary_engine uses doesn't apply here; surfaceless is the only
// option and is supported).
//
// Reuses the headless-engine pattern from shell::secondary_engine
// (offscreen=true, the shared detail::fbo's lazily-created FBO) but does
// NOT share a GL context with anything -- there is no cross-process GL
// context sharing. Instead: Flutter renders into one fixed render_tex via
// the normal offscreen path; after each frame, a glCopyTexSubImage2D
// blits that into whichever of kBufferCount GBM/dma-buf-backed ring
// buffers is next in rotation, and the buffer's dma-buf fd (already
// exported once, at startup, during the handshake) is announced ready to
// the parent over the IPC socket, carrying a native fence the parent must
// wait on before sampling it.

#ifdef SHELL_RENDERER_GL

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>

#include <shell/backend.hpp>
#include <shell/egl.hpp>
#include <shell/engine.hpp>
#include <shell/gbm_export.hpp>
#include <shell/ipc.hpp>
#include <shell/log.hpp>
#include <shell/renderer.hpp>
#include <shell/texture.hpp>

namespace shell::child_renderer
{

inline constexpr std::uint32_t kBufferCount = 3;

struct config
{
    std::string_view assets;
    std::string_view icu_data;
    std::string_view aot_library;
    std::uint32_t width{1280};
    std::uint32_t height{720};
    double pixel_ratio{1.0};
    double fps{60.0}; // no real display to sync to -- timer-pumped vsync.
    std::string log_tag;
    int ipc_fd{-1};
};

struct ring_buffer
{
    gbm::buffer gbm{};
    texture::dmabuf_image image{}; // self-imported, see run()
    std::uint32_t gl_texture{};
    int pending_release_fence{-1}; // from the parent; waited-on before reuse
};

struct state
{
    config cfg;
    gbm::device gbm_device;
    EGLDisplay egl_display{EGL_NO_DISPLAY};
    EGLConfig egl_config{};
    // Bootstraps render_tex before engine::init() exists (offscreen_color_tex
    // must be set before init(), since detail::fbo attaches it on the
    // engine's first frame); the engine's own gl_ctx, created inside
    // init(), shares with this one.
    EGLContext creator_ctx{EGL_NO_CONTEXT};
    engine::state engine;
    backend::output out{};
    std::uint32_t render_tex{};
    std::array<ring_buffer, kBufferCount> ring;
    std::uint32_t next_index{};
    std::uint64_t frame_number{};
    std::atomic<bool> vsync_running{false};
    std::thread vsync_thread;
    std::atomic<bool> release_listener_running{false};
    std::thread release_listener_thread;
};

// The child's entire post-exec program. Blocks until the parent goes away
// (IPC socket EOF) or the process is killed. Returns a process exit code.
auto run(config cfg) -> int;

} // namespace shell::child_renderer

#include <shell/child_renderer_impl.hpp>

#endif // SHELL_RENDERER_GL
