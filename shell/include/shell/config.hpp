#pragma once

#include <shell/renderer.hpp>

namespace shell
{

enum class backend_type
{
    automatic,
    wayland,
    drm,
};

struct config
{
    backend_type backend{backend_type::automatic};
    renderer::type renderer{renderer::type::automatic};
};

}
