#include "cli_ai.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>

#include "ai/config.hpp"
#include "ai/investigator.hpp"
#include "ai/subject.hpp"
#include "cli.hpp"
#include "core/filter.hpp"
#include "tui/finding.hpp"
#include "tui/style.hpp"

namespace culprit::cli {
namespace {

constexpr const char* kAiHelp = R"(culprit ai - set up the AI that investigates errors

Usage:
  culprit ai setup      choose a model: Ollama on this computer, a free cloud service, an API key you
                        have (Claude, OpenAI ...), or your company's server; or none
  culprit ai status     show the settings and test the connection
  culprit ai on | off   turn the AI on or off without changing the settings

culprit never uses AI unless it's set up here, and only when you ask: `culprit investigate`, or i in
the viewer. Turn it off for one run with --no-ai, everywhere with CULPRIT_AI=off, or for a project with
`ai = false` in .culprit/config.toml.
)";

constexpr const char* kInvestigateHelp = R"(culprit investigate - ask the AI to find the root cause in your code

Usage:
  culprit investigate [options] [file...]
  command | culprit investigate [options]

Reads the logs, finds the likely root cause (as culprit analyze does), and asks the AI set up with
`culprit ai setup` to investigate it: it reads the code at the stack trace, searches the project, checks
recent git changes, and answers with evidence that culprit checks against your files.

Choose a different error:
      --line <n>         the entry starting at line n of the log
      --problem <id>     the first entry of a problem, e.g. hikari.pool-exhausted
      --group <n>        the first entry of a group of unrecognised errors

Options:
      --project <dir>    the project to read (default: the git repository you're in)
      --format <f>       text (default), markdown or json
      --yes              don't ask before sending to a provider that isn't on this computer
      --show-prompt      print what would be sent to the AI, and stop
      --model <name>     use another model this time
      --max-steps <n>    allow more or fewer steps
      --rules <path>     more rules
  -F, --filter <query>   only consider entries that match
      --no-ai            don't use AI (prints why it would have been used)
      --no-color
)";

std::string env_hint(const std::string& var) {
#ifdef _WIN32
    return "setx " + var + " \"your-key\"   (then open a new terminal)";
#else
    return "export " + var + "=\"your-key\"   (add it to ~/.bashrc or ~/.zshrc to keep it)";
#endif
}

int choose(const std::string& question, int count, int fallback) {
    while (true) {
        bool ok = false;
        std::string a = ask(question, &ok);
        if (!ok) return -1;
        if (a.empty() && fallback > 0) return fallback;
        try {
            int n = std::stoi(a);
            if (n >= 1 && n <= count) return n;
        } catch (...) {
        }
        std::fprintf(stderr, "  Type a number from 1 to %d.\n", count);
    }
}

// lets the user pick a model from the provider's list, or type one
std::string pick_model(ai::Settings& s, ai::Transport& t, const std::string& hint) {
    auto model = ai::make_model(s, t);
    std::string error;
    std::vector<std::string> models = model->list_models(&error);
    if (s.provider == "openrouter") {
        std::vector<std::string> free;
        for (const auto& m : models) {
            if (m.size() > 5 && m.compare(m.size() - 5, 5, ":free") == 0) free.push_back(m);
        }
        if (!free.empty()) models = free;
    }
    if (models.empty()) {
        if (!error.empty()) std::fprintf(stderr, "  (couldn't list the models: %s)\n", error.c_str());
        bool ok = false;
        std::string typed = ask("  Model name" + (hint.empty() ? std::string() : " [" + hint + "]") + ": ", &ok);
        return typed.empty() ? hint : typed;
    }
    size_t show = std::min<size_t>(models.size(), 30);
    int suggested = 0;
    for (size_t i = 0; i < models.size(); ++i) {
        if (!hint.empty() && models[i] == hint) suggested = static_cast<int>(i) + 1;
    }
    if (suggested > static_cast<int>(show)) {
        std::swap(models[0], models[static_cast<size_t>(suggested - 1)]);
        suggested = 1;
    }
    std::fprintf(stderr, "\n  Models:\n");
    for (size_t i = 0; i < show; ++i) {
        std::fprintf(stderr, "  %3zu. %s%s\n", i + 1, models[i].c_str(), static_cast<int>(i) + 1 == suggested ? "  (suggested)" : "");
    }
    if (models.size() > show) std::fprintf(stderr, "       ... %zu more; type a name to use one of them\n", models.size() - show);
    while (true) {
        bool ok = false;
        std::string a = ask("  Choose a model (number or name)" + std::string(suggested ? " [" + std::to_string(suggested) + "]" : "") + ": ", &ok);
        if (!ok) return {};
        if (a.empty() && suggested) return models[static_cast<size_t>(suggested - 1)];
        if (a.empty()) continue;
        if (std::all_of(a.begin(), a.end(), [](char c) { return c >= '0' && c <= '9'; })) {
            size_t n = std::stoul(a);
            if (n >= 1 && n <= show) return models[n - 1];
            continue;
        }
        return a;
    }
}

// a one-line test: can we talk to it?
bool test_connection(const ai::Settings& s, ai::Transport& t, std::string& detail) {
    auto model = ai::make_model(s, t);
    std::vector<ai::Message> m{{ai::Message::Role::User, "Reply with just the word OK.", {}, {}, {}}};
    ai::Reply r = model->chat("You are a connection test.", m, {}, nullptr);
    if (!r.ok) {
        detail = r.error;
        return false;
    }
    detail = r.text.substr(0, 60);
    return true;
}

int setup() {
    if (!stdin_is_terminal()) {
        std::fputs("culprit ai setup asks questions; run it in a terminal\n", stderr);
        return 2;
    }
    auto transport = ai::system_transport();
    ai::Settings s;
    if (auto existing = ai::load_settings()) s = *existing;

    std::fputs("\nWhich AI should culprit use to investigate errors?\n\n"
               "  1. A model on this computer (Ollama)   free and private: nothing leaves your machine;\n"
               "                                          needs Ollama and about 8 GB of free memory\n"
               "  2. A free cloud service                 no install: Groq, OpenRouter, Cerebras, Google Gemini\n"
               "  3. An API key you already have          Anthropic (Claude), OpenAI, Mistral, DeepSeek\n"
               "  4. Your company's AI server             any OpenAI-compatible URL\n"
               "  5. None                                 turn the AI off; everything else works without it\n\n",
               stderr);
    int kind = choose("Choose 1-5: ", 5, 0);
    if (kind < 0) return 1;

    if (kind == 5) {
        s.enabled = false;
        std::string err = ai::save_settings(s);
        if (!err.empty()) return std::fprintf(stderr, "culprit: %s\n", err.c_str()), 1;
        std::fprintf(stderr, "\nAI is off. Turn it on again with: culprit ai on (or culprit ai setup)\n");
        return 0;
    }

    const ai::Preset* preset = nullptr;
    if (kind == 1) {
        preset = ai::find_preset("ollama");
    } else if (kind == 2 || kind == 3) {
        std::vector<const ai::Preset*> list;
        for (const char* id : kind == 2 ? std::vector<const char*>{"groq", "openrouter", "cerebras", "gemini"}
                                        : std::vector<const char*>{"anthropic", "openai", "mistral", "deepseek"}) {
            list.push_back(ai::find_preset(id));
        }
        std::fputs("\n", stderr);
        for (size_t i = 0; i < list.size(); ++i) std::fprintf(stderr, "  %zu. %-20s %s\n", i + 1, list[i]->name, list[i]->note);
        int n = choose("\nChoose 1-" + std::to_string(list.size()) + ": ", static_cast<int>(list.size()), 0);
        if (n < 0) return 1;
        preset = list[static_cast<size_t>(n - 1)];
    }

    if (preset) {
        s.provider = preset->id;
        s.name = preset->name;
        s.url = preset->url;
        s.api_key_env = preset->key_env;
    } else {
        bool ok = false;
        s.provider = "openai";
        s.name = "Company server";
        std::string url = ask("\n  The server's OpenAI-compatible URL (ending in /v1), e.g. https://ai.example.com/v1: ", &ok);
        if (!ok || url.empty()) return 1;
        while (!url.empty() && url.back() == '/') url.pop_back();
        s.url = url;
        s.api_key_env = ask("  The environment variable with its API key (empty if none): ", &ok);
    }

    if (s.provider == "ollama") {
        std::string error;
        auto models = ai::make_model(s, *transport)->list_models(&error);
        if (models.empty()) {
            std::fprintf(stderr,
                         "\nOllama isn't answering at %s%s.\n\n"
                         "  1. Install it from https://ollama.com/download (Windows, macOS, Linux)\n"
                         "  2. Download a model:  ollama pull qwen3:8b      (about 5 GB; fits an 8 GB GPU)\n"
                         "                    or  ollama pull qwen3-coder:30b  (smarter, needs ~24 GB of memory)\n"
                         "  3. Run culprit ai setup again\n",
                         s.url.c_str(), error.find("listed no models") != std::string::npos ? " with any models" : "");
            return 1;
        }
    } else if (!s.api_key_env.empty() && s.api_key().empty()) {
        std::fprintf(stderr,
                     "\n%s needs an API key in the environment variable %s, and it isn't set in this terminal.\n"
                     "%s%s%s"
                     "  Set it:  %s\n"
                     "  culprit reads the key from there and never stores it.\n",
                     s.name.c_str(), s.api_key_env.c_str(), preset ? "  Get one: " : "", preset ? preset->note : "", preset ? "\n" : "",
                     env_hint(s.api_key_env).c_str());
        if (!ask_yes_no("\nSave these settings anyway, and set the key later?", true)) return 1;
        s.model = preset && *preset->model_hint ? preset->model_hint : s.model;
        if (s.model.empty()) {
            bool ok = false;
            s.model = ask("  Model name: ", &ok);
        }
        s.enabled = true;
        std::string err = ai::save_settings(s);
        if (!err.empty()) return std::fprintf(stderr, "culprit: %s\n", err.c_str()), 1;
        std::fprintf(stderr, "\nSaved to %s. Set %s, then check it with: culprit ai status\n", ai::settings_path().c_str(),
                     s.api_key_env.c_str());
        return 0;
    }

    s.model = pick_model(s, *transport, preset ? preset->model_hint : "");
    if (s.model.empty()) return 1;

    std::fprintf(stderr, "\nTesting %s with %s ... ", s.name.c_str(), s.model.c_str());
    std::string detail;
    bool works = test_connection(s, *transport, detail);
    std::fprintf(stderr, "%s\n", works ? "it answers." : "it didn't work:");
    if (!works) {
        std::fprintf(stderr, "  %s\n", detail.c_str());
        if (!ask_yes_no("Save these settings anyway?", false)) return 1;
    }

    if (!s.local()) {
        std::fprintf(stderr,
                     "\nInvestigations send the error, nearby log lines and the project files the AI chooses to read\n"
                     "to %s. Files that may hold secrets are never read, and values like passwords are redacted.\n",
                     s.host().c_str());
        s.confirm = ask_yes_no("Ask before each investigation sends anything?", true);
    }
    s.enabled = true;
    std::string err = ai::save_settings(s);
    if (!err.empty()) return std::fprintf(stderr, "culprit: %s\n", err.c_str()), 1;
    std::fprintf(stderr,
                 "\nDone. Saved to %s\n\n"
                 "  Investigate the likely root cause:   culprit investigate app.log\n"
                 "  In the viewer:                       select an error and press i\n"
                 "  Turn it off again:                   culprit ai off\n",
                 ai::settings_path().c_str());
    return 0;
}

int status() {
    auto avail = ai::availability(false, ".");
    std::string error;
    auto s = ai::load_settings(&error);
    if (!s) {
        std::printf("%s\n", error.empty() ? "AI isn't set up. Run: culprit ai setup" : error.c_str());
        return 1;
    }
    std::printf("Settings:  %s\n", ai::settings_path().c_str());
    std::printf("Enabled:   %s\n", s->enabled ? "yes" : "no");
    std::printf("Provider:  %s  %s%s\n", s->name.c_str(), s->url.c_str(), s->local() ? "  (on this computer)" : "");
    std::printf("Model:     %s\n", s->model.c_str());
    if (!s->api_key_env.empty()) std::printf("API key:   %s %s\n", s->api_key_env.c_str(), s->api_key().empty() ? "(not set!)" : "(set)");
    std::printf("Steps:     up to %d per investigation\n", s->max_steps);
    if (!s->local()) std::printf("Confirm:   %s\n", s->confirm ? "asks before sending" : "sends without asking");
    if (!avail.allowed) {
        std::printf("\nNot available here: %s\n", avail.reason.c_str());
        return 1;
    }
    auto transport = ai::system_transport();
    std::printf("\nTesting ... ");
    std::fflush(stdout);
    std::string detail;
    if (!test_connection(*s, *transport, detail)) {
        std::printf("failed:\n  %s\n", detail.c_str());
        return 1;
    }
    std::printf("it answers (\"%s\").\n", detail.c_str());
    return 0;
}

int toggle(bool on) {
    std::string error;
    auto s = ai::load_settings(&error);
    if (!s) {
        std::fprintf(stderr, "%s\n", error.empty() ? "AI isn't set up yet. Run: culprit ai setup" : error.c_str());
        return 1;
    }
    s->enabled = on;
    std::string err = ai::save_settings(*s);
    if (!err.empty()) return std::fprintf(stderr, "culprit: %s\n", err.c_str()), 1;
    std::printf("AI is %s (%s, %s).\n", on ? "on" : "off", s->name.c_str(), s->model.c_str());
    return 0;
}

}  // namespace

