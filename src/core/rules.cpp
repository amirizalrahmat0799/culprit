#include "core/rules.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <queue>
#include <sstream>

#include "core/filter.hpp"
#include "core/parser.hpp"
#include "core/toml.hpp"

namespace culprit {

// generated at build time from rules/*.toml (cmake/embed_rules.cmake)
struct EmbeddedFile {
    const char* name;
    const unsigned char* data;
    size_t size;
};
extern const EmbeddedFile kBuiltinRules[];
extern const size_t kBuiltinRuleCount;

namespace {

namespace fs = std::filesystem;

char fold(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = fold(c);
    return out;
}

bool valid_id(const std::string& id) {
    if (id.empty()) return false;
    for (char c : id) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_')) return false;
    }
    return true;
}

// the {names} used in a template
std::vector<std::string> placeholders(std::string_view text) {
    std::vector<std::string> out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '{') continue;
        size_t end = text.find('}', i);
        if (end == std::string_view::npos) break;
        std::string name(text.substr(i + 1, end - i - 1));
        bool ok = !name.empty() && std::all_of(name.begin(), name.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        });
        if (ok) out.push_back(name);
        i = end;
    }
    return out;
}

struct RuleError {
    int line;
    std::string message;
};

std::string as_string(const toml::Value& v, const char* key) {
    if (v.type != toml::Value::Type::String) throw RuleError{v.line, std::string(key) + " must be a \"string\""};
    return v.str;
}

std::vector<std::string> as_strings(const toml::Value& v, const char* key) {
    if (v.type == toml::Value::Type::String) return {v.str};
    if (v.type != toml::Value::Type::Array) throw RuleError{v.line, std::string(key) + " must be a list of strings"};
    std::vector<std::string> out;
    for (const auto& item : v.items) {
        if (item.type != toml::Value::Type::String) throw RuleError{item.line, std::string(key) + " must be a list of strings"};
        out.push_back(item.str);
    }
    return out;
}

bool as_bool(const toml::Value& v, const char* key) {
    if (v.type != toml::Value::Type::Bool) throw RuleError{v.line, std::string(key) + " must be true or false"};
    return v.boolean;
}

