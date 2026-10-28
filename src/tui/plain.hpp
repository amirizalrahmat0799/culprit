#pragma once
// Plain output: one formatted line per entry (plus its stack trace), for pipes, files and CI, where an
// interactive viewer makes no sense.

#include <cstdio>
#include <string>

#include "core/entry.hpp"
#include "core/filter.hpp"

namespace culprit {

struct PlainOptions {
    bool color = true;
    bool show_source = false;
    bool show_stack = true;
};

void print_plain(std::FILE* out, const Entry& e, const PlainOptions& options);

}  // namespace culprit
