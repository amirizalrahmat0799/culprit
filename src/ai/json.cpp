#include "ai/json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace culprit::ai {
namespace {

const Json& null_value() {
    static const Json n;
    return n;
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

    bool run(Json& out, std::string& error) {
        if (!value(out, 0)) {
            error = error_.empty() ? "invalid JSON" : error_;
            error += " at offset " + std::to_string(pos_);
            return false;
        }
        space();
        if (pos_ != s_.size()) {
            error = "unexpected text after the JSON value at offset " + std::to_string(pos_);
            return false;
        }
        return true;
    }

private:
    void space() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) ++pos_;
    }
    bool fail(const char* msg) {
        error_ = msg;
        return false;
    }

    bool value(Json& out, int depth) {
        if (depth > 200) return fail("nested too deeply");
        space();
        if (pos_ >= s_.size()) return fail("unexpected end");
        char c = s_[pos_];
        if (c == '{') return object(out, depth);
        if (c == '[') return array(out, depth);
        if (c == '"') {
            std::string str;
            if (!string(str)) return false;
            out = Json(std::move(str));
            return true;
        }
        if (s_.substr(pos_, 4) == "true") return pos_ += 4, out = Json(true), true;
        if (s_.substr(pos_, 5) == "false") return pos_ += 5, out = Json(false), true;
        if (s_.substr(pos_, 4) == "null") return pos_ += 4, out = Json(), true;
        if (c == '-' || (c >= '0' && c <= '9')) {
            size_t start = pos_;
            if (s_[pos_] == '-') ++pos_;
            while (pos_ < s_.size() && ((s_[pos_] >= '0' && s_[pos_] <= '9') || s_[pos_] == '.' || s_[pos_] == 'e' ||
                                        s_[pos_] == 'E' || s_[pos_] == '+' || s_[pos_] == '-')) {
                ++pos_;
            }
            std::string num(s_.substr(start, pos_ - start));
            char* end = nullptr;
            double d = std::strtod(num.c_str(), &end);
            if (end != num.c_str() + num.size()) return fail("bad number");
            out = Json(d);
            return true;
        }
        return fail("unexpected character");
    }

    bool string(std::string& out) {
        ++pos_;  // "
        while (true) {
            if (pos_ >= s_.size()) return fail("unterminated string");
            char c = s_[pos_++];
            if (c == '"') return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= s_.size()) return fail("unterminated string");
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
                    uint32_t cp = 0;
                    if (!hex4(cp)) return fail("bad \\u escape");
                    if (cp >= 0xD800 && cp <= 0xDBFF && s_.substr(pos_, 2) == "\\u") {
                        pos_ += 2;
                        uint32_t lo = 0;
                        if (!hex4(lo)) return fail("bad \\u escape");
                        if (lo >= 0xDC00 && lo <= 0xDFFF) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        else {
                            append_utf8(out, 0xFFFD);
                            cp = lo;
                        }
                    } else if (cp >= 0xD800 && cp <= 0xDFFF) {
                        cp = 0xFFFD;
                    }
                    append_utf8(out, cp);
                    break;
                }
                default: return fail("bad escape");
            }
        }
    }

    bool hex4(uint32_t& cp) {
        if (pos_ + 4 > s_.size()) return false;
        for (int i = 0; i < 4; ++i) {
            char h = s_[pos_++];
            int d = h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : h >= 'A' && h <= 'F' ? h - 'A' + 10 : -1;
            if (d < 0) return false;
            cp = cp * 16 + static_cast<uint32_t>(d);
        }
        return true;
    }

    bool array(Json& out, int depth) {
        ++pos_;
        out = Json::array();
        space();
        if (pos_ < s_.size() && s_[pos_] == ']') return ++pos_, true;
        while (true) {
            Json item;
            if (!value(item, depth + 1)) return false;
            out.push(std::move(item));
            space();
            if (pos_ >= s_.size()) return fail("unterminated array");
            if (s_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (s_[pos_] == ']') return ++pos_, true;
            return fail("expected , or ]");
        }
    }

    bool object(Json& out, int depth) {
        ++pos_;
        out = Json::object();
        space();
        if (pos_ < s_.size() && s_[pos_] == '}') return ++pos_, true;
        while (true) {
            space();
            if (pos_ >= s_.size() || s_[pos_] != '"') return fail("expected a key");
            std::string key;
            if (!string(key)) return false;
            space();
            if (pos_ >= s_.size() || s_[pos_] != ':') return fail("expected :");
            ++pos_;
            Json v;
            if (!value(v, depth + 1)) return false;
            out.set(std::move(key), std::move(v));
            space();
            if (pos_ >= s_.size()) return fail("unterminated object");
            if (s_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (s_[pos_] == '}') return ++pos_, true;
            return fail("expected , or }");
        }
    }

    std::string_view s_;
    size_t pos_ = 0;
    std::string error_;
};

void dump_to(const Json& j, std::string& out) {
    switch (j.type()) {
        case Json::Type::Null: out += "null"; break;
        case Json::Type::Bool: out += j.boolean() ? "true" : "false"; break;
        case Json::Type::Number: {
            double d = j.number();
            if (!std::isfinite(d)) {
                out += "null";
            } else if (d == std::floor(d) && std::fabs(d) < 9e15) {
                out += std::to_string(static_cast<long long>(d));
            } else {
                char buf[32];
                std::snprintf(buf, sizeof buf, "%.17g", d);
                out += buf;
            }
            break;
        }
        case Json::Type::String: out += json_quote(j.str()); break;
        case Json::Type::Array: {
            out += '[';
            bool first = true;
            for (const auto& item : j.items()) {
                if (!first) out += ',';
                first = false;
                dump_to(item, out);
            }
            out += ']';
            break;
        }
        case Json::Type::Object: {
            out += '{';
            bool first = true;
            for (const auto& [k, v] : j.members()) {
                if (!first) out += ',';
                first = false;
                out += json_quote(k);
                out += ':';
                dump_to(v, out);
            }
            out += '}';
            break;
        }
    }
}

}  // namespace

