#pragma once
// The report `culprit analyze` prints: as text for a terminal, Markdown for a pull request or chat, or
// JSON for other tools. Also the listings of `culprit rules`.

#include <cstdio>
#include <string>

#include "core/analyzer.hpp"
#include "core/rules.hpp"

namespace culprit {

enum class ReportFormat { Text, Markdown, Json };

struct ReportOptions {
    ReportFormat format = ReportFormat::Text;
    bool color = false;
    std::string title;  // what was analysed: file names, or "stdin"
    size_t top = 10;    // groups of unrecognised errors to list
    int width = 100;
};

void print_report(std::FILE* out, const Report& report, const ReportOptions& options);

/// `culprit rules`: one line per rule.
void print_rule_list(std::FILE* out, const RuleSet& rules, bool color);
/// `culprit rules <id>`: everything about one rule.
void print_rule(std::FILE* out, const Rule& rule, bool color);

}  // namespace culprit