// One [[rule]] table; throws RuleError. `disabled` is reported separately.
Rule build_rule(const toml::Table& t, bool& disabled) {
    static const char* known[] = {"id",    "title",    "severity", "symptom", "priority", "tags",
                                  "contains", "regex", "captures", "field",   "values",   "level",
                                  "explain",  "fix",   "docs",     "disabled", "examples"};
    for (const auto& [k, v] : t.values) {
        if (std::find_if(std::begin(known), std::end(known), [&](const char* n) { return k == n; }) == std::end(known)) {
            throw RuleError{v.line, "unknown key \"" + k + "\""};
        }
    }

    Rule r;
    r.line = t.line;
    const toml::Value* id = t.get("id");
    if (!id) throw RuleError{t.line, "the rule has no id"};
    r.id = as_string(*id, "id");
    if (!valid_id(r.id)) throw RuleError{id->line, "id \"" + r.id + "\" may only use a-z, 0-9, '.', '-' and '_'"};

    disabled = false;
    if (const auto* v = t.get("disabled")) disabled = as_bool(*v, "disabled");
    if (disabled) return r;

    if (const auto* v = t.get("title")) r.title = as_string(*v, "title");
    else throw RuleError{t.line, "rule " + r.id + " has no title"};

    if (const auto* v = t.get("severity")) {
        std::string s = lower(as_string(*v, "severity"));
        if (s == "info") r.severity = Severity::Info;
        else if (s == "warning" || s == "warn") r.severity = Severity::Warning;
        else if (s == "error") r.severity = Severity::Error;
        else if (s == "critical") r.severity = Severity::Critical;
        else throw RuleError{v->line, "severity must be info, warning, error or critical"};
    }
    if (const auto* v = t.get("symptom")) r.symptom = as_bool(*v, "symptom");
    if (const auto* v = t.get("priority")) {
        if (v->type != toml::Value::Type::Integer) throw RuleError{v->line, "priority must be a whole number"};
        r.priority = static_cast<int>(v->integer);
    }
    if (const auto* v = t.get("tags")) r.tags = as_strings(*v, "tags");

    if (const auto* v = t.get("contains")) {
        for (auto& s : as_strings(*v, "contains")) {
            if (s.empty()) throw RuleError{v->line, "contains has an empty string"};
            r.contains.push_back(lower(s));
        }
    }
    if (const auto* v = t.get("field")) {
        r.field = as_string(*v, "field");
        const auto* values = t.get("values");
        if (!values) throw RuleError{v->line, "field needs values = [...] to compare with"};
        r.values = as_strings(*values, "values");
    } else if (t.get("values")) {
        throw RuleError{t.get("values")->line, "values only works together with field"};
    }
    if (r.contains.empty() && r.field.empty()) {
        throw RuleError{t.line, "rule " + r.id + " needs contains = [...] (or field and values)"};
    }

    if (const auto* v = t.get("regex")) {
        r.regex_text = as_string(*v, "regex");
        if (r.contains.empty()) throw RuleError{v->line, "regex needs contains = [...] too; it's only tried on lines that contain one of them"};
        try {
            r.regex = std::make_shared<std::regex>(r.regex_text, std::regex::ECMAScript | std::regex::optimize);
        } catch (const std::regex_error& e) {
            throw RuleError{v->line, std::string("regex doesn't compile: ") + e.what()};
        }
        if (const auto* c = t.get("captures")) {
            r.captures = as_strings(*c, "captures");
            if (r.captures.size() > r.regex->mark_count()) {
                throw RuleError{c->line, "captures names " + std::to_string(r.captures.size()) + " groups but the regex has " +
                                             std::to_string(r.regex->mark_count())};
            }
        }
    } else if (t.get("captures")) {
        throw RuleError{t.get("captures")->line, "captures only works together with regex"};
    }

    if (const auto* v = t.get("level")) {
        std::string s = lower(as_string(*v, "level"));
        if (s == "any") {
            r.any_level = true;
        } else {
            if (s.rfind(">=", 0) == 0) s.erase(0, 2);
            auto level = parse_level(s);
            if (!level || *level == Level::Unknown) throw RuleError{v->line, "level must be \"any\" or a level such as \">=warn\""};
            r.min_level = *level;
        }
    }

    if (const auto* v = t.get("explain")) r.explain = as_string(*v, "explain");
    if (const auto* v = t.get("fix")) r.fix = as_strings(*v, "fix");
    if (const auto* v = t.get("docs")) r.docs = as_string(*v, "docs");
    if (const auto* v = t.get("examples")) r.examples = as_strings(*v, "examples");

    // every {name} must be a capture
    auto check = [&](const std::string& text, int line) {
        for (const auto& name : placeholders(text)) {
            if (name != r.field && std::find(r.captures.begin(), r.captures.end(), name) == r.captures.end()) {
                throw RuleError{line, "{" + name + "} isn't one of the captures" + std::string(r.field.empty() ? "" : " or the field")};
            }
        }
    };
    check(r.title, t.get("title")->line);
    if (const auto* v = t.get("explain")) check(r.explain, v->line);
    if (const auto* v = t.get("fix")) {
        for (const auto& f : r.fix) check(f, v->line);
    }
    return r;
}

bool level_ok(const Rule& r, Level level) {
    if (r.any_level || level == Level::Unknown) return true;
    return level >= r.min_level;
}

// "app-1    | text" -> "text"
std::string_view without_prefix(std::string_view line) {
    size_t bar = line.find(" | ");
    if (bar != std::string_view::npos && bar < 48) {
        bool name = true;
        for (size_t i = 0; i < bar && name; ++i) {
            char c = line[i];
            name = c == ' ' || c == '-' || c == '_' || c == '.' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        }
        if (name) line.remove_prefix(bar + 3);
    }
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
    return line;
}

// The matched line, shortened so the part that matched (at `pos`) is near the start: long lines are
// shown cut to the screen's width, and the timestamp and logger in front aren't the interesting part.
std::string around(std::string_view line, size_t pos) {
    line = without_prefix(line);
    if (pos < 70) return std::string(line);
    size_t from = pos - 40;
    while (from < line.size() && (static_cast<unsigned char>(line[from]) & 0xC0) == 0x80) ++from;
    size_t space = line.find(' ', from);
    if (space != std::string_view::npos && space < pos) from = space + 1;
    return "…" + std::string(line.substr(from));
}

// the first line of `text` containing one of the rule's words
std::string evidence_line(const Rule& r, std::string_view text) {
    size_t start = 0;
    while (start < text.size()) {
        size_t nl = text.find('\n', start);
        std::string_view line = text.substr(start, nl == std::string_view::npos ? std::string_view::npos : nl - start);
        start = nl == std::string_view::npos ? text.size() : nl + 1;
        std::string lowered(without_prefix(line));
        for (char& c : lowered) c = fold(c);
        for (const auto& w : r.contains) {
            size_t at = lowered.find(w);
            if (at != std::string::npos) return around(without_prefix(line), at);
        }
    }
    return {};
}

