#pragma once

namespace shell::renderer
{

enum class type
{
    automatic,
    gl,
    software,
};

struct state
{
    type selected{type::automatic};
};

} // namespace shell::renderer
