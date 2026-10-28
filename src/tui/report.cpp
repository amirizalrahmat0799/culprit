#include "tui/report.hpp"

#include <algorithm>
#include <cstdint>

#include "tui/style.hpp"

namespace culprit {
namespace {

struct Colors {
    const char *reset = "", *bold = "", *dim = "", *red = "", *yellow = "", *green = "", *cyan = "", *magenta = "";
    explicit Colors(bool on) {
        if (!on) return;
        reset = "\x1b[0m";
        bold = "\x1b[1m";
        dim = "\x1b[2m";
        red = "\x1b[31m";
        yellow = "\x1b[33m";
        green = "\x1b[32m";
        cyan = "\x1b[36m";
        magenta = "\x1b[35m";
    }
    const char* severity(Severity s) const {
        switch (s) {
            case Severity::Critical:
            case Severity::Error: return red;
            case Severity::Warning: return yellow;
            case Severity::Info: return cyan;
        }
        return reset;
    }
};

std::string upper(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return out;
}

// display width, counting UTF-8 characters
size_t width_of(std::string_view s) {
    size_t n = 0;
    for (unsigned char c : s) n += (c & 0xC0) != 0x80;
    return n;
}

std::string fit(std::string_view s, size_t width) {
    if (width_of(s) <= width) return std::string(s) + std::string(width - width_of(s), ' ');
    std::string out;
    size_t n = 0;
    for (size_t i = 0; i < s.size() && n + 1 < width; ++i) {
        out += s[i];
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) {
            // finish the character
            while (i + 1 < s.size() && (static_cast<unsigned char>(s[i + 1]) & 0xC0) == 0x80) out += s[++i];
            ++n;
        }
    }
    return out + "…" + std::string(width > n + 1 ? width - n - 1 : 0, ' ');
}

// word-wraps `text` to `width`, each line starting with `indent`
std::string wrap(std::string_view text, size_t width, const std::string& indent) {
    std::string out;
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find('\n', start);
        std::string_view para = text.substr(start, nl == std::string_view::npos ? std::string_view::npos : nl - start);
        std::string line;
        size_t pos = 0;
        while (pos < para.size()) {
            size_t sp = para.find(' ', pos);
            std::string_view word = para.substr(pos, sp == std::string_view::npos ? std::string_view::npos : sp - pos);
            if (!line.empty() && width_of(line) + 1 + width_of(word) > width) {
                out += indent + line + "\n";
                line.clear();
            }
            if (!line.empty()) line += ' ';
            line.append(word);
            if (sp == std::string_view::npos) break;
            pos = sp + 1;
        }
        out += indent + line + "\n";
        if (nl == std::string_view::npos) break;
        start = nl + 1;
    }
    return out;
}

