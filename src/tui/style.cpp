#include "tui/style.hpp"

namespace culprit {

std::string short_logger(std::string_view logger) {
    if (logger.size() <= 24) {
        // already short ("app.api", "root"), unless it's a long dotted Java name
        if (logger.find('.') == std::string_view::npos || logger.size() <= 14) return std::string(logger);
    }
    // the part after the last dot that isn't inside brackets: o.a.c.c.C.[.[.[/].[dispatcherServlet] -> dispatcherServlet
    std::string_view s = logger;
    if (!s.empty() && s.back() == ']') {
        size_t open = s.rfind('[');
        if (open != std::string_view::npos) return std::string(s.substr(open + 1, s.size() - open - 2));
    }
    size_t dot = s.rfind('.');
    return std::string(dot == std::string_view::npos ? s : s.substr(dot + 1));
}

std::string one_line(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        if (c == '\n' || c == '\r' || c == '\t') {
            if (!out.empty() && out.back() != ' ') out += ' ';
        } else {
            out += c;
        }
    }
    return out;
}

size_t line_count(std::string_view text) {
    if (text.empty()) return 0;
    size_t n = 1;
    for (char c : text) n += c == '\n';
    return n;
}

std::string thousands(size_t n) {
    std::string digits = std::to_string(n);
    std::string out;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i > 0 && (digits.size() - i) % 3 == 0) out += ',';
        out += digits[i];
    }
    return out;
}

}  // namespace culprit
