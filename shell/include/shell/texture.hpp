#pragma once

#include <array>
#include <cstdint>
#include <mutex>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

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
    std::uint32_t gl_format{0x8058}; // GL_RGBA8
#endif
    std::uint32_t width{};
    std::uint32_t height{};
    bool ready{};
};

struct cpu_buffer
{
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t stride{};
    std::vector<std::uint8_t> pixels;
};

struct state
{
    std::mutex mutex;
    std::unordered_map<id, entry> map;
    std::unordered_map<id, cpu_buffer> cpu_buffers;
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
    if (e.gl_name == 0)
    {
        GLuint name = 0;
        glGenTextures(1, &name);
        if (name == 0)
            return false;
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
                         std::uint32_t height, std::span<const std::uint8_t> rgba)
    -> bool
{
    if (!ensure_texture(s, texture_id, width, height))
        return false;
    std::lock_guard lock(s.mutex);
    auto it = s.map.find(texture_id);
    if (it == s.map.end())
        return false;
    entry& e = it->second;
    glBindTexture(GL_TEXTURE_2D, e.gl_name);
    if (width != e.width || height != e.height)
    {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(width),
                     static_cast<GLsizei>(height), 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     nullptr);
        e.width = width;
        e.height = height;
    }
    if (!rgba.empty())
    {
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
    if (it != s.map.end() && it->second.gl_name != 0)
    {
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
    if (it != s.map.end() && it->second.gl_name != 0)
    {
        GLuint name = it->second.gl_name;
        glDeleteTextures(1, &name);
        it->second.gl_name = 0;
        it->second.ready = false;
    }
}
#else
inline auto ensure_texture(state&, id, std::uint32_t, std::uint32_t) -> bool
{
    return false;
}
inline auto upload_frame(state&, id, std::uint32_t, std::uint32_t,
                         std::span<const std::uint8_t>) -> bool
{
    return false;
}
inline auto get_texture(state&, id, gl&) -> bool
{
    return false;
}
inline auto release_texture(state&, id) -> void
{
}
#endif // SHELL_RENDERER_GL

// ---- DMA-BUF -> EGLImage -> GL texture (M10, accelerated) -------------------
#ifdef SHELL_RENDERER_GL
#endif

inline auto set_cpu_buffer(state& s, id texture_id, std::uint32_t w, std::uint32_t h,
                           std::uint32_t stride, std::span<const std::uint8_t> pixels)
    -> bool
{
    std::lock_guard lock(s.mutex);
    cpu_buffer& b = s.cpu_buffers[texture_id];
    b.width = w;
    b.height = h;
    b.stride = stride;
    b.pixels.assign(pixels.begin(), pixels.end());
    return true;
}

#ifdef SHELL_RENDERER_GL
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2ext.h>

// Wraps a single-plane DMA-BUF fd as an EGLImage and binds it to a GL texture.
struct dmabuf_image
{
    EGLImageKHR image{};
    std::uint32_t gl_texture{};
};

inline auto import_dmabuf(EGLDisplay dpy, const dmabuf& buf, EGLint width,
                          EGLint height, std::uint32_t* gl_name_out) -> dmabuf_image
{
    dmabuf_image out{};
    if (buf.planes.empty() || buf.planes.front().fd < 0)
        return out;
    const dmabuf_plane& p = buf.planes.front();
    const EGLint attribs[] = {EGL_WIDTH,
                              width,
                              EGL_HEIGHT,
                              height,
                              EGL_LINUX_DRM_FOURCC_EXT,
                              static_cast<EGLint>(buf.format),
                              EGL_DMA_BUF_PLANE0_FD_EXT,
                              p.fd,
                              EGL_DMA_BUF_PLANE0_OFFSET_EXT,
                              static_cast<EGLint>(p.offset),
                              EGL_NONE};
    out.image =
        eglCreateImageKHR(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attribs);
    if (out.image == EGL_NO_IMAGE_KHR)
        return out;
    glGenTextures(1, &out.gl_texture);
    glBindTexture(GL_TEXTURE_2D, out.gl_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    // glEGLImageTargetTexture2DOES is an extension fetched at runtime.
    auto target = reinterpret_cast<void (*)(GLenum, GLeglImageOES)>(
        eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    if (target)
        target(GL_TEXTURE_2D, out.image);
    if (gl_name_out)
        *gl_name_out = out.gl_texture;
    return out;
}

inline auto release_image(EGLDisplay dpy, dmabuf_image& img) -> void
{
    if (img.gl_texture)
        glDeleteTextures(1, &img.gl_texture);
    if (img.image)
        eglDestroyImageKHR(dpy, img.image);
    img.image = EGL_NO_IMAGE_KHR;
    img.gl_texture = 0;
}
#endif // SHELL_RENDERER_GL

} // namespace shell::texture
