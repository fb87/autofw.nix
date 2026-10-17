#define _GNU_SOURCE 1

#pragma once

// WL_EGL_PLATFORM must be defined (see CMakeLists.txt) before the *first*
// <EGL/egl.h> is processed anywhere in the translation unit -- its include
// guard means a #define here is too late, since shell/egl.hpp (pulled in
// earlier via shell/engine.hpp) already included it.

#include <cstdint>
#include <string_view>

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <sys/errno.h>
#include <sys/mman.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <thread>

#include <wayland-client.h>
#include <wayland-egl.h>

#include <shell/egl.hpp>
#include <shell/error.hpp>
#include <shell/event.hpp>
#include <shell/input.hpp>
#include <shell/log.hpp>
#include <shell/result.hpp>

#include <shell/protocol/xdg-shell-client-protocol.h>

namespace shell::backend::wayland
{

struct config
{
    std::string_view display;
    std::uint32_t width{1280};
    std::uint32_t height{720};
};

struct callbacks
{
    std::function<void()> on_vsync;
    std::function<void(input::pointer_event)> on_pointer;
    std::function<void(std::uint32_t, bool)> on_key;
    std::function<void(int, std::uint32_t)> on_keymap;
    std::function<void(std::uint32_t, std::uint32_t)> on_resize;
    std::function<void()> on_close;
    // Called each event-loop iteration on the platform thread; used by the
    // shell to marshal injected key events (--test-input) onto that thread.
    std::function<void()> on_poll;
};

struct state
{
    config cfg;
    wl_display* display{};
    wl_registry* registry{};
    wl_compositor* compositor{};
    wl_surface* surface{};
    wl_seat* seat{};
    wl_pointer* pointer{};
    wl_keyboard* keyboard{};
    wl_callback* frame_cb{};
    wl_shm* shm{};
    wl_buffer* shm_buffer{};
    std::size_t shm_pool_size{};
    wl_egl_window* wl_window{};
    xdg_wm_base* wm_base{};
    xdg_surface* xs{};
    xdg_toplevel* xt{};
    egl::state egl;
    std::uint32_t width{1280};
    std::uint32_t height{720};
    double dpi{1.0};
    bool running{};
    bool configured{};
    bool pointer_added{};
    bool frame_ready{};     // a frame callback has fired (ready for next)
    bool frame_in_flight{}; // a vsync was handed out, awaiting a present
    bool presented_once{};
    std::uint32_t buttons{};
    double pointer_x{};
    double pointer_y{};
    callbacks cb;
};

namespace detail
{
inline void reg_global(void* d, wl_registry* reg, std::uint32_t name, const char* iface,
                       std::uint32_t version);
inline void reg_remove(void*, wl_registry*, std::uint32_t)
{
}
inline void wm_ping(void*, xdg_wm_base* w, std::uint32_t serial)
{
    xdg_wm_base_pong(w, serial);
}
inline void xdg_configure(void* d, xdg_surface* s, std::uint32_t serial)
{
    xdg_surface_ack_configure(s, serial);
    static_cast<state*>(d)->configured = true;
}
inline void top_configure(void* d, xdg_toplevel*, std::int32_t w, std::int32_t h,
                          wl_array*);
inline void top_close(void* d, xdg_toplevel*);
inline void seat_caps(void* d, wl_seat* seat, std::uint32_t caps);
inline void seat_name(void*, wl_seat*, const char*)
{
}
inline void p_enter(void* d, wl_pointer*, std::uint32_t, wl_surface*, wl_fixed_t x,
                    wl_fixed_t y);
inline void p_leave(void* d, wl_pointer*, std::uint32_t, wl_surface*);
inline void p_motion(void* d, wl_pointer*, std::uint32_t, wl_fixed_t x, wl_fixed_t y);
inline void p_button(void* d, wl_pointer*, std::uint32_t, std::uint32_t,
                     std::uint32_t b, std::uint32_t s);
inline void p_axis(void* d, wl_pointer*, std::uint32_t, std::uint32_t axis,
                   wl_fixed_t v);
inline void p_axis_source(void*, wl_pointer*, std::uint32_t)
{
}
inline void p_axis_stop(void*, wl_pointer*, std::uint32_t, std::uint32_t)
{
}
inline void p_axis_discrete(void*, wl_pointer*, std::uint32_t, std::int32_t)
{
}
inline void p_frame(void*, wl_pointer*)
{
}
inline void k_keymap(void* d, wl_keyboard*, std::uint32_t f, std::int32_t fd,
                     std::uint32_t size);
inline void k_enter(void*, wl_keyboard*, std::uint32_t, wl_surface*, wl_array*)
{
}
inline void k_leave(void*, wl_keyboard*, std::uint32_t, wl_surface*)
{
}
inline void k_key(void* d, wl_keyboard*, std::uint32_t, std::uint32_t,
                  std::uint32_t key, std::uint32_t s);
inline void k_mods(void*, wl_keyboard*, std::uint32_t, std::uint32_t, std::uint32_t,
                   std::uint32_t, std::uint32_t)
{
}
inline void k_repeat(void*, wl_keyboard*, std::int32_t, std::int32_t)
{
}
inline void frame_done(void* d, wl_callback* cb, std::uint32_t)
{
    wl_callback_destroy(cb);
    auto* s = static_cast<state*>(d);
    // The compositor has displayed the previous buffer; the FrameClife says we
    // may submit the next one. Signal one vsync to the engine (consumed by run).
    if (s->frame_cb == cb)
        s->frame_cb = nullptr;
    // The compositor has displayed the previous buffer; a new frame may be
    // submitted now.
    s->frame_ready = true;
    s->frame_in_flight = false;
}
} // namespace detail

inline const wl_registry_listener kRegistry = {.global = detail::reg_global,
                                               .global_remove = detail::reg_remove};
inline const xdg_wm_base_listener kWmBase = {.ping = detail::wm_ping};
inline const xdg_surface_listener kXdgSurface = {.configure = detail::xdg_configure};
inline const xdg_toplevel_listener kToplevel = {.configure = detail::top_configure,
                                                .close = detail::top_close};
inline const wl_seat_listener kSeat = {.capabilities = detail::seat_caps,
                                       .name = detail::seat_name};
inline const wl_pointer_listener kPointer = {.enter = detail::p_enter,
                                             .leave = detail::p_leave,
                                             .motion = detail::p_motion,
                                             .button = detail::p_button,
                                             .axis = detail::p_axis,
                                             .frame = detail::p_frame,
                                             .axis_source = detail::p_axis_source,
                                             .axis_stop = detail::p_axis_stop,
                                             .axis_discrete = detail::p_axis_discrete};
inline const wl_keyboard_listener kKeyboard = {.keymap = detail::k_keymap,
                                               .enter = detail::k_enter,
                                               .leave = detail::k_leave,
                                               .key = detail::k_key,
                                               .modifiers = detail::k_mods,
                                               .repeat_info = detail::k_repeat};
inline const wl_callback_listener kFrame = {.done = detail::frame_done};

} // namespace shell::backend::wayland

