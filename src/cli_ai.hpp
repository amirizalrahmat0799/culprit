#pragma once
// The AI commands: culprit ai setup | status | on | off, culprit investigate, and analyze --investigate.

#include <cstdint>
#include <string>
#include <vector>

#include "core/analyzer.hpp"
#include "core/entry.hpp"
#include "core/rules.hpp"
#include "tui/report.hpp"

namespace culprit::cli {

int run_ai(int argc, char** argv);
int run_investigate(int argc, char** argv);

struct InvestigateOptions {
    ReportFormat format = ReportFormat::Text;
    bool color = false;
    bool yes = false;          // don't ask before sending to a provider that isn't local
    bool no_ai = false;
    bool show_prompt = false;  // print what would be sent, and stop
    std::string project;       // the project folder; the current one by default
    std::string model;         // overrides the configured model
    int max_steps = 0;         // overrides the configured limit
    int width = 100;
};

/// Investigates entries[index]. Returns the exit status: 0 when there's a finding.
int investigate_entry(const std::vector<Entry>& entries, uint32_t index, const Report& report, const RuleSet& rules,
                      const InvestigateOptions& options);

}  // namespace culprit::cli
