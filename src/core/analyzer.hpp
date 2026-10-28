#pragma once
// Looks at entries as they arrive and works out what went wrong:
//
//   problems   entries a rule recognised ("Port 8080 is already in use"), one per rule, with a count
//   groups     errors and warnings no rule knows, grouped when they're the same error (same exception,
//              same message apart from numbers and ids, same place in your code)
//   root cause the earliest problem that causes others rather than being caused (a migration that failed,
//              not the "connection refused" that follows), and how many errors came after it
//
// Entries are analysed one at a time, so the viewer can keep it up to date while following a log.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/entry.hpp"
#include "core/rules.hpp"
#include "core/stack.hpp"

namespace culprit {

struct Occurrence {
    uint32_t index = 0;  // position among all entries
    uint32_t line = 0;   // line in its input, 1-based
    std::string source;
    std::string time;
    std::optional<int64_t> epoch_ms;
    bool zoned = false;
};

/// A rule that matched one or more entries.
struct Problem {
    const Rule* rule = nullptr;
    size_t count = 0;
    Occurrence first, last;
    Captures captures;                  // from the first match
    std::vector<std::string> variants;  // distinct titles, when captures differ ("Port 8080", "Port 9090")
    std::vector<std::string> sources;
    std::optional<Frame> where;         // first frame in your code, from the first match
    std::string message;                // the line that matched, from the first entry

    std::string title() const { return render(rule->title, captures); }
    std::string explain() const { return render(rule->explain, captures); }
    std::vector<std::string> fix() const;
};

/// Errors (or warnings) that no rule recognised, grouped by what they have in common.
struct ErrorGroup {
    uint32_t id = 0;
    Level level = Level::Error;
    std::string title;      // the first message, on one line
    std::string exception;  // the root exception: the last "Caused by:", or the traceback's last line
    size_t count = 0;
    Occurrence first, last;
    std::vector<std::string> sources;
    std::optional<Frame> where;
};

struct RootCause {
    enum class Kind { Problem, Group };
    enum class Confidence { High, Medium, Low };
    Kind kind = Kind::Problem;
    size_t index = 0;  // into Report::problems or Report::groups
    Confidence confidence = Confidence::High;
    std::string why;
    size_t followers = 0;                                     // errors after it that are likely consequences
    std::vector<std::pair<std::string, size_t>> follower_top;  // what they were, most common first
    std::string window;                                       // "in the next 2 minutes", "after it"
};

struct Report {
    std::shared_ptr<const RuleSet> rules;  // keeps the rules the problems point to alive
    size_t entries = 0;
    size_t errors = 0;
    size_t warnings = 0;
    std::vector<Problem> problems;     // most severe first, then earliest
    std::vector<ErrorGroup> groups;    // errors (then warnings), most frequent first
    std::optional<RootCause> root;
    std::optional<Occurrence> first_error;
    std::string first_error_text;
};

class Analyzer {
public:
    explicit Analyzer(std::shared_ptr<const RuleSet> rules = nullptr);

    /// Analyses one entry (its position among all entries is `index`) and marks it: `e.rule` when a rule
    /// matched, `e.group` when it joined a group of unrecognised errors or warnings.
    void add(Entry& e, uint32_t index);

    Report report() const;

    const RuleSet* rules() const { return rules_.get(); }
    /// Problems plus groups of unrecognised errors so far, for the viewer's header.
    size_t problem_count() const { return problems_.size() + error_groups_; }
    const ErrorGroup* group(uint32_t id) const { return id < groups_.size() ? &groups_[id] : nullptr; }

private:
    struct Event {
        uint32_t index;
        std::optional<int64_t> epoch_ms;
        bool zoned;
        uint16_t source;
        int32_t problem;  // -1 when it's a group
        uint32_t group;
        bool error;       // level error or worse (or a rule of severity error or worse)
    };

    std::shared_ptr<const RuleSet> rules_;
    std::vector<Problem> problems_;
    std::unordered_map<const Rule*, size_t> problem_index_;
    std::vector<ErrorGroup> groups_;
    std::unordered_map<std::string, uint32_t> group_index_;
    size_t error_groups_ = 0;
    std::vector<Event> events_;
    size_t entries_ = 0, errors_ = 0, warnings_ = 0;
    std::optional<Occurrence> first_error_;
    std::vector<std::string> sources_;  // interned, for events
    uint16_t source_id(const std::string& s);
    std::string first_error_text_;
};

/// The message with numbers, ids and hashes replaced, so the same error with different values groups
/// together: "Order 8812 not found" -> "Order # not found".
std::string normalize_message(std::string_view message);

/// The root exception of a stack trace: the last "Caused by:" line, or the exception line of a traceback.
std::string root_exception(const std::vector<Frame>& frames);

}  // namespace culprit
