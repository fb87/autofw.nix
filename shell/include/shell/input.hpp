#pragma once

#include <cstdint>

namespace shell::input
{

enum class pointer_phase
{
    add,
    remove,
    hover,
    down,
    move,
    up,
    cancel,
};

struct pointer_event
{
    std::uint32_t device{};
    pointer_phase phase{};
    double x{};
    double y{};
    std::uint64_t timestamp{};
};

struct state
{
};

}
