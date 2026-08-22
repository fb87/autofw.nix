#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

// Software renderer.
//
// Uses Flutter's software-renderer path and CPU-owned pixel buffers. The
// backend presents each frame as a `buffer`: Wayland via wl_shm, DRM/KMS via
// a CPU dumb buffer.

namespace shell::renderer::software
{

enum class pixel_format
{
    bgra8888,
    rgba8888,
};

struct buffer
{
    std::span<std::byte> pixels;
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t stride{};
    pixel_format format{pixel_format::bgra8888};
};

// A backend provides this to present a CPU pixel buffer to the display.
using present_fn = bool (*)(void* output, const buffer& frame);

struct presenters
{
    void* output{};
    present_fn present{};
};

struct state
{
    presenters hosts;
};

}  // namespace shell::renderer::software
