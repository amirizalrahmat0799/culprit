#include "core/json.hpp"

#include <cstdint>

namespace culprit::json {
namespace {

class Reader {
public:
    explicit Reader(std::string_view text) : s_(text) {}

    bool object(const std::string& prefix, Fields& out) {
        skip_space();
        if (!eat('{')) return false;
        skip_space();
        if (eat('}')) return true;
        while (true) {
            skip_space();
            std::string key;
            if (!string(key)) return false;
            skip_space();
            if (!eat(':')) return false;
            skip_space();
            std::string full = prefix.empty() ? key : prefix + "." + key;
            if (peek() == '{') {
                if (!object(full, out)) return false;
            } else {
                std::string value;
                if (!value_text(value)) return false;
                out.emplace_back(std::move(full), std::move(value));
            }
            skip_space();
            if (eat(',')) continue;
            if (eat('}')) return true;
            return false;
        }
    }

    bool at_end() {
        skip_space();
        return pos_ == s_.size();
    }

private:
    char peek() const { return pos_ < s_.size() ? s_[pos_] : '\0'; }
    bool eat(char c) {
        if (peek() != c) return false;
        ++pos_;
        return true;
    }
    void skip_space() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) ++pos_;
    }

    static void append_utf8(std::string& out, uint32_t cp) {
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

    bool hex4(uint32_t& v) {
        if (pos_ + 4 > s_.size()) return false;
        v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = s_[pos_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else return false;
        }
        return true;
    }

    bool string(std::string& out) {
        if (!eat('"')) return false;
        while (pos_ < s_.size()) {
            char c = s_[pos_++];
            if (c == '"') return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= s_.size()) return false;
            char e = s_[pos_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    uint32_t cp;
                    if (!hex4(cp)) return false;
                    // a surrogate pair encodes characters outside the basic plane (emoji)
                    if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 6 <= s_.size() && s_[pos_] == '\\' && s_[pos_ + 1] == 'u') {
                        size_t save = pos_;
                        pos_ += 2;
                        uint32_t low;
                        if (hex4(low) && low >= 0xDC00 && low <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        } else {
                            pos_ = save;
                        }
                    }
                    append_utf8(out, cp);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }

    // Skips any JSON value, for arrays kept as raw text
    bool skip_value() {
        skip_space();
        char c = peek();
        if (c == '"') {
            std::string ignored;
            return string(ignored);
        }
        if (c == '{' || c == '[') {
            char close = c == '{' ? '}' : ']';
            ++pos_;
            skip_space();
            if (eat(close)) return true;
            while (true) {
                if (c == '{') {
                    skip_space();
                    std::string ignored;
                    if (!string(ignored)) return false;
                    skip_space();
                    if (!eat(':')) return false;
                }
                if (!skip_value()) return false;
                skip_space();
                if (eat(',')) continue;
                return eat(close);
            }
        }
        size_t start = pos_;
        while (pos_ < s_.size() && s_[pos_] != ',' && s_[pos_] != '}' && s_[pos_] != ']' && s_[pos_] != ' ' &&
               s_[pos_] != '\n' && s_[pos_] != '\r' && s_[pos_] != '\t') {
            ++pos_;
        }
        return pos_ > start;
    }

    bool value_text(std::string& out) {
        char c = peek();
        if (c == '"') return string(out);
        size_t start = pos_;
        if (!skip_value()) return false;
        out.assign(s_.substr(start, pos_ - start));
        return true;
    }

    std::string_view s_;
    size_t pos_ = 0;
};

}  // namespace

std::optional<Fields> parse_object(std::string_view text) {
    Reader r(text);
    Fields out;
    if (!r.object("", out)) return std::nullopt;
    if (!r.at_end()) return std::nullopt;
    return out;
}

}  // namespace culprit::json
