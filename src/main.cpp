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

#include "core/analyzer.hpp"
#include "core/filter.hpp"
#include "core/model.hpp"
#include "core/reader.hpp"
#include "core/rules.hpp"
#include "tui/plain.hpp"
#include "tui/report.hpp"
#include "tui/viewer.hpp"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#define isatty _isatty
#define fileno _fileno
#else
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace {

constexpr const char* kHelp = R"(culprit - read logs in the terminal, follow them live, and find what went wrong

Usage:
  culprit [options] [file...]          view log files (followed for new lines, like tail -F)
  command | culprit [options]          view what a command prints
  culprit analyze [options] [file...]  print what went wrong: the likely root cause, known problems
                                       and repeated errors (reads stdin when no file is given)
  culprit rules [--check] [id...]      list the rules culprit knows, show one, or check your own

Examples:
  culprit app.log
  docker compose logs -f | culprit
  kubectl logs -f deploy/api | culprit --filter "level>=warn"
  culprit analyze app.log
  docker compose logs --no-color | culprit analyze --format markdown > report.md

Options:
  -F, --filter <query>   start with a filter, e.g. "level>=warn payment"
  -n, --no-follow        read files to the end and stop watching them
  -p, --plain            print instead of opening the viewer (automatic when output is not a terminal)
      --no-stack         with --plain: leave out stack traces
      --no-color         no colours (also when NO_COLOR is set)
      --rules <path>     more rules: a .toml file or a folder of them (can be repeated)
  -v, --version
  -h, --help

Rules are also read from ~/.config/culprit/rules and from .culprit/rules in the current folder or
any folder above it. See https://github.com/amirizalrahmat0799/culprit/blob/main/docs/rules.md
In the viewer, press a for the problems it found, and ? for the keys.
)";

constexpr const char* kAnalyzeHelp = R"(culprit analyze - find what went wrong in a log

Usage:
  culprit analyze [options] [file...]
  command | culprit analyze [options]

Reads the logs to the end and prints the likely root cause, the problems the rules recognise, and the
errors no rule knows, grouped.

Options:
      --format <f>       text (default), markdown (for a pull request or chat), or json
      --fail-on <s>      exit with 1 when there's a problem at least this severe: critical, error,
                         warning; or never (default). For CI.
  -F, --filter <query>   analyse only the entries that match, e.g. "source=api"
      --top <n>          how many groups of unrecognised errors to list (default 10)
      --rules <path>     more rules: a .toml file or a folder of them (can be repeated)
      --no-color         no colours (also when NO_COLOR is set or the output isn't a terminal)
)";

constexpr const char* kRulesHelp = R"(culprit rules - the rules culprit uses to recognise problems

Usage:
  culprit rules                 list every rule: culprit's own and yours
  culprit rules <id>...         everything about these rules (an id prefix lists matching rules)
  culprit rules --check         check rule files: syntax, regexes, and that each rule recognises its examples

Options:
      --rules <path>     more rules: a .toml file or a folder of them (can be repeated)
      --no-color         no colours

