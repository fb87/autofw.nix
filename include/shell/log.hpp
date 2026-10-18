#pragma once

#include <cstdio>
#include <string>
#include <string_view>
#include <utility>

namespace shell::log
{

// Per-thread label so N concurrent displays' interleaved stderr stays
// attributable (e.g. "d0*" for the primary display, "d1" for a secondary
// one). Empty by default -- single-display processes see byte-identical
// output to before this existed.
inline thread_local std::string g_tag;

inline void set_tag(std::string tag) { g_tag = std::move(tag); }

inline void info(std::string_view message)
{
    if (g_tag.empty())
        std::fprintf(stderr, "[shell] %.*s\n", static_cast<int>(message.size()),
                     message.data());
    else
        std::fprintf(stderr, "[shell:%s] %.*s\n", g_tag.c_str(),
                     static_cast<int>(message.size()), message.data());
}

} // namespace shell::log
