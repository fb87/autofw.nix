#pragma once

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include <shell/renderer.hpp>
#include <shell/result.hpp>

namespace shell::native
{

// Rendering capabilities a native surface/plugin supports (DESIGN.md 19.4).
enum class capability
{
    dmabuf,
    gl_texture,
    cpu_buffer,
};

struct capabilities
{
    std::vector<capability> supported;
};

inline auto supports(const capabilities& c, capability cap) -> bool
{
    for (auto k : c.supported)
        if (k == cap)
            return true;
    return false;
}

// Check whether a native surface's capabilities are compatible with the active
// renderer. In software mode a plugin that only offers GPU paths (dmabuf/gl)
// must fail cleanly.
inline auto negotiate(const renderer::type renderer_type, const capabilities& caps)
    -> result<void>
{
    if (renderer_type == renderer::type::software ||
        renderer_type == renderer::type::automatic)
    {
        if (supports(caps, capability::cpu_buffer))
            return {};
        return shell::unexpected(error{
            .code = error_code::plugin,
            .message = "plugin has no cpu_buffer capability for software mode",
        });
    }
    // Accelerated mode accepts any supported path, but at least one is needed.
    if (caps.supported.empty())
        return shell::unexpected(error{.code = error_code::plugin,
                                     .message = "plugin declares no capabilities"});
    return {};
}

// Tracks the negotiated capability for a native surface.
struct state
{
    capabilities caps;
    std::optional<capability> active;
};

inline auto register_capabilities(state& s, capabilities caps) -> void
{
    s.caps = std::move(caps);
    s.active.reset();
}

inline auto negotiate(state& s, const renderer::type renderer_type) -> result<void>
{
    if (auto err = negotiate(renderer_type, s.caps); !err)
        return err;
    s.active = (renderer_type == renderer::type::software) ? capability::cpu_buffer
                                                           : capability::gl_texture;
    return {};
}

} // namespace shell::native
