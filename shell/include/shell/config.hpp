#pragma once

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
};

}
