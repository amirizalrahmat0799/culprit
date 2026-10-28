// culprit: read logs in the terminal, follow them live, and find what went wrong.

#include <cstdio>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "core/filter.hpp"
#include "core/model.hpp"
#include "core/reader.hpp"
#include "tui/plain.hpp"
#include "tui/viewer.hpp"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#define isatty _isatty
#define fileno _fileno
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

constexpr const char* kHelp = R"(culprit - read logs in the terminal, follow them live, and find what went wrong

Usage:
  culprit [options] [file...]          view log files (followed for new lines, like tail -F)
  command | culprit [options]          view what a command prints
  culprit --plain [file...]            print formatted entries instead of opening the viewer

Examples:
  culprit app.log
  docker compose logs -f | culprit
  kubectl logs -f deploy/api | culprit --filter "level>=warn"
  culprit --plain --filter has:stack app.log > errors.txt

Options:
  -F, --filter <query>   start with a filter, e.g. "level>=warn payment"
  -n, --no-follow        read files to the end and stop watching them
  -p, --plain            print instead of opening the viewer (automatic when output is not a terminal)
      --no-stack         with --plain: leave out stack traces
      --no-color         with --plain: no colours (also when NO_COLOR is set)
  -v, --version
  -h, --help

Understands JSON logs (Spring Boot, pino, zap, slog, Python), logfmt, Spring Boot and logback text,
Python tracebacks, Go panics, nginx and Apache access logs, and docker compose / kubectl prefixes.
In the viewer, press ? for the keys.
)";

bool stdout_is_terminal() { return isatty(fileno(stdout)) != 0; }
bool stdin_is_terminal() { return isatty(fileno(stdin)) != 0; }

// Keeps a copy of the piped stdin for reading logs, then points stdin back at the keyboard so the viewer
// can read keys. Returns the copy (a descriptor, or a HANDLE on Windows), or -1 when that isn't possible.
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

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> files;
    std::string filter;
    bool follow = true, plain = false, color = std::getenv("NO_COLOR") == nullptr, stack = true;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto value = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "culprit: %s needs a value\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") {
            std::fputs(kHelp, stdout);
            return 0;
        } else if (a == "-v" || a == "--version") {
            std::printf("culprit %s\n", CULPRIT_VERSION);
            return 0;
        } else if (a == "-F" || a == "--filter") {
            filter = value("--filter");
        } else if (a.rfind("--filter=", 0) == 0) {
            filter = a.substr(9);
        } else if (a == "-n" || a == "--no-follow") {
            follow = false;
        } else if (a == "-p" || a == "--plain") {
            plain = true;
        } else if (a == "--no-color") {
            color = false;
        } else if (a == "--no-stack") {
            stack = false;
        } else if (a == "-") {
            files.push_back("-");
        } else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "culprit: unknown option %s (see culprit --help)\n", a.c_str());
            return 2;
        } else {
            files.push_back(a);
        }
    }

    if (files.empty()) {
        if (stdin_is_terminal()) {
            std::fputs(kHelp, stdout);
            return 0;
        }
        files.push_back("-");
    }
    if (!stdout_is_terminal()) plain = true;

    if (!filter.empty()) {
        auto f = culprit::Filter::parse(filter);
        if (!f.error().empty()) {
            std::fprintf(stderr, "culprit: %s\n", f.error().c_str());
            return 2;
        }
    }

    // inputs, labelled by file name when there are several
    std::vector<culprit::Reader::Input> inputs;
    for (const auto& f : files) {
        culprit::Reader::Input in;
        in.path = f;
        if (files.size() > 1) in.label = f == "-" ? "stdin" : std::filesystem::path(f).filename().string();
        inputs.push_back(in);
    }
    bool reads_stdin = std::find(files.begin(), files.end(), "-") != files.end();

    if (plain) {
        // print entries as they come; files are read to the end unless --follow was left on explicitly
        culprit::Reader reader(inputs, false);
        culprit::Filter f = culprit::Filter::parse(filter);
        culprit::PlainOptions options{color && stdout_is_terminal(), files.size() > 1, stack};
        bool sources_seen = false;
        std::string first_source;
        reader.start(nullptr);
        while (true) {
            bool done = reader.finished();
            for (auto& err : reader.take_errors()) std::fprintf(stderr, "culprit: %s\n", err.c_str());
            for (auto& e : reader.take()) {
                if (!e.source.empty()) {
                    if (first_source.empty()) first_source = e.source;
                    else if (e.source != first_source) sources_seen = true;
                }
                options.show_source = files.size() > 1 || sources_seen;
                if (f.matches(e)) culprit::print_plain(stdout, e, options);
            }
            std::fflush(stdout);
            if (done) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        reader.stop();
        return 0;
    }

    if (reads_stdin && !stdin_is_terminal()) {
        intptr_t copy = take_piped_stdin();
        if (copy == -1) {
            std::fputs("culprit: can't read the keyboard while logs are piped in; try --plain\n", stderr);
            return 1;
        }
        for (auto& in : inputs) {
            if (in.path == "-") in.native = copy;
        }
    }

    std::string title;
    for (const auto& f : files) {
        if (!title.empty()) title += ", ";
        title += f == "-" ? "stdin" : std::filesystem::path(f).filename().string();
    }

    culprit::Reader reader(inputs, follow);
    culprit::ViewerOptions options{title, filter};
    return culprit::run_viewer(reader, options);
}
