// shell_ctrl -- a tiny CLI tool that talks to an already-running shell_app
// over its control socket (shell::ctrl) to change runtime behavior. First
// real command: toggling a layer's visibility. One request, one response,
// then exits -- not a persistent daemon.
#include <cstdio>
#include <string>
#include <vector>

#include <shell/ctrl.hpp>

int main(int argc, char** argv)
{
    std::string socket_path = "/tmp/shell_app-ctrl.sock";
    std::vector<std::string> words;
    for (int i = 1; i < argc; ++i)
    {
        std::string_view a = argv[i];
        if (a.rfind("--socket=", 0) == 0)
            socket_path = std::string(a.substr(9));
        else
            words.emplace_back(a);
    }
    if (words.empty())
    {
        std::fprintf(stderr,
                     "usage: %s [--socket=PATH] <command> [args...]\n"
                     "  set-layer-visible <id> <true|false>\n",
                     argv[0]);
        return 1;
    }

    std::string line;
    for (auto& w : words)
    {
        if (!line.empty())
            line += ' ';
        line += w;
    }

    auto resp = shell::ctrl::send_command(socket_path, line);
    if (!resp)
    {
        std::fprintf(stderr, "shell_ctrl: %s\n", resp.error().message.c_str());
        return 1;
    }
    std::printf("%s\n", resp->c_str());
    return resp->rfind("error", 0) == 0 ? 1 : 0;
}