// ---- implementations --------------------------------------------------------
namespace shell::backend::wayland
{
namespace detail
{
inline void reg_global(void* d, wl_registry* reg, std::uint32_t name, const char* iface,
                       std::uint32_t version)
{
    auto* s = static_cast<state*>(d);
    if (std::strcmp(iface, wl_compositor_interface.name) == 0)
    {
        s->compositor = static_cast<wl_compositor*>(
            wl_registry_bind(reg, name, &wl_compositor_interface, 4));
    }
    else if (std::strcmp(iface, xdg_wm_base_interface.name) == 0)
    {
        s->wm_base = static_cast<xdg_wm_base*>(
            wl_registry_bind(reg, name, &xdg_wm_base_interface, 1));
        xdg_wm_base_add_listener(s->wm_base, &kWmBase, s);
    }
    else if (std::strcmp(iface, wl_seat_interface.name) == 0)
    {
        s->seat =
            static_cast<wl_seat*>(wl_registry_bind(reg, name, &wl_seat_interface, 7));
        wl_seat_add_listener(s->seat, &kSeat, s);
    }
    else if (std::strcmp(iface, wl_shm_interface.name) == 0)
    {
        s->shm =
            static_cast<wl_shm*>(wl_registry_bind(reg, name, &wl_shm_interface, 1));
    }
    (void)version;
}
inline void top_configure(void* d, xdg_toplevel*, std::int32_t w, std::int32_t h,
                          wl_array*)
{
    auto* s = static_cast<state*>(d);
    if (s->wl_window)
        wl_egl_window_resize(s->wl_window, w, h, 0, 0);
    if (w > 0 && h > 0)
    {
        s->width = static_cast<std::uint32_t>(w);
        s->height = static_cast<std::uint32_t>(h);
        if (s->cb.on_resize)
            s->cb.on_resize(s->width, s->height);
    }
}
inline void top_close(void* d, xdg_toplevel*)
{
    if (auto* s = static_cast<state*>(d); s->cb.on_close)
        s->cb.on_close();
}
inline void seat_caps(void* d, wl_seat* seat, std::uint32_t caps)
{
    auto* s = static_cast<state*>(d);
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !s->pointer)
    {
        s->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(s->pointer, &kPointer, s);
    }
    else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && s->pointer)
    {
        wl_pointer_destroy(s->pointer);
        s->pointer = nullptr;
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !s->keyboard)
    {
        s->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(s->keyboard, &kKeyboard, s);
    }
    else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && s->keyboard)
    {
        wl_keyboard_destroy(s->keyboard);
        s->keyboard = nullptr;
    }
}
inline void p_enter(void* d, wl_pointer*, std::uint32_t, wl_surface* sx, wl_fixed_t px,
                    wl_fixed_t py)
{
    auto* s = static_cast<state*>(d);
    s->pointer_added = true;
    s->pointer_x = wl_fixed_to_double(px);
    s->pointer_y = wl_fixed_to_double(py);
    if (s->cb.on_pointer)
    {
        s->cb.on_pointer(input::pointer_event{.phase = input::pointer_phase::add,
                                              .x = s->pointer_x,
                                              .y = s->pointer_y,
                                              .buttons = s->buttons});
    }
}
inline void p_leave(void* d, wl_pointer*, std::uint32_t, wl_surface*)
{
    auto* s = static_cast<state*>(d);
    s->pointer_added = false;
    s->buttons = 0;
    if (s->cb.on_pointer)
    {
        s->cb.on_pointer(input::pointer_event{.phase = input::pointer_phase::remove});
    }
}
inline void p_motion(void* d, wl_pointer*, std::uint32_t, wl_fixed_t px, wl_fixed_t py)
{
    auto* s = static_cast<state*>(d);
    s->pointer_x = wl_fixed_to_double(px);
    s->pointer_y = wl_fixed_to_double(py);
    if (s->cb.on_pointer)
    {
        s->cb.on_pointer(
            input::pointer_event{.phase = s->buttons ? input::pointer_phase::move
                                                     : input::pointer_phase::hover,
                                 .x = s->pointer_x,
                                 .y = s->pointer_y,
                                 .buttons = s->buttons});
    }
}
inline void p_button(void* d, wl_pointer*, std::uint32_t, std::uint32_t,
                     std::uint32_t button, std::uint32_t button_state)
{
    auto* s = static_cast<state*>(d);
    std::uint32_t mask = 0;
    if (button == BTN_LEFT)
        mask = 0x1;
    else if (button == BTN_RIGHT)
        mask = 0x2;
    else if (button == BTN_MIDDLE)
        mask = 0x4;
    else
        return;
    if (button_state == WL_POINTER_BUTTON_STATE_PRESSED)
        s->buttons |= mask;
    else
        s->buttons &= ~mask;
    if (s->cb.on_pointer)
    {
        s->cb.on_pointer(input::pointer_event{
            .phase = (button_state == WL_POINTER_BUTTON_STATE_PRESSED)
                         ? input::pointer_phase::down
                         : input::pointer_phase::up,
            .x = s->pointer_x,
            .y = s->pointer_y,
            .buttons = s->buttons});
    }
}
inline void p_axis(void* d, wl_pointer*, std::uint32_t, std::uint32_t, wl_fixed_t)
{
    auto* s = static_cast<state*>(d);
    if (s->cb.on_pointer)
    {
        s->cb.on_pointer(input::pointer_event{.phase = input::pointer_phase::up,
                                              .x = s->pointer_x,
                                              .y = s->pointer_y,
                                              .buttons = s->buttons});
    }
}
inline void k_keymap(void* d, wl_keyboard*, std::uint32_t, std::int32_t fd,
                     std::uint32_t size)
{
    auto* s = static_cast<state*>(d);
    if (s->cb.on_keymap && fd >= 0)
        s->cb.on_keymap(fd, size);
    else if (fd >= 0)
        close(fd);
}
inline void k_key(void* d, wl_keyboard*, std::uint32_t, std::uint32_t,
                  std::uint32_t key, std::uint32_t kstate)
{
    auto* s = static_cast<state*>(d);
    if (s->cb.on_key)
        s->cb.on_key(key, kstate == WL_KEYBOARD_KEY_STATE_PRESSED);
}
} // namespace detail

