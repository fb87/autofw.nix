#pragma once

#include <algorithm>
#include <array>
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

// One opened GPU/device node (/dev/dri/cardN), shared by every output that
// lives on it. Automotive SoCs commonly drive two displays (e.g. cluster +
// center stack) off one GPU -- `card` is what lets two `state` (output)
// instances share one fd/gbm_device/EGLDisplay safely instead of each
// independently opening the device and racing to eglTerminate() it.
struct card
{
    int fd{-1};
    gbm_device* gbm_dev{};
    void* egl_display{};   // EGLDisplay, created ONCE per card
    void* egl_config{};    // EGLConfig
    std::string device_path;
    std::vector<std::uint32_t> used_crtcs; // CRTC collision avoidance across sibling outputs
    int refcount{};        // number of live `state` (output) instances opened on this card

    // Move-only: a copy would duplicate the owned fd/gbm_dev/EGLDisplay,
    // and two copies independently reaching refcount==0 would double-close
    // them (double eglTerminate, double close(fd), ...).
    card() = default;
    card(const card&) = delete;
    card& operator=(const card&) = delete;
    card(card&&) = default;
    card& operator=(card&&) = default;
};

// Callbacks the engine installs to drive frames/resize/close. Input
// (pointer/keyboard/touch) is no longer wired per-output here -- a single
// process-wide `shell::input_router` owns the one libinput/seat context and
// routes decoded events straight to whichever engine a device is assigned
// to, avoiding the double-seat-grab hazard of each output independently
// calling libinput_udev_assign_seat("seat0").
struct callbacks
{
    std::function<void()> on_vsync;
    std::function<void(std::uint32_t, std::uint32_t)> on_resize;
    std::function<void()> on_close;
    std::function<void()> on_poll;
};

struct dumb_buf
{
    std::uint32_t handle{};
    std::uint32_t fb_id{};
    void* map{};
    std::size_t size{};
};

// One output = one connector/CRTC/mode/surface, bound to a shared `card`.
struct state
{
    card* owner{}; // non-owning; must outlive this

    std::uint32_t crtc_id{};
    std::uint32_t connector_id{};
    drmModeModeInfo mode{};

    gbm_surface* gbm_surf{};
    egl::state egl; // egl.display/config mirror owner's; egl.surface is per-output

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

    // Move-only: a copy would duplicate the owned gbm_surf/egl.surface, and
    // two copies independently reaching close_output() would double-free
    // them.
    state() = default;
    state(const state&) = delete;
    state& operator=(const state&) = delete;
    state(state&&) = default;
    state& operator=(state&&) = default;
};

// ---- device discovery -------------------------------------------------------
// Probes every /dev/dri/card* node for a connected connector with at least
// one mode -- i.e. the one actually driving a display -- and returns the
// first match. A machine with multiple GPUs (common with hybrid graphics, or
// a discrete GPU alongside an iGPU) may have the display wired to a card
// other than card0, which is otherwise an unexplained connect failure.
// Same probe as find_connected_device(), but returns every card with at
// least one connected connector instead of just the first -- lets
// multi-display auto-detection round-robin across distinct cards instead of
// always picking the same one.
inline auto find_connected_devices() -> std::vector<std::string>
{
    std::vector<std::string> candidates;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator("/dev/dri", ec))
    {
        if (entry.path().filename().string().rfind("card", 0) == 0)
            candidates.push_back(entry.path().string());
    }
    std::sort(candidates.begin(), candidates.end());

    std::vector<std::string> connected_devices;
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
            connected_devices.push_back(path);
    }
    return connected_devices;
}

inline auto find_connected_device() -> result<std::string>
{
    auto devices = find_connected_devices();
    if (!devices.empty())
        return devices.front();
    bool any_card_exists = false;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator("/dev/dri", ec))
        if (entry.path().filename().string().rfind("card", 0) == 0)
            any_card_exists = true;
    return unexpected(error{.code = error_code::drm,
                            .message = any_card_exists
                                           ? "no connected display on any "
                                             "/dev/dri/card* device"
                                           : "no /dev/dri/card* device found"});
}

