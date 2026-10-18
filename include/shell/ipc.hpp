#pragma once

// Process-spawning + Unix-domain-socket fd-passing for Step 4 (process-level
// isolation). Confirmed zero prior occurrence of any of this anywhere in the
// repo (no fork/exec/socketpair/SCM_RIGHTS/waitpid) -- entirely new
// infrastructure.

#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <shell/error.hpp>
#include <shell/result.hpp>

namespace shell::ipc
{

struct spawn_config
{
    std::string exe_path{"/proc/self/exe"};
    std::string assets;
    std::string icu_data;
    std::string aot_library;
    std::uint32_t width{};
    std::uint32_t height{};
};

struct child_process
{
    pid_t pid{-1};
    int parent_fd{-1};       // frame lifecycle: handshake/frame_ready/buffer_released.
    int parent_input_fd{-1}; // pointer/key events only -- see input_msg below. Kept
                             // separate from parent_fd deliberately: each fd has
                             // exactly one reader thread on each side today, which
                             // only works because each fd carries exactly one
                             // message shape; mixing input into the frame-lifecycle
                             // fd would force one reader to disambiguate two
                             // unrelated message shapes with no tag.
};

// Spawns the SAME shell_app binary in --child-renderer mode, connected to
// the returned child_process::parent_fd/parent_input_fd via two socketpairs.
//
// Between fork() and execv(), the child may only call async-signal-safe
// functions -- the process calling this almost certainly already has other
// threads running (the primary engine's own Flutter threads), and fork()
// only clones the calling thread; any lock held by a now-vanished thread
// (e.g. inside glibc malloc) stays locked forever in the child. That is
// why every std::string/std::vector allocation below happens BEFORE
// fork(), in the parent, where all threads are alive and able to release
// their own locks normally -- the child-side branch below does nothing but
// close/fcntl/execv/_exit.
inline auto spawn_child(const spawn_config& cfg) -> result<child_process>
{
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) != 0)
        return unexpected(error{.code = error_code::system, .message = "socketpair"});
    int input_fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, input_fds) != 0)
    {
        close(fds[0]);
        close(fds[1]);
        return unexpected(error{.code = error_code::system, .message = "input socketpair"});
    }

    std::string ipc_fd_arg = "--ipc-fd=" + std::to_string(fds[1]);
    std::string input_fd_arg = "--input-fd=" + std::to_string(input_fds[1]);
    std::string width_arg = "--width=" + std::to_string(cfg.width);
    std::string height_arg = "--height=" + std::to_string(cfg.height);
    std::string assets_arg = "--assets=" + cfg.assets;
    std::string icu_arg = "--icu=" + cfg.icu_data;
    std::string aot_arg = "--aot-lib=" + cfg.aot_library;

    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(cfg.exe_path.c_str()));
    argv.push_back(const_cast<char*>("--child-renderer"));
    argv.push_back(const_cast<char*>(ipc_fd_arg.c_str()));
    argv.push_back(const_cast<char*>(input_fd_arg.c_str()));
    argv.push_back(const_cast<char*>(width_arg.c_str()));
    argv.push_back(const_cast<char*>(height_arg.c_str()));
    argv.push_back(const_cast<char*>(assets_arg.c_str()));
    argv.push_back(const_cast<char*>(icu_arg.c_str()));
    if (!cfg.aot_library.empty())
        argv.push_back(const_cast<char*>(aot_arg.c_str()));
    argv.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0)
    {
        close(fds[0]);
        close(fds[1]);
        close(input_fds[0]);
        close(input_fds[1]);
        return unexpected(error{.code = error_code::system, .message = "fork"});
    }
    if (pid == 0)
    {
        // Child: async-signal-safe only, from here to execv/_exit.
        close(fds[0]);
        close(input_fds[0]);
        fcntl(fds[1], F_SETFD, 0);       // clear CLOEXEC -- must survive exec
        fcntl(input_fds[1], F_SETFD, 0); // same
        execv(cfg.exe_path.c_str(), argv.data());
        _exit(127); // execv failed
    }
    close(fds[1]);
    close(input_fds[1]);
    return child_process{pid, fds[0], input_fds[0]};
}

