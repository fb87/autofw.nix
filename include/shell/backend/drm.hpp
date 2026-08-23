#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include <EGL/egl.h>
#include <poll.h>
#include <sys/errno.h>
#include <time.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <shell/backend.hpp>
#include <shell/egl.hpp>
#include <shell/error.hpp>
#include <shell/event.hpp>
#include <shell/log.hpp>
#include <shell/renderer.hpp>
#include <shell/result.hpp>
#include <shell/software.hpp>

namespace shell::backend::drm
{

struct config
{
    std::string_view device{"/dev/dri/card0"};
    std::optional<std::string_view> connector;
    std::uint32_t width{};
    std::uint32_t height{};
};

// Callbacks the engine installs to drive frames/input (DRM has no built-in
// input; vsync comes from page-filp timing).
struct callbacks
{
    std::function<void()> on_vsync;
    std::function<void(std::uint32_t, std::uint32_t)> on_resize;
    std::function<void()> on_close;
};

struct dumb_buf
{
    std::uint32_t handle{};
    std::uint32_t fb_id{};
    void* map{};
    std::size_t size{};
};

struct state
{
    config cfg;

    int fd{-1};
    std::uint32_t crtc_id{};
    std::uint32_t connector_id{};
    drmModeModeInfo mode{};

    gbm_device* gbm_dev{};
    gbm_surface* gbm_surf{};

    egl::state egl;

    std::uint32_t width{};
    std::uint32_t height{};
    double dpi{1.0};

    bool running{};
    bool configured{};
    std::uint32_t fb_id{};

    // Software (CPU) scanout buffers: double-buffered dumb buffers.
    std::uint32_t dumb_stride{};
    dumb_buf dumb[2];
    std::uint32_t dumb_index{};

    callbacks cb;
};

// ---- helpers ---------------------------------------------------------------
inline auto open_device(state& s) -> result<void>
{
    int fd = open(s.cfg.device.data(), O_RDWR | O_CLOEXEC);
    if (fd < 0)
    {
        return std::unexpected(
            error{.code = error_code::drm, .message = "open drm device"});
    }
    drmVersionPtr ver = drmGetVersion(fd);
    if (!ver)
    {
        close(fd);
        return std::unexpected(
            error{.code = error_code::drm, .message = "drmGetVersion failed"});
    }
    drmFreeVersion(ver);
    s.fd = fd;
    return {};
}

inline auto setup_kms(state& s) -> result<void>
{
    drmModeResPtr res = drmModeGetResources(s.fd);
    if (!res)
    {
        return std::unexpected(
            error{.code = error_code::drm, .message = "drmModeGetResources"});
    }
    bool found = false;
    for (int i = 0; i < res->count_connectors; ++i)
    {
        drmModeConnector* conn = drmModeGetConnector(s.fd, res->connectors[i]);
        if (!conn)
            continue;
        if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0)
        {
            s.connector_id = conn->connector_id;
            drmModeModeInfo* best = &conn->modes[0];
            for (int m = 0; m < conn->count_modes; ++m)
                if (conn->modes[m].type & DRM_MODE_TYPE_PREFERRED)
                    best = &conn->modes[m];
            s.mode = *best;
            s.width = best->hdisplay;
            s.height = best->vdisplay;
            for (int crtc = 0; crtc < res->count_crtcs; ++crtc)
            {
                if (conn->encoder_id)
                {
                    drmModeEncoderPtr enc = drmModeGetEncoder(s.fd, conn->encoder_id);
                    if (enc && (enc->possible_crtcs & (1 << crtc)))
                        s.crtc_id = res->crtcs[crtc];
                    if (enc)
                        drmModeFreeEncoder(enc);
                }
            }
            drmModeFreeConnector(conn);
            found = true;
            break;
        }
        drmModeFreeConnector(conn);
    }
    drmModeFreeResources(res);
    if (!found || !s.crtc_id)
    {
        return std::unexpected(
            error{.code = error_code::drm, .message = "no connector/crtc"});
    }
    return {};
}