// ---- card lifecycle ---------------------------------------------------------
// Opens the device node, creates the GBM device, and brings up EGL exactly
// once for this card. Every output subsequently opened on this card (via
// open_output) shares this fd/gbm_dev/EGLDisplay/EGLConfig.
inline auto open_card(const std::string& device_path) -> result<card>
{
    card c;
    c.device_path = device_path;

    int fd = open(device_path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return unexpected(error{.code = error_code::drm, .message = "open drm device"});
    drmVersionPtr ver = drmGetVersion(fd);
    if (!ver)
    {
        close(fd);
        return unexpected(error{.code = error_code::drm, .message = "drmGetVersion failed"});
    }
    drmFreeVersion(ver);
    c.fd = fd;

    c.gbm_dev = gbm_create_device(c.fd);
    if (!c.gbm_dev)
    {
        close(c.fd);
        return unexpected(error{.code = error_code::drm, .message = "gbm_create_device"});
    }

#ifdef SHELL_RENDERER_GL
    static const EGLAttrib kAttribs[] = {EGL_NONE};
    c.egl_display =
        eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, (void*)c.gbm_dev, kAttribs);
    if (c.egl_display == nullptr ||
        eglInitialize(static_cast<EGLDisplay>(c.egl_display), nullptr, nullptr) !=
            EGL_TRUE)
    {
        gbm_device_destroy(c.gbm_dev);
        close(c.fd);
        return unexpected(error{.code = error_code::egl, .message = "egl init"});
    }
    EGLConfig cfg{};
    auto err = egl::choose_config(static_cast<EGLDisplay>(c.egl_display), &cfg);
    if (!err)
    {
        eglTerminate(static_cast<EGLDisplay>(c.egl_display));
        gbm_device_destroy(c.gbm_dev);
        close(c.fd);
        return unexpected(err.error());
    }
    c.egl_config = cfg;
#endif
    return c;
}

// Only tears the card down once the last output on it has closed
// (refcount == 0) -- this is what makes it safe for two outputs to share a
// card: whichever output closes first just decrements the count, and
// eglTerminate()/gbm_device_destroy()/close() only fire once, from whichever
// output happens to be last.
inline auto close_card(card& c) -> void
{
    if (c.refcount > 0)
        return;
#ifdef SHELL_RENDERER_GL
    if (c.egl_display)
        eglTerminate(static_cast<EGLDisplay>(c.egl_display));
#endif
    if (c.gbm_dev)
        gbm_device_destroy(c.gbm_dev);
    if (c.fd >= 0)
        close(c.fd);
    c.fd = -1;
    c.gbm_dev = nullptr;
    c.egl_display = nullptr;
    c.egl_config = nullptr;
}

struct connector_info
{
    std::uint32_t connector_id{};
    bool connected{};
    int mode_count{};
};

// Enumerates every connector on this card (fixes the old setup_kms(), which
// stopped at the first connected connector and had no way to tell a caller
// about any others -- the thing that made shared-GPU multi-display
// impossible).
inline auto list_connectors(card& c) -> std::vector<connector_info>
{
    std::vector<connector_info> out;
    drmModeResPtr res = drmModeGetResources(c.fd);
    if (!res)
        return out;
    for (int i = 0; i < res->count_connectors; ++i)
    {
        drmModeConnector* conn = drmModeGetConnector(c.fd, res->connectors[i]);
        if (!conn)
            continue;
        out.push_back({static_cast<std::uint32_t>(conn->connector_id),
                       conn->connection == DRM_MODE_CONNECTED, conn->count_modes});
        drmModeFreeConnector(conn);
    }
    drmModeFreeResources(res);
    return out;
}

