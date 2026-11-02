#pragma once
// Printing an AI investigation: its steps as they happen, and the finding.

#include <cstdio>
#include <string>

#include "ai/investigator.hpp"
#include "ai/json.hpp"
#include "tui/report.hpp"

namespace culprit {

/// The finding as JSON (what `culprit investigate --format json` prints).
ai::Json finding_json(const ai::Finding& finding, const ai::Subject& subject);

void print_step(std::FILE* out, const ai::Step& step, bool color);
void print_finding(std::FILE* out, const ai::Finding& finding, const ai::Subject& subject, ReportFormat format, bool color,
                   int width = 100);

}  // namespace culprit