inline auto create(const config& cfg) -> state
{
    state s;
    s.cfg = cfg;
    s.width = cfg.width;
    s.height = cfg.height;
    return s;
}

inline auto connect(state& s) -> result<void>
{
    s.display =
        wl_display_connect(s.cfg.display.empty() ? nullptr : s.cfg.display.data());
    if (s.display == nullptr)
    {
        return unexpected(
            error{.code = error_code::wayland, .message = "connect failed"});
    }
    s.registry = wl_display_get_registry(s.display);
    wl_registry_add_listener(s.registry, &kRegistry, &s);
    wl_display_roundtrip(s.display);
    if (!s.compositor || !s.wm_base || !s.seat)
    {
        return unexpected(
            error{.code = error_code::wayland, .message = "missing globals"});
    }
    return {};
}

inline auto setup_egl(state& s) -> result<void>
{
#ifndef SHELL_RENDERER_GL
    // Software rendering does not need an EGL display.
    (void)s;
    return {};
#else
    static const EGLAttrib kAttribs[] = {EGL_NONE};
    s.egl.display =
        eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_EXT, s.display, kAttribs);
    if (s.egl.display == nullptr ||
        eglInitialize(static_cast<EGLDisplay>(s.egl.display), nullptr, nullptr) !=
            EGL_TRUE)
    {
        return unexpected(error{.code = error_code::egl, .message = "egl init"});
    }
    return egl::choose_config(static_cast<EGLDisplay>(s.egl.display),
                              static_cast<EGLConfig*>(&s.egl.config));
