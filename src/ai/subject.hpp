#pragma once
// Turns what culprit knows (an entry, the rule that matched it, the analysis of the whole log) into an
// investigation subject.

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "ai/investigator.hpp"
#include "core/analyzer.hpp"
#include "core/entry.hpp"
#include "core/rules.hpp"

namespace culprit::ai {

/// `before`: the entries just before it, oldest first. `report` and `rules` may be null.
Subject make_subject(const Entry& e, const std::vector<const Entry*>& before, const Report* report, const RuleSet* rules);

/// What to investigate when nothing is chosen: the likely root cause's first entry, else the first error.
std::optional<uint32_t> default_target(const Report& report);

/// The most useful occurrence of the same problem or group near `index`: one with a stack trace, if one
/// of the next entries recognised the same way has it. `count` entries are reachable through `get`.
uint32_t richest_occurrence(uint32_t index, size_t count, const std::function<const Entry&(uint32_t)>& get);

/// Entries containing `text` (ignoring case), as "line N  source  message" lines, at most `max`.
std::string search_entries(const std::vector<Entry>& entries, const std::string& text, size_t max = 30);

}  // namespace culprit::ai