// cut to `width` without padding
std::string clip(std::string_view s, size_t width) {
    std::string out = fit(s, width);
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::string plural(size_t n, const char* one, const char* many) { return thousands(n) + " " + (n == 1 ? one : many); }

std::string where_text(const Occurrence& o) {
    std::string s;
    if (!o.source.empty()) s += o.source;
    if (!o.time.empty()) s += (s.empty() ? "" : " · ") + o.time;
    if (o.line) s += (s.empty() ? "line " : " · line ") + std::to_string(o.line);
    return s;
}

std::string frame_text(const Frame& f) {
    std::string s = f.file;
    if (f.line) s += ":" + std::to_string(f.line);
    if (!f.function.empty()) s += "  (" + f.function + ")";
    return s;
}

const char* confidence_name(RootCause::Confidence c) {
    switch (c) {
        case RootCause::Confidence::High: return "high";
        case RootCause::Confidence::Medium: return "medium";
        case RootCause::Confidence::Low: return "low";
    }
    return "low";
}

std::string times(size_t n) { return "×" + thousands(n); }

std::string followers_text(const RootCause& root) {
    if (root.followers == 0) return {};
    std::string s = thousands(root.followers) + (root.followers == 1 ? " error" : " errors") + " followed " + root.window + ": ";
    for (size_t i = 0; i < root.follower_top.size(); ++i) {
        if (i) s += ", ";
        s += root.follower_top[i].first + " " + times(root.follower_top[i].second);
    }
    return s;
}

// ---- text ---------------------------------------------------------------------------------------------

void text_report(std::FILE* out, const Report& r, const ReportOptions& o) {
    Colors c(o.color);
    const size_t width = static_cast<size_t>(std::max(60, o.width));
    std::fprintf(out, "%sculprit%s %s%s · %s · %s%s%s · %s%s\n\n", c.bold, c.reset, c.dim, o.title.c_str(),
                 plural(r.entries, "entry", "entries").c_str(), r.errors ? c.red : "", plural(r.errors, "error", "errors").c_str(),
                 r.errors ? c.dim : "", plural(r.warnings, "warning", "warnings").c_str(), c.reset);

    if (r.problems.empty() && r.groups.empty()) {
        std::fprintf(out, "%sNo problems found.%s\n", c.green, c.reset);
        return;
    }

    if (r.root) {
        const RootCause& root = *r.root;
        std::fprintf(out, "%sLIKELY ROOT CAUSE%s %s(%s confidence)%s\n", c.bold, c.reset, c.dim, confidence_name(root.confidence), c.reset);
        if (root.kind == RootCause::Kind::Problem) {
            const Problem& p = r.problems[root.index];
            std::fprintf(out, "  %s%s✖ %s%s\n", c.bold, c.severity(p.rule->severity), p.title().c_str(), c.reset);
            std::fprintf(out, "    %s%s · %s · %s%s\n", c.dim, where_text(p.first).c_str(), times(p.count).c_str(),
                         p.rule->id.c_str(), c.reset);
            if (!p.message.empty()) std::fprintf(out, "    %s%s%s\n", c.dim, clip(p.message, width - 4).c_str(), c.reset);
            std::fputs("\n", out);
            std::fputs(wrap(p.explain(), width - 4, "    ").c_str(), out);
            auto fixes = p.fix();
            if (!fixes.empty()) {
                std::fprintf(out, "\n    %sWhat to do%s\n", c.bold, c.reset);
                for (size_t i = 0; i < fixes.size(); ++i) {
                    std::string item = wrap(fixes[i], width - 9, "       ");
                    item.replace(0, 7, "    " + std::to_string(i + 1) + ". ");
                    std::fputs(item.c_str(), out);
                }
            }
            if (p.where) std::fprintf(out, "\n    %s▶ in your code:%s %s\n", c.green, c.reset, frame_text(*p.where).c_str());
        } else {
            const ErrorGroup& g = r.groups[root.index];
            std::fprintf(out, "  %s%s✖ %s%s\n", c.bold, c.red, fit(g.title, width - 4).c_str(), c.reset);
            std::fprintf(out, "    %s%s · %s%s\n", c.dim, where_text(g.first).c_str(), times(g.count).c_str(), c.reset);
            if (!g.exception.empty() && g.exception != g.title) {
                std::fprintf(out, "    %sroot exception:%s %s\n", c.yellow, c.reset, fit(g.exception, width - 22).c_str());
            }
            if (g.where) std::fprintf(out, "    %s▶ in your code:%s %s\n", c.green, c.reset, frame_text(*g.where).c_str());
            std::fprintf(out, "    %sNo rule knows this error. Open it in the viewer for the full stack trace.%s\n", c.dim, c.reset);
        }
        if (std::string f = followers_text(root); !f.empty()) {
            std::fputs("\n", out);
            std::fputs(wrap("Then " + f, width - 4, "    ").c_str(), out);
        }
        // the first error, when the cause came later (often the other side of the failure)
        if (r.first_error) {
            const Occurrence& cause = root.kind == RootCause::Kind::Problem ? r.problems[root.index].first
                                                                            : r.groups[root.index].first;
            if (r.first_error->index != cause.index) {
                std::fprintf(out, "\n    %sThe first error came before it:%s %s\n      %s\n", c.yellow, c.reset,
                             where_text(*r.first_error).c_str(), clip(r.first_error_text, width - 6).c_str());
            }
        }
        std::fprintf(out, "\n%s", c.dim);
        std::fputs(wrap("Why this one: " + root.why + ".", width - 4, "    ").c_str(), out);
        std::fprintf(out, "%s\n", c.reset);
    }

    if (!r.problems.empty()) {
        std::fprintf(out, "%sPROBLEMS (%zu)%s\n", c.bold, r.problems.size(), c.reset);
        // severity, title, count, first seen, source; the rule id goes under the title when there's room
        const size_t title_w = width > 66 ? width - 47 : 19;
        for (const auto& p : r.problems) {
            std::string sev = upper(severity_name(p.rule->severity));
            std::string title = p.title();
            if (p.variants.size() > 1) title += " (+" + std::to_string(p.variants.size() - 1) + " more)";
            std::fprintf(out, "  %s%-8s%s  %s %s%7s%s  %s%-12s %s%s\n", c.severity(p.rule->severity), sev.c_str(), c.reset,
                         fit(title, title_w).c_str(), c.bold, times(p.count).c_str(), c.reset, c.dim,
                         p.first.time.empty() ? "-" : p.first.time.c_str(), clip(p.first.source.empty() ? "-" : p.first.source, 12).c_str(),
                         c.reset);
            std::fprintf(out, "            %s%s%s\n", c.cyan, p.rule->id.c_str(), c.reset);
        }
        std::fputs("\n", out);
    }

    size_t errors = 0, warnings = 0;
    for (const auto& g : r.groups) (g.level >= Level::Error ? errors : warnings) += 1;
    auto groups = [&](bool want_errors, size_t total, const char* heading) {
        if (!total) return;
        std::fprintf(out, "%s%s (%zu)%s %snot recognised by a rule; the same error is counted once%s\n", c.bold, heading, total,
                     c.reset, c.dim, c.reset);
        size_t shown = 0;
        for (const auto& g : r.groups) {
            if ((g.level >= Level::Error) != want_errors) continue;
            if (shown++ == o.top) {
                std::fprintf(out, "  %s… %zu more (--top to show more)%s\n", c.dim, total - o.top, c.reset);
                break;
            }
            std::fprintf(out, "  %s%7s%s  %s %s%-12s %s%s\n", c.bold, times(g.count).c_str(), c.reset,
                         fit(g.title, width > 50 ? width - 38 : 12).c_str(), c.dim, g.first.time.empty() ? "-" : g.first.time.c_str(),
                         clip(g.first.source.empty() ? "-" : g.first.source, 12).c_str(), c.reset);
            if (!g.exception.empty() && g.exception != g.title) {
                std::fprintf(out, "           %s%s%s\n", c.yellow, clip(g.exception, width - 12).c_str(), c.reset);
            }
            if (g.where) std::fprintf(out, "           %s▶ %s%s\n", c.green, frame_text(*g.where).c_str(), c.reset);
        }
        std::fputs("\n", out);
    };
    groups(true, errors, "OTHER ERRORS");
    groups(false, warnings, "OTHER WARNINGS");
}

// ---- Markdown -------------------------------------------------------------------------------------------

std::string md_cell(std::string_view s) {
    std::string out;
    for (char ch : s) {
        if (ch == '|') out += "\\|";
        else if (ch == '\n') out += ' ';
        else out += ch;
    }
    return out;
}

std::string md_code(std::string_view s) {
    std::string out = "`";
    for (char ch : s) out += ch == '`' ? '\'' : ch;
    return out + "`";
}

void markdown_report(std::FILE* out, const Report& r, const ReportOptions& o) {
    std::fprintf(out, "## culprit: %s\n\n", md_cell(o.title).c_str());
    std::fprintf(out, "%s, %s, %s.\n\n", plural(r.entries, "entry", "entries").c_str(), plural(r.errors, "error", "errors").c_str(),
                 plural(r.warnings, "warning", "warnings").c_str());
    if (r.problems.empty() && r.groups.empty()) {
        std::fputs("No problems found.\n", out);
        return;
    }
    if (r.root) {
        const RootCause& root = *r.root;
        std::fprintf(out, "### Likely root cause (%s confidence)\n\n", confidence_name(root.confidence));
        if (root.kind == RootCause::Kind::Problem) {
            const Problem& p = r.problems[root.index];
            std::fprintf(out, "**%s** — %s, seen %s, first at %s\n\n", md_cell(p.title()).c_str(),
                         std::string(severity_name(p.rule->severity)).c_str(), times(p.count).c_str(),
                         md_cell(where_text(p.first)).c_str());
            if (!p.message.empty()) std::fprintf(out, "```\n%s\n```\n\n", p.message.c_str());
            std::fprintf(out, "%s\n\n", p.explain().c_str());
            auto fixes = p.fix();
            if (!fixes.empty()) {
                std::fputs("**What to do**\n\n", out);
                for (size_t i = 0; i < fixes.size(); ++i) std::fprintf(out, "%zu. %s\n", i + 1, fixes[i].c_str());
                std::fputs("\n", out);
            }
            if (p.where) std::fprintf(out, "In your code: %s\n\n", md_code(frame_text(*p.where)).c_str());
        } else {
            const ErrorGroup& g = r.groups[root.index];
            std::fprintf(out, "**%s** — seen %s, first at %s\n\n", md_cell(g.title).c_str(), times(g.count).c_str(),
                         md_cell(where_text(g.first)).c_str());
            if (!g.exception.empty() && g.exception != g.title) std::fprintf(out, "Root exception: %s\n\n", md_code(g.exception).c_str());
            if (g.where) std::fprintf(out, "In your code: %s\n\n", md_code(frame_text(*g.where)).c_str());
        }
        if (std::string f = followers_text(root); !f.empty()) std::fprintf(out, "Then %s.\n\n", md_cell(f).c_str());
        if (r.first_error) {
            const Occurrence& cause = root.kind == RootCause::Kind::Problem ? r.problems[root.index].first : r.groups[root.index].first;
            if (r.first_error->index != cause.index) {
                std::fprintf(out, "The first error came before it, at %s:\n\n```\n%s\n```\n\n", md_cell(where_text(*r.first_error)).c_str(),
                             r.first_error_text.c_str());
            }
        }
    }
    if (!r.problems.empty()) {
        std::fputs("### Problems\n\n| Severity | Problem | Count | First seen | Rule |\n|---|---|---:|---|---|\n", out);
        for (const auto& p : r.problems) {
            std::fprintf(out, "| %s | %s | %zu | %s | %s |\n", std::string(severity_name(p.rule->severity)).c_str(),
                         md_cell(p.title()).c_str(), p.count, md_cell(where_text(p.first)).c_str(), md_code(p.rule->id).c_str());
        }
        std::fputs("\n", out);
    }
    if (!r.groups.empty()) {
        std::fputs("### Other errors and warnings\n\n| Level | Message | Count | First seen |\n|---|---|---:|---|\n", out);
        size_t shown = 0;
        for (const auto& g : r.groups) {
            if (shown++ == o.top) break;
            std::fprintf(out, "| %s | %s | %zu | %s |\n", std::string(level_name(g.level)).c_str(), md_cell(fit(g.title, 140)).c_str(),
                         g.count, md_cell(where_text(g.first)).c_str());
        }
        if (r.groups.size() > o.top) std::fprintf(out, "\n…and %zu more.\n", r.groups.size() - o.top);
        std::fputs("\n", out);
    }
}

// ---- JSON -------------------------------------------------------------------------------------------------

std::string js(std::string_view s) {
    std::string out = "\"";
    for (unsigned char ch : s) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (ch < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", ch);
                    out += buf;
                } else {
                    out += static_cast<char>(ch);
                }
        }
    }
    return out + "\"";
}