// tries the regex on each line of `text` that contains one of the rule's words
bool regex_lines(const Rule& r, std::string_view text, Captures& captures, std::string& evidence) {
    size_t start = 0;
    while (start < text.size()) {
        size_t nl = text.find('\n', start);
        std::string_view line = text.substr(start, nl == std::string_view::npos ? std::string_view::npos : nl - start);
        start = nl == std::string_view::npos ? text.size() : nl + 1;
        if (line.size() > 4000) line = line.substr(0, 4000);  // std::regex recurses; keep it bounded
        bool candidate = std::any_of(r.contains.begin(), r.contains.end(),
                                     [&](const std::string& w) { return contains_icase(line, w); });
        if (!candidate) continue;
        std::match_results<std::string_view::const_iterator> m;
        if (std::regex_search(line.begin(), line.end(), m, *r.regex)) {
            const size_t offset = static_cast<size_t>(without_prefix(line).data() - line.data());
            const size_t at = static_cast<size_t>(m.position(0));
            evidence = around(without_prefix(line), at >= offset ? at - offset : 0);
            for (size_t i = 0; i < r.captures.size(); ++i) {
                captures.emplace_back(r.captures[i], m[i + 1].matched ? m[i + 1].str() : std::string());
            }
            return true;
        }
    }
    return false;
}

}  // namespace

std::string_view severity_name(Severity s) {
    switch (s) {
        case Severity::Info: return "info";
        case Severity::Warning: return "warning";
        case Severity::Error: return "error";
        case Severity::Critical: return "critical";
    }
    return "error";
}

std::string render(std::string_view text, const Captures& captures) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '{') {
            size_t end = text.find('}', i);
            if (end != std::string_view::npos) {
                std::string_view name = text.substr(i + 1, end - i - 1);
                auto it = std::find_if(captures.begin(), captures.end(), [&](const auto& c) { return c.first == name; });
                if (it != captures.end()) {
                    out += it->second.empty() ? "?" : it->second;
                    i = end;
                    continue;
                }
            }
        }
        out += text[i];
    }
    return out;
}

std::shared_ptr<RuleSet> RuleSet::builtin() {
    auto set = std::make_shared<RuleSet>();
    std::vector<std::string> errors;
    for (size_t i = 0; i < kBuiltinRuleCount; ++i) {
        const auto& f = kBuiltinRules[i];
        set->add(std::string_view(reinterpret_cast<const char*>(f.data), f.size), std::string("built-in/") + f.name, errors);
    }
    // the tests load every built-in file and fail on any error, so this can't happen in a release
    for (const auto& e : errors) std::fprintf(stderr, "culprit: %s\n", e.c_str());
    for (auto& r : set->rules_) r->origin = "built-in";
    return set;
}

void RuleSet::add(std::string_view text, const std::string& origin, std::vector<std::string>& errors) {
    toml::Error err;
    auto doc = toml::parse(text, err);
    if (!doc) {
        errors.push_back(origin + ":" + std::to_string(err.line) + ": " + err.message);
        return;
    }
    for (const auto& [name, table] : doc->tables) {
        errors.push_back(origin + ":" + std::to_string(table.line) + ": unknown section [" + name + "]; rules are [[rule]]");
    }
    for (const auto& [name, list] : doc->arrays) {
        if (name != "rule") {
            errors.push_back(origin + ":" + std::to_string(list.front().line) + ": unknown section [[" + name + "]]; rules are [[rule]]");
        }
    }
    if (!doc->root.values.empty()) {
        errors.push_back(origin + ":" + std::to_string(doc->root.values.front().second.line) +
                         ": keys must be inside a [[rule]] section");
    }
    auto it = doc->arrays.find("rule");
    if (it == doc->arrays.end()) return;

    for (const auto& table : it->second) {
        try {
            bool disabled = false;
            Rule r = build_rule(table, disabled);
            r.origin = origin;
            auto existing = std::find_if(rules_.begin(), rules_.end(), [&](const auto& p) { return p->id == r.id; });
            if (disabled) {
                if (existing != rules_.end()) rules_.erase(existing);
            } else if (existing != rules_.end()) {
                // a later file (yours) replaces an earlier rule (culprit's) with the same id; a repeat in
                // the same file is a mistake
                if ((*existing)->origin == origin) {
                    errors.push_back(origin + ":" + std::to_string(table.line) + ": rule " + r.id + " is defined twice");
                    continue;
                }
                **existing = std::move(r);
            } else {
                rules_.push_back(std::make_unique<Rule>(std::move(r)));
            }
        } catch (const RuleError& e) {
            errors.push_back(origin + ":" + std::to_string(e.line) + ": " + e.message);
        }
    }
    compiled_ = false;
}

