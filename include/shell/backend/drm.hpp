#pragma once

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

#include <libinput.h>
#include <libudev.h>
#include <linux/input-event-codes.h>

#include <shell/backend.hpp>
#include <shell/egl.hpp>
#include <shell/error.hpp>
#include <shell/event.hpp>
#include <shell/input.hpp>
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

// Callbacks the engine installs to drive frames/input. Vsync comes from
// page-flip timing; pointer/keyboard come from libinput (DRM has no
// compositor of its own to supply either).
struct callbacks
{
    std::function<void()> on_vsync;
    std::function<void(std::uint32_t, std::uint32_t)> on_resize;
    std::function<void()> on_close;
    std::function<void(const input::pointer_event&)> on_pointer;
    std::function<void(std::uint32_t, bool)> on_key;
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

    udev* udev_ctx{};
    libinput* li{};
    double pointer_x{};
    double pointer_y{};
    std::uint32_t buttons{};
    // Last known position per touch seat slot (libinput seat slots are
    // small, reused non-negative integers -- not a growing counter), for
    // LIBINPUT_EVENT_TOUCH_UP, which carries no coordinates of its own.
    // Indexed directly by slot; a fixed array keeps this allocation-free.
    static constexpr std::size_t kMaxTouchSlots = 10;
    std::array<std::pair<double, double>, kMaxTouchSlots> touch_pos{};

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

// ---- device discovery -------------------------------------------------------
// Probes every /dev/dri/card* node for a connected connector with at least
// one mode -- i.e. the one actually driving a display -- and returns the
// first match. A machine with multiple GPUs (common with hybrid graphics, or
// a discrete GPU alongside an iGPU) may have the display wired to a card
// other than card0, which is otherwise an unexplained connect failure.
inline auto find_connected_device() -> result<std::string>
{
    std::vector<std::string> candidates;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator("/dev/dri", ec))
    {
        if (entry.path().filename().string().rfind("card", 0) == 0)
            candidates.push_back(entry.path().string());
    }
    std::sort(candidates.begin(), candidates.end());

    for (const auto& path : candidates)
    {
        int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0)
            continue;
        drmModeResPtr res = drmModeGetResources(fd);
        bool connected = false;
        if (res)
        {
            for (int i = 0; i < res->count_connectors && !connected; ++i)
            {
                drmModeConnector* conn = drmModeGetConnector(fd, res->connectors[i]);
                if (conn)
                {
                    if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0)
                        connected = true;
                    drmModeFreeConnector(conn);
                }
            }
            drmModeFreeResources(res);
        }
        close(fd);
        if (connected)
            return path;
    }
    return unexpected(error{.code = error_code::drm,
                            .message = candidates.empty()
                                           ? "no /dev/dri/card* device found"
                                           : "no connected display on any "
                                             "/dev/dri/card* device"});
}

