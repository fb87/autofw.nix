#pragma once

#include <string>

namespace shell
{

enum class error_code
{
    none,
    invalid_argument,
    unavailable,
    system,
    flutter,
    wayland,
    drm,
    egl,
    texture,
    plugin,
};

struct error
{
    error_code code{error_code::none};
    int native_code{};
    std::string message;
};

} // namespace shell
