#pragma once

#include <cstdint>

#include <shell/stage.hpp>

namespace shell::layer
{

using id = std::uint64_t;

enum class role
{
    background,
    application,
    system_ui,
    overlay,
    notification,
    keyboard,
    emergency,
};

enum class input_mode
{
    passthrough,
    normal,
    exclusive,
};

struct geometry
{
    float x{};
    float y{};
    float width{};
    float height{};
};

struct state
{
    id value{};
    stage::id stage{};
    role kind{role::application};
    geometry bounds{};
    float opacity{1.0F};
    bool visible{true};
    input_mode input{input_mode::normal};
};

}