int run_ai(int argc, char** argv) {
    std::string sub = argc > 2 ? argv[2] : "";
    if (sub == "setup") return setup();
    if (sub == "status") return status();
    if (sub == "on") return toggle(true);
    if (sub == "off") return toggle(false);
    std::fputs(kAiHelp, sub.empty() || sub == "-h" || sub == "--help" ? stdout : stderr);
    return sub.empty() || sub == "-h" || sub == "--help" ? 0 : 2;
}

int investigate_entry(const std::vector<Entry>& entries, uint32_t index, const Report& report, const RuleSet& rules,
                      const InvestigateOptions& o) {
    std::error_code ec;
    std::string start = o.project.empty() ? std::filesystem::current_path(ec).string() : o.project;
    auto avail = ai::availability(o.no_ai, start);
    if (!avail.allowed && !o.show_prompt) {
        std::fprintf(stderr, "culprit: %s\n", avail.reason.c_str());
        return 2;
    }
    ai::Settings s = avail.allowed ? avail.settings : ai::Settings{};
    if (!o.model.empty()) s.model = o.model;
    if (o.max_steps > 0) s.max_steps = std::min(o.max_steps, 40);

    const Entry& e = entries[index];
    std::vector<const Entry*> before;
    for (uint32_t i = index >= 20 ? index - 20 : 0; i < index; ++i) before.push_back(&entries[i]);
    ai::Subject subject = ai::make_subject(e, before, &report, &rules);
    subject.search_log = [&entries](const std::string& text) { return ai::search_entries(entries, text); };

    auto transport = ai::system_transport();
    auto model = ai::make_model(s, *transport);
    ai::Workspace workspace(ai::find_project_root(start));
    ai::Investigator investigator(*model, workspace, s);

    if (o.show_prompt) {
        std::printf("=== system ===\n%s\n\n=== first message ===\n%s\n", ai::Investigator::system_prompt(s.max_steps).c_str(),
                    investigator.first_message(subject).c_str());
        return 0;
    }
    if (!s.local() && s.confirm && !o.yes) {
        std::string first = investigator.first_message(subject);
        std::fprintf(stderr,
                     "This investigation sends to %s (%s):\n"
                     "  - the error, its stack trace and the %zu log lines before it, and a summary of the log (%s characters)\n"
                     "  - the names of the project's files, and the files the AI chooses to read\n"
                     "Files that may hold secrets are never read; values like passwords are redacted.\n"
                     "See exactly what's sent first with --show-prompt.\n",
                     s.host().c_str(), s.name.c_str(), before.size(), thousands(first.size()).c_str());
        bool can_ask = false;
        bool yes = false;
        {
            std::string a = ask("Send it? [y/N] ", &can_ask);
            yes = !a.empty() && (a[0] == 'y' || a[0] == 'Y');
        }
        if (!can_ask) {
            std::fprintf(stderr, "culprit: no terminal to ask on; add --yes to send to %s without asking\n", s.host().c_str());
            return 2;
        }
        if (!yes) return 1;
    }

    const bool text = o.format == ReportFormat::Text;
    std::FILE* progress = text ? stdout : stderr;
    const bool progress_color = text ? o.color : (o.color && stderr_is_terminal());
    std::fprintf(progress, "%s%s%s\n  %s\n  %swith %s · %s%s%s\n\n", o.color && text ? "\x1b[1m" : "", subject.title.c_str(),
                 o.color && text ? "\x1b[0m" : "", subject.where.c_str(), o.color && text ? "\x1b[2m" : "", s.name.c_str(),
                 s.model.c_str(), s.local() ? " (on this computer)" : "", o.color && text ? "\x1b[0m" : "");
    ai::Finding f = investigator.run(subject, [&](const ai::Step& step) { print_step(progress, step, progress_color); }, nullptr);
    print_finding(stdout, f, subject, o.format, o.color, o.width);
    return f.ok ? 0 : 1;
}

