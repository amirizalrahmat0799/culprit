#pragma once
// The investigation: give a model one error (with what culprit already knows about it), let it read the
// project through the Workspace's tools, step by step, and take its answer only with evidence culprit
// has checked against the real files.

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "ai/config.hpp"
#include "ai/provider.hpp"
#include "ai/workspace.hpp"

namespace culprit::ai {

/// What to investigate, gathered by the caller (the CLI or the viewer).
struct Subject {
    std::string title;      // "Database migration V7__add_refunds.sql failed", or the error message
    std::string where;      // "app-1 · 09:41:04.262 · line 11"
    std::string entry;      // the log entry as written, with its stack trace
    std::string known;      // what culprit's rules say about it, if a rule matched
    std::string context;    // log lines just before it
    std::string analysis;   // the whole log's analysis: likely root cause, what followed, first error
    std::vector<std::string> frames;  // stack frames in the project: "OrderService.java:87"
    /// Searches the whole log for text, for the search_log tool. May be empty.
    std::function<std::string(const std::string& text)> search_log;
};

struct Evidence {
    std::string file;
    int line = 0;
    std::string quote;
    bool verified = false;
    std::string note;  // why not verified, or "found at line N" when the model's number was off
};

struct Finding {
    bool ok = false;
    std::string error;          // when !ok
    std::string summary;        // the cause, in one sentence
    std::string explanation;
    std::vector<std::string> fix;
    std::vector<Evidence> evidence;
    std::string confidence;     // high, medium, low (lowered when evidence doesn't check out)
    std::string confidence_note;
    bool structured = true;     // false when the model answered in plain text instead of calling finish
    int steps = 0;
    long input_tokens = 0, output_tokens = 0;
    std::vector<std::string> files_read;
    std::string provider, model;
    bool local = true;
};

struct Step {
    enum class Kind { Thinking, Tool, Note, Warning };
    Kind kind;
    std::string text;
};

class Investigator {
public:
    Investigator(Model& model, Workspace& workspace, Settings settings);

    /// Runs to the end, calling `on_step` as it goes (from this thread). `cancel` stops it between steps
    /// and aborts a request in flight.
    Finding run(const Subject& subject, const std::function<void(const Step&)>& on_step, const std::atomic<bool>* cancel);

    /// The first message, as it will be sent (for showing what leaves the machine).
    std::string first_message(const Subject& subject);
    static std::string system_prompt(int max_steps);
    static std::vector<ToolSpec> tools();

    /// Checks each quote against the file: near the given line, then anywhere in it.
    void verify(Finding& f);

private:
    std::string run_tool(const ToolCall& call, const Subject& subject, const std::function<void(const Step&)>& on_step);

    Model& model_;
    Workspace& ws_;
    Settings settings_;
};

}  // namespace culprit::ai
