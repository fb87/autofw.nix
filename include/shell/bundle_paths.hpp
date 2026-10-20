#pragma once

// Defaults icu/aot_lib for one Flutter asset bundle, given just its assets
// dir -- shared by shell_app and every standalone test/example binary,
// each of which takes its own --assets (+ optional --icu/--aot-lib) and
// needs the exact same defaulting pass. A dashboard-style app with
// multiple independent bundles (primary + cluster + infotainment) just
// calls this once per bundle.

#include <cstdio>
#include <filesystem>
#include <string>

namespace shell
{

// icu defaults to <assets>/icudtl.dat; aot_lib auto-detects either
// flutter-pi/ardera-style app.so straight in the assets dir, or Flutter's
// own release_bundle_linux-<arch>_assets layout (sibling lib/libapp.so).
inline auto apply_default_bundle_paths(const std::string& assets, std::string& icu,
                                       std::string& aot_lib) -> void
{
    if (icu.empty())
        icu = assets + "/icudtl.dat";
    if (aot_lib.empty())
    {
        auto assets_path = std::filesystem::path(assets);
        auto candidates = {
            assets_path / "app.so",
            assets_path.parent_path() / "lib" / "libapp.so",
        };
        for (const auto& candidate : candidates)
        {
            if (!std::filesystem::exists(candidate))
                continue;
            if (aot_lib.empty())
            {
                aot_lib = candidate.string();
            }
            else
            {
                // Both conventions present, almost always a stale leftover
                // from an earlier build with the other toolchain. Make the
                // ignored one visible instead of silently picking one.
                std::fprintf(stderr, "warning: ignoring %s (using %s instead)\n",
                             candidate.string().c_str(), aot_lib.c_str());
            }
        }
    }
}

} // namespace shell