void RuleSet::add_path(const std::string& path, std::vector<std::string>& errors) {
    std::error_code ec;
    std::vector<fs::path> files;
    if (fs::is_directory(path, ec)) {
        for (const auto& entry : fs::directory_iterator(path, ec)) {
            if (entry.path().extension() == ".toml") files.push_back(entry.path());
        }
        std::sort(files.begin(), files.end());
    } else if (fs::exists(path, ec)) {
        files.push_back(path);
    } else {
        errors.push_back(path + ": no such file or folder");
        return;
    }
    for (const auto& f : files) {
        std::ifstream in(f, std::ios::binary);
        if (!in) {
            errors.push_back(f.string() + ": can't be read");
            continue;
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        add(ss.str(), f.string(), errors);
    }
}

const Rule* RuleSet::find(std::string_view id) const {
    for (const auto& r : rules_) {
        if (r->id == id) return r.get();
    }
    return nullptr;
}

void RuleSet::compile() const {
    nodes_.assign(1, Node{});
    field_rules_.clear();
    for (size_t ri = 0; ri < rules_.size(); ++ri) {
        const Rule& r = *rules_[ri];
        if (r.contains.empty()) field_rules_.push_back(static_cast<int>(ri));
        for (const auto& word : r.contains) {
            int n = 0;
            for (unsigned char c : word) {
                auto& next = nodes_[n].next;
                auto it = std::find_if(next.begin(), next.end(), [c](const auto& p) { return p.first == c; });
                if (it != next.end()) {
                    n = it->second;
                } else {
                    nodes_.push_back(Node{});
                    int created = static_cast<int>(nodes_.size()) - 1;
                    nodes_[n].next.emplace_back(c, created);
                    n = created;
                }
            }
            nodes_[n].rules.push_back(static_cast<int>(ri));
        }
    }
    // failure links, breadth first
    std::queue<int> q;
    for (const auto& [c, child] : nodes_[0].next) {
        nodes_[child].fail = 0;
        q.push(child);
    }
    while (!q.empty()) {
        int n = q.front();
        q.pop();
        for (const auto& [c, child] : nodes_[n].next) {
            int f = nodes_[n].fail;
            while (true) {
                auto& fn = nodes_[f].next;
                auto it = std::find_if(fn.begin(), fn.end(), [c = c](const auto& p) { return p.first == c; });
                if (it != fn.end() && it->second != child) {
                    nodes_[child].fail = it->second;
                    break;
                }
                if (f == 0) {
                    nodes_[child].fail = 0;
                    break;
                }
                f = nodes_[f].fail;
            }
            const auto& inherited = nodes_[nodes_[child].fail].rules;
            nodes_[child].rules.insert(nodes_[child].rules.end(), inherited.begin(), inherited.end());
            q.push(child);
        }
    }
    // a full transition table, built breadth first so a node's failure target is complete before it
    delta_.assign(nodes_.size() * 256, 0);
    std::vector<int> order{0};
    for (size_t i = 0; i < order.size(); ++i) {
        for (const auto& [c, child] : nodes_[order[i]].next) order.push_back(child);
    }
    for (int n : order) {
        int32_t* row = &delta_[static_cast<size_t>(n) * 256];
        if (n != 0) {
            const int32_t* fail_row = &delta_[static_cast<size_t>(nodes_[n].fail) * 256];
            std::copy(fail_row, fail_row + 256, row);
        }
        for (const auto& [c, child] : nodes_[n].next) {
            row[c] = child;
            // ASCII words were stored lower case; upper case input folds to them
            if (c >= 'a' && c <= 'z') row[c - 'a' + 'A'] = child;
        }
    }
    compiled_ = true;
}

void RuleSet::scan(std::string_view text, std::vector<int>& hits) const {
    int32_t n = 0;
    const int32_t* delta = delta_.data();
    for (char raw : text) {
        n = delta[static_cast<size_t>(n) * 256 + static_cast<unsigned char>(raw)];
        if (!nodes_[n].rules.empty()) {
            for (int r : nodes_[n].rules) hits.push_back(r);
        }
    }
}

std::optional<RuleMatch> RuleSet::match(const Entry& e) const {
    if (rules_.empty()) return std::nullopt;
    if (!compiled_) compile();

    // Text logs: the whole entry as written, since parsing may have moved part of the text into the thread
    // or logger. JSON logs: the message, the stack and every field (the raw text has JSON escapes).
    const bool json = e.format == "json";
    std::vector<int> candidates;
    if (json) {
        scan(e.message, candidates);
        scan(e.stack, candidates);
        for (const auto& [k, v] : e.fields) scan(v, candidates);
    } else {
        scan(e.raw.empty() ? e.message : e.raw, candidates);
    }
    for (int r : field_rules_) candidates.push_back(r);
    if (candidates.empty()) return std::nullopt;
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    // highest priority first; equal priorities keep load order
    std::stable_sort(candidates.begin(), candidates.end(),
                     [&](int a, int b) { return rules_[a]->priority > rules_[b]->priority; });

    for (int ri : candidates) {
        const Rule& r = *rules_[ri];
        if (!level_ok(r, e.level)) continue;
        Captures caps;
        if (!r.field.empty()) {
            // the field's value is available as {field}
            const std::string* v = e.field(r.field);
            if (!v || std::find(r.values.begin(), r.values.end(), *v) == r.values.end()) continue;
            caps.emplace_back(r.field, *v);
        }
        const std::string_view text = !json && !e.raw.empty() ? std::string_view(e.raw) : std::string_view(e.message);
        if (!r.regex) {
            std::string evidence = r.contains.empty() ? std::string(without_prefix(text.substr(0, text.find('\n'))))
                                                      : evidence_line(r, text);
            if (evidence.empty() && json) evidence = evidence_line(r, e.stack);
            if (evidence.empty()) evidence = e.message.substr(0, e.message.find('\n'));
            return RuleMatch{&r, std::move(caps), std::move(evidence)};
        }
        std::string evidence;
        if (!json) {
            if (regex_lines(r, text, caps, evidence)) return RuleMatch{&r, std::move(caps), std::move(evidence)};
            continue;
        }
        if (regex_lines(r, e.message, caps, evidence) || regex_lines(r, e.stack, caps, evidence)) {
            return RuleMatch{&r, std::move(caps), std::move(evidence)};
        }
        for (const auto& [k, v] : e.fields) {
            if (regex_lines(r, v, caps, evidence)) return RuleMatch{&r, std::move(caps), std::move(evidence)};
        }
    }
    return std::nullopt;
}

size_t RuleSet::check_examples(std::vector<std::string>& errors) const {
    size_t checked = 0;
    for (const auto& r : rules_) {
        for (size_t i = 0; i < r->examples.size(); ++i) {
            ++checked;
            Parser parser;
            std::vector<Entry> entries;
            std::string_view text = r->examples[i];
            size_t start = 0;
            while (start <= text.size()) {
                size_t nl = text.find('\n', start);
                parser.feed(text.substr(start, nl == std::string_view::npos ? std::string_view::npos : nl - start), entries);
                if (nl == std::string_view::npos) break;
                start = nl + 1;
            }
            parser.flush(entries);
            std::string where = r->origin + ":" + std::to_string(r->line) + ": " + r->id + " example " + std::to_string(i + 1);
            if (entries.empty()) {
                errors.push_back(where + " has no log line");
                continue;
            }
            auto m = match(entries.front());
            if (!m) {
                errors.push_back(where + " isn't recognised by any rule");
            } else if (m->rule != r.get()) {
                errors.push_back(where + " is recognised by " + m->rule->id + " instead (raise this rule's priority?)");
            } else {
                for (const auto& [name, value] : m->captures) {
                    if (value.empty() && name != r->field) errors.push_back(where + " captured nothing for {" + name + "}");
                }
            }
        }
    }
    return checked;
}

std::vector<std::string> default_rule_paths(const std::string& start) {
    std::vector<std::string> out;
    std::error_code ec;
#ifdef _WIN32
    if (const char* appdata = std::getenv("APPDATA")) {
        fs::path p = fs::path(appdata) / "culprit" / "rules";
        if (fs::is_directory(p, ec)) out.push_back(p.string());
    }
#else
    fs::path config;
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) config = xdg;
    else if (const char* home = std::getenv("HOME")) config = fs::path(home) / ".config";
    if (!config.empty() && fs::is_directory(config / "culprit" / "rules", ec)) out.push_back((config / "culprit" / "rules").string());
#endif
    fs::path dir = fs::absolute(start.empty() ? fs::path(".") : fs::path(start), ec);
    while (!dir.empty()) {
        fs::path candidate = dir / ".culprit" / "rules";
        if (fs::is_directory(candidate, ec)) {
            out.push_back(candidate.string());
            break;
        }
        fs::path parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }
    return out;
}

}  // namespace culprit
