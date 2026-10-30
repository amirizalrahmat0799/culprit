#include "ai/investigator.hpp"

#include <algorithm>

namespace culprit::ai {
namespace {

constexpr size_t kMaxSentBytes = 400 * 1024;  // stop reading more once this much has gone to the model

// whitespace collapsed, for comparing a quote with the file
std::string squash(std::string_view s) {
    std::string out;
    bool space = false;
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            space = !out.empty();
            continue;
        }
        if (space) out += ' ';
        space = false;
        out += c;
    }
    return out;
}

// "87: foo();" -> "foo();" (models often copy the line numbers read_file adds)
std::string strip_line_numbers(const std::string& quote) {
    std::string out;
    size_t start = 0;
    while (start <= quote.size()) {
        size_t nl = quote.find('\n', start);
        std::string line = quote.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        size_t i = 0;
        while (i < line.size() && line[i] == ' ') ++i;
        size_t d = i;
        while (d < line.size() && line[d] >= '0' && line[d] <= '9') ++d;
        if (d > i && d + 1 < line.size() + 1 && d < line.size() && line[d] == ':') line = line.substr(d + 1);
        out += line;
        if (nl == std::string::npos) break;
        out += '\n';
        start = nl + 1;
    }
    return out;
}

std::string arg_string(const Json& args, const char* key) {
    const Json& v = args[key];
    if (v.is_string()) return v.str();
    if (v.is_number()) return std::to_string(v.integer());
    return {};
}

int arg_int(const Json& args, const char* key, int fallback) {
    const Json& v = args[key];
    // numbers from the model: clamp to a range that line arithmetic (line - 1 - d) can't overflow
    auto clamp = [](int64_t n) { return static_cast<int>(std::clamp<int64_t>(n, -1000000000, 1000000000)); };
    if (v.is_number()) return clamp(v.integer());
    if (v.is_string() && !v.str().empty()) {
        try {
            return clamp(std::stoll(v.str()));
        } catch (...) {
        }
    }
    return fallback;
}

Json prop(const char* type, const char* description) {
    return Json::object({{"type", type}, {"description", description}});
}

}  // namespace

Investigator::Investigator(Model& model, Workspace& workspace, Settings settings)
    : model_(model), ws_(workspace), settings_(std::move(settings)) {}

std::string Investigator::system_prompt(int max_steps) {
    return "You are culprit's investigator. A program failed, and culprit, a log analyser, has collected the error. "
           "Your job: find the root cause in the project's code, configuration or recent changes, and say how to fix it.\n\n"
           "You can only read. Use the tools to open files, search the code, look at recent git changes and search the "
           "log. A good order: read the code at the stack frames that are in the project; follow the bad value or state "
           "back to where it comes from; check recent changes to those files; then call finish.\n\n"
           "Rules:\n"
           "- You have at most " + std::to_string(max_steps) + " steps. Each step can call several tools.\n"
           "- Don't guess what a file says: read it.\n"
           "- Evidence quotes must be copied exactly from read_file output (without the 'N: ' line-number prefix), with "
           "the file and line number. culprit checks every quote against the real file and marks the ones that don't match.\n"
           "- If the cause isn't in the code (configuration, the database, another service, the infrastructure), say "
           "that, and give the evidence you have from the log and the files.\n"
           "- Some files can't be read and some values show as [redacted]: they may hold secrets. Don't ask for them.\n"
           "- Be concrete and brief. The reader is a developer who wants to fix this now.\n"
           "- Finish by calling the finish tool. If you can't find the cause, call finish anyway with what you found and "
           "confidence low.";
}

