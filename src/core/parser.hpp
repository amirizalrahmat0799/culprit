#pragma once
// Turns lines of log text into entries.
//
// Recognised without configuration:
//   JSON         Spring Boot structured logging (ECS, Logstash, GELF), pino, bunyan, zap, slog JSON,
//                python-json-logger, Docker's json-file wrapper
//   logfmt       Go slog text, logrus, Heroku-style key=value
//   text         Spring Boot's default pattern, logback, log4j, java.util.logging, Python logging and
//                uvicorn, Go's log package, nginx error logs, and anything with a level word up front
// Prefixes added by `docker compose logs` ("backend-1  | ") and `kubectl logs --prefix` ("[pod/api/c] ")
// become the entry's source.
//
// Multi-line entries: Java stack traces ("at ...", "Caused by: ..."), Python tracebacks and Go panics
// are attached to the entry they follow, so one error is one entry.

#include <string>
#include <string_view>
#include <vector>

#include "core/entry.hpp"

namespace culprit {

class Parser {
public:
    /// `source` labels every entry (a file name), unless a Compose/kubectl prefix names one.
    explicit Parser(std::string source = {}) : source_(std::move(source)) {}

    /// Feeds one line, without its newline. Entries that are complete are appended to `out`; the newest
    /// one is held back until the next line shows whether more of it follows (a stack trace).
    void feed(std::string_view line, std::vector<Entry>& out);

    /// Emits the entry being held back. Call at the end of input, or when input has paused for a moment.
    void flush(std::vector<Entry>& out);

    bool has_pending() const { return pending_; }

private:
    bool is_continuation(std::string_view text, const std::string& prefix_source) const;

    std::string source_;
    Entry current_;
    bool pending_ = false;
    bool in_stack_ = false;
};

/// Parses a single line on its own (no multi-line handling). Exposed for tests.
Entry parse_line(std::string_view line);

}  // namespace culprit