#endif
}

inline auto setup_surface(state& s) -> result<void>
{
    s.surface = wl_compositor_create_surface(s.compositor);
    if (!s.surface)
    {
        return unexpected(
            error{.code = error_code::wayland, .message = "create_surface"});
    }
    s.xs = xdg_wm_base_get_xdg_surface(s.wm_base, s.surface);
    xdg_surface_add_listener(s.xs, &kXdgSurface, &s);
    s.xt = xdg_surface_get_toplevel(s.xs);
    xdg_toplevel_add_listener(s.xt, &kToplevel, &s);
    xdg_toplevel_set_app_id(s.xt, "openauto");
    xdg_toplevel_set_title(s.xt, "OpenAuto");
    xdg_toplevel_set_min_size(s.xt, 1, 1);
    wl_surface_commit(s.surface);
    wl_display_roundtrip(s.display);

#ifdef SHELL_RENDERER_GL
    s.wl_window = wl_egl_window_create(s.surface, static_cast<int>(s.width),
                                       static_cast<int>(s.height));
    if (!s.wl_window)
    {
        return unexpected(error{.code = error_code::egl, .message = "wl_egl_window"});
    }
    EGLNativeWindowType native = reinterpret_cast<EGLNativeWindowType>(
        reinterpret_cast<std::uintptr_t>(s.wl_window));
    s.egl.surface =
        eglCreateWindowSurface(static_cast<EGLDisplay>(s.egl.display),
                               static_cast<EGLConfig>(s.egl.config), native, nullptr);
    if (s.egl.surface == EGL_NO_SURFACE)
    {
        return unexpected(
            error{.code = error_code::egl, .message = "egl window surface"});
    }
#else
    // Software rendering presents via wl_shm buffers on s.surface.
#endif
    return {};
}

// Arm a Wayland frame callback on the surface. The compositor fires it once it
// has displayed the presented buffer; that is the natural vsync for Flutter.
inline auto arm_frame(state& s) -> void
{
    if (s.frame_cb)
        return;
    s.frame_cb = wl_surface_frame(s.surface);
    wl_callback_add_listener(s.frame_cb, &kFrame, &s);
    wl_surface_commit(s.surface);
}

inline auto run(state& s) -> void
{
    s.running = true;
    // Drive vsync on a ~60 Hz timer (the DESIGN-listed fallback). The engine
    // only consumes a vsync when it has a pending request, so over-provisioning
    // ticks is harmless; the compositor frame callback (frame_ready) can be
    // layered on top later to eliminate over-draw.
    constexpr auto kPeriod = std::chrono::milliseconds(16);
    auto next_tick = std::chrono::steady_clock::now();
    if (s.cb.on_vsync)
        s.cb.on_vsync();

    while (s.running)
    {
        auto now = std::chrono::steady_clock::now();
        auto to = next_tick - now;
        if (to < std::chrono::steady_clock::duration::zero())
            to = {};
        int to_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(to).count());

        pollfd pfd{};
        pfd.fd = wl_display_get_fd(s.display);
        pfd.events = POLLIN;
        int rc = poll(&pfd, 1, to_ms);
        if (rc > 0 && (pfd.revents & POLLIN))
        {
            wl_display_dispatch(s.display);
            wl_display_flush(s.display);
        }
        now = std::chrono::steady_clock::now();
        if (s.cb.on_vsync && now >= next_tick)
        {
            s.cb.on_vsync();
            next_tick += kPeriod;
        }
        // Let the shell drain any injected key events onto the platform thread.
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
    bool ok = false;
#ifdef SHELL_RENDERER_GL
    ok = eglSwapBuffers(static_cast<EGLDisplay>(s.egl.display),
                        static_cast<EGLSurface>(s.egl.surface)) == EGL_TRUE;
#else
    ok = s.shm_buffer != nullptr;
#endif
    if (ok)
    {
        s.presented_once = true;
        s.frame_in_flight = false;
        // Ask the compositor to report when this frame has been shown; that
        // frame callback is the vsync that gates the engine's next render.
        arm_frame(s);
    }
    return ok;
}

