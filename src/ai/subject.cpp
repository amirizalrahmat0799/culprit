#include "ai/subject.hpp"

#include <algorithm>

#include "core/filter.hpp"
#include "core/stack.hpp"

namespace culprit::ai {
namespace {

std::string one_line(std::string_view s, size_t max = 300) {
    std::string out;
    for (char c : s) {
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        out += c;
        if (out.size() >= max) {
            out += "...";
            break;
        }
    }
    return out;
}

std::string where_text(const Occurrence& o) {
    std::string s = o.source;
    if (!o.time.empty()) s += (s.empty() ? "" : " · ") + o.time;
    if (o.line) s += (s.empty() ? "line " : " · line ") + std::to_string(o.line);
    return s;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

}  // namespace

Subject make_subject(const Entry& e, const std::vector<const Entry*>& before, const Report* report, const RuleSet* rules) {
    Subject s;
    s.title = one_line(e.message, 200);
    s.where = e.source;
    if (!e.time_text.empty()) s.where += (s.where.empty() ? "" : " · ") + e.time_text;
    if (e.line) s.where += (s.where.empty() ? "line " : " · line ") + std::to_string(e.line);
    s.entry = e.raw.empty() ? e.message + (e.stack.empty() ? "" : "\n" + e.stack) : e.raw;

    if (rules) {
        if (auto m = rules->match(e)) {
            s.title = render(m->rule->title, m->captures);
            s.known = s.title + ". " + render(m->rule->explain, m->captures);
            if (m->rule->symptom) s.known += " (This is usually a symptom of another problem.)";
        }
    }

    if (e.has_stack()) {
        std::vector<std::string> seen;
        for (const auto& f : parse_stack(e.stack)) {
            if (f.kind != Frame::Kind::Frame || !f.own || f.file.empty()) continue;
            std::string ref = f.file + ":" + std::to_string(f.line);
            if (std::find(seen.begin(), seen.end(), ref) != seen.end()) continue;
            seen.push_back(ref);
            s.frames.push_back(ref);
            if (seen.size() >= 8) break;
        }
    }

    for (const Entry* b : before) {
        std::string line = (b->time_text.empty() ? "" : b->time_text + " ") + std::string(level_name(b->level)) + " " +
                           (b->source.empty() ? "" : b->source + " ") + one_line(b->message);
        s.context += line + "\n";
    }

    if (report) {
        std::string a;
        if (report->root) {
            const RootCause& root = *report->root;
            const char* conf = root.confidence == RootCause::Confidence::High ? "high"
                               : root.confidence == RootCause::Confidence::Medium ? "medium" : "low";
            if (root.kind == RootCause::Kind::Problem) {
                const Problem& p = report->problems[root.index];
                a += "- Likely root cause (" + std::string(conf) + " confidence): " + p.title() + ", at " + where_text(p.first) + "\n";
            } else {
                const ErrorGroup& g = report->groups[root.index];
                a += "- Likely root cause (" + std::string(conf) + " confidence): " + one_line(g.title) + ", at " + where_text(g.first) + "\n";
            }
            if (root.followers) {
                a += "- Then " + std::to_string(root.followers) + " errors followed:";
                for (const auto& [t, n] : root.follower_top) a += " " + t + " x" + std::to_string(n) + ";";
                a += "\n";
            }
        }
        if (report->first_error) {
            a += "- First error in the log: " + one_line(report->first_error_text) + ", at " + where_text(*report->first_error) + "\n";
        }
        size_t shown = 0;
        for (const auto& p : report->problems) {
            if (shown++ >= 8) break;
            a += "- Problem: " + p.title() + " x" + std::to_string(p.count) + " (" + std::string(severity_name(p.rule->severity)) + ")\n";
        }
        shown = 0;
        for (const auto& g : report->groups) {
            if (g.level < Level::Error) continue;
            if (shown++ >= 5) break;
            a += "- Unrecognised error: " + one_line(g.title, 160) + " x" + std::to_string(g.count) + "\n";
        }
        s.analysis = a;
    }
    return s;
}

std::optional<uint32_t> default_target(const Report& report) {
    if (report.root) {
        return report.root->kind == RootCause::Kind::Problem ? report.problems[report.root->index].first.index
                                                             : report.groups[report.root->index].first.index;
    }
    if (report.first_error) return report.first_error->index;
    return std::nullopt;
}

uint32_t richest_occurrence(uint32_t index, size_t count, const std::function<const Entry&(uint32_t)>& get) {
    const Entry& first = get(index);
    if (first.has_stack()) return index;
    const size_t end = std::min(count, static_cast<size_t>(index) + 500);
    for (size_t i = index + 1; i < end; ++i) {
        const Entry& e = get(static_cast<uint32_t>(i));
        bool same = first.rule ? e.rule == first.rule : (first.group != UINT32_MAX && e.group == first.group);
        if (same && e.has_stack()) return static_cast<uint32_t>(i);
    }
    return index;
}

std::string search_entries(const std::vector<Entry>& entries, const std::string& text, size_t max) {
    if (text.size() < 2) return "error: search for at least 2 characters";
    std::string needle = lower(text);
    std::string out;
    size_t n = 0;
    for (const auto& e : entries) {
        if (!contains_icase(e.message, needle) && !contains_icase(e.stack, needle)) continue;
        out += "line " + std::to_string(e.line) + "  " + (e.time_text.empty() ? "" : e.time_text + "  ") +
               std::string(level_name(e.level)) + "  " + (e.source.empty() ? "" : e.source + "  ") + one_line(e.message, 240) + "\n";
        if (++n >= max) {
            out += "[more matches; search for something more specific]\n";
            break;
        }
    }
    return n ? out : "no log entries contain \"" + text + "\"";
}

}  // namespace culprit::ai
