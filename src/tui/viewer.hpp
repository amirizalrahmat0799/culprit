#pragma once
// The interactive terminal viewer.

#include <string>

#include "core/reader.hpp"

namespace culprit {

struct ViewerOptions {
    std::string title;          // shown in the header: file names, or "stdin"
    std::string initial_filter;
};

/// Runs until the user quits. Returns the process exit code.
int run_viewer(Reader& reader, const ViewerOptions& options);

}  // namespace culprit
