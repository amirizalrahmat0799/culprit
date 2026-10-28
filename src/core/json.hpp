#pragma once
// A small JSON reader for log lines: parses one object and flattens it into ("key.sub", "value") pairs.
// Logs are mostly flat objects with strings and numbers, so this is faster and simpler than a full DOM.

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace culprit::json {

using Fields = std::vector<std::pair<std::string, std::string>>;

/// Parses `text` as a JSON object. Nested objects become dotted keys ({"log":{"level":"INFO"}} ->
/// "log.level"); arrays are kept as their JSON text; strings are unescaped; numbers, true, false and
/// null keep their spelling. Returns nullopt when the text isn't a single JSON object.
std::optional<Fields> parse_object(std::string_view text);

}  // namespace culprit::json
