#include "web/server.hpp"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

#include <cstdlib>

namespace culprit::web {

bool open_in_browser(const std::string& url) {
    // only ever our own http://127.0.0.1 address, never text from a log
    if (url.rfind("http://127.0.0.1:", 0) != 0) return false;
#ifdef _WIN32
    auto result = reinterpret_cast<INT_PTR>(ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    return result > 32;
#else
#ifdef __APPLE__
    const char* program = "open";
#else
    const char* program = "xdg-open";
    // nothing to open a browser on: a server, a container or an SSH session without a desktop
    if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY") && !std::getenv("WSL_DISTRO_NAME")) return false;
    if (std::getenv("WSL_DISTRO_NAME") && access("/usr/bin/wslview", X_OK) == 0) program = "wslview";
#endif
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    // the opener's own messages would land in the middle of culprit's output
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    std::string arg0 = program, arg1 = url;
    char* argv[] = {arg0.data(), arg1.data(), nullptr};
    pid_t pid = 0;
    int rc = posix_spawnp(&pid, program, &actions, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    if (rc != 0) return false;
    int status = 0;
    // xdg-open and open return once the browser has the URL
    if (waitpid(pid, &status, 0) < 0) return true;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
}

}  // namespace culprit::web