// Sends SIGTERM only -- deliberately does not touch parent_fd/parent_input_fd.
// The caller (shell::isolated_app::destroy) must join whatever thread is
// blocked reading them BEFORE closing: closing a fd that another thread is
// blocked on is racy (the fd number can be reused by a concurrently opened,
// unrelated fd, waking the blocked read on the wrong socket). SIGTERM
// causes the child to exit, which closes its ends and unblocks the readers
// with a clean EOF -- that is the intended wakeup path, not this function
// closing anything itself.
inline auto terminate(child_process& c) -> void
{
    if (c.pid > 0)
        kill(c.pid, SIGTERM);
}

inline auto wait_blocking(child_process& c, int* status_out) -> void
{
    if (c.pid <= 0)
        return;
    int status = 0;
    waitpid(c.pid, &status, 0);
    if (status_out)
        *status_out = status;
    c.pid = -1;
}

// ---- handshake (once, multi-fd) --------------------------------------------

struct handshake
{
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t format{};
    std::uint32_t stride{};
    std::uint32_t buffer_count{};
};

inline auto send_handshake(int fd, const handshake& hs, std::span<const int> fds)
    -> result<void>
{
    iovec iov{.iov_base = const_cast<handshake*>(&hs), .iov_len = sizeof(hs)};
    std::vector<char> control(CMSG_SPACE(fds.size() * sizeof(int)));
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control.data();
    msg.msg_controllen = control.size();

    cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(fds.size() * sizeof(int));
    std::memcpy(CMSG_DATA(cmsg), fds.data(), fds.size() * sizeof(int));

    if (sendmsg(fd, &msg, 0) < 0)
        return unexpected(error{.code = error_code::system, .message = "sendmsg handshake"});
    return {};
}

inline auto recv_handshake(int fd, handshake& hs, std::vector<int>& fds_out,
                           int timeout_ms = 3000) -> result<void>
{
    pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
    int rc = poll(&pfd, 1, timeout_ms);
    if (rc <= 0 || !(pfd.revents & POLLIN))
        return unexpected(
            error{.code = error_code::system, .message = "handshake timed out"});

    // Up to 16 fds is far more than this module ever sends (kBufferCount=3);
    // generous but bounded so a misbehaving peer can't make us allocate
    // unbounded control-message space.
    constexpr std::size_t kMaxFds = 16;
    std::vector<char> control(CMSG_SPACE(kMaxFds * sizeof(int)));
    iovec iov{.iov_base = &hs, .iov_len = sizeof(hs)};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control.data();
    msg.msg_controllen = control.size();

    ssize_t n = recvmsg(fd, &msg, 0);
    if (n != static_cast<ssize_t>(sizeof(hs)))
        return unexpected(error{.code = error_code::system, .message = "recvmsg handshake"});

    fds_out.clear();
    for (cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr;
        cmsg = CMSG_NXTHDR(&msg, cmsg))
    {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS)
        {
            std::size_t count = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            const int* received = reinterpret_cast<const int*>(CMSG_DATA(cmsg));
            fds_out.assign(received, received + count);
        }
    }
    if (fds_out.size() != hs.buffer_count)
        return unexpected(
            error{.code = error_code::system, .message = "handshake fd count mismatch"});
    return {};
}

// ---- per-frame messages, each carrying exactly one fence fd ---------------
//
// Two independent directions over the same full-duplex socket:
//   child -> parent, frame_ready: "buffer K has new content; GPU-done once
//     acquire_fence_fd signals." The parent must wait_fence() on it before
//     sampling the texture.
//   parent -> child, buffer_released: "I'm done reading buffer K; safe to
//     reuse once release_fence_fd signals." The child must wait_fence() on
//     it before rendering into that buffer slot again.
// Proven correct (sans the fence fd) by the fenceless de-risking spike
// before this was added.

namespace detail
{
// Shared by both message directions: one fixed-size payload + exactly one
// SCM_RIGHTS fd.
template <typename Msg>
inline auto send_with_fd(int fd, const Msg& msg, int carried_fd) -> result<void>
{
    iovec iov{.iov_base = const_cast<Msg*>(&msg), .iov_len = sizeof(msg)};
    char control[CMSG_SPACE(sizeof(int))];
    msghdr out{};
    out.msg_iov = &iov;
    out.msg_iovlen = 1;
    out.msg_control = control;
    out.msg_controllen = sizeof(control);

    cmsghdr* cmsg = CMSG_FIRSTHDR(&out);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(cmsg), &carried_fd, sizeof(int));

    if (sendmsg(fd, &out, 0) < 0)
        return unexpected(error{.code = error_code::system, .message = "sendmsg"});
    return {};
}