std::string js_occurrence(const Occurrence& o) {
    std::string s = "{\"source\":" + js(o.source) + ",\"time\":" + js(o.time) + ",\"line\":" + std::to_string(o.line) +
                    ",\"index\":" + std::to_string(o.index);
    if (o.epoch_ms) s += ",\"epoch_ms\":" + std::to_string(*o.epoch_ms);
    return s + "}";
}

std::string js_frame(const std::optional<Frame>& f) {
    if (!f) return "null";
    return "{\"file\":" + js(f->file) + ",\"line\":" + std::to_string(f->line) + ",\"function\":" + js(f->function) + "}";
}

std::string js_strings(const std::vector<std::string>& v) {
    std::string s = "[";
    for (size_t i = 0; i < v.size(); ++i) s += (i ? "," : "") + js(v[i]);
    return s + "]";
}

void json_report(std::FILE* out, const Report& r, const ReportOptions& o) {
    std::string s = "{\"title\":" + js(o.title) + ",\"entries\":" + std::to_string(r.entries) + ",\"errors\":" +
                    std::to_string(r.errors) + ",\"warnings\":" + std::to_string(r.warnings);
    s += ",\"root_cause\":";
    if (r.root) {
        const RootCause& root = *r.root;
        s += "{\"kind\":";
        s += root.kind == RootCause::Kind::Problem ? "\"problem\"" : "\"group\"";
        s += ",\"index\":" + std::to_string(root.index) + ",\"confidence\":" + js(confidence_name(root.confidence)) +
             ",\"why\":" + js(root.why) + ",\"followers\":" + std::to_string(root.followers) + ",\"followers_top\":[";
        for (size_t i = 0; i < root.follower_top.size(); ++i) {
            s += (i ? "," : "") + std::string("{\"title\":") + js(root.follower_top[i].first) +
                 ",\"count\":" + std::to_string(root.follower_top[i].second) + "}";
        }
        s += "]}";
    } else {
        s += "null";
    }
    s += ",\"first_error\":";
    s += r.first_error ? js_occurrence(*r.first_error) : "null";
    s += ",\"problems\":[";
    for (size_t i = 0; i < r.problems.size(); ++i) {
        const Problem& p = r.problems[i];
        s += (i ? "," : "") + std::string("{\"rule\":") + js(p.rule->id) + ",\"severity\":" + js(severity_name(p.rule->severity)) +
             ",\"symptom\":" + (p.rule->symptom ? "true" : "false") + ",\"title\":" + js(p.title()) +
             ",\"count\":" + std::to_string(p.count) + ",\"first\":" + js_occurrence(p.first) + ",\"last\":" + js_occurrence(p.last) +
             ",\"sources\":" + js_strings(p.sources) + ",\"variants\":" + js_strings(p.variants) + ",\"message\":" + js(p.message) +
             ",\"explain\":" + js(p.explain()) + ",\"fix\":" + js_strings(p.fix()) + ",\"in_your_code\":" + js_frame(p.where) +
             ",\"captures\":{";
        for (size_t j = 0; j < p.captures.size(); ++j) {
            s += (j ? "," : "") + js(p.captures[j].first) + ":" + js(p.captures[j].second);
        }
        s += "}}";
    }
    s += "],\"groups\":[";
    for (size_t i = 0; i < r.groups.size(); ++i) {
        const ErrorGroup& g = r.groups[i];
        s += (i ? "," : "") + std::string("{\"id\":") + std::to_string(g.id) + ",\"level\":" + js(level_name(g.level)) +
             ",\"title\":" + js(g.title) + ",\"exception\":" + js(g.exception) + ",\"count\":" + std::to_string(g.count) +
             ",\"first\":" + js_occurrence(g.first) + ",\"last\":" + js_occurrence(g.last) + ",\"sources\":" + js_strings(g.sources) +
             ",\"in_your_code\":" + js_frame(g.where) + "}";
    }
    s += "]}\n";
    std::fputs(s.c_str(), out);
}

}  // namespace

