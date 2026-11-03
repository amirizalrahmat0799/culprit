#include "cli_web.hpp"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "cli.hpp"
#include "core/filter.hpp"
#include "web/server.hpp"

namespace culprit::cli {
namespace {

constexpr const char* kWebHelp = R"(culprit web - the logs, what went wrong, and the AI investigator in your browser

Usage:
  culprit web [options] [file...]
  command | culprit web [options]

Starts a small server on this computer (127.0.0.1 only) and opens the page in your browser. Files are
followed for new lines, like tail -F; piped input is read as it comes. Press Ctrl+C to stop.

The address includes a key, so other websites and other users on this computer can't read your logs
through it. Open it from the terminal, and don't share it.

Options:
  -F, --filter <query>   start with a filter, e.g. "level>=warn payment"
  -n, --no-follow        read files to the end and stop watching them
      --port <n>         the port to try first (default 4790; the next free one is used when it's taken)
      --no-open          print the address instead of opening the browser
      --rules <path>     more rules: a .toml file or a folder of them (can be repeated)
      --no-ai            don't offer AI investigation in this run
  -h, --help

Examples:
  culprit web app.log
  docker compose logs -f | culprit web
  kubectl logs -f deploy/api | culprit web --filter "level>=warn"
)";

void on_interrupt(int) {
    // stop() joins threads, which a signal handler mustn't do; leaving is enough
    std::_Exit(0);
}

}  // namespace

int run_web(int argc, char** argv) {
    std::vector<std::string> files, rule_paths;
    std::string filter;
    bool follow = true, open = true, no_ai = false;
    int port = 4790;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        auto value = [&](const char* name) -> std::string {
            std::string prefix = std::string(name) + "=";
            if (a.rfind(prefix, 0) == 0) return a.substr(prefix.size());
            if (i + 1 >= argc) {
                std::fprintf(stderr, "culprit: %s needs a value\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        auto is = [&](const char* name) { return a == name || a.rfind(std::string(name) + "=", 0) == 0; };
        if (a == "-h" || a == "--help") {
            std::fputs(kWebHelp, stdout);
            return 0;
        } else if (a == "-F" || is("--filter")) {
            filter = value(a == "-F" ? "-F" : "--filter");
        } else if (is("--rules")) {
            rule_paths.push_back(value("--rules"));
        } else if (is("--port")) {
            std::string v = value("--port");
            char* end = nullptr;
            long p = std::strtol(v.c_str(), &end, 10);
            if (v.empty() || *end || p < 0 || p > 65535) {
                std::fprintf(stderr, "culprit: --port is a number from 1 to 65535 (0 for any free port)\n");
                return 2;
            }
            port = static_cast<int>(p);
        } else if (a == "-n" || a == "--no-follow") {
            follow = false;
        } else if (a == "--no-open") {
            open = false;
        } else if (a == "--no-ai") {
            no_ai = true;
        } else if (a == "-") {
            files.push_back("-");
        } else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "culprit: unknown option %s (see culprit web --help)\n", a.c_str());
            return 2;
        } else {
            files.push_back(a);
        }
    }
    if (files.empty() && stdin_is_terminal()) {
        std::fputs(kWebHelp, stdout);
        return 0;
    }
    if (!filter.empty()) {
        auto f = Filter::parse(filter);
        if (!f.error().empty()) {
            std::fprintf(stderr, "culprit: %s\n", f.error().c_str());
            return 2;
        }
    }

    web::Options o;
    std::vector<std::string> rule_errors;
    o.rules = load_rules(rule_paths, rule_errors);
    for (const auto& e : rule_errors) std::fprintf(stderr, "culprit: %s\n", e.c_str());
    o.rule_errors = rule_errors;
    Inputs in = make_inputs(files);
    o.inputs = in.list;
    o.follow = follow;
    o.title = in.title;
    o.version = CULPRIT_VERSION;
    o.filter = filter;
    o.no_ai = no_ai;
    o.port = port;
    o.on_read_error = [](const std::string& e) {
        std::fprintf(stderr, "culprit: %s\n", e.c_str());
        std::fflush(stderr);
    };
    std::error_code ec;
    o.project_start = std::filesystem::current_path(ec).string();

    web::Server server(std::move(o));
    std::string error;
    if (!server.start(&error)) {
        std::fprintf(stderr, "culprit: %s\n", error.c_str());
        return 1;
    }
    std::signal(SIGINT, on_interrupt);
    std::signal(SIGTERM, on_interrupt);

    const std::string url = server.url();
    std::fprintf(stderr, "culprit web is running at\n\n  %s\n\nOnly this computer can open it. Press Ctrl+C to stop.\n", url.c_str());
    if (open) {
        // xdg-open can wait for the browser on some desktops; don't hold up the server for it
        std::thread([url] {
            if (!web::open_in_browser(url)) std::fputs("(Open the address above in your browser.)\n", stderr);
        }).detach();
    }
    std::fflush(stderr);
    server.wait();
    return 0;
}

}  // namespace culprit::cli
