#pragma once
// Rules that recognise known problems in log entries ("port 8080 is already in use", "the connection pool
// ran out") and say what they mean and what to do. Rules are TOML files: culprit's own are compiled into
// the program, and more can be added per user (~/.config/culprit/rules) and per project (.culprit/rules).
//
//   [[rule]]
//   id       = "spring.port-in-use"
//   title    = "Port {port} is already in use"
//   severity = "critical"                       # info, warning, error (default), critical
//   contains = ["was already in use"]           # any of these, ignoring case
//   regex    = 'Port (\d+) was already in use'  # optional, on the lines that contain one of the above
//   captures = ["port"]                         # names for the regex groups: {port}
//   explain  = "Another program is listening on {port}..."
//   fix      = ["Find it: lsof -i :{port}", "Or start on another port: server.port=8081"]
//
// See docs/rules.md for every key.

#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/entry.hpp"

namespace culprit {

enum class Severity : uint8_t { Info, Warning, Error, Critical };
std::string_view severity_name(Severity s);  // "info", "warning", "error", "critical"

struct Rule {
    std::string id;
    std::string title;
    Severity severity = Severity::Error;
    bool symptom = false;  // usually the result of another problem (connection refused, 502), not the cause
    int priority = 0;      // when several rules match one entry, the highest wins
    std::vector<std::string> tags;

    std::vector<std::string> contains;  // lower case
    std::string regex_text;
    std::shared_ptr<const std::regex> regex;
    std::vector<std::string> captures;
    std::string field;                  // match this field's value instead of the text
    std::vector<std::string> values;
    Level min_level = Level::Warn;      // entries with a known level below this don't match
    bool any_level = false;

    std::string explain;
    std::vector<std::string> fix;
    std::string docs;
    std::vector<std::string> examples;  // log text this rule must recognise; checked by `culprit rules --check`

    std::string origin;  // "built-in", or the file it came from
    int line = 0;
};

using Captures = std::vector<std::pair<std::string, std::string>>;

struct RuleMatch {
    const Rule* rule = nullptr;
    Captures captures;
    std::string evidence;  // the line that matched, as written (without a Compose/kubectl prefix)
};

/// Replaces {name} with the captured value.
std::string render(std::string_view text, const Captures& captures);

class RuleSet {
public:
    /// culprit's own rules.
    static std::shared_ptr<RuleSet> builtin();

    /// Adds the rules in one TOML text. A rule with the id of an earlier one replaces it, and
    /// `disabled = true` removes it. Problems are added to `errors` as "origin:line: message"; a rule
    /// with a problem is skipped, the others still load.
    void add(std::string_view toml_text, const std::string& origin, std::vector<std::string>& errors);
    /// A .toml file, or every .toml file in a folder (sorted by name).
    void add_path(const std::string& path, std::vector<std::string>& errors);

    /// The best rule for this entry, if any.
    std::optional<RuleMatch> match(const Entry& e) const;

    const std::vector<std::unique_ptr<Rule>>& rules() const { return rules_; }
    const Rule* find(std::string_view id) const;

    /// Parses each rule's examples and checks the rule recognises them (and no other rule wins). Problems
    /// are added to `errors`; returns how many examples were checked.
    size_t check_examples(std::vector<std::string>& errors) const;

private:
    void compile() const;

    std::vector<std::unique_ptr<Rule>> rules_;  // pointers stay valid while the set lives

    // Aho-Corasick automaton over every rule's `contains` words, so each entry is scanned once no matter
    // how many rules there are. Built on first use after rules change.
    struct Node {
        std::vector<std::pair<unsigned char, int>> next;
        int fail = 0;
        std::vector<int> rules;  // indices into rules_ that end here (including via fail links)
    };
    mutable std::vector<Node> nodes_;
    mutable std::vector<int32_t> delta_;  // nodes x 256: the next node for every byte, failure links folded in
    mutable std::vector<int> field_rules_;
    mutable bool compiled_ = false;
    void scan(std::string_view text, std::vector<int>& hits) const;
};

/// Where culprit looks for more rules: the user's folder, then the nearest .culprit/rules above `start`.
std::vector<std::string> default_rule_paths(const std::string& start);

}  // namespace culprit