// false means EOF or error -- the peer is gone. fd_out is -1 if the
// message carried no fence (treated as "already satisfied," matching
// egl::wait_fence's own fence_fd < 0 no-op).
template <typename Msg>
inline auto recv_with_fd(int fd, Msg& msg, int& fd_out) -> bool
{
    fd_out = -1;
    iovec iov{.iov_base = &msg, .iov_len = sizeof(msg)};
    char control[CMSG_SPACE(sizeof(int))];
    msghdr in{};
    in.msg_iov = &iov;
    in.msg_iovlen = 1;
    in.msg_control = control;
    in.msg_controllen = sizeof(control);

    ssize_t n = recvmsg(fd, &in, 0);
    if (n != static_cast<ssize_t>(sizeof(msg)))
        return false;
    for (cmsghdr* cmsg = CMSG_FIRSTHDR(&in); cmsg != nullptr; cmsg = CMSG_NXTHDR(&in, cmsg))
    {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS)
            std::memcpy(&fd_out, CMSG_DATA(cmsg), sizeof(int));
    }
    return true;
}
} // namespace detail

struct frame_ready
{
    std::uint32_t buffer_index{};
    std::uint64_t frame_number{};
};

inline auto send_frame_ready(int fd, const frame_ready& msg, int acquire_fence_fd)
    -> result<void>
{
    return detail::send_with_fd(fd, msg, acquire_fence_fd);
}

inline auto recv_frame_ready(int fd, frame_ready& msg, int& acquire_fence_fd_out) -> bool
{
    return detail::recv_with_fd(fd, msg, acquire_fence_fd_out);
}

struct buffer_released
{
    std::uint32_t buffer_index{};
};

inline auto send_buffer_released(int fd, const buffer_released& msg, int release_fence_fd)
    -> result<void>
{
    return detail::send_with_fd(fd, msg, release_fence_fd);
}

inline auto recv_buffer_released(int fd, buffer_released& msg, int& release_fence_fd_out)
    -> bool
{
    return detail::recv_with_fd(fd, msg, release_fence_fd_out);
}

// ---- input events, parent -> child only, over parent_input_fd -------------
//
// Deliberately NOT built on detail::send_with_fd/recv_with_fd: those always
// carry exactly one real fd via SCM_RIGHTS, and a pointer/key event has no
// fd concept at all -- reusing them with a meaningless dummy fd would
// conflate "no fence" (a real, documented sentinel elsewhere in this file)
// with "this message type never had an fd to begin with". A plain
// write()/read() is simpler and more honest for a message with nothing to
// carry. One struct serves both event kinds (pointer/key fields are
// mutually exclusive, discriminated by `kind`) -- field duplication across
// a small struct is cheap and avoids a union/variant across what is, in
// effect, an ABI boundary between two processes.
enum class input_kind : std::uint32_t
{
    pointer,
    key,
};

struct input_msg
{
    input_kind kind{};
    // pointer fields (kind == pointer) -- mirrors shell::input::pointer_event.
    double x{};
    double y{};
    std::uint32_t phase{};
    std::uint32_t buttons{};
    std::uint32_t device{};
    std::uint32_t pointer_device_kind{};
    // key fields (kind == key).
    std::uint32_t evdev_code{};
    std::uint32_t key_down{}; // 0/1 -- not bool, to keep the wire struct's layout
                              // unambiguous across the process boundary.
};

inline auto send_input(int fd, const input_msg& msg) -> result<void>
{
    const auto* p = reinterpret_cast<const char*>(&msg);
    std::size_t remaining = sizeof(msg);
    while (remaining > 0)
    {
        ssize_t n = write(fd, p, remaining);
        if (n <= 0)
            return unexpected(error{.code = error_code::system, .message = "write input_msg"});
        p += n;
        remaining -= static_cast<std::size_t>(n);
    }
    return {};
}

// false means EOF or error -- the peer is gone.
inline auto recv_input(int fd, input_msg& msg) -> bool
{
    auto* p = reinterpret_cast<char*>(&msg);
    std::size_t remaining = sizeof(msg);
    while (remaining > 0)
    {
        ssize_t n = read(fd, p, remaining);
        if (n <= 0)
            return false;
        p += n;
        remaining -= static_cast<std::size_t>(n);
    }
    return true;
}

} // namespace shell::ipc