inline auto setup_gbm(state& s) -> result<void>
{
    s.gbm_dev = gbm_create_device(s.fd);
    if (!s.gbm_dev)
    {
        return std::unexpected(
            error{.code = error_code::drm, .message = "gbm_create_device"});
    }
    s.gbm_surf = gbm_surface_create(s.gbm_dev, s.width, s.height, GBM_FORMAT_XRGB8888,
                                    GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    if (!s.gbm_surf)
    {
        return std::unexpected(
            error{.code = error_code::drm, .message = "gbm_surface_create"});
    }
    return {};
}

inline auto setup_egl(state& s) -> result<void>
{
#ifndef SHELL_RENDERER_GL
    (void)s;
    return {};
#else
    static const EGLAttrib kAttribs[] = {EGL_NONE};
    s.egl.display =
        eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, (void*)s.gbm_dev, kAttribs);
    if (s.egl.display == nullptr ||
        eglInitialize(static_cast<EGLDisplay>(s.egl.display), nullptr, nullptr) !=
            EGL_TRUE)
    {
        return std::unexpected(error{.code = error_code::egl, .message = "egl init"});
    }
    auto err = egl::choose_config(static_cast<EGLDisplay>(s.egl.display),
                                  static_cast<EGLConfig*>(&s.egl.config));
    if (!err)
        return err;
    s.egl.surface = eglCreatePlatformWindowSurface(
        static_cast<EGLDisplay>(s.egl.display), static_cast<EGLConfig>(s.egl.config),
        (void*)s.gbm_surf, nullptr);
    if (s.egl.surface == EGL_NO_SURFACE)
    {
        return std::unexpected(
            error{.code = error_code::egl, .message = "egl window surface"});
    }
    return {};
#endif
}

// ---- backend API -----------------------------------------------------------
inline auto create(const config& cfg) -> state
{
    state s;
    s.cfg = cfg;
    return s;
}

inline auto connect(state& s) -> result<void>
{
    if (auto err = open_device(s); !err)
        return err;
    if (auto err = setup_kms(s); !err)
        return err;
    if (auto err = setup_gbm(s); !err)
        return err;
    if (auto err = setup_egl(s); !err)
        return err;
    return {};
}

// Flip-complete handler: the compositor has scanned out the presented buffer;
// that physical refresh is the vsync the engine should follow (M19).
inline void on_flip_complete(int, unsigned int, unsigned int, unsigned int, void* d)
{
    auto* s = static_cast<state*>(d);
    if (s->cb.on_vsync) s->cb.on_vsync();
}

inline void run(state& s)
{
    s.running = true;
    // Bootstrap vsync so the engine starts drawing its first frame.
    if (s.cb.on_vsync)
        s.cb.on_vsync();
    while (s.running)
    {
        pollfd pfd{};
        pfd.fd = s.fd;
        pfd.events = POLLIN;
        int rc = poll(&pfd, 1, 100);
        if (rc > 0)
        {
            // Handle DRM events, signalling vsync only on a completed
            // page flip (the display's physical refresh boundary).
            drmEventContext ev{};
            ev.version = DRM_EVENT_CONTEXT_VERSION;
            ev.page_flip_handler = on_flip_complete;
            drmHandleEvent(s.fd, &ev);
        }
    }
}

inline auto stop(state& s) -> void
{
    s.running = false;
}

inline auto swap(state& s) -> bool
{
    EGLDisplay dpy = static_cast<EGLDisplay>(s.egl.display);
#ifdef SHELL_RENDERER_GL
    if (eglSwapBuffers(dpy, static_cast<EGLSurface>(s.egl.surface)) != EGL_TRUE)
        return false;
#endif
    gbm_bo* bo = gbm_surface_lock_front_buffer(s.gbm_surf);
    if (!bo)
        return false;
    std::uint32_t handle = gbm_bo_get_handle(bo).u32;
    if (s.fb_id)
        drmModeRmFB(s.fd, s.fb_id);
    s.fb_id = 0;
    if (drmModeAddFB(s.fd, gbm_bo_get_width(bo), gbm_bo_get_height(bo), 24, 32,
                     gbm_bo_get_stride(bo), handle, &s.fb_id) != 0)
        return false;
    if (drmModePageFlip(s.fd, s.crtc_id, s.fb_id, DRM_MODE_PAGE_FLIP_EVENT, nullptr) !=
        0)
    {
        drmModeSetCrtc(s.fd, s.crtc_id, s.fb_id, 0, 0, &s.connector_id, 1, &s.mode);
    }
    return true;
}

