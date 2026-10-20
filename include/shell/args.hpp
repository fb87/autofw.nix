#pragma once

// Tiny shared helper for standalone binaries' own minimal CLI parsing
// (--assets/--icu/--aot-lib/--renderer and whatever else a given binary
// needs) -- not a replacement for getopt_long, just avoids every new
// test/example main() redefining the same "--flag=value" match-and-extract
// one-liner.

#include <string>
#include <string_view>

namespace shell::args
{

// If `arg` is "<flag>=<value>", sets `out` to <value> and returns true.
// Otherwise leaves `out` untouched and returns false.
inline auto match(std::string_view arg, std::string_view flag, std::string& out) -> bool
{
    if (arg.size() <= flag.size() || arg.substr(0, flag.size()) != flag ||
        arg[flag.size()] != '=')
        return false;
    out = std::string(arg.substr(flag.size() + 1));
    return true;
}

} // namespace shell::args