// ---- output lifecycle --------------------------------------------------------
// Binds one connector on an already-open card to a CRTC (avoiding any CRTC
// already claimed by a sibling output on the same card), and builds that
// output's own GBM surface + EGL window surface against the card's shared
// EGLDisplay/EGLConfig.
inline auto open_output(card& c, std::uint32_t connector_id) -> result<state>
{
    state s;
    s.owner = &c;

    drmModeResPtr res = drmModeGetResources(c.fd);
    if (!res)
        return unexpected(error{.code = error_code::drm, .message = "drmModeGetResources"});
    drmModeConnector* conn = drmModeGetConnector(c.fd, connector_id);
    if (!conn || conn->connection != DRM_MODE_CONNECTED || conn->count_modes == 0)
    {
        if (conn)
            drmModeFreeConnector(conn);
        drmModeFreeResources(res);
        return unexpected(
            error{.code = error_code::drm, .message = "connector not connected"});
    }

    s.connector_id = connector_id;
    drmModeModeInfo* best = &conn->modes[0];
    for (int m = 0; m < conn->count_modes; ++m)
        if (conn->modes[m].type & DRM_MODE_TYPE_PREFERRED)
            best = &conn->modes[m];
    s.mode = *best;
    s.width = best->hdisplay;
    s.height = best->vdisplay;

    bool found_crtc = false;
    if (conn->encoder_id)
    {
        drmModeEncoderPtr enc = drmModeGetEncoder(c.fd, conn->encoder_id);
        if (enc)
        {
            for (int crtc = 0; crtc < res->count_crtcs; ++crtc)
            {
                std::uint32_t candidate = res->crtcs[crtc];
                bool already_used =
                    std::find(c.used_crtcs.begin(), c.used_crtcs.end(), candidate) !=
                    c.used_crtcs.end();
                if ((enc->possible_crtcs & (1u << crtc)) && !already_used)
                {
                    s.crtc_id = candidate;
                    found_crtc = true;
                    break;
                }
            }
            drmModeFreeEncoder(enc);
        }
    }
    drmModeFreeConnector(conn);
    drmModeFreeResources(res);
    if (!found_crtc)
        return unexpected(
            error{.code = error_code::drm, .message = "no free crtc for connector"});

    s.gbm_surf = gbm_surface_create(c.gbm_dev, s.width, s.height, GBM_FORMAT_XRGB8888,
                                    GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    if (!s.gbm_surf)
        return unexpected(error{.code = error_code::drm, .message = "gbm_surface_create"});

    s.egl.display = c.egl_display;
    s.egl.config = c.egl_config;
#ifdef SHELL_RENDERER_GL
    s.egl.surface = eglCreatePlatformWindowSurface(
        static_cast<EGLDisplay>(c.egl_display), static_cast<EGLConfig>(c.egl_config),
        (void*)s.gbm_surf, nullptr);
    if (s.egl.surface == EGL_NO_SURFACE)
    {
        gbm_surface_destroy(s.gbm_surf);
        return unexpected(error{.code = error_code::egl, .message = "egl window surface"});
    }
#endif

    c.used_crtcs.push_back(s.crtc_id);
    c.refcount++;
    return s;
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
        pollfd pfd{};
        pfd.fd = s.owner->fd;
        pfd.events = POLLIN;
        int rc = poll(&pfd, 1, 100);
        if (rc > 0 && (pfd.revents & POLLIN))
        {
            // Handle DRM events, signalling vsync only on a completed
            // page flip (the display's physical refresh boundary).
            drmEventContext ev{};
            ev.version = DRM_EVENT_CONTEXT_VERSION;
            ev.page_flip_handler = on_flip_complete;
            drmHandleEvent(s.owner->fd, &ev);
        }
        if (s.cb.on_poll)
            s.cb.on_poll();
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
    int fd = s.owner->fd;
    if (s.fb_id)
        drmModeRmFB(fd, s.fb_id);
    s.fb_id = 0;
    if (drmModeAddFB(fd, gbm_bo_get_width(bo), gbm_bo_get_height(bo), 24, 32,
                     gbm_bo_get_stride(bo), handle, &s.fb_id) != 0)
        return false;
    if (drmModePageFlip(fd, s.crtc_id, s.fb_id, DRM_MODE_PAGE_FLIP_EVENT, nullptr) != 0)
    {
        drmModeSetCrtc(fd, s.crtc_id, s.fb_id, 0, 0, &s.connector_id, 1, &s.mode);
    }
    return true;
}

// Tears down whatever a (possibly partial) setup_dumb_buffers attempt
// created, resetting every field back to its zero default. Used both by
// close_output's real teardown and by setup_dumb_buffers' own failure
// paths (so a retry starts from a clean slate instead of a mix of set-up
// and leaked buffers, which previously left close_output's "already set
// up" guard -- s.dumb[0].fb_id != 0 -- lying about dumb[1]'s actual state).
inline auto teardown_dumb_buffer(int fd, dumb_buf& buf) -> void
{
    if (buf.map)
        munmap(buf.map, buf.size);
    if (buf.fb_id && fd >= 0)
        drmModeRmFB(fd, buf.fb_id);
    if (buf.handle && fd >= 0)
    {
        drm_mode_destroy_dumb req{};
        req.handle = buf.handle;
        drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &req);
    }
    buf = dumb_buf{};
}