void print_report(std::FILE* out, const Report& report, const ReportOptions& options) {
    switch (options.format) {
        case ReportFormat::Text: text_report(out, report, options); break;
        case ReportFormat::Markdown: markdown_report(out, report, options); break;
        case ReportFormat::Json: json_report(out, report, options); break;
    }
}

void print_rule_list(std::FILE* out, const RuleSet& rules, bool color) {
    Colors c(color);
    std::fprintf(out, "%s%zu rules%s\n", c.bold, rules.rules().size(), c.reset);
    for (const auto& r : rules.rules()) {
        std::fprintf(out, "  %s%-34s%s %s%-8s%s %s%s%s\n", c.cyan, r->id.c_str(), c.reset, c.severity(r->severity),
                     std::string(severity_name(r->severity)).c_str(), c.reset, render(r->title, {}).c_str(),
                     r->origin == "built-in" ? "" : (std::string(c.dim) + "  (" + r->origin + ")").c_str(), c.reset);
    }
}

void print_rule(std::FILE* out, const Rule& r, bool color) {
    Colors c(color);
    std::fprintf(out, "%s%s%s  %s%s%s%s\n", c.bold, r.id.c_str(), c.reset, c.severity(r.severity),
                 std::string(severity_name(r.severity)).c_str(), r.symptom ? " (usually a symptom)" : "", c.reset);
    std::fprintf(out, "%s%s%s\n", c.bold, r.title.c_str(), c.reset);
    std::fprintf(out, "%sfrom %s, line %d · priority %d", c.dim, r.origin.c_str(), r.line, r.priority);
    if (!r.tags.empty()) {
        std::fputs(" · tags:", out);
        for (const auto& t : r.tags) std::fprintf(out, " %s", t.c_str());
    }
    std::fprintf(out, "%s\n\n", c.reset);
    std::fputs("Matches ", out);
    if (!r.field.empty()) {
        std::fprintf(out, "%s = one of", r.field.c_str());
        for (const auto& v : r.values) std::fprintf(out, " %s", v.c_str());
        if (!r.contains.empty()) std::fputs(" and ", out);
    }
    if (!r.contains.empty()) {
        std::fputs("text containing", out);
        for (size_t i = 0; i < r.contains.size(); ++i) std::fprintf(out, "%s \"%s\"", i ? " or" : "", r.contains[i].c_str());
    }
    if (!r.regex_text.empty()) std::fprintf(out, "\n  and the regex %s", r.regex_text.c_str());
    std::fprintf(out, "\n  at level %s\n\n", r.any_level ? "any" : (">=" + std::string(level_name(r.min_level)) + " (or no level)").c_str());
    std::fputs(wrap(r.explain, 96, "").c_str(), out);
    if (!r.fix.empty()) {
        std::fprintf(out, "\n%sWhat to do%s\n", c.bold, c.reset);
        for (size_t i = 0; i < r.fix.size(); ++i) {
            std::string item = wrap(r.fix[i], 92, "   ");
            item.replace(0, 3, std::to_string(i + 1) + ". ");
            std::fputs(item.c_str(), out);
        }
    }
    if (!r.docs.empty()) std::fprintf(out, "\nMore: %s\n", r.docs.c_str());
    if (!r.examples.empty()) {
        std::fprintf(out, "\n%sExample%s\n", c.bold, r.examples.size() > 1 ? "s" : "");
        for (const auto& e : r.examples) std::fprintf(out, "%s%s%s\n", c.dim, e.c_str(), c.reset);
    }
}

}  // namespace culprit
