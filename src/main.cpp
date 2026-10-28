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
#include "cli.hpp"
#ifdef CULPRIT_WITH_AI
#include "ai/subject.hpp"
#include "cli_ai.hpp"
#endif
#include "tui/plain.hpp"
#include "tui/report.hpp"
#include "tui/viewer.hpp"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
// without these, windows.h defines min and max as macros, which breaks std::min / std::max
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

namespace {

constexpr const char* kHelp = R"(culprit - read logs in the terminal, follow them live, and find what went wrong

Usage:
  culprit [options] [file...]          view log files (followed for new lines, like tail -F)
  command | culprit [options]          view what a command prints
  culprit analyze [options] [file...]  print what went wrong: the likely root cause, known problems
                                       and repeated errors (reads stdin when no file is given)
  culprit rules [--check] [id...]      list the rules culprit knows, show one, or check your own
  culprit investigate [file...]        ask an AI to find the root cause in your code (set it up first)
  culprit ai setup                     choose the AI: Ollama on this computer, a free cloud service,
                                       Claude or OpenAI with your key, or none

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
      --no-ai            don't use AI in this run
  -v, --version
  -h, --help

Rules are also read from ~/.config/culprit/rules and from .culprit/rules in the current folder or
any folder above it. See https://github.com/amirizalrahmat0799/culprit/blob/main/docs/rules.md
In the viewer, press a for the problems it found, i to investigate an error with AI, and ? for the keys.
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
      --investigate      also ask the AI (culprit ai setup) to investigate the likely root cause
      --yes              with --investigate: don't ask before sending to a provider that isn't local
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

using namespace culprit::cli;

int run_analyze(int argc, char** argv) {
    std::vector<std::string> files, rule_paths;
    std::string filter, format = "text", fail_on = "never";
    size_t top = 10;
    bool color = std::getenv("NO_COLOR") == nullptr && stdout_is_terminal();
    bool investigate = false, yes = false;
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
        } else if (a == "--investigate") {
            investigate = true;
        } else if (a == "--yes" || a == "-y") {
            yes = true;
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
    std::vector<culprit::Entry> kept;  // only for --investigate
    read_all(in, [&](culprit::Entry& e) {
        if (!f.matches(e)) return;
        analyzer.add(e, index++);
        if (investigate) kept.push_back(std::move(e));
    });
    culprit::Report report = analyzer.report();

    options.color = color && options.format == culprit::ReportFormat::Text;
    options.title = in.title;
    options.top = top;
    if (stdout_is_terminal()) options.width = std::max(60, std::min(140, terminal_width()));
    culprit::print_report(stdout, report, options);

    if (investigate) {
#ifdef CULPRIT_WITH_AI
        if (auto target = culprit::ai::default_target(report)) {
            *target = culprit::ai::richest_occurrence(*target, kept.size(), [&](uint32_t i) -> const culprit::Entry& { return kept[i]; });
            std::fputs(options.format == culprit::ReportFormat::Text ? "\n" : "", stdout);
            std::fflush(stdout);
            InvestigateOptions io;
            io.format = options.format;
            io.color = options.color;
            io.yes = yes;
            io.width = options.width;
            investigate_entry(kept, *target, report, *rules, io);
        }
#else
        (void)yes;
        std::fputs("culprit: this culprit was built without AI (CULPRIT_AI=OFF)\n", stderr);
#endif
    }

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
    if (argc > 1 && (std::strcmp(argv[1], "ai") == 0 || std::strcmp(argv[1], "investigate") == 0)) {
#ifdef CULPRIT_WITH_AI
        return std::strcmp(argv[1], "ai") == 0 ? run_ai(argc, argv) : run_investigate(argc, argv);
#else
        std::fputs("culprit: this culprit was built without AI (CULPRIT_AI=OFF)\n", stderr);
        return 2;
#endif
    }

    std::vector<std::string> files, rule_paths;
    std::string filter;
    bool follow = true, plain = false, color = std::getenv("NO_COLOR") == nullptr, stack = true, no_ai = false;

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
        } else if (a == "--no-ai") {
            no_ai = true;
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
    options.no_ai = no_ai;
    if (!rule_errors.empty()) {
        options.message = rule_errors.size() == 1 ? rule_errors.front()
                                                  : std::to_string(rule_errors.size()) + " problems in rule files; run culprit rules --check";
    }
    return culprit::run_viewer(reader, options);
}
