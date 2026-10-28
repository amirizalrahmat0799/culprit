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
    enum class Kind { Text, LevelCmp, Field, Regex, HasStack };
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

}  // namespace culprit
