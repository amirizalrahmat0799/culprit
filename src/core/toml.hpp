#pragma once
// The part of TOML that rule files use: comments, `key = value`, `[table]`, `[[array of tables]]`,
// strings (basic "…", literal '…', and both multi-line forms), integers, booleans, and arrays of those.
// Anything else (dates, floats, inline tables, dotted keys) is reported as an error with its line number,
// so a rule file never half-loads.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace culprit::toml {

struct Value {
    enum class Type { String, Integer, Bool, Array };
    Type type = Type::String;
    std::string str;
    int64_t integer = 0;
    bool boolean = false;
    std::vector<Value> items;
    int line = 0;
};

struct Table {
    std::vector<std::pair<std::string, Value>> values;  // in file order
    int line = 0;

    const Value* get(std::string_view key) const {
        for (const auto& [k, v] : values) {
            if (k == key) return &v;
        }
        return nullptr;
    }
};

struct Document {
    Table root;
    std::map<std::string, Table> tables;               // [name]
    std::map<std::string, std::vector<Table>> arrays;  // [[name]]
};

struct Error {
    int line = 0;
    std::string message;
};

/// Parses `text`; on failure returns nullopt and fills `error`.
std::optional<Document> parse(std::string_view text, Error& error);

}  // namespace culprit::toml
