#pragma once

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <cstdint>
#include <string_view>
#include <poll.h>
#include <unistd.h>

#include <shell/error.hpp>
#include <shell/log.hpp>
#include <shell/result.hpp>

namespace shell::egl
{

struct state
{
    void* display{};
    void* config{};
    void* surface{};
    void* context{};
    void* upload_context{};
};

inline auto choose_config(EGLDisplay display, EGLConfig* config_out) -> result<void>
{
    static const EGLint kAttribs[] = {EGL_SURFACE_TYPE,
                                      EGL_WINDOW_BIT,
                                      EGL_RENDERABLE_TYPE,
                                      EGL_OPENGL_ES2_BIT,
                                      EGL_RED_SIZE,
                                      8,
                                      EGL_GREEN_SIZE,
                                      8,
                                      EGL_BLUE_SIZE,
                                      8,
                                      EGL_ALPHA_SIZE,
                                      8,
                                      EGL_DEPTH_SIZE,
                                      16,
                                      EGL_NONE};

    EGLint num_configs = 0;
    EGLConfig configs[8] = {};
    if (!eglChooseConfig(display, kAttribs, configs, 8, &num_configs) ||
        num_configs == 0)
    {
        return unexpected(
            error{.code = error_code::egl, .message = "no suitable EGL config"});
    }
    *config_out = configs[0];
    return {};
}

inline auto create_context(EGLDisplay display, EGLConfig config, EGLContext share_with)
    -> EGLContext
{
    eglBindAPI(EGL_OPENGL_ES_API);
    static const EGLint kContextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    return eglCreateContext(display, config, share_with, kContextAttribs);
}

// Separate from choose_config() because EGL_PBUFFER_BIT-capable configs
// are commonly disjoint from EGL_WINDOW_BIT ones -- a secondary/headless
// engine's pbuffer doesn't need to match the primary's window config (it
// only exists to give eglMakeCurrent a legal surface to bind; the actual
// render target is a lazily-created FBO, see shell::secondary_engine).
inline auto choose_pbuffer_config(EGLDisplay display, EGLConfig* config_out)
    -> result<void>
{
    static const EGLint kAttribs[] = {EGL_SURFACE_TYPE,
                                      EGL_PBUFFER_BIT,
                                      EGL_RENDERABLE_TYPE,
                                      EGL_OPENGL_ES2_BIT,
                                      EGL_RED_SIZE,
                                      8,
                                      EGL_GREEN_SIZE,
                                      8,
                                      EGL_BLUE_SIZE,
                                      8,
                                      EGL_ALPHA_SIZE,
                                      8,
                                      EGL_NONE};

    EGLint num_configs = 0;
    EGLConfig configs[8] = {};
    if (!eglChooseConfig(display, kAttribs, configs, 8, &num_configs) ||
        num_configs == 0)
    {
        return unexpected(
            error{.code = error_code::egl, .message = "no suitable EGL pbuffer config"});
    }
    *config_out = configs[0];
    return {};
}

inline auto create_pbuffer_surface(EGLDisplay display, EGLConfig config,
                                   std::uint32_t width, std::uint32_t height)
    -> EGLSurface
{
    const EGLint attribs[] = {EGL_WIDTH, static_cast<EGLint>(width), EGL_HEIGHT,
                              static_cast<EGLint>(height), EGL_NONE};
    return eglCreatePbufferSurface(display, config, attribs);
}

inline auto has_extension(EGLDisplay display, const char* name) -> bool
{
    const char* extensions = eglQueryString(display, EGL_EXTENSIONS);
    return extensions != nullptr &&
           std::string_view(extensions).find(name) != std::string_view::npos;
}

// ---- EGL_ANDROID_native_fence_sync (Step 4: explicit cross-process GPU
// buffer synchronization) -- check has_extension(dpy,
// "EGL_ANDROID_native_fence_sync") before relying on either of these;
// despite the "ANDROID" name this is a standard, widely-supported Mesa
// extension on Linux, not Android-specific.
//
// These extension functions are declared in eglext.h (EGLAPI prototypes,
// visible since EGL_EGLEXT_PROTOTYPES is defined project-wide) but are
// NOT linkable symbols in -lEGL on this toolchain/libglvnd -- same
// situation texture.hpp's import_dmabuf already works around for
// glEGLImageTargetTexture2DOES. Resolved via eglGetProcAddress instead,
// matching that established pattern.
namespace detail
{
using PFNEGLCREATESYNCKHR = EGLSyncKHR (*)(EGLDisplay, EGLenum, const EGLint*);
using PFNEGLDESTROYSYNCKHR = EGLBoolean (*)(EGLDisplay, EGLSyncKHR);
using PFNEGLWAITSYNCKHR = EGLint (*)(EGLDisplay, EGLSyncKHR, EGLint);
using PFNEGLDUPNATIVEFENCEFDANDROID = EGLint (*)(EGLDisplay, EGLSyncKHR);

inline auto create_sync_khr() -> PFNEGLCREATESYNCKHR
{
    return reinterpret_cast<PFNEGLCREATESYNCKHR>(eglGetProcAddress("eglCreateSyncKHR"));
}
inline auto destroy_sync_khr() -> PFNEGLDESTROYSYNCKHR
{
    return reinterpret_cast<PFNEGLDESTROYSYNCKHR>(eglGetProcAddress("eglDestroySyncKHR"));
}
inline auto wait_sync_khr() -> PFNEGLWAITSYNCKHR
{
    return reinterpret_cast<PFNEGLWAITSYNCKHR>(eglGetProcAddress("eglWaitSyncKHR"));
}
inline auto dup_native_fence_fd_android() -> PFNEGLDUPNATIVEFENCEFDANDROID
{
    return reinterpret_cast<PFNEGLDUPNATIVEFENCEFDANDROID>(
        eglGetProcAddress("eglDupNativeFenceFDANDROID"));
}
} // namespace detail

// Creates a fence representing "all GPU work submitted on this context (on
// the calling thread, with its context current) up to this point." Returns
// a native sync_file fd the caller owns (must eventually close it, or hand
// ownership to shell::ipc, which closes after sendmsg), or -1 on failure.
// Coarse-grained by design: covers the whole frame's GPU work, not just
// operations touching one specific resource -- a safe over-approximation,
// never an under-wait.
inline auto create_fence(EGLDisplay display) -> int
{
    auto create_sync = detail::create_sync_khr();
    auto destroy_sync = detail::destroy_sync_khr();
    auto dup_fd = detail::dup_native_fence_fd_android();
    if (!create_sync || !destroy_sync || !dup_fd)
        return -1;
    EGLSyncKHR sync = create_sync(display, EGL_SYNC_NATIVE_FENCE_ANDROID, nullptr);
    if (sync == EGL_NO_SYNC_KHR)
        return -1;
    int fd = dup_fd(display, sync);
    destroy_sync(display, sync);
    return fd;
}

// Imports fence_fd (typically received from another process) and inserts
// a GPU-side wait into the current context's command stream -- cheap, does
// not block the calling thread/CPU. Always consumes fence_fd: the caller
// must not close it themselves after calling this, on success or failure
// (on success EGL takes ownership per the extension spec; on failure this
// function closes it itself, keeping the contract uniform either way).
inline auto wait_fence(EGLDisplay display, int fence_fd) -> void
{
    if (fence_fd < 0)
        return;
    auto create_sync = detail::create_sync_khr();
    auto destroy_sync = detail::destroy_sync_khr();
    auto wait_sync = detail::wait_sync_khr();
    if (!create_sync || !destroy_sync || !wait_sync)
    {
        close(fence_fd);
        return;
    }
    const EGLint attribs[] = {EGL_SYNC_NATIVE_FENCE_FD_ANDROID, fence_fd, EGL_NONE};
    EGLSyncKHR sync = create_sync(display, EGL_SYNC_NATIVE_FENCE_ANDROID, attribs);
    if (sync == EGL_NO_SYNC_KHR)
    {
        close(fence_fd);
        return;
    }
    wait_sync(display, sync, 0);
    destroy_sync(display, sync);
}

// CPU-blocking counterpart to wait_fence: polls the native fence fd itself
// (a Linux sync_file, which signals POLLIN when its fence has signaled) via
// a plain poll() -- no EGLDisplay/EGLContext involved at all. For a thread
// with no GL context of its own actively doing the later GPU read (e.g.
// shell::isolated_app's receiver thread -- that read happens later,
// asynchronously, on Flutter's own rasterizer thread/context, so there is
// no "current context" here to usefully insert a GPU-side wait into).
// Always consumes fence_fd (closes it) regardless of outcome. A negative
// fence_fd or a poll error/timeout is treated as "nothing to wait for" --
// same non-blocking default `wait_fence` uses for fence_fd < 0.
inline auto wait_fence_cpu(int fence_fd, int timeout_ms = -1) -> void
{
    if (fence_fd < 0)
        return;
    pollfd pfd{.fd = fence_fd, .events = POLLIN, .revents = 0};
    poll(&pfd, 1, timeout_ms);
    close(fence_fd);
}

} // namespace shell::egl
