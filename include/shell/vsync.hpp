#pragma once

#include <cstdint>

namespace shell::vsync
{

struct state
{
    std::intptr_t baton{};
};

} // namespace shell::vsync