std::vector<ToolSpec> Investigator::tools() {
    std::vector<ToolSpec> t;
    t.push_back({"read_file", "Read lines of a file in the project, with line numbers. Paths from stack traces work too (bare "
                              "file names, or absolute paths from a container).",
                 Json::object({{"type", "object"},
                               {"properties", Json::object({{"path", prop("string", "File path, relative to the project")},
                                                            {"start_line", prop("integer", "First line (default 1)")},
                                                            {"end_line", prop("integer", "Last line (at most 250 lines at once)")}})},
                               {"required", Json::array({"path"})}})});
    t.push_back({"search", "Search the project's files for text (not a regex; case is ignored). Returns file:line: text.",
                 Json::object({{"type", "object"},
                               {"properties", Json::object({{"text", prop("string", "Text to look for")},
                                                            {"under", prop("string", "Only in this folder (optional)")}})},
                               {"required", Json::array({"text"})}})});
    t.push_back({"find_files", "Find files by name or the end of their path, e.g. OrderService.java or order/service.py.",
                 Json::object({{"type", "object"},
                               {"properties", Json::object({{"name", prop("string", "File name or path end")}})},
                               {"required", Json::array({"name"})}})});
    t.push_back({"list_dir", "List a folder in the project.",
                 Json::object({{"type", "object"},
                               {"properties", Json::object({{"path", prop("string", "Folder, relative to the project ('.' for the root)")}})},
                               {"required", Json::array({"path"})}})});
    t.push_back({"git_history", "Uncommitted changes and recent commits with their diffs, for one file, or the latest commits of "
                                "the whole project when no path is given.",
                 Json::object({{"type", "object"},
                               {"properties", Json::object({{"path", prop("string", "File (optional)")}})}})});
    t.push_back({"search_log", "Search the whole log being analysed for text (case is ignored). Returns matching entries.",
                 Json::object({{"type", "object"},
                               {"properties", Json::object({{"text", prop("string", "Text to look for")}})},
                               {"required", Json::array({"text"})}})});
    Json evidence_item = Json::object(
        {{"type", "object"},
         {"properties", Json::object({{"file", prop("string", "Project-relative path")},
                                      {"line", prop("integer", "Line number of the quote's first line")},
                                      {"quote", prop("string", "The exact text from the file (one or a few lines)")}})},
         {"required", Json::array({"file", "line", "quote"})}});
    Json confidence = prop("string", "How sure you are");
    confidence.set("enum", Json::array({"high", "medium", "low"}));
    t.push_back({"finish", "Give your answer. Call this once, at the end.",
                 Json::object({{"type", "object"},
                               {"properties",
                                Json::object({{"summary", prop("string", "The root cause, in one sentence")},
                                              {"explanation", prop("string", "How it leads to the error, in a few sentences")},
                                              {"fix", Json::object({{"type", "array"}, {"items", Json::object({{"type", "string"}})},
                                                                    {"description", "Steps to fix it, most important first"}})},
                                              {"evidence", Json::object({{"type", "array"}, {"items", evidence_item},
                                                                         {"description", "Quotes from the project's files"}})},
                                              {"confidence", confidence}})},
                               {"required", Json::array({"summary", "explanation", "confidence"})}})});
    return t;
}

std::string Investigator::first_message(const Subject& s) {
    std::string m = "Investigate this error.\n\n";
    m += "Error: " + s.title + "\n";
    if (!s.where.empty()) m += "Where in the log: " + s.where + "\n";
    m += s.known.empty() ? "No culprit rule recognises this error.\n" : "What culprit's rules say: " + s.known + "\n";

    std::string entry = s.entry;
    if (entry.size() > 12000) entry = entry.substr(0, 12000) + "\n[... cut]";
    m += "\nThe log entry:\n```\n" + entry + "\n```\n";

    if (!s.frames.empty()) {
        m += "\nStack frames in this project:\n";
        for (const auto& f : s.frames) {
            std::string file = f.substr(0, f.rfind(':'));
            std::string why;
            auto rel = ws_.resolve(file, &why);
            m += "- " + f + (rel ? "  ->  " + *rel : "  (" + why + ")") + "\n";
        }
    }
    if (!s.context.empty()) {
        std::string ctx = s.context;
        if (ctx.size() > 6000) ctx = "[...]\n" + ctx.substr(ctx.size() - 6000);
        m += "\nLog just before it:\n```\n" + ctx + "\n```\n";
    }
    if (!s.analysis.empty()) m += "\nculprit's analysis of the whole log:\n" + s.analysis + "\n";
    m += "\n" + ws_.overview() + "\n";
    return m;
}

