#pragma once
// Formatting shared by the viewer and the plain output.

#include <string>
#include <string_view>

#include "core/entry.hpp"

namespace culprit {

/// "com.example.kedai.order.OrderService" -> "OrderService"; keeps short names as they are.
std::string short_logger(std::string_view logger);

/// The message on one line: newlines and tabs become spaces.
std::string one_line(std::string_view text);

/// Number of lines in a stack trace.
size_t line_count(std::string_view text);

/// "12403" -> "12,403"
std::string thousands(size_t n);

}  // namespace culprit
