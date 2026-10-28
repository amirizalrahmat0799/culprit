#pragma once
// Pieces the commands share: reading inputs, loading rules, and talking to the terminal.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/entry.hpp"
#include "core/reader.hpp"
#include "core/rules.hpp"

namespace culprit::cli {

bool stdout_is_terminal();
bool stdin_is_terminal();
bool stderr_is_terminal();
int terminal_width();

/// Keeps a copy of piped stdin for reading logs and points stdin back at the keyboard. Returns the copy
/// (a descriptor, or a HANDLE on Windows), or -1 when that isn't possible.
intptr_t take_piped_stdin();

/// Asks a question on the terminal (even when stdin is a pipe) and returns the answer, without its newline.
/// nullopt-like empty string with `ok` false when there's no terminal to ask on.
std::string ask(const std::string& question, bool* ok = nullptr);
/// y/n question; `fallback` when the answer is empty.
bool ask_yes_no(const std::string& question, bool fallback);

/// culprit's rules, then the user's, the project's, and any given with --rules.
std::shared_ptr<RuleSet> load_rules(const std::vector<std::string>& extra, std::vector<std::string>& errors);

struct Inputs {
    std::vector<std::string> files;
    std::vector<Reader::Input> list;
    std::string title;
};
/// No files means stdin.
Inputs make_inputs(std::vector<std::string> files);
/// Reads every input to the end, calling `each` for every entry in order.
void read_all(const Inputs& in, const std::function<void(Entry&)>& each);

}  // namespace culprit::cli
