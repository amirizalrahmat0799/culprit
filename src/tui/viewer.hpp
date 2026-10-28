#pragma once
// The interactive terminal viewer.

#include <memory>
#include <string>

#include "core/reader.hpp"
#include "core/rules.hpp"

namespace culprit {

struct ViewerOptions {
    std::string title;          // shown in the header: file names, or "stdin"
    std::string initial_filter;
    std::shared_ptr<const RuleSet> rules;  // for recognising problems; none means no analysis
    std::string message;                   // shown in the status line at start (problems in rule files)
    bool no_ai = false;                    // --no-ai
};

/// Runs until the user quits. Returns the process exit code.
int run_viewer(Reader& reader, const ViewerOptions& options);

}  // namespace culprit