inline void destroy(state& s)
{
    // Release software dumb buffers.
    for (auto& buf : s.dumb)
    {
        if (buf.map) munmap(buf.map, buf.size);
        if (buf.fb_id) drmModeRmFB(s.fd, buf.fb_id);
        if (buf.handle)
        {
            drm_mode_destroy_dumb req{};
            req.handle = buf.handle;
            drmIoctl(s.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &req);
        }
    }
    if (s.fb_id)
        drmModeRmFB(s.fd, s.fb_id);
#ifdef SHELL_RENDERER_GL
    if (s.egl.surface)
        eglDestroySurface(static_cast<EGLDisplay>(s.egl.display),
                          static_cast<EGLSurface>(s.egl.surface));
#endif
    if (s.gbm_surf)
        gbm_surface_destroy(s.gbm_surf);
#ifdef SHELL_RENDERER_GL
    if (s.egl.display)
        eglTerminate(static_cast<EGLDisplay>(s.egl.display));
#endif
    if (s.gbm_dev)
        gbm_device_destroy(s.gbm_dev);
    if (s.fd >= 0)
        close(s.fd);
}

// ---- software (CPU dumb buffer) presentation --------------------------------
inline auto setup_dumb_buffers(state& s) -> result<void>
{
    if (s.dumb[0].fb_id != 0) return {};
    for (auto& buf : s.dumb)
    {
        drm_mode_create_dumb req{};
        req.width = s.width;
        req.height = s.height;
        req.bpp = 32;
        if (drmIoctl(s.fd, DRM_IOCTL_MODE_CREATE_DUMB, &req) < 0)
            return std::unexpected(error{.code = error_code::drm,
                                         .message = "create dumb buffer"});
        buf.handle = req.handle;
        s.dumb_stride = req.pitch;
        buf.size = static_cast<std::size_t>(req.size);
        drm_mode_map_dumb mp{};
        mp.handle = buf.handle;
        if (drmIoctl(s.fd, DRM_IOCTL_MODE_MAP_DUMB, &mp) < 0)
            return std::unexpected(error{.code = error_code::drm,
                                         .message = "map dumb buffer"});
        buf.map = mmap(nullptr, buf.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                       s.fd, mp.offset);
        if (buf.map == MAP_FAILED)
            return std::unexpected(error{.code = error_code::drm,
                                         .message = "mmap dumb buffer"});
        if (drmModeAddFB(s.fd, s.width, s.height, 24, 32, s.dumb_stride,
                         buf.handle, &buf.fb_id) != 0)
            return std::unexpected(error{.code = error_code::drm,
                                         .message = "add dumb fb"});
    }
    return {};
}

inline bool software_present(state& s, const renderer::software::buffer& frame)
{
    auto& buf = s.dumb[s.dumb_index & 1];
    if (buf.map == nullptr)
    {
        if (!setup_dumb_buffers(s)) return false;
        buf = s.dumb[0];
        s.dumb_index = 1;
    }
    auto* dst = static_cast<std::uint8_t*>(buf.map);
    const auto* src =
        reinterpret_cast<const std::uint8_t*>(frame.pixels.data());
    std::uint32_t copy_w = frame.width < s.width ? frame.width : s.width;
    std::uint32_t copy_h = frame.height < s.height ? frame.height : s.height;
    for (std::uint32_t y = 0; y < copy_h; ++y)
    {
        std::memcpy(dst + static_cast<std::size_t>(y) * s.dumb_stride,
                    src + static_cast<std::size_t>(y) * frame.stride,
                    static_cast<std::size_t>(copy_w) * 4);
    }
    if (drmModePageFlip(s.fd, s.crtc_id, buf.fb_id, DRM_MODE_PAGE_FLIP_EVENT,
                        nullptr) != 0)
    {
        drmModeSetCrtc(s.fd, s.crtc_id, buf.fb_id, 0, 0, &s.connector_id, 1,
                       &s.mode);
    }
    s.dumb_index++;
    // Signalled via the page-flip event in run().
    return true;
}

namespace
{
inline bool drm_software_present_cb(void* d, const renderer::software::buffer& frame)
{
    return software_present(*static_cast<shell::backend::drm::state*>(d), frame);
}
}

// ---- generic output adapter ------------------------------------------------
namespace output
{
inline bool present(void* d)
{
    return shell::backend::drm::swap(*static_cast<shell::backend::drm::state*>(d));
}
inline void stop(void* d)
{
    shell::backend::drm::stop(*static_cast<shell::backend::drm::state*>(d));
}
inline auto take(shell::backend::drm::state& s) -> shell::backend::output
{
    shell::backend::output o{};
    o.state = &s;
    o.egl_display = s.egl.display;
    o.egl_config = s.egl.config;
    o.egl_surface = s.egl.surface;
    o.width = s.width;
    o.height = s.height;
    o.pixel_ratio = s.dpi;
    o.present = &present;
    o.stop = &stop;
    o.software_present.output = &s;
    o.software_present.present = &drm_software_present_cb;
    return o;
}
} // namespace output

} // namespace shell::backend::drm
