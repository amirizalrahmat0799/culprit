#pragma once
// Timestamps as logs write them: ISO 8601 ("2026-10-28T12:00:01.234+08:00"), the space-separated form
// Python and Log4j use ("2026-10-28 12:00:01,234"), a bare clock ("12:00:01.234", logback's default), and
// Unix epoch numbers in JSON (seconds or milliseconds).

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace culprit {

struct Timestamp {
    std::string clock;                // "12:00:01.234", what the viewer shows
    std::optional<int64_t> epoch_ms;  // when the date (and offset, or local time) is known
    bool zoned = false;               // the text said which time zone (Z, +08:00, UTC, or an epoch number)
    size_t length = 0;                // characters consumed from the start of the text
};

/// Reads a timestamp at the very start of `text`.
std::optional<Timestamp> parse_timestamp(std::string_view text);

/// Reads a JSON-style timestamp value: an ISO string or an epoch number (seconds, or milliseconds when
/// the number is large enough).
std::optional<Timestamp> parse_time_value(std::string_view value);

/// "12:00:01.234" in local time.
std::string clock_from_epoch_ms(int64_t ms);

}  // namespace culprit