// ---- helpers ---------------------------------------------------------------
inline auto open_device(state& s) -> result<void>
{
    int fd = open(s.cfg.device.data(), O_RDWR | O_CLOEXEC);
    if (fd < 0)
    {
        return unexpected(error{.code = error_code::drm, .message = "open drm device"});
    }
    drmVersionPtr ver = drmGetVersion(fd);
    if (!ver)
    {
        close(fd);
        return unexpected(
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
        return unexpected(
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
        return unexpected(
            error{.code = error_code::drm, .message = "no connector/crtc"});
    }
    return {};
}

inline auto setup_gbm(state& s) -> result<void>
{
    s.gbm_dev = gbm_create_device(s.fd);
    if (!s.gbm_dev)
    {
        return unexpected(
            error{.code = error_code::drm, .message = "gbm_create_device"});
    }
    s.gbm_surf = gbm_surface_create(s.gbm_dev, s.width, s.height, GBM_FORMAT_XRGB8888,
                                    GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    if (!s.gbm_surf)
    {
        return unexpected(
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
        return unexpected(error{.code = error_code::egl, .message = "egl init"});
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
        return unexpected(
            error{.code = error_code::egl, .message = "egl window surface"});
    }
    return {};
#endif
}

// ---- libinput (M18: real input source, not just the shape) -----------------
namespace detail
{
inline int li_open_restricted(const char* path, int flags, void*)
{
    return open(path, flags);
}
inline void li_close_restricted(int fd, void*)
{
    close(fd);
}
inline const libinput_interface kLibinputInterface = {
    .open_restricted = li_open_restricted,
    .close_restricted = li_close_restricted,
};

inline auto button_mask(std::uint32_t button) -> std::uint32_t
{
    if (button == BTN_LEFT)
        return 0x1;
    if (button == BTN_RIGHT)
        return 0x2;
    if (button == BTN_MIDDLE)
        return 0x4;
    return 0;
}

// Routes libinput's own diagnostics through shell::log instead of its
// default raw-stderr handler. Priority stays at libinput's default
// (LIBINPUT_LOG_PRIORITY_ERROR) -- it's not verbose by default.
inline void li_log_handler(libinput*, libinput_log_priority priority,
                           const char* format, va_list args)
{
    char buf[512];
    std::vsnprintf(buf, sizeof(buf), format, args);
    const char* level = priority >= LIBINPUT_LOG_PRIORITY_ERROR  ? "error"
                        : priority >= LIBINPUT_LOG_PRIORITY_INFO ? "info"
                                                                 : "debug";
    log::info(std::string("libinput ") + level + ": " + buf);
}
} // namespace detail

inline auto setup_libinput(state& s) -> result<void>
{
    s.udev_ctx = udev_new();
    if (!s.udev_ctx)
    {
        return unexpected(error{.code = error_code::drm, .message = "udev_new"});
    }
    s.li = libinput_udev_create_context(&detail::kLibinputInterface, &s, s.udev_ctx);
    if (!s.li)
    {
        return unexpected(
            error{.code = error_code::drm, .message = "libinput_udev_create_context"});
    }
    libinput_log_set_handler(s.li, detail::li_log_handler);
    if (libinput_udev_assign_seat(s.li, "seat0") != 0)
    {
        return unexpected(
            error{.code = error_code::drm, .message = "libinput_udev_assign_seat"});
    }
    return {};
}

// Drains libinput's event queue, translating each event into the same
// shell::input::pointer_event / evdev-keycode shapes backend::wayland
// already feeds to engine::send_pointer/send_key.
inline void dispatch_libinput(state& s)
{
    if (!s.li)
        return;
    libinput_dispatch(s.li);
    libinput_event* ev;
    while ((ev = libinput_get_event(s.li)) != nullptr)
    {
        switch (libinput_event_get_type(ev))
        {
        case LIBINPUT_EVENT_DEVICE_ADDED:
        case LIBINPUT_EVENT_DEVICE_REMOVED:
        {
            const char* name = libinput_device_get_name(libinput_event_get_device(ev));
            log::info(std::string("libinput: device ") +
                      (libinput_event_get_type(ev) == LIBINPUT_EVENT_DEVICE_ADDED
                           ? "added: "
                           : "removed: ") +
                      (name ? name : "?"));
            break;
        }
        case LIBINPUT_EVENT_KEYBOARD_KEY:
        {
            auto* kev = libinput_event_get_keyboard_event(ev);
            std::uint32_t code = libinput_event_keyboard_get_key(kev);
            bool pressed = libinput_event_keyboard_get_key_state(kev) ==
                           LIBINPUT_KEY_STATE_PRESSED;
            if (s.cb.on_key)
                s.cb.on_key(code, pressed);
            break;
        }
        case LIBINPUT_EVENT_POINTER_MOTION:
        {
            auto* pev = libinput_event_get_pointer_event(ev);
            s.pointer_x = std::clamp(s.pointer_x + libinput_event_pointer_get_dx(pev),
                                     0.0, static_cast<double>(s.width));
            s.pointer_y = std::clamp(s.pointer_y + libinput_event_pointer_get_dy(pev),
                                     0.0, static_cast<double>(s.height));
            if (s.cb.on_pointer)
            {
                s.cb.on_pointer(input::pointer_event{
                    .phase = s.buttons ? input::pointer_phase::move
                                       : input::pointer_phase::hover,
                    .x = s.pointer_x,
                    .y = s.pointer_y,
                    .buttons = s.buttons});
            }
            break;
        }
        case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE:
        {
            auto* pev = libinput_event_get_pointer_event(ev);
            s.pointer_x =
                libinput_event_pointer_get_absolute_x_transformed(pev, s.width);
            s.pointer_y =
                libinput_event_pointer_get_absolute_y_transformed(pev, s.height);
            if (s.cb.on_pointer)
            {
                s.cb.on_pointer(input::pointer_event{
                    .phase = s.buttons ? input::pointer_phase::move
                                       : input::pointer_phase::hover,
                    .x = s.pointer_x,
                    .y = s.pointer_y,
                    .buttons = s.buttons});
            }
            break;
        }
        case LIBINPUT_EVENT_POINTER_BUTTON:
        {
            auto* pev = libinput_event_get_pointer_event(ev);
            std::uint32_t mask =
                detail::button_mask(libinput_event_pointer_get_button(pev));
            if (mask)
            {
                bool pressed = libinput_event_pointer_get_button_state(pev) ==
                               LIBINPUT_BUTTON_STATE_PRESSED;
                if (pressed)
                    s.buttons |= mask;
                else
                    s.buttons &= ~mask;
                if (s.cb.on_pointer)
                {
                    s.cb.on_pointer(input::pointer_event{
                        .phase = pressed ? input::pointer_phase::down
                                         : input::pointer_phase::up,
                        .x = s.pointer_x,
                        .y = s.pointer_y,
                        .buttons = s.buttons});
                }
            }
            break;
        }
        case LIBINPUT_EVENT_TOUCH_DOWN:
        case LIBINPUT_EVENT_TOUCH_MOTION:
        {
            auto* tev = libinput_event_get_touch_event(ev);
            std::int32_t slot = libinput_event_touch_get_seat_slot(tev);
            double x = libinput_event_touch_get_x_transformed(tev, s.width);
            double y = libinput_event_touch_get_y_transformed(tev, s.height);
            if (slot >= 0 && static_cast<std::size_t>(slot) < state::kMaxTouchSlots)
                s.touch_pos[static_cast<std::size_t>(slot)] = {x, y};
            if (s.cb.on_pointer)
            {
                s.cb.on_pointer(input::pointer_event{
                    .device = static_cast<std::uint32_t>(slot),
                    .phase = libinput_event_get_type(ev) == LIBINPUT_EVENT_TOUCH_DOWN
                                 ? input::pointer_phase::down
                                 : input::pointer_phase::move,
                    .x = x,
                    .y = y,
                    .kind = input::device_type::touch,
                    .buttons = 0x1});
            }
            break;
        }
        case LIBINPUT_EVENT_TOUCH_UP:
        {
            auto* tev = libinput_event_get_touch_event(ev);
            std::int32_t slot = libinput_event_touch_get_seat_slot(tev);
            auto [x, y] =
                (slot >= 0 && static_cast<std::size_t>(slot) < state::kMaxTouchSlots)
                    ? s.touch_pos[static_cast<std::size_t>(slot)]
                    : std::pair<double, double>{};
            if (s.cb.on_pointer)
            {
                s.cb.on_pointer(
                    input::pointer_event{.device = static_cast<std::uint32_t>(slot),
                                         .phase = input::pointer_phase::up,
                                         .x = x,
                                         .y = y,
                                         .kind = input::device_type::touch});
            }
            break;
        }
        default:
            break;
        }
        libinput_event_destroy(ev);
    }
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
    if (auto err = setup_libinput(s); !err)
        return err;
    return {};
}

// Flip-complete handler: the compositor has scanned out the presented buffer;
// that physical refresh is the vsync the engine should follow (M19).
inline void on_flip_complete(int, unsigned int, unsigned int, unsigned int, void* d)
{
    auto* s = static_cast<state*>(d);
    if (s->cb.on_vsync)
        s->cb.on_vsync();
}

inline void run(state& s)
{
    s.running = true;
    // Bootstrap vsync so the engine starts drawing its first frame.
    if (s.cb.on_vsync)
        s.cb.on_vsync();
    while (s.running)
    {
        pollfd pfds[2] = {};
        pfds[0].fd = s.fd;
        pfds[0].events = POLLIN;
        pfds[1].fd = s.li ? libinput_get_fd(s.li) : -1;
        pfds[1].events = POLLIN;
        int rc = poll(pfds, 2, 100);
        if (rc > 0 && (pfds[0].revents & POLLIN))
        {
            // Handle DRM events, signalling vsync only on a completed
            // page flip (the display's physical refresh boundary).
            drmEventContext ev{};
            ev.version = DRM_EVENT_CONTEXT_VERSION;
            ev.page_flip_handler = on_flip_complete;
            drmHandleEvent(s.fd, &ev);
        }
        if (rc > 0 && (pfds[1].revents & POLLIN))
            dispatch_libinput(s);
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
        if (buf.map)
            munmap(buf.map, buf.size);
        if (buf.fb_id)
            drmModeRmFB(s.fd, buf.fb_id);
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
    if (s.li)
        libinput_unref(s.li);
    if (s.udev_ctx)
        udev_unref(s.udev_ctx);
    if (s.fd >= 0)
        close(s.fd);
}

// ---- software (CPU dumb buffer) presentation --------------------------------
inline auto setup_dumb_buffers(state& s) -> result<void>
{
    if (s.dumb[0].fb_id != 0)
        return {};
    for (auto& buf : s.dumb)
    {
        drm_mode_create_dumb req{};
        req.width = s.width;
        req.height = s.height;
        req.bpp = 32;
        if (drmIoctl(s.fd, DRM_IOCTL_MODE_CREATE_DUMB, &req) < 0)
            return unexpected(
                error{.code = error_code::drm, .message = "create dumb buffer"});
        buf.handle = req.handle;
        s.dumb_stride = req.pitch;
        buf.size = static_cast<std::size_t>(req.size);
        drm_mode_map_dumb mp{};
        mp.handle = buf.handle;
        if (drmIoctl(s.fd, DRM_IOCTL_MODE_MAP_DUMB, &mp) < 0)
            return unexpected(
                error{.code = error_code::drm, .message = "map dumb buffer"});
        buf.map = mmap(nullptr, buf.size, PROT_READ | PROT_WRITE, MAP_SHARED, s.fd,
                       mp.offset);
        if (buf.map == MAP_FAILED)
            return unexpected(
                error{.code = error_code::drm, .message = "mmap dumb buffer"});
        if (drmModeAddFB(s.fd, s.width, s.height, 24, 32, s.dumb_stride, buf.handle,
                         &buf.fb_id) != 0)
            return unexpected(error{.code = error_code::drm, .message = "add dumb fb"});
    }
    return {};
}

inline bool software_present(state& s, const renderer::software::buffer& frame)
{
    auto& buf = s.dumb[s.dumb_index & 1];
    if (buf.map == nullptr)
    {
        if (!setup_dumb_buffers(s))
            return false;
        buf = s.dumb[0];
        s.dumb_index = 1;
    }
    auto* dst = static_cast<std::uint8_t*>(buf.map);
    const auto* src = reinterpret_cast<const std::uint8_t*>(frame.pixels.data());
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
        drmModeSetCrtc(s.fd, s.crtc_id, buf.fb_id, 0, 0, &s.connector_id, 1, &s.mode);
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
} // namespace

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