std::string Investigator::run_tool(const ToolCall& call, const Subject& subject, const std::function<void(const Step&)>& on_step) {
    const Json& a = call.arguments;
    auto say = [&](const std::string& text) { on_step({Step::Kind::Tool, text}); };
    if (call.name == "read_file") {
        std::string path = arg_string(a, "path");
        int start = arg_int(a, "start_line", 1), end = arg_int(a, "end_line", 0);
        say("reading " + path + (a.has("start_line") ? ":" + std::to_string(start) + (end ? "-" + std::to_string(end) : "") : ""));
        return ws_.read_file(path, start, end);
    }
    if (call.name == "search") {
        std::string text = arg_string(a, "text"), under = arg_string(a, "under");
        say("searching for \"" + text + "\"" + (under.empty() ? "" : " in " + under));
        return ws_.search(text, under);
    }
    if (call.name == "find_files") {
        say("finding " + arg_string(a, "name"));
        return ws_.find_files(arg_string(a, "name"));
    }
    if (call.name == "list_dir") {
        std::string p = arg_string(a, "path");
        say("listing " + (p.empty() ? "." : p));
        return ws_.list_dir(p);
    }
    if (call.name == "git_history") {
        std::string p = arg_string(a, "path");
        say("checking git history" + (p.empty() ? "" : " of " + p));
        return ws_.git_history(p);
    }
    if (call.name == "search_log") {
        std::string text = arg_string(a, "text");
        say("searching the log for \"" + text + "\"");
        if (!subject.search_log) return "error: the log isn't available";
        return subject.search_log(text);
    }
    on_step({Step::Kind::Warning, "the model asked for an unknown tool: " + call.name});
    return "error: there is no tool called " + call.name + ". Use read_file, search, find_files, list_dir, git_history, "
           "search_log, or finish.";
}

void Investigator::verify(Finding& f) {
    size_t bad = 0;
    for (auto& ev : f.evidence) {
        std::string quote = squash(strip_line_numbers(ev.quote));
        if (quote.empty()) {
            ev.note = "no quote";
            ++bad;
            continue;
        }
        std::string why;
        auto rel = ws_.resolve(ev.file, &why);
        if (!rel) {
            ev.note = why.empty() ? "no such file in the project" : why;
            ++bad;
            continue;
        }
        ev.file = *rel;
        auto ls = ws_.lines(*rel);
        if (!ls) {
            ev.note = "the file can't be read";
            ++bad;
            continue;
        }
        const int n = static_cast<int>(ls->size());
        // the quote can span lines: compare against runs of lines starting at each candidate
        int quote_lines = static_cast<int>(std::count(ev.quote.begin(), ev.quote.end(), '\n')) + 1;
        auto matches_at = [&](int start) {
            std::string joined;
            for (int i = start; i < std::min(n, start + quote_lines + 2); ++i) joined += (*ls)[static_cast<size_t>(i)] + "\n";
            return squash(joined).find(quote) != std::string::npos;
        };
        int found = -1;
        for (int d = 0; d <= 6 && found < 0; ++d) {
            for (int cand : {ev.line - 1 - d, ev.line - 1 + d}) {
                if (cand >= 0 && cand < n && matches_at(cand)) {
                    found = cand;
                    break;
                }
            }
        }
        for (int i = 0; i < n && found < 0; ++i) {
            if (matches_at(i)) found = i;
        }
        if (found < 0) {
            ev.note = "this text isn't in the file";
            ++bad;
            continue;
        }
        // the first line of the quote within the run
        for (int i = found; i < std::min(n, found + quote_lines + 2); ++i) {
            std::string first = squash(strip_line_numbers(ev.quote.substr(0, ev.quote.find('\n'))));
            if (!first.empty() && squash((*ls)[static_cast<size_t>(i)]).find(first) != std::string::npos) {
                found = i;
                break;
            }
        }
        if (ev.line != found + 1) ev.note = "found at line " + std::to_string(found + 1) + ", not " + std::to_string(ev.line);
        ev.line = found + 1;
        ev.verified = true;
    }
    if (f.confidence != "high" && f.confidence != "medium" && f.confidence != "low") f.confidence = "low";
    if (bad > 0) {
        f.confidence = f.confidence == "high" ? "medium" : "low";
        f.confidence_note = std::to_string(bad) + " of " + std::to_string(f.evidence.size()) +
                            " quotes didn't match the files, so confidence was lowered";
    } else if (f.evidence.empty() && f.confidence == "high") {
        f.confidence = "medium";
        f.confidence_note = "no evidence was quoted from the files";
    }
}