inline auto destroy(state& s) -> void
{
#ifdef SHELL_RENDERER_GL
    if (s.egl.surface)
    {
        eglDestroySurface(static_cast<EGLDisplay>(s.egl.display),
                          static_cast<EGLSurface>(s.egl.surface));
    }
    if (s.wl_window)
        wl_egl_window_destroy(s.wl_window);
#endif
    if (s.surface)
        wl_surface_destroy(s.surface);
#ifdef SHELL_RENDERER_GL
    if (s.egl.display)
        eglTerminate(static_cast<EGLDisplay>(s.egl.display));
#endif
    if (s.display)
        wl_display_disconnect(s.display);
}

// ---- generic output adapter -------------------------------------------------
// Forward declaration (defined below).
namespace detail
{
bool software_present(void* d, const renderer::software::buffer& frame);
}

// Expose this Wayland backend through the backend-agnostic `shell::backend::output`
// interface so the engine can run without depending on Wayland headers.
namespace output
{
inline bool present(void* d)
{
    return shell::backend::wayland::swap(
        *static_cast<shell::backend::wayland::state*>(d));
}
inline void stop(void* d)
{
    shell::backend::wayland::stop(*static_cast<shell::backend::wayland::state*>(d));
}
inline auto take(shell::backend::wayland::state& s) -> shell::backend::output
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
    o.software_present.present = &detail::software_present;
    return o;
}
} // namespace output

// ---- software (wl_shm) presentation
namespace detail
{
inline bool software_present(void* d, const renderer::software::buffer& frame)
{
    auto* s = static_cast<shell::backend::wayland::state*>(d);
    if (!s->shm || !s->surface)
        return false;

    if (frame.stride == 0 || frame.height == 0)
        return false;
    std::size_t size = static_cast<std::size_t>(frame.stride) * frame.height;

    int fd = static_cast<int>(memfd_create("shell-shm", 0));
    if (fd < 0)
    {
        fd = ::shm_open("/shell-shm", O_RDWR | O_CREAT, 0600);
        if (fd < 0)
            return false;
    }
    if (ftruncate(fd, static_cast<off_t>(size)) < 0)
    {
        ::close(fd);
        return false;
    }

    void* mapped = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapped == MAP_FAILED)
    {
        ::close(fd);
        return false;
    }
    // Copy the CPU frame (bgra8888) row by row.
    auto* dst = static_cast<std::uint8_t*>(mapped);
    const auto* src = reinterpret_cast<const std::uint8_t*>(frame.pixels.data());
    for (std::uint32_t y = 0; y < frame.height; ++y)
    {
        std::memcpy(dst + static_cast<std::size_t>(y) * frame.stride,
                    src + static_cast<std::size_t>(y) * frame.stride, frame.stride);
    }
    // Release the previous wl_buffer and build a new one.
    if (s->shm_buffer)
        wl_buffer_destroy(s->shm_buffer);
    wl_shm_pool* pool = wl_shm_create_pool(s->shm, fd, static_cast<std::int32_t>(size));
    s->shm_buffer = wl_shm_pool_create_buffer(
        pool, 0, static_cast<std::int32_t>(frame.width),
        static_cast<std::int32_t>(frame.height),
        static_cast<std::int32_t>(frame.stride), WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    munmap(mapped, size);
    ::close(fd);

    // Attach and present the software buffer on the surface.
    wl_surface_attach(s->surface, s->shm_buffer, 0, 0);
    wl_surface_damage(s->surface, 0, 0, static_cast<std::int32_t>(frame.width),
                      static_cast<std::int32_t>(frame.height));
    wl_surface_commit(s->surface);
    // Arm a frame callback so the next render is gated on the compositor
    // having displayed this software frame.
    if (!s->frame_cb)
    {
        s->frame_cb = wl_surface_frame(s->surface);
        wl_callback_add_listener(s->frame_cb, &kFrame, s);
    }
    s->presented_once = true;
    s->frame_in_flight = false;
    wl_display_flush(s->display);
    return true;
}
} // namespace detail
} // namespace shell::backend::wayland
