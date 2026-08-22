#pragma once

#include <optional>
#include <string_view>

namespace shell::backend::drm
{

struct config
{
    std::string_view device{"/dev/dri/card0"};
    std::optional<std::string_view> connector;
};

struct state
{
    int fd{-1};
    void* gbm_device{};
    void* gbm_surface{};
};

} // namespace shell::backend::drm