Finding Investigator::run(const Subject& subject, const std::function<void(const Step&)>& on_step, const std::atomic<bool>* cancel) {
    Finding f;
    f.provider = settings_.name;
    f.model = settings_.model;
    f.local = settings_.local();
    const std::string system = system_prompt(settings_.max_steps);
    const auto tool_list = tools();

    std::vector<Message> messages;
    messages.push_back({Message::Role::User, first_message(subject), {}, {}, {}});
    size_t sent = system.size() + messages.back().text.size();
    bool nudged = false;

    for (int step = 1; step <= settings_.max_steps + 1; ++step) {
        if (cancel && cancel->load()) {
            f.error = "cancelled";
            break;
        }
        const bool last = step >= settings_.max_steps;
        if (last || sent > kMaxSentBytes) {
            messages.push_back({Message::Role::User,
                                "That's the last step: call finish now with what you've found, even if you're not sure.", {}, {}, {}});
        }
        on_step({Step::Kind::Thinking, f.model + " is thinking (step " + std::to_string(step) + ")"});
        Reply r = model_.chat(system, messages, last || sent > kMaxSentBytes ? std::vector<ToolSpec>{tool_list.back()} : tool_list, cancel);
        f.steps = step;
        f.input_tokens += r.input_tokens;
        f.output_tokens += r.output_tokens;
        if (!r.ok) {
            f.error = r.error;
            break;
        }
        if (!r.text.empty() && !r.calls.empty()) on_step({Step::Kind::Note, r.text});

        if (r.calls.empty()) {
            // a plain-text answer: ask once for the structured one, then take what there is
            if (!nudged && step < settings_.max_steps + 1) {
                nudged = true;
                messages.push_back({Message::Role::Assistant, r.text, {}, {}, {}});
                messages.push_back({Message::Role::User,
                                    "Please give your answer by calling the finish tool, with evidence quoted from the files.", {}, {}, {}});
                continue;
            }
            f.ok = !r.text.empty();
            if (!f.ok) f.error = "the model gave no answer";
            f.structured = false;
            f.summary = r.text.substr(0, r.text.find('\n'));
            f.explanation = r.text;
            f.confidence = "low";
            f.confidence_note = "the model answered in plain text, without evidence culprit could check";
            break;
        }

        messages.push_back({Message::Role::Assistant, r.text, r.calls, {}, {}});
        bool finished = false;
        for (const auto& call : r.calls) {
            if (call.name == "finish") {
                const Json& a = call.arguments;
                f.summary = arg_string(a, "summary");
                f.explanation = arg_string(a, "explanation");
                for (const auto& item : a["fix"].items()) {
                    if (item.is_string() && !item.str().empty()) f.fix.push_back(item.str());
                }
                if (a["fix"].is_string()) f.fix.push_back(a["fix"].str());
                for (const auto& e : a["evidence"].items()) {
                    Evidence ev;
                    ev.file = arg_string(e, "file");
                    ev.line = arg_int(e, "line", 0);
                    ev.quote = arg_string(e, "quote");
                    if (!ev.file.empty() || !ev.quote.empty()) f.evidence.push_back(std::move(ev));
                }
                f.confidence = arg_string(a, "confidence");
                f.ok = !f.summary.empty() || !f.explanation.empty();
                if (!f.ok) f.error = "the model finished without an answer";
                finished = true;
                break;
            }
            std::string result = run_tool(call, subject, on_step);
            sent += result.size();
            messages.push_back({Message::Role::Tool, result, {}, call.id, call.name});
        }
        if (finished) break;
        if (step > settings_.max_steps) {
            f.error = "the model didn't finish within " + std::to_string(settings_.max_steps) + " steps";
            break;
        }
    }
    f.files_read = ws_.files_read();
    if (f.ok && f.structured) {
        on_step({Step::Kind::Thinking, "checking the evidence"});
        verify(f);
    }
    return f;
}

}  // namespace culprit::ai