Your rules go in .culprit/rules/*.toml in your project, or ~/.config/culprit/rules/*.toml.
A rule with the id of one of culprit's replaces it; `disabled = true` turns it off.
)";

int culprit_terminal_width() {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)) return info.srWindow.Right - info.srWindow.Left + 1;
#else
    struct winsize ws {};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) return ws.ws_col;
#endif
    return 100;
}

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

// culprit's rules, then the user's, the project's, and any given with --rules. Problems in rule files are
// added to `errors`; the rules that are fine still load.
std::shared_ptr<culprit::RuleSet> load_rules(const std::vector<std::string>& extra, std::vector<std::string>& errors) {
    auto rules = culprit::RuleSet::builtin();
    std::error_code ec;
    for (const auto& path : culprit::default_rule_paths(std::filesystem::current_path(ec).string())) rules->add_path(path, errors);
    for (const auto& path : extra) rules->add_path(path, errors);
    return rules;
}

struct Inputs {
    std::vector<std::string> files;
    std::vector<culprit::Reader::Input> list;
    std::string title;
};

Inputs make_inputs(std::vector<std::string> files) {
    Inputs in;
    if (files.empty()) files.push_back("-");
    for (const auto& f : files) {
        culprit::Reader::Input input;
        input.path = f;
        if (files.size() > 1) input.label = f == "-" ? "stdin" : std::filesystem::path(f).filename().string();
        in.list.push_back(input);
        if (!in.title.empty()) in.title += ", ";
        in.title += f == "-" ? "stdin" : std::filesystem::path(f).filename().string();
    }
    in.files = std::move(files);
    return in;
}

// Reads every input to the end, calling `each` for every entry in order.
template <typename F>
void read_all(const Inputs& in, F&& each) {
    culprit::Reader reader(in.list, false);
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

int run_analyze(int argc, char** argv) {
    std::vector<std::string> files, rule_paths;
    std::string filter, format = "text", fail_on = "never";
    size_t top = 10;
    bool color = std::getenv("NO_COLOR") == nullptr && stdout_is_terminal();
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
            std::fputs(kAnalyzeHelp, stdout);
            return 0;
        } else if (is("--format")) {
            format = value("--format");
        } else if (is("--fail-on")) {
            fail_on = value("--fail-on");
        } else if (a == "-F" || is("--filter")) {
            filter = a == "-F" ? (i + 1 < argc ? argv[++i] : "") : value("--filter");
        } else if (is("--top")) {
            top = static_cast<size_t>(std::max(0, std::atoi(value("--top").c_str())));
        } else if (is("--rules")) {
            rule_paths.push_back(value("--rules"));
        } else if (a == "--no-color") {
            color = false;
        } else if (a == "-") {
            files.push_back(a);
        } else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "culprit: unknown option %s (see culprit analyze --help)\n", a.c_str());
            return 2;
        } else {
            files.push_back(a);
        }
    }

    culprit::ReportOptions options;
    if (format == "text") options.format = culprit::ReportFormat::Text;
    else if (format == "markdown" || format == "md") options.format = culprit::ReportFormat::Markdown;
    else if (format == "json") options.format = culprit::ReportFormat::Json;
    else {
        std::fprintf(stderr, "culprit: --format is text, markdown or json\n");
        return 2;
    }
    int fail_level = -1;  // severity index, -1 = never
    if (fail_on == "critical") fail_level = static_cast<int>(culprit::Severity::Critical);
    else if (fail_on == "error") fail_level = static_cast<int>(culprit::Severity::Error);
    else if (fail_on == "warning" || fail_on == "warn") fail_level = static_cast<int>(culprit::Severity::Warning);
    else if (fail_on != "never") {
        std::fprintf(stderr, "culprit: --fail-on is critical, error, warning or never\n");
        return 2;
    }
    if (files.empty() && stdin_is_terminal()) {
        std::fputs(kAnalyzeHelp, stdout);
        return 0;
    }
    culprit::Filter f = culprit::Filter::parse(filter);
    if (!f.error().empty()) {
        std::fprintf(stderr, "culprit: %s\n", f.error().c_str());
        return 2;
    }

    std::vector<std::string> errors;
    auto rules = load_rules(rule_paths, errors);
    for (const auto& e : errors) std::fprintf(stderr, "culprit: %s\n", e.c_str());

    Inputs in = make_inputs(files);
    culprit::Analyzer analyzer(rules);
    uint32_t index = 0;
    read_all(in, [&](culprit::Entry& e) {
        if (f.matches(e)) analyzer.add(e, index++);
    });
    culprit::Report report = analyzer.report();

    options.color = color && options.format == culprit::ReportFormat::Text;
    options.title = in.title;
    options.top = top;
    if (stdout_is_terminal()) options.width = std::max(60, std::min(140, culprit_terminal_width()));
    culprit::print_report(stdout, report, options);

    if (fail_level >= 0) {
        for (const auto& p : report.problems) {
            if (static_cast<int>(p.rule->severity) >= fail_level) return 1;
        }
        // errors no rule knows count as errors
        if (fail_level <= static_cast<int>(culprit::Severity::Error)) {
            for (const auto& g : report.groups) {
                if (g.level >= culprit::Level::Error) return 1;
            }
        }
    }
    return 0;
}

int run_rules(int argc, char** argv) {
    std::vector<std::string> ids, rule_paths;
    bool check = false;
    bool color = std::getenv("NO_COLOR") == nullptr && stdout_is_terminal();
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            std::fputs(kRulesHelp, stdout);
            return 0;
        } else if (a == "--check") {
            check = true;
        } else if (a == "--rules" && i + 1 < argc) {
            rule_paths.push_back(argv[++i]);
        } else if (a.rfind("--rules=", 0) == 0) {
            rule_paths.push_back(a.substr(8));
        } else if (a == "--no-color") {
            color = false;
        } else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "culprit: unknown option %s (see culprit rules --help)\n", a.c_str());
            return 2;
        } else {
            ids.push_back(a);
        }
    }
    std::vector<std::string> errors;
    auto rules = load_rules(rule_paths, errors);

    if (check) {
        size_t examples = rules->check_examples(errors);
        size_t yours = 0, without = 0;
        for (const auto& r : rules->rules()) {
            if (r->origin == "built-in") continue;
            ++yours;
            if (r->examples.empty()) ++without;
        }
        for (const auto& e : errors) std::fprintf(stdout, "%s\n", e.c_str());
        std::printf("%zu rules (%zu yours), %zu examples checked, %zu problem%s\n", rules->rules().size(), yours, examples,
                    errors.size(), errors.size() == 1 ? "" : "s");
        if (without) {
            std::printf("%zu of your rules %s no examples; add examples = [\"...\"] so %s can be checked\n", without,
                        without == 1 ? "has" : "have", without == 1 ? "it" : "they");
        }
        return errors.empty() ? 0 : 1;
    }
    for (const auto& e : errors) std::fprintf(stderr, "culprit: %s\n", e.c_str());

    if (ids.empty()) {
        culprit::print_rule_list(stdout, *rules, color);
        return 0;
    }
    int status = 0;
    for (const auto& id : ids) {
        if (const auto* r = rules->find(id)) {
            culprit::print_rule(stdout, *r, color);
            continue;
        }
        // an id prefix lists the matching rules
        size_t n = 0;
        for (const auto& r : rules->rules()) n += r->id.rfind(id, 0) == 0;
        if (n == 0) {
            std::fprintf(stderr, "culprit: no rule %s (culprit rules lists them)\n", id.c_str());
            status = 1;
            continue;
        }
        for (const auto& r : rules->rules()) {
            if (r->id.rfind(id, 0) == 0) {
                std::printf("%s%-34s%s %s\n", color ? "\x1b[36m" : "", r->id.c_str(), color ? "\x1b[0m" : "", r->title.c_str());
            }
        }
    }
    return status;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "analyze") == 0) return run_analyze(argc, argv);
    if (argc > 1 && std::strcmp(argv[1], "analyse") == 0) return run_analyze(argc, argv);
    if (argc > 1 && std::strcmp(argv[1], "rules") == 0) return run_rules(argc, argv);

    std::vector<std::string> files, rule_paths;
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
        } else if (a == "--rules") {
            rule_paths.push_back(value("--rules"));
        } else if (a.rfind("--rules=", 0) == 0) {
            rule_paths.push_back(a.substr(8));
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

    if (files.empty() && stdin_is_terminal()) {
        std::fputs(kHelp, stdout);
        return 0;
    }
    if (!stdout_is_terminal()) plain = true;

    if (!filter.empty()) {
        auto f = culprit::Filter::parse(filter);
        if (!f.error().empty()) {
            std::fprintf(stderr, "culprit: %s\n", f.error().c_str());
            return 2;
        }
    }

    std::vector<std::string> rule_errors;
    auto rules = load_rules(rule_paths, rule_errors);
    Inputs in = make_inputs(files);
    bool reads_stdin = std::find(in.files.begin(), in.files.end(), "-") != in.files.end();

    if (plain) {
        // print entries as they come; files are read to the end
        for (const auto& e : rule_errors) std::fprintf(stderr, "culprit: %s\n", e.c_str());
        culprit::Filter f = culprit::Filter::parse(filter);
        culprit::Analyzer analyzer(rules);  // so problem: filters work here too
        culprit::PlainOptions options{color && stdout_is_terminal(), in.files.size() > 1, stack};
        bool sources_seen = false;
        std::string first_source;
        uint32_t index = 0;
        read_all(in, [&](culprit::Entry& e) {
            analyzer.add(e, index++);
            if (!e.source.empty()) {
                if (first_source.empty()) first_source = e.source;
                else if (e.source != first_source) sources_seen = true;
            }
            options.show_source = in.files.size() > 1 || sources_seen;
            if (f.matches(e)) culprit::print_plain(stdout, e, options);
            std::fflush(stdout);
        });
        return 0;
    }

    if (reads_stdin && !stdin_is_terminal()) {
        intptr_t copy = take_piped_stdin();
        if (copy == -1) {
            std::fputs("culprit: can't read the keyboard while logs are piped in; try --plain\n", stderr);
            return 1;
        }
        for (auto& input : in.list) {
            if (input.path == "-") input.native = copy;
        }
    }

    culprit::Reader reader(in.list, follow);
    culprit::ViewerOptions options;
    options.title = in.title;
    options.initial_filter = filter;
    options.rules = rules;
    if (!rule_errors.empty()) {
        options.message = rule_errors.size() == 1 ? rule_errors.front()
                                                  : std::to_string(rule_errors.size()) + " problems in rule files; run culprit rules --check";
    }
    return culprit::run_viewer(reader, options);
}