// Tears this output down (surfaces/FBs/CRTC claim) and, once the owning
// card's refcount reaches zero (i.e. this was the last output on it), tears
// the shared card down too.
inline void close_output(state& s)
{
    int fd = s.owner ? s.owner->fd : -1;
    for (auto& buf : s.dumb)
        teardown_dumb_buffer(fd, buf);
    if (s.fb_id && fd >= 0)
        drmModeRmFB(fd, s.fb_id);
#ifdef SHELL_RENDERER_GL
    if (s.egl.surface)
        eglDestroySurface(static_cast<EGLDisplay>(s.egl.display),
                          static_cast<EGLSurface>(s.egl.surface));
#endif
    if (s.gbm_surf)
        gbm_surface_destroy(s.gbm_surf);
    if (s.owner)
    {
        auto& used = s.owner->used_crtcs;
        used.erase(std::remove(used.begin(), used.end(), s.crtc_id), used.end());
        if (s.owner->refcount > 0)
            s.owner->refcount--;
        close_card(*s.owner);
    }
}

// ---- software (CPU dumb buffer) presentation --------------------------------
inline auto setup_dumb_buffers(state& s) -> result<void>
{
    if (s.dumb[0].fb_id != 0)
        return {};
    int fd = s.owner->fd;
    for (auto& buf : s.dumb)
    {
        drm_mode_create_dumb req{};
        req.width = s.width;
        req.height = s.height;
        req.bpp = 32;
        if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &req) < 0)
        {
            for (auto& b : s.dumb)
                teardown_dumb_buffer(fd, b);
            return unexpected(
                error{.code = error_code::drm, .message = "create dumb buffer"});
        }
        buf.handle = req.handle;
        s.dumb_stride = req.pitch;
        buf.size = static_cast<std::size_t>(req.size);
        drm_mode_map_dumb mp{};
        mp.handle = buf.handle;
        if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mp) < 0)
        {
            for (auto& b : s.dumb)
                teardown_dumb_buffer(fd, b);
            return unexpected(
                error{.code = error_code::drm, .message = "map dumb buffer"});
        }
        buf.map =
            mmap(nullptr, buf.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mp.offset);
        if (buf.map == MAP_FAILED)
        {
            buf.map = nullptr; // teardown_dumb_buffer's munmap guard checks this, not MAP_FAILED
            for (auto& b : s.dumb)
                teardown_dumb_buffer(fd, b);
            return unexpected(
                error{.code = error_code::drm, .message = "mmap dumb buffer"});
        }
        if (drmModeAddFB(fd, s.width, s.height, 24, 32, s.dumb_stride, buf.handle,
                         &buf.fb_id) != 0)
        {
            for (auto& b : s.dumb)
                teardown_dumb_buffer(fd, b);
            return unexpected(error{.code = error_code::drm, .message = "add dumb fb"});
        }
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
    if (drmModePageFlip(s.owner->fd, s.crtc_id, buf.fb_id, DRM_MODE_PAGE_FLIP_EVENT,
                        nullptr) != 0)
    {
        drmModeSetCrtc(s.owner->fd, s.crtc_id, buf.fb_id, 0, 0, &s.connector_id, 1,
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
