#pragma once

// Generic runtime control channel: a Unix domain socket a running shell_app
// listens on, and a tiny request/response transport shell_ctrl (or anything
// else) uses to talk to it. One line in, one line out, newline-terminated
// plain text -- simple, greppable, easy to extend with more commands later
// without a schema/codec. Not meant for a high-frequency channel (that's
// what the layer/texture/input machinery already is); this is for
// occasional, human- or tool-driven runtime control.
//
// Deliberately knows nothing about layers/engines/dashboards -- command
// *parsing and dispatch* is the caller's job (see shell_app's
// dispatch_ctrl_command); this module only moves lines of text across a
// socket.

#include <cstring>
#include <optional>
#include <string>
#include <string_view>

#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <unistd.h>

#include <shell/error.hpp>
#include <shell/result.hpp>

namespace shell::ctrl
{

struct listener
{
    int fd{-1};
    std::string path;
};

// Binds + listens on a Unix domain socket at `path`. Removes any stale
// socket file left by a previous unclean shutdown first -- bind() fails
// with EADDRINUSE on a leftover path otherwise, even though nothing is
// actually listening on it anymore.
inline auto listen(const std::string& path) -> result<listener>
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return unexpected(error{.code = error_code::system, .message = "ctrl: socket"});

    ::unlink(path.c_str());

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        close(fd);
        return unexpected(error{.code = error_code::system, .message = "ctrl: bind " + path});
    }
    if (::listen(fd, 4) != 0)
    {
        close(fd);
        ::unlink(path.c_str());
        return unexpected(error{.code = error_code::system, .message = "ctrl: listen"});
    }
    return listener{fd, path};
}

inline auto close(listener& l) -> void
{
    if (l.fd >= 0)
        ::close(l.fd);
    if (!l.path.empty())
        ::unlink(l.path.c_str());
    l.fd = -1;
    l.path.clear();
}

struct command
{
    int conn_fd{-1}; // caller must respond() (which closes it) exactly once.
    std::string line;
};

// Waits up to timeout_ms for one incoming connection, reads exactly one
// newline-terminated line from it, and returns it -- std::nullopt on
// timeout (poll a `running` flag around this in a loop) or on any error
// (a malformed/aborted connection is not worth distinguishing from "no
// request this tick" to the caller).
inline auto accept_command(listener& l, int timeout_ms) -> std::optional<command>
{
    pollfd pfd{.fd = l.fd, .events = POLLIN, .revents = 0};
    if (poll(&pfd, 1, timeout_ms) <= 0 || !(pfd.revents & POLLIN))
        return std::nullopt;

    int conn = accept4(l.fd, nullptr, nullptr, SOCK_CLOEXEC);
    if (conn < 0)
        return std::nullopt;

    std::string line;
    char buf[256];
    while (line.find('\n') == std::string::npos)
    {
        ssize_t n = read(conn, buf, sizeof(buf));
        if (n <= 0)
        {
            ::close(conn);
            return std::nullopt;
        }
        line.append(buf, static_cast<std::size_t>(n));
        if (line.size() > 4096) // a well-behaved client never sends anywhere near this.
        {
            ::close(conn);
            return std::nullopt;
        }
    }
    line.resize(line.find('\n'));
    return command{conn, line};
}

inline auto respond(command& c, std::string_view text) -> void
{
    if (c.conn_fd < 0)
        return;
    std::string out(text);
    out.push_back('\n');
    std::size_t sent = 0;
    while (sent < out.size())
    {
        ssize_t n = write(c.conn_fd, out.data() + sent, out.size() - sent);
        if (n <= 0)
            break;
        sent += static_cast<std::size_t>(n);
    }
    ::close(c.conn_fd);
    c.conn_fd = -1;
}

// ---- client side (shell_ctrl) ----------------------------------------------

inline auto send_command(const std::string& path, std::string_view line) -> result<std::string>
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return unexpected(error{.code = error_code::system, .message = "ctrl: socket"});

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        ::close(fd);
        return unexpected(
            error{.code = error_code::system, .message = "ctrl: connect " + path});
    }

    std::string out(line);
    out.push_back('\n');
    std::size_t sent = 0;
    while (sent < out.size())
    {
        ssize_t n = write(fd, out.data() + sent, out.size() - sent);
        if (n <= 0)
        {
            ::close(fd);
            return unexpected(error{.code = error_code::system, .message = "ctrl: write"});
        }
        sent += static_cast<std::size_t>(n);
    }

    std::string response;
    char buf[256];
    while (response.find('\n') == std::string::npos)
    {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0)
        {
            ::close(fd);
            return unexpected(error{.code = error_code::system, .message = "ctrl: no response"});
        }
        response.append(buf, static_cast<std::size_t>(n));
        if (response.size() > 4096)
        {
            ::close(fd);
            return unexpected(
                error{.code = error_code::system, .message = "ctrl: response too long"});
        }
    }
    response.resize(response.find('\n'));
    ::close(fd);
    return response;
}

} // namespace shell::ctrl
