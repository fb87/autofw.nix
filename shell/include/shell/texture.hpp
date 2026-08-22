#pragma once

#include <cstdint>
#include <mutex>
#include <span>
#include <unordered_map>

#ifdef SHELL_RENDERER_GL
#include <GLES2/gl2.h>
#endif

#include <shell/handle.hpp>
#include <shell/result.hpp>

namespace shell::texture
{

using id = std::int64_t;

struct gl
{
    std::uint32_t name{};
    std::uint32_t width{};
    std::uint32_t height{};
};

struct dmabuf_plane
{
    int fd{-1};
    std::uint32_t offset{};
    std::uint32_t stride{};
};

struct dmabuf
{
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t format{};
    std::uint64_t modifier{};
    std::span<const dmabuf_plane> planes;
};

struct entry
{
#ifdef SHELL_RENDERER_GL
    std::uint32_t gl_name{};
    std::uint32_t gl_target{GL_TEXTURE_2D};
    std::uint32_t gl_format{0x8058};  // GL_RGBA8
#endif
    std::uint32_t width{};
    std::uint32_t height{};
    bool ready{};
};

struct state
{
    std::mutex mutex;
    std::unordered_map<id, entry> map;
};

inline auto register_id(state& s, id texture_id) -> void
{
    std::lock_guard lock(s.mutex);
    s.map.emplace(texture_id, entry{});
}

inline auto unregister_id(state& s, id texture_id) -> void
{
    std::lock_guard lock(s.mutex);
    s.map.erase(texture_id);
}

inline auto has(state& s, id texture_id) -> bool
{
    std::lock_guard lock(s.mutex);
    return s.map.contains(texture_id);
}

#ifdef SHELL_RENDERER_GL
inline auto ensure_texture(state& s, id texture_id, std::uint32_t width,
                           std::uint32_t height) -> bool
{
    std::lock_guard lock(s.mutex);
    entry& e = s.map[texture_id];
    if (e.gl_name == 0) {
        GLuint name = 0;
        glGenTextures(1, &name);
        if (name == 0) return false;
        e.gl_name = name;
        e.gl_target = GL_TEXTURE_2D;
        e.gl_format = 0x8058;
        e.width = width;
        e.height = height;
        e.ready = false;
        glBindTexture(GL_TEXTURE_2D, name);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(width),
                     static_cast<GLsizei>(height), 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     nullptr);
    }
    return true;
}

inline auto upload_frame(state& s, id texture_id, std::uint32_t width,
                         std::uint32_t height,
                         std::span<const std::uint8_t> rgba) -> bool
{
    if (!ensure_texture(s, texture_id, width, height)) return false;
    std::lock_guard lock(s.mutex);
    auto it = s.map.find(texture_id);
    if (it == s.map.end()) return false;
    entry& e = it->second;
    glBindTexture(GL_TEXTURE_2D, e.gl_name);
    if (width != e.width || height != e.height) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(width),
                     static_cast<GLsizei>(height), 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     nullptr);
        e.width = width;
        e.height = height;
    }
    if (!rgba.empty()) {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(width),
                        static_cast<GLsizei>(height), GL_RGBA, GL_UNSIGNED_BYTE,
                        rgba.data());
    }
    e.ready = true;
    return true;
}

inline auto get_texture(state& s, id texture_id, gl& out) -> bool
{
    std::lock_guard lock(s.mutex);
    auto it = s.map.find(texture_id);
    if (it != s.map.end() && it->second.gl_name != 0) {
        out.name = it->second.gl_name;
        out.width = it->second.width ? it->second.width : 1;
        out.height = it->second.height ? it->second.height : 1;
        return true;
    }
    out.name = 0;
    out.width = 1;
    out.height = 1;
    return false;
}

inline auto release_texture(state& s, id texture_id) -> void
{
    std::lock_guard lock(s.mutex);
    auto it = s.map.find(texture_id);
    if (it != s.map.end() && it->second.gl_name != 0) {
        GLuint name = it->second.gl_name;
        glDeleteTextures(1, &name);
        it->second.gl_name = 0;
        it->second.ready = false;
    }
}
#else
inline auto ensure_texture(state&, id, std::uint32_t, std::uint32_t) -> bool { return false; }
inline auto upload_frame(state&, id, std::uint32_t, std::uint32_t, std::span<const std::uint8_t>) -> bool { return false; }
inline auto get_texture(state&, id, gl&) -> bool { return false; }
inline auto release_texture(state&, id) -> void {}
#endif  // SHELL_RENDERER_GL

}  // namespace shell::texture
