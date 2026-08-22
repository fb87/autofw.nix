#pragma once

#include <cstdint>
#include <string>

namespace shell::plugin
{

using id = std::uint64_t;

enum class type
{
    flutter,
    native,
    flutter_engine,
};

struct manifest
{
    std::string id;
    std::string name;
    std::string version;
    type kind{type::flutter};
};

struct state
{
};

} // namespace shell::plugin
