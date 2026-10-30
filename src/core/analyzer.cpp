#include "core/analyzer.hpp"

#include <algorithm>
#include <map>

namespace culprit {
namespace {

constexpr size_t kMaxGroups = 5000;     // beyond this, unrecognised errors share one "other" group
constexpr size_t kMaxVariants = 5;
constexpr size_t kMaxSources = 8;
constexpr int64_t kCascadeMs = 5 * 60 * 1000;  // errors this soon after the cause count as its consequences
constexpr uint32_t kCascadeEntries = 5000;     // when there are no timestamps

bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_hex(char c) { return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
bool is_word(char c) { return is_hex(c) || (c >= 'g' && c <= 'z') || (c >= 'G' && c <= 'Z') || c == '_'; }

std::string first_line(std::string_view s) {
    size_t nl = s.find('\n');
    std::string out(s.substr(0, nl));
    while (!out.empty() && (out.back() == '\r' || out.back() == ' ')) out.pop_back();
    if (out.size() > 300) {
        out.resize(297);
        while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80) out.pop_back();
        if (!out.empty() && (static_cast<unsigned char>(out.back()) & 0x80)) out.pop_back();
        out += "...";
    }
    return out;
}

Occurrence occurrence(const Entry& e, uint32_t index) {
    return Occurrence{index, e.line, e.source, e.time_text, e.epoch_ms, e.time_zoned};
}

// Times compare when both are known and either both say their time zone, or both come from the same
// source. (A container logging "09:41:12" in UTC next to an app logging "09:41:12+08:00" can't be
// compared; their order of arrival is a better guide.)
bool comparable(const std::optional<int64_t>& a, bool az, const std::string& as, const std::optional<int64_t>& b, bool bz,
                const std::string& bs) {
    return a && b && ((az && bz) || as == bs);
}

// a before b: by time when the times compare, else by arrival
bool earlier(const Occurrence& a, const Occurrence& b) {
    if (comparable(a.epoch_ms, a.zoned, a.source, b.epoch_ms, b.zoned, b.source) && *a.epoch_ms != *b.epoch_ms) {
        return *a.epoch_ms < *b.epoch_ms;
    }
    return a.index < b.index;
}

void add_source(std::vector<std::string>& sources, const std::string& s) {
    if (s.empty() || sources.size() >= kMaxSources) return;
    if (std::find(sources.begin(), sources.end(), s) == sources.end()) sources.push_back(s);
}

}  // namespace

std::vector<std::string> Problem::fix() const {
    std::vector<std::string> out;
    for (const auto& f : rule->fix) out.push_back(render(f, captures));
    return out;
}

std::string normalize_message(std::string_view m) {
    std::string out;
    out.reserve(m.size());
    size_t i = 0;
    while (i < m.size()) {
        char c = m[i];
        if (c == '\n') break;
        // a run of letters and digits: keep words, replace numbers, hex ids and UUIDs
        if (is_word(c) || c == '-') {
            size_t j = i;
            bool digit = false, word = false;
            while (j < m.size() && (is_word(m[j]) || m[j] == '-')) {
                digit |= is_digit(m[j]);
                word |= !is_hex(m[j]) && m[j] != '-';
                ++j;
            }
            std::string_view tok = m.substr(i, j - i);
            bool all_digits = std::all_of(tok.begin(), tok.end(), [](char ch) { return is_digit(ch) || ch == '-'; });
            if (digit && (all_digits || (!word && tok.size() >= 6) || tok.size() >= 20)) {
                out += '#';
            } else if (digit) {
                // words with numbers in them (pod-7d9f8, order42, 350ms): keep letters, one # per run of
                // digits, so "82ms" and "3454ms" are the same error
                for (size_t k = 0; k < tok.size(); ++k) {
                    if (!is_digit(tok[k])) out += tok[k];
                    else if (k == 0 || !is_digit(tok[k - 1])) out += '#';
                }
            } else {
                out.append(tok);
            }
            i = j;
            continue;
        }
        if (c == ' ' || c == '\t') {
            if (!out.empty() && out.back() != ' ') out += ' ';
            ++i;
            continue;
        }
        out += c;
        ++i;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    if (out.size() > 240) out.resize(240);
    return out;
}

std::string root_exception(const std::vector<Frame>& frames) {
    const Frame* best = nullptr;
    for (const auto& f : frames) {
        if (f.kind == Frame::Kind::CausedBy && f.text.rfind("Caused by:", 0) == 0) best = &f;
    }
    if (best) {
        std::string s = best->text.substr(10);
        while (!s.empty() && s.front() == ' ') s.erase(0, 1);
        return first_line(s);
    }
    // a Python traceback ends with the exception; a Java trace starts with it
    for (auto it = frames.rbegin(); it != frames.rend(); ++it) {
        if (it->kind != Frame::Kind::Header) continue;
        const std::string& t = it->text;
        size_t colon = t.find(':');
        std::string_view type = std::string_view(t).substr(0, colon);
        bool looks_like_exception = !type.empty() && type.find(' ') == std::string_view::npos &&
                                    (type.ends_with("Error") || type.ends_with("Exception") || type.ends_with("Exit") ||
                                     type.find('.') != std::string_view::npos || type == "panic");
        if (looks_like_exception) return first_line(t);
    }
    return {};
}

Analyzer::Analyzer(std::shared_ptr<const RuleSet> rules) : rules_(std::move(rules)) {}

uint16_t Analyzer::source_id(const std::string& s) {
    for (size_t i = 0; i < sources_.size(); ++i) {
        if (sources_[i] == s) return static_cast<uint16_t>(i);
    }
    if (sources_.size() >= 65535) return 65535;
    sources_.push_back(s);
    return static_cast<uint16_t>(sources_.size() - 1);
}

void Analyzer::add(Entry& e, uint32_t index) {
    ++entries_;
    const bool error_level = e.level >= Level::Error;
    if (error_level) ++errors_;
    else if (e.level == Level::Warn) ++warnings_;

    // a rule?
    std::optional<RuleMatch> m;
    if (rules_) m = rules_->match(e);
    if (m) {
        e.rule = m->rule;
        auto [it, created] = problem_index_.try_emplace(m->rule, problems_.size());
        if (created) {
            Problem p;
            p.rule = m->rule;
            p.first = occurrence(e, index);
            p.captures = m->captures;
            p.message = first_line(m->evidence.empty() ? e.message : m->evidence);
            if (e.has_stack()) p.where = first_own_frame(parse_stack(e.stack));
            problems_.push_back(std::move(p));
        }
        Problem& p = problems_[it->second];
        ++p.count;
        p.last = occurrence(e, index);
        if (!created && earlier(p.last, p.first)) p.first = p.last;  // lines from another source that happened earlier
        if (p.variants.size() < kMaxVariants) {
            std::string t = render(m->rule->title, m->captures);
            if (std::find(p.variants.begin(), p.variants.end(), t) == p.variants.end()) p.variants.push_back(std::move(t));
        }
        add_source(p.sources, e.source);
        const bool serious = error_level || m->rule->severity >= Severity::Error;
        if (serious || e.level == Level::Warn || m->rule->severity == Severity::Warning) {
            events_.push_back(Event{index, e.epoch_ms, e.time_zoned, source_id(e.source), static_cast<int32_t>(it->second), 0, serious});
        }
        if (serious && (!first_error_ || earlier(occurrence(e, index), *first_error_))) {
            first_error_ = occurrence(e, index);
            first_error_text_ = first_line(e.message);
        }
        return;
    }

    // an unrecognised error or warning: group it
    if (e.level < Level::Warn) return;
    std::vector<Frame> frames;
    if (e.has_stack()) frames = parse_stack(e.stack);
    std::string exception = root_exception(frames);
    std::optional<Frame> where = first_own_frame(frames);

    std::string key;
    if (groups_.size() >= kMaxGroups) {
        key = "\x01other";
    } else {
        key = std::string(1, static_cast<char>('0' + static_cast<int>(e.level)));
        key += '\x1f';
        key += normalize_message(exception.empty() ? e.message : exception.substr(0, exception.find(':')));
        key += '\x1f';
        key += normalize_message(e.message);
        if (where) {
            key += '\x1f';
            key += where->file + ":" + std::to_string(where->line);
        }
    }
    auto [it, created] = group_index_.try_emplace(key, static_cast<uint32_t>(groups_.size()));
    if (created) {
        ErrorGroup g;
        g.id = it->second;
        g.level = e.level;
        g.title = key == "\x01other" ? "Other errors and warnings" : first_line(e.message);
        if (g.title.empty()) g.title = exception;
        g.exception = exception;
        g.first = occurrence(e, index);
        g.where = where;
        groups_.push_back(std::move(g));
        if (e.level >= Level::Error) ++error_groups_;
    }
    ErrorGroup& g = groups_[it->second];
    ++g.count;
    g.last = occurrence(e, index);
    if (!created && earlier(g.last, g.first)) g.first = g.last;
    add_source(g.sources, e.source);
    e.group = g.id;

    events_.push_back(Event{index, e.epoch_ms, e.time_zoned, source_id(e.source), -1, g.id, error_level});
    if (error_level && (!first_error_ || earlier(occurrence(e, index), *first_error_))) {
        first_error_ = occurrence(e, index);
        first_error_text_ = first_line(e.message);
    }
}

Report Analyzer::report() const {
    Report r;
    r.rules = rules_;
    r.entries = entries_;
    r.errors = errors_;
    r.warnings = warnings_;
    r.first_error = first_error_;
    r.first_error_text = first_error_text_;

    // problems: most severe first, then earliest
    std::vector<size_t> porder(problems_.size());
    for (size_t i = 0; i < porder.size(); ++i) porder[i] = i;
    std::stable_sort(porder.begin(), porder.end(), [&](size_t a, size_t b) {
        const auto& pa = problems_[a];
        const auto& pb = problems_[b];
        if (pa.rule->severity != pb.rule->severity) return pa.rule->severity > pb.rule->severity;
        if (pa.rule->symptom != pb.rule->symptom) return !pa.rule->symptom;
        return earlier(pa.first, pb.first);
    });
    std::vector<size_t> problem_pos(problems_.size());
    for (size_t i = 0; i < porder.size(); ++i) {
        problem_pos[porder[i]] = i;
        r.problems.push_back(problems_[porder[i]]);
    }

    // groups: errors before warnings, then most frequent, then earliest
    std::vector<size_t> gorder(groups_.size());
    for (size_t i = 0; i < gorder.size(); ++i) gorder[i] = i;
    std::stable_sort(gorder.begin(), gorder.end(), [&](size_t a, size_t b) {
        const auto& ga = groups_[a];
        const auto& gb = groups_[b];
        bool ea = ga.level >= Level::Error, eb = gb.level >= Level::Error;
        if (ea != eb) return ea;
        if (ga.count != gb.count) return ga.count > gb.count;
        return earlier(ga.first, gb.first);
    });
    std::vector<size_t> group_pos(groups_.size());
    for (size_t i = 0; i < gorder.size(); ++i) {
        group_pos[gorder[i]] = i;
        r.groups.push_back(groups_[gorder[i]]);
    }

    // the likely root cause: the earliest problem that causes rather than results (High), else the earliest
    // unrecognised error (Medium), else the earliest symptom (Low)
    std::optional<size_t> cause, symptom;
    for (size_t i = 0; i < problems_.size(); ++i) {
        const Problem& p = problems_[i];
        if (p.rule->severity < Severity::Error) continue;
        auto& slot = p.rule->symptom ? symptom : cause;
        if (!slot || earlier(p.first, problems_[*slot].first)) slot = i;
    }
    std::optional<size_t> group;
    for (size_t i = 0; i < groups_.size(); ++i) {
        if (groups_[i].level < Level::Error) continue;
        if (!group || earlier(groups_[i].first, groups_[*group].first)) group = i;
    }

    RootCause root;
    const Occurrence* start = nullptr;
    int32_t self_problem = -1;
    int64_t self_group = -1;
    if (cause) {
        root.kind = RootCause::Kind::Problem;
        root.index = problem_pos[*cause];
        root.confidence = RootCause::Confidence::High;
        root.why = "the earliest known problem that causes failures rather than resulting from them";
        start = &problems_[*cause].first;
        self_problem = static_cast<int32_t>(*cause);
    } else if (group) {
        root.kind = RootCause::Kind::Group;
        root.index = group_pos[*group];
        root.confidence = RootCause::Confidence::Medium;
        root.why = "the earliest error; no rule recognises it";
        start = &groups_[*group].first;
        self_group = *group;
    } else if (symptom) {
        root.kind = RootCause::Kind::Problem;
        root.index = problem_pos[*symptom];
        root.confidence = RootCause::Confidence::Low;
        root.why = "the earliest error, but it's usually a consequence of something else, possibly in another service's log";
        start = &problems_[*symptom].first;
        self_problem = static_cast<int32_t>(*symptom);
    }

    if (start) {
        // what followed: errors soon after it, apart from more of the same
        bool timed = false;
        std::map<std::string, size_t> kinds;
        for (const Event& ev : events_) {
            if (!ev.error) continue;
            if (ev.problem >= 0 ? ev.problem == self_problem : static_cast<int64_t>(ev.group) == self_group) continue;
            bool after, within;
            const std::string& ev_source = ev.source < sources_.size() ? sources_[ev.source] : start->source;
            if (comparable(start->epoch_ms, start->zoned, start->source, ev.epoch_ms, ev.zoned, ev_source)) {
                timed = true;
                after = *ev.epoch_ms > *start->epoch_ms || (*ev.epoch_ms == *start->epoch_ms && ev.index > start->index);
                within = *ev.epoch_ms - *start->epoch_ms <= kCascadeMs;
            } else {
                after = ev.index > start->index;
                within = ev.index - start->index <= kCascadeEntries;
            }
            if (!after || !within) continue;
            ++root.followers;
            const std::string& title = ev.problem >= 0 ? problems_[static_cast<size_t>(ev.problem)].rule->title
                                                       : groups_[ev.group].title;
            ++kinds[ev.problem >= 0 ? render(title, problems_[static_cast<size_t>(ev.problem)].captures) : title];
        }
        std::vector<std::pair<std::string, size_t>> top(kinds.begin(), kinds.end());
        std::stable_sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        if (top.size() > 3) top.resize(3);
        root.follower_top = std::move(top);
        root.window = timed ? "within 5 minutes" : "after it";
        r.root = std::move(root);
    }
    return r;
}

}  // namespace culprit
