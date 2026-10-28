#pragma once
// Everything the viewer shows, without the terminal: all entries, the current filter, which entries pass
// it, and counts per level. Kept apart from the UI so it can be tested on its own.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/analyzer.hpp"
#include "core/entry.hpp"
#include "core/filter.hpp"

namespace culprit {

class LogModel {
public:
    explicit LogModel(std::shared_ptr<const RuleSet> rules = nullptr) : analyzer_(std::move(rules)) {}

    /// Adds entries; the new ones are checked against the current filter only.
    void append(std::vector<Entry> batch);

    /// Replaces the filter and recomputes which entries are visible.
    void set_filter(const std::string& query);
    const std::string& query() const { return query_; }
    const Filter& filter() const { return filter_; }

    size_t total() const { return entries_.size(); }
    const Entry& entry(size_t index) const { return entries_[index]; }
    /// Indices of the entries that pass the filter, in order.
    const std::vector<uint32_t>& visible() const { return visible_; }

    size_t count(Level level) const { return counts_[static_cast<size_t>(level)]; }
    /// Whether several sources (files or Compose services) appear, so the viewer shows a source column.
    bool multiple_sources() const { return multiple_sources_; }

    /// The next (or previous, `direction` -1) visible row after `from` with a level of at least `min`.
    std::optional<size_t> find(size_t from, int direction, Level min) const;

    /// What the analyser found so far.
    const Analyzer& analyzer() const { return analyzer_; }

private:
    std::vector<Entry> entries_;
    std::vector<uint32_t> visible_;
    std::array<size_t, 7> counts_{};
    std::string query_;
    Filter filter_;
    std::string first_source_;
    bool multiple_sources_ = false;
    Analyzer analyzer_;
};

}  // namespace culprit
