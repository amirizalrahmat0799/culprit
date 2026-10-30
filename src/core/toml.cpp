#include "core/toml.hpp"

#include <cstdint>
#include <cstdio>

namespace culprit::toml {
namespace {

class ParseError {
public:
    ParseError(int line, std::string message) : line(line), message(std::move(message)) {}
    int line;
    std::string message;
};

bool is_bare_key_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

void append_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

class Parser {
public:
    explicit Parser(std::string_view s) : s_(s) {}

    Document run() {
        Document doc;
        Table* current = &doc.root;
        while (true) {
            skip_blank_lines();
            if (eof()) break;
            if (peek() == '[') {
                const int at = line_;
                bool array = peek(1) == '[';
                pos_ += array ? 2 : 1;
                skip_spaces();
                std::string name = key();
                skip_spaces();
                if (!consume(']') || (array && !consume(']'))) fail(array ? "expected ]] after the table name" : "expected ] after the table name");
                end_of_line();
                if (array) {
                    auto& list = doc.arrays[name];
                    list.emplace_back();
                    list.back().line = at;
                    current = &list.back();
                } else {
                    if (doc.tables.count(name)) fail("[" + name + "] appears twice");
                    current = &doc.tables[name];
                    current->line = at;
                }
                continue;
            }
            const int at = line_;
            std::string k = key();
            skip_spaces();
            if (peek() == '.') fail("dotted keys aren't supported in rule files; write the key in full");
            if (!consume('=')) fail("expected = after \"" + k + "\"");
            skip_spaces();
            Value v = value();
            v.line = at;
            if (current->get(k)) fail("\"" + k + "\" is set twice");
            current->values.emplace_back(std::move(k), std::move(v));
            end_of_line();
        }
        return doc;
    }

private:
    bool eof() const { return pos_ >= s_.size(); }
    char peek(size_t ahead = 0) const { return pos_ + ahead < s_.size() ? s_[pos_ + ahead] : '\0'; }
    bool consume(char c) {
        if (peek() != c) return false;
        ++pos_;
        return true;
    }
    [[noreturn]] void fail(const std::string& message) const { throw ParseError(line_, message); }

    void newline() {
        ++pos_;
        ++line_;
    }
    void skip_spaces() {
        while (peek() == ' ' || peek() == '\t') ++pos_;
    }
    void skip_comment() {
        if (peek() == '#') {
            while (!eof() && peek() != '\n') ++pos_;
        }
    }
    // whitespace, comments and newlines, e.g. between array items
    void skip_all() {
        while (true) {
            skip_spaces();
            skip_comment();
            if (peek() == '\r' && peek(1) == '\n') ++pos_;
            if (peek() == '\n') newline();
            else break;
        }
    }
    void skip_blank_lines() { skip_all(); }
    void end_of_line() {
        skip_spaces();
        skip_comment();
        if (peek() == '\r') ++pos_;
        if (eof()) return;
        if (peek() != '\n') fail("unexpected text after the value");
        newline();
    }

    std::string key() {
        if (peek() == '"' || peek() == '\'') {
            Value v = string_value();
            return v.str;
        }
        std::string k;
        while (is_bare_key_char(peek())) k += s_[pos_++];
        if (k.empty()) fail("expected a key");
        return k;
    }

    Value value() {
        char c = peek();
        if (c == '"' || c == '\'') return string_value();
        if (c == '[') return array_value();
        if (c == '{') fail("inline tables aren't supported in rule files");
        if (s_.substr(pos_, 4) == "true") {
            pos_ += 4;
            Value v;
            v.type = Value::Type::Bool;
            v.boolean = true;
            return v;
        }
        if (s_.substr(pos_, 5) == "false") {
            pos_ += 5;
            Value v;
            v.type = Value::Type::Bool;
            return v;
        }
        if (c == '-' || c == '+' || (c >= '0' && c <= '9')) {
            Value v;
            v.type = Value::Type::Integer;
            bool negative = c == '-';
            if (c == '-' || c == '+') ++pos_;
            if (!(peek() >= '0' && peek() <= '9')) fail("expected a number");
            int64_t n = 0;
            while ((peek() >= '0' && peek() <= '9') || peek() == '_') {
                if (peek() != '_') {
                    if (n > (INT64_MAX - (peek() - '0')) / 10) fail("the number is too big");
                    n = n * 10 + (peek() - '0');
                }
                ++pos_;
            }
            if (peek() == '.' || peek() == 'e' || peek() == 'E' || peek() == '-' || peek() == ':') {
                fail("only whole numbers are supported in rule files");
            }
            v.integer = negative ? -n : n;
            return v;
        }
        fail("expected a value: a \"string\", a number, true/false or a [list]");
    }