std::string json_quote(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else if (c >= 0x80) {
                    // keep valid UTF-8; replace bytes that aren't (log files and source files can contain anything)
                    size_t len = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
                    bool ok = len > 0 && i + len <= s.size();
                    for (size_t k = 1; ok && k < len; ++k) ok = (static_cast<unsigned char>(s[i + k]) & 0xC0) == 0x80;
                    if (ok) {
                        out.append(s.substr(i, len));
                        i += len - 1;
                    } else {
                        out += "\\ufffd";
                    }
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    out += '"';
    return out;
}

std::optional<Json> Json::parse(std::string_view text, std::string* error) {
    Json out;
    std::string err;
    if (!Parser(text).run(out, err)) {
        if (error) *error = err;
        return std::nullopt;
    }
    return out;
}

std::string Json::dump() const {
    std::string out;
    dump_to(*this, out);
    return out;
}

const std::string& Json::str() const {
    static const std::string empty;
    return type_ == Type::String ? string_ : empty;
}

const Json::Array& Json::items() const {
    static const Array empty;
    return type_ == Type::Array ? array_ : empty;
}

const Json::Object& Json::members() const {
    static const Object empty;
    return type_ == Type::Object ? object_ : empty;
}

const Json& Json::operator[](std::string_view key) const {
    if (type_ != Type::Object) return null_value();
    for (const auto& [k, v] : object_) {
        if (k == key) return v;
    }
    return null_value();
}

const Json& Json::operator[](size_t index) const {
    if (type_ != Type::Array || index >= array_.size()) return null_value();
    return array_[index];
}

bool Json::has(std::string_view key) const {
    if (type_ != Type::Object) return false;
    for (const auto& [k, v] : object_) {
        if (k == key) return true;
    }
    return false;
}

Json& Json::set(std::string key, Json value) {
    if (type_ != Type::Object) {
        *this = object();
    }
    for (auto& [k, v] : object_) {
        if (k == key) {
            v = std::move(value);
            return *this;
        }
    }
    object_.emplace_back(std::move(key), std::move(value));
    return *this;
}

Json& Json::push(Json value) {
    if (type_ != Type::Array) *this = array();
    array_.push_back(std::move(value));
    return *this;
}

size_t Json::size() const {
    if (type_ == Type::Array) return array_.size();
    if (type_ == Type::Object) return object_.size();
    return 0;
}

}  // namespace culprit::ai
