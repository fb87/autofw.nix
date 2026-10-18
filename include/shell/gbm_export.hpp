#pragma once

// GBM standalone-buffer allocation + dma-buf export -- the producer side of
// Step 4's cross-process GPU buffer sharing (shell::child_renderer renders
// into these; shell::isolated_app/texture::import_dmabuf is the consumer
// side, already existing).
//
// Distinct from backend/drm.hpp's GBM usage: that file only ever uses
// gbm_surface_create()'s scanout-oriented swapchain (GEM handles via
// drmModeAddFB, for driving a real display). This module allocates
// standalone buffer objects via gbm_bo_create() and exports them as dma-buf
// file descriptors via gbm_bo_get_fd() -- a GBM usage pattern with zero
// prior occurrence anywhere in this repo.
//
// Opens a DRM *render* node (/dev/dri/renderD128 by default), not a
// display-driving card node: render nodes are the standard, lower-
// privilege way to do GPU-render-only work, and using one here decouples
// the child renderer entirely from whichever backend (Wayland or DRM) the
// primary process happens to be using.

#ifdef SHELL_RENDERER_GL

#include <cstdint>
#include <string>

#include <fcntl.h>
#include <unistd.h>

#include <gbm.h>

#include <shell/error.hpp>
#include <shell/result.hpp>
#include <shell/texture.hpp>

namespace shell::gbm
{

struct device
{
    int fd{-1};
    gbm_device* dev{};
};

inline auto open_render_node(const std::string& path = "/dev/dri/renderD128")
    -> result<device>
{
    device d;
    d.fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (d.fd < 0)
        return unexpected(
            error{.code = error_code::drm, .message = "open render node: " + path});
    d.dev = gbm_create_device(d.fd);
    if (!d.dev)
    {
        close(d.fd);
        d.fd = -1;
        return unexpected(error{.code = error_code::drm, .message = "gbm_create_device"});
    }
    return d;
}

inline auto close_device(device& d) -> void
{
    if (d.dev)
        gbm_device_destroy(d.dev);
    if (d.fd >= 0)
        close(d.fd);
    d.dev = nullptr;
    d.fd = -1;
}

struct buffer
{
    gbm_bo* bo{};
    int fd{-1}; // owned; caller (destroy_buffer) closes it
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t stride{};
    std::uint32_t format{};
};

// GBM_BO_USE_LINEAR forces a simple, portable stride layout, sidestepping
// vendor tiling modifiers entirely -- texture::dmabuf::modifier is
// deliberately left unwired (see texture.hpp's import_dmabuf) because
// every buffer this module produces is guaranteed linear.
inline auto create_buffer(device& d, std::uint32_t w, std::uint32_t h,
                          std::uint32_t format = GBM_FORMAT_ARGB8888) -> result<buffer>
{
    buffer b;
    b.width = w;
    b.height = h;
    b.format = format;
    b.bo = gbm_bo_create(d.dev, w, h, format,
                         GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
    if (!b.bo)
        return unexpected(error{.code = error_code::drm, .message = "gbm_bo_create"});
    b.fd = gbm_bo_get_fd(b.bo);
    if (b.fd < 0)
    {
        gbm_bo_destroy(b.bo);
        b.bo = nullptr;
        return unexpected(error{.code = error_code::drm, .message = "gbm_bo_get_fd"});
    }
    b.stride = gbm_bo_get_stride(b.bo);
    return b;
}

inline auto destroy_buffer(buffer& b) -> void
{
    if (b.fd >= 0)
        close(b.fd);
    if (b.bo)
        gbm_bo_destroy(b.bo);
    b.fd = -1;
    b.bo = nullptr;
}

inline auto as_dmabuf_plane(const buffer& b) -> texture::dmabuf_plane
{
    return {.fd = b.fd, .offset = 0, .stride = b.stride};
}

} // namespace shell::gbm

#endif // SHELL_RENDERER_GL