int run_investigate(int argc, char** argv) {
    std::vector<std::string> files, rule_paths;
    std::string filter, format = "text", problem;
    long line = -1, group = -1;
    InvestigateOptions o;
    o.color = std::getenv("NO_COLOR") == nullptr && stdout_is_terminal();
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
            std::fputs(kInvestigateHelp, stdout);
            return 0;
        } else if (is("--line")) {
            line = std::atol(value("--line").c_str());
        } else if (is("--problem")) {
            problem = value("--problem");
        } else if (is("--group")) {
            group = std::atol(value("--group").c_str());
        } else if (is("--project")) {
            o.project = value("--project");
        } else if (is("--format")) {
            format = value("--format");
        } else if (is("--model")) {
            o.model = value("--model");
        } else if (is("--max-steps")) {
            o.max_steps = std::atoi(value("--max-steps").c_str());
        } else if (is("--rules")) {
            rule_paths.push_back(value("--rules"));
        } else if (a == "-F" || is("--filter")) {
            filter = a == "-F" ? (i + 1 < argc ? argv[++i] : "") : value("--filter");
        } else if (a == "--yes" || a == "-y") {
            o.yes = true;
        } else if (a == "--show-prompt") {
            o.show_prompt = true;
        } else if (a == "--no-ai") {
            o.no_ai = true;
        } else if (a == "--no-color") {
            o.color = false;
        } else if (a == "-") {
            files.push_back(a);
        } else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "culprit: unknown option %s (see culprit investigate --help)\n", a.c_str());
            return 2;
        } else {
            files.push_back(a);
        }
    }
    if (format == "text") o.format = ReportFormat::Text;
    else if (format == "markdown" || format == "md") o.format = ReportFormat::Markdown;
    else if (format == "json") o.format = ReportFormat::Json;
    else return std::fprintf(stderr, "culprit: --format is text, markdown or json\n"), 2;
    if (o.format != ReportFormat::Text) o.color = false;
    if (files.empty() && stdin_is_terminal()) {
        std::fputs(kInvestigateHelp, stdout);
        return 0;
    }
    // say early when AI can't be used, before reading a big log
    std::error_code ec;
    auto avail = ai::availability(o.no_ai, o.project.empty() ? std::filesystem::current_path(ec).string() : o.project);
    if (!avail.allowed && !o.show_prompt) {
        std::fprintf(stderr, "culprit: %s\n", avail.reason.c_str());
        return 2;
    }

    Filter f = Filter::parse(filter);
    if (!f.error().empty()) return std::fprintf(stderr, "culprit: %s\n", f.error().c_str()), 2;
    std::vector<std::string> errors;
    auto rules = load_rules(rule_paths, errors);
    for (const auto& e : errors) std::fprintf(stderr, "culprit: %s\n", e.c_str());

    Inputs in = make_inputs(files);
    std::vector<Entry> entries;
    Analyzer analyzer(rules);
    if (!read_all(in, [&](Entry& e) {
            if (!f.matches(e)) return;
            analyzer.add(e, static_cast<uint32_t>(entries.size()));
            entries.push_back(std::move(e));
        })) {
        return 2;
    }
    Report report = analyzer.report();

    std::optional<uint32_t> target;
    if (line >= 0) {
        for (uint32_t i = 0; i < entries.size() && !target; ++i) {
            if (entries[i].line == static_cast<uint32_t>(line)) target = i;
        }
        if (!target) return std::fprintf(stderr, "culprit: no entry starts at line %ld\n", line), 2;
    } else if (!problem.empty()) {
        for (const auto& p : report.problems) {
            if (p.rule->id == problem || p.rule->id.rfind(problem, 0) == 0) {
                target = p.first.index;
                break;
            }
        }
        if (!target) return std::fprintf(stderr, "culprit: no problem %s in this log (culprit analyze lists them)\n", problem.c_str()), 2;
    } else if (group >= 0) {
        for (const auto& g : report.groups) {
            if (g.id == static_cast<uint32_t>(group)) target = g.first.index;
        }
        if (!target) return std::fprintf(stderr, "culprit: no group %ld\n", group), 2;
    } else {
        target = ai::default_target(report);
        if (!target) {
            std::fprintf(stderr, "culprit: no errors in %s to investigate\n", in.title.c_str());
            return 0;
        }
    }
    if (stdout_is_terminal()) o.width = std::max(60, std::min(120, terminal_width()));
    if (line < 0) *target = ai::richest_occurrence(*target, entries.size(), [&](uint32_t i) -> const Entry& { return entries[i]; });
    return investigate_entry(entries, *target, report, *rules, o);
}

}  // namespace culprit::cli
