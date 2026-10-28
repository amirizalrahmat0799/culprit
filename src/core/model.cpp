#include "core/model.hpp"

namespace culprit {

void LogModel::append(std::vector<Entry> batch) {
    entries_.reserve(entries_.size() + batch.size());
    for (auto& e : batch) {
        ++counts_[static_cast<size_t>(e.level)];
        if (!e.source.empty()) {
            if (first_source_.empty()) first_source_ = e.source;
            else if (e.source != first_source_) multiple_sources_ = true;
        }
        analyzer_.add(e, static_cast<uint32_t>(entries_.size()));
        if (filter_.matches(e)) visible_.push_back(static_cast<uint32_t>(entries_.size()));
        entries_.push_back(std::move(e));
    }
}

void LogModel::set_filter(const std::string& query) {
    query_ = query;
    filter_ = Filter::parse(query);
    visible_.clear();
    for (size_t i = 0; i < entries_.size(); ++i) {
        if (filter_.matches(entries_[i])) visible_.push_back(static_cast<uint32_t>(i));
    }
}

std::optional<size_t> LogModel::find(size_t from, int direction, Level min) const {
    if (visible_.empty()) return std::nullopt;
    long long i = static_cast<long long>(from);
    const long long n = static_cast<long long>(visible_.size());
    for (long long step = 0; step < n; ++step) {
        i += direction;
        if (i < 0 || i >= n) return std::nullopt;
        const Entry& e = entries_[visible_[static_cast<size_t>(i)]];
        if (e.level != Level::Unknown && e.level >= min) return static_cast<size_t>(i);
    }
    return std::nullopt;
}

}  // namespace culprit