    Value array_value() {
        Value v;
        v.type = Value::Type::Array;
        ++pos_;  // [
        while (true) {
            skip_all();
            if (consume(']')) break;
            if (eof()) fail("the list isn't closed with ]");
            const int at = line_;
            Value item = value();
            item.line = at;
            v.items.push_back(std::move(item));
            skip_all();
            if (consume(',')) continue;
            if (consume(']')) break;
            fail("expected , or ] in the list");
        }
        return v;
    }

    Value string_value() {
        Value v;
        const char quote = peek();
        const bool multi = peek(1) == quote && peek(2) == quote;
        pos_ += multi ? 3 : 1;
        if (multi) {
            // a newline right after the opening quotes isn't part of the string
            if (peek() == '\r' && peek(1) == '\n') ++pos_;
            if (peek() == '\n') newline();
        }
        while (true) {
            if (eof()) fail("the string isn't closed");
            char c = peek();
            if (c == quote) {
                if (!multi) {
                    ++pos_;
                    break;
                }
                if (peek(1) == quote && peek(2) == quote) {
                    pos_ += 3;
                    // up to two more quotes right before the closing ones belong to the string
                    for (int extra = 0; extra < 2 && peek() == quote; ++extra) {
                        v.str += quote;
                        ++pos_;
                    }
                    break;
                }
                v.str += c;
                ++pos_;
                continue;
            }
            if (c == '\n') {
                if (!multi) fail("the string isn't closed on this line (use \"\"\" for text over several lines)");
                v.str += '\n';
                newline();
                continue;
            }
            if (c == '\r' && peek(1) == '\n') {
                ++pos_;
                continue;
            }
            if (c == '\\' && quote == '"') {
                ++pos_;
                char e = peek();
                ++pos_;
                switch (e) {
                    case 'n': v.str += '\n'; break;
                    case 't': v.str += '\t'; break;
                    case 'r': v.str += '\r'; break;
                    case '"': v.str += '"'; break;
                    case '\\': v.str += '\\'; break;
                    case 'b': v.str += '\b'; break;
                    case 'f': v.str += '\f'; break;
                    case 'e': v.str += '\x1b'; break;
                    case 'u':
                    case 'U': {
                        int n = e == 'u' ? 4 : 8;
                        uint32_t cp = 0;
                        for (int i = 0; i < n; ++i) {
                            char h = peek();
                            ++pos_;
                            int d = h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : h >= 'A' && h <= 'F' ? h - 'A' + 10 : -1;
                            if (d < 0) fail("bad \\u escape");
                            cp = cp * 16 + static_cast<uint32_t>(d);
                        }
                        append_utf8(v.str, cp);
                        break;
                    }
                    case '\n':
                    case '\r':
                    case ' ':
                    case '\t':
                        if (!multi) fail("bad escape");
                        // line-ending backslash: skip the newline and the next line's indentation
                        --pos_;
                        while (peek() == ' ' || peek() == '\t' || peek() == '\r' || peek() == '\n') {
                            if (peek() == '\n') newline();
                            else ++pos_;
                        }
                        break;
                    default: {
                        std::string msg = "unknown escape \\";
                        msg += e;
                        msg += " (in regular expressions, write \\\\ or use 'single quotes')";
                        fail(msg);
                    }
                }
                continue;
            }
            v.str += c;
            ++pos_;
        }
        return v;
    }

    std::string_view s_;
    size_t pos_ = 0;
    int line_ = 1;
};

}  // namespace

std::optional<Document> parse(std::string_view text, Error& error) {
    try {
        return Parser(text).run();
    } catch (const ParseError& e) {
        error.line = e.line;
        error.message = e.message;
        return std::nullopt;
    }
}

}  // namespace culprit::toml
