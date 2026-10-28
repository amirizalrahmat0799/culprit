#include "cli.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <thread>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#define isatty _isatty
#define fileno _fileno
#else
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace culprit::cli {

bool stdout_is_terminal() { return isatty(fileno(stdout)) != 0; }
bool stdin_is_terminal() { return isatty(fileno(stdin)) != 0; }
bool stderr_is_terminal() { return isatty(fileno(stderr)) != 0; }

int terminal_width() {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)) return info.srWindow.Right - info.srWindow.Left + 1;
#else
    struct winsize ws {};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) return ws.ws_col;
#endif
    return 100;
}

intptr_t take_piped_stdin() {
#ifdef _WIN32
    HANDLE original = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE copy = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), original, GetCurrentProcess(), &copy, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        return -1;
    }
    HANDLE console = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                 OPEN_EXISTING, 0, nullptr);
    if (console == INVALID_HANDLE_VALUE) return -1;
    SetStdHandle(STD_INPUT_HANDLE, console);
    std::FILE* ignored = nullptr;
    freopen_s(&ignored, "CONIN$", "r", stdin);
    return reinterpret_cast<intptr_t>(copy);
#else
    int copy = dup(STDIN_FILENO);
    if (copy < 0) return -1;
    int tty = open("/dev/tty", O_RDONLY);
    if (tty < 0) {
        close(copy);
        return -1;
    }
    dup2(tty, STDIN_FILENO);
    close(tty);
    return copy;
#endif
}

std::string ask(const std::string& question, bool* ok) {
    std::fputs(question.c_str(), stderr);
    std::fflush(stderr);
    std::string answer;
    if (stdin_is_terminal()) {
        if (!std::getline(std::cin, answer)) {
            if (ok) *ok = false;
            return {};
        }
    } else {
        // stdin is the log: ask on the terminal itself
#ifdef _WIN32
        std::FILE* tty = nullptr;
        fopen_s(&tty, "CONIN$", "r");
#else
        std::FILE* tty = std::fopen("/dev/tty", "r");
#endif
        if (!tty) {
            std::fputs("\n", stderr);
            if (ok) *ok = false;
            return {};
        }
        char buf[512];
        if (std::fgets(buf, sizeof buf, tty)) answer = buf;
        std::fclose(tty);
    }
    while (!answer.empty() && (answer.back() == '\n' || answer.back() == '\r')) answer.pop_back();
    if (ok) *ok = true;
    return answer;
}

bool ask_yes_no(const std::string& question, bool fallback) {
    bool ok = false;
    std::string a = ask(question + (fallback ? " [Y/n] " : " [y/N] "), &ok);
    if (!ok || a.empty()) return ok && fallback;
    return a[0] == 'y' || a[0] == 'Y';
}

std::shared_ptr<RuleSet> load_rules(const std::vector<std::string>& extra, std::vector<std::string>& errors) {
    auto rules = RuleSet::builtin();
    std::error_code ec;
    for (const auto& path : default_rule_paths(std::filesystem::current_path(ec).string())) rules->add_path(path, errors);
    for (const auto& path : extra) rules->add_path(path, errors);
    return rules;
}

Inputs make_inputs(std::vector<std::string> files) {
    Inputs in;
    if (files.empty()) files.push_back("-");
    for (const auto& f : files) {
        Reader::Input input;
        input.path = f;
        if (files.size() > 1) input.label = f == "-" ? "stdin" : std::filesystem::path(f).filename().string();
        in.list.push_back(input);
        if (!in.title.empty()) in.title += ", ";
        in.title += f == "-" ? "stdin" : std::filesystem::path(f).filename().string();
    }
    in.files = std::move(files);
    return in;
}

void read_all(const Inputs& in, const std::function<void(Entry&)>& each) {
    Reader reader(in.list, false);
    reader.start(nullptr);
    while (true) {
        bool done = reader.finished();
        for (auto& err : reader.take_errors()) std::fprintf(stderr, "culprit: %s\n", err.c_str());
        for (auto& e : reader.take()) each(e);
        if (done) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    reader.stop();
}

}  // namespace culprit::cli
