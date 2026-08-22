#pragma once

#include <cstdio>
#include <string_view>

namespace shell::log
{

inline void info(std::string_view message)
{
    std::fprintf(
        stderr,
        "[shell] %.*s\n",
        static_cast<int>(message.size()),
        message.data());
}

}
