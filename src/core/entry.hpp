#pragma once
// One log entry: a line (or a line plus its stack trace) broken into the parts every format shares.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace culprit {

struct Rule;

enum class Level : uint8_t { Unknown, Trace, Debug, Info, Warn, Error, Fatal };

std::string_view level_name(Level level);        // "ERROR", "-" for unknown
std::optional<Level> parse_level(std::string_view text);  // "warn", "WARNING", "E", "50" (pino) ...

struct Entry {
    // When it happened. `time_text` is the clock time as written in the log ("12:00:01.234"), shown as is;
    // `epoch_ms` is set when the timestamp could be parsed completely, for ordering and "jump to time".
    std::string time_text;
    std::optional<int64_t> epoch_ms;
    bool time_zoned = false;  // the timestamp said its time zone, so it compares with other sources' times

    Level level = Level::Unknown;
    std::string source;   // file name, or Docker Compose service ("backend-1"), when there's more than one
    std::string logger;   // "com.example.shop.OrderService"
    std::string thread;
    std::string message;
    std::string stack;    // stack trace or traceback, possibly many lines
    std::vector<std::pair<std::string, std::string>> fields;  // everything else (JSON fields, key=value pairs)

    std::string raw;      // the original text, including continuation lines
    std::string format;   // which parser understood it: "json", "spring", "logback", "logfmt", "python", "text"
    uint32_t line = 0;    // line number in its input where the entry starts, 1-based

    // set by the analyser
    const Rule* rule = nullptr;     // the rule that recognised it
    uint32_t group = UINT32_MAX;    // the group of unrecognised errors or warnings it belongs to

    const std::string* field(std::string_view key) const {
        for (const auto& [k, v] : fields) {
            if (k == key) return &v;
        }
        return nullptr;
    }
    bool has_stack() const { return !stack.empty(); }
};

}  // namespace culprit
