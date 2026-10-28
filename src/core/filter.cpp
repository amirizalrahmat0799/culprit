#include "core/filter.hpp"

#include <algorithm>
#include <cctype>

namespace culprit {
namespace {

char fold(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = fold(c);
    return out;
}

// splits on spaces, keeping "quoted phrases" and /regex with spaces/ together
std::vector<std::string> tokenize(std::string_view q) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < q.size()) {
        while (i < q.size() && q[i] == ' ') ++i;
        if (i >= q.size()) break;
        std::string tok;
        bool negate = q[i] == '-' && i + 1 < q.size() && q[i + 1] != ' ';
        if (negate) {
            tok += '-';
            ++i;
        }
        if (q[i] == '"' || q[i] == '/') {
            char close = q[i];
            tok += q[i++];
            while (i < q.size() && q[i] != close) tok += q[i++];
            if (i < q.size()) tok += q[i++];
        } else {
            while (i < q.size() && q[i] != ' ') {
                if (q[i] == '"') {  // key="value with spaces"
                    tok += q[i++];
                    while (i < q.size() && q[i] != '"') tok += q[i++];
                    if (i < q.size()) tok += q[i++];
                } else {
                    tok += q[i++];
                }
            }
        }
        out.push_back(std::move(tok));
    }
    return out;
}

std::string unquote(std::string_view s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') return std::string(s.substr(1, s.size() - 2));
    if (!s.empty() && s.front() == '"') return std::string(s.substr(1));
    return std::string(s);
}

}  // namespace

bool contains_icase(std::string_view hay, std::string_view needle) {
    if (needle.empty()) return true;
    if (needle.size() > hay.size()) return false;
    const char first = needle[0];
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        if (fold(hay[i]) != first) continue;
        size_t j = 1;
        while (j < needle.size() && fold(hay[i + j]) == needle[j]) ++j;
        if (j == needle.size()) return true;
    }
    return false;
}

Filter Filter::parse(std::string_view query) {
    Filter f;
    for (std::string tok : tokenize(query)) {
        Term t;
        if (tok.size() > 1 && tok[0] == '-') {
            t.negate = true;
            tok.erase(0, 1);
        }
        if (tok.size() >= 2 && tok.front() == '/' ) {
            std::string pattern = tok.substr(1, tok.back() == '/' && tok.size() > 1 ? tok.size() - 2 : std::string::npos);
            if (pattern.empty()) continue;
            // std::regex throws on a bad pattern; the core of culprit otherwise avoids exceptions
            try {
                t.regex = std::make_shared<std::regex>(pattern, std::regex::ECMAScript | std::regex::icase);
                t.kind = Kind::Regex;
            } catch (const std::regex_error&) {
                f.error_ = "not a valid regular expression: " + pattern;
                continue;
            }
            f.terms_.push_back(std::move(t));
            continue;
        }
        if (lower(tok) == "has:stack") {
            t.kind = Kind::HasStack;
            f.terms_.push_back(std::move(t));
            continue;
        }

        // key<op>value
        size_t op_pos = tok.find_first_of("=!<>:");
        if (op_pos != std::string::npos && op_pos > 0 && tok[0] != '"') {
            std::string key = lower(tok.substr(0, op_pos));
            std::string rest = tok.substr(op_pos);
            Op op = Op::Eq;
            size_t skip = 1;
            if (rest.rfind(">=", 0) == 0) op = Op::Ge, skip = 2;
            else if (rest.rfind("<=", 0) == 0) op = Op::Le, skip = 2;
            else if (rest.rfind("!=", 0) == 0) op = Op::Ne, skip = 2;
            else if (rest[0] == '>') op = Op::Gt;
            else if (rest[0] == '<') op = Op::Lt;
            else if (rest[0] == '!') op = Op::Ne;
            std::string value = lower(unquote(rest.substr(skip)));
            if (value.empty()) continue;  // still typing

            if (key == "level" || key == "lvl") {
                auto lv = parse_level(value);
                if (!lv) {
                    f.error_ = "unknown level \"" + value + "\" (trace, debug, info, warn, error, fatal)";
                    continue;
                }
                t.kind = Kind::LevelCmp;
                t.op = op;
                t.level = static_cast<int>(*lv);
                f.terms_.push_back(std::move(t));
                continue;
            }
            t.kind = Kind::Field;
            t.key = key;
            t.value = value;
            t.op = op == Op::Ne ? Op::Ne : Op::Eq;
            if (t.op == Op::Ne) t.negate = !t.negate;
            f.highlights_.push_back(value);
            f.terms_.push_back(std::move(t));
            continue;
        }

        t.kind = Kind::Text;
        t.value = lower(unquote(tok));
        if (t.value.empty()) continue;
        if (!t.negate) f.highlights_.push_back(t.value);
        f.terms_.push_back(std::move(t));
    }
    return f;
}

bool Filter::match_term(const Term& t, const Entry& e) const {
    switch (t.kind) {
        case Kind::HasStack: return e.has_stack();
        case Kind::LevelCmp: {
            if (e.level == Level::Unknown) return false;
            int l = static_cast<int>(e.level);
            switch (t.op) {
                case Op::Ge: return l >= t.level;
                case Op::Le: return l <= t.level;
                case Op::Gt: return l > t.level;
                case Op::Lt: return l < t.level;
                case Op::Ne: return l != t.level;
                case Op::Eq: return l == t.level;
            }
            return false;
        }
        case Kind::Regex:
            return std::regex_search(e.message, *t.regex) || std::regex_search(e.stack, *t.regex) ||
                   std::regex_search(e.logger, *t.regex);
        case Kind::Field: {
            const std::string* value = nullptr;
            if (t.key == "logger") value = &e.logger;
            else if (t.key == "thread") value = &e.thread;
            else if (t.key == "source" || t.key == "service") value = &e.source;
            else if (t.key == "msg" || t.key == "message") value = &e.message;
            else if (t.key == "stack") value = &e.stack;
            else {
                for (const auto& [k, v] : e.fields) {
                    if (k.size() == t.key.size() && lower(k) == t.key) {
                        value = &v;
                        break;
                    }
                }
            }
            return value && contains_icase(*value, t.value);
        }
        case Kind::Text:
            if (contains_icase(e.message, t.value) || contains_icase(e.logger, t.value) ||
                contains_icase(e.stack, t.value) || contains_icase(e.thread, t.value) ||
                contains_icase(e.source, t.value)) {
                return true;
            }
            for (const auto& [k, v] : e.fields) {
                if (contains_icase(v, t.value)) return true;
            }
            return false;
    }
    return false;
}

bool Filter::matches(const Entry& e) const {
    for (const auto& t : terms_) {
        if (match_term(t, e) == t.negate) return false;
    }
    return true;
}

}  // namespace culprit
