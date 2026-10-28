#pragma once
// Printing an AI investigation: its steps as they happen, and the finding.

#include <cstdio>
#include <string>

#include "ai/investigator.hpp"
#include "tui/report.hpp"

namespace culprit {

void print_step(std::FILE* out, const ai::Step& step, bool color);
void print_finding(std::FILE* out, const ai::Finding& finding, const ai::Subject& subject, ReportFormat format, bool color,
                   int width = 100);

}  // namespace culprit
