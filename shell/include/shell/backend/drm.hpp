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
            drmEventContext ev{};
            drmHandleEvent(s.fd, &ev);
            // A page flip completed: signal one vsync to the engine.
            if (s.cb.on_vsync)
                s.cb.on_vsync();
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
    o.software_present.present = nullptr; // DRM software (M17) pending
    return o;
}
} // namespace output

} // namespace shell::backend::drm
