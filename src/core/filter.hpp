#pragma once
// The filter you type in the viewer. Terms are separated by spaces and must all match:
//
//   payment                 the word anywhere (message, logger, stack trace, fields), ignoring case
//   "order failed"          a phrase
//   -health                 leave out entries containing the word
//   level>=warn             level at least WARN (also level=error, level<=info, level:error)
//   logger=OrderService     a part matches: logger, thread, source, message, or any field by name
//   userId!=42              the field doesn't contain this
//   /time(d)? out/          a regular expression, ignoring case
//   has:stack               only entries with a stack trace
//   has:problem             entries a rule recognised
//   problem:hikari          entries recognised by a rule whose id starts with this
//   group:3                 unrecognised errors in group 3 (as numbered by the analyser)

#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

#include "core/entry.hpp"

namespace culprit {

class Filter {
public:
    /// An empty query matches everything. On a mistake (bad regex, unknown level), `error()` explains it
    /// and the filter matches everything, so typing never hides the logs mid-edit.
    static Filter parse(std::string_view query);

    bool matches(const Entry& e) const;
    bool empty() const { return terms_.empty(); }
    const std::string& error() const { return error_; }
    /// Words and phrases to highlight in the viewer.
    const std::vector<std::string>& highlights() const { return highlights_; }

private:
    enum class Kind { Text, LevelCmp, Field, Regex, HasStack, Problem, Group };
    enum class Op { Eq, Ne, Ge, Le, Gt, Lt };
    struct Term {
        Kind kind = Kind::Text;
        bool negate = false;
        std::string key;    // lower case
        std::string value;  // lower case
        Op op = Op::Eq;
        int level = 0;
        std::shared_ptr<std::regex> regex;
    };
    bool match_term(const Term& t, const Entry& e) const;

    std::vector<Term> terms_;
    std::vector<std::string> highlights_;
    std::string error_;
};

/// Case-insensitive substring search (ASCII case folding, UTF-8 safe).
bool contains_icase(std::string_view haystack, std::string_view needle_lower);

/// std::regex recurses about once per character it scans. With libstdc++ and libc++ one long line (a 50 KB
/// JSON message) overflows the stack; MSVC instead throws regex_error past a depth of 600 on x64
/// (_REGEX_MAX_STACK_COUNT). Lines longer than this are searched in overlapping windows; a match longer
/// than the overlap that crosses a window edge is missed, which is the price of not crashing.
inline constexpr size_t kRegexWindow = 500;

/// Calls `f` with `line` itself, or with overlapping windows of it, until `f` returns true. When `f` throws
/// regex_error (MSVC's depth or complexity limit), the window is searched again in smaller pieces.
template <class F>
bool regex_windows(std::string_view line, F&& f, size_t window = kRegexWindow) {
    auto attempt = [&](std::string_view w) -> bool {
        try {
            return f(w);
        } catch (const std::regex_error&) {
            if (window <= 64) return false;
            return regex_windows(w, f, window / 2);
        }
    };
    if (line.size() <= window) return attempt(line);
    const size_t step = window - window / 5;
    for (size_t at = 0;; at += step) {
        if (attempt(line.substr(at, window))) return true;
        if (at + window >= line.size()) return false;
    }
}

/// Whether `re` matches any line of `text` (searched line by line and in windows, see kRegexWindow).
bool regex_search_lines(std::string_view text, const std::regex& re);

}  // namespace culprit
