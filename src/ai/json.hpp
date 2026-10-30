#pragma once
// A JSON value for talking to AI APIs: requests are built as values and written out, responses are
// parsed into values. (Log lines use core/json, which flattens objects for speed; this one keeps them.)

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace culprit::ai {

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };
    using Array = std::vector<Json>;
    using Object = std::vector<std::pair<std::string, Json>>;  // keeps key order, as APIs show it

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool b) : type_(Type::Bool), bool_(b) {}
    Json(int n) : type_(Type::Number), number_(n) {}
    Json(int64_t n) : type_(Type::Number), number_(static_cast<double>(n)) {}
    Json(size_t n) : type_(Type::Number), number_(static_cast<double>(n)) {}
    Json(double n) : type_(Type::Number), number_(n) {}
    Json(const char* s) : type_(Type::String), string_(s) {}
    Json(std::string s) : type_(Type::String), string_(std::move(s)) {}
    Json(std::string_view s) : type_(Type::String), string_(s) {}

    static Json array(Array items = {}) {
        Json j;
        j.type_ = Type::Array;
        j.array_ = std::move(items);
        return j;
    }
    static Json object(Object members = {}) {
        Json j;
        j.type_ = Type::Object;
        j.object_ = std::move(members);
        return j;
    }

    /// Parses JSON text; nullopt (and `error`) when it isn't valid.
    static std::optional<Json> parse(std::string_view text, std::string* error = nullptr);
    /// Compact JSON text.
    std::string dump() const;

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_string() const { return type_ == Type::String; }
    bool is_object() const { return type_ == Type::Object; }
    bool is_array() const { return type_ == Type::Array; }
    bool is_number() const { return type_ == Type::Number; }
    bool is_bool() const { return type_ == Type::Bool; }

    // Lenient accessors: the wrong type gives an empty or zero value, so reading an API response
    // doesn't need a check at every step.
    const std::string& str() const;
    double number() const { return type_ == Type::Number ? number_ : 0; }
    /// The number as an integer, rounded toward zero and clamped to int64 (0 for NaN and non-numbers).
    int64_t integer() const {
        if (type_ != Type::Number || number_ != number_) return 0;
        if (number_ >= 9.2e18) return INT64_MAX;
        if (number_ <= -9.2e18) return INT64_MIN;
        return static_cast<int64_t>(number_);
    }
    bool boolean() const { return type_ == Type::Bool && bool_; }
    const Array& items() const;
    const Object& members() const;

    /// The member called `key`, or a null value.
    const Json& operator[](std::string_view key) const;
    /// The item at `index`, or a null value.
    const Json& operator[](size_t index) const;
    bool has(std::string_view key) const;

    /// Adds or replaces a member (turns a null into an object).
    Json& set(std::string key, Json value);
    /// Appends an item (turns a null into an array).
    Json& push(Json value);
    size_t size() const;

private:
    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0;
    std::string string_;
    Array array_;
    Object object_;
};

/// `s` as a JSON string literal, with quotes.
std::string json_quote(std::string_view s);

}  // namespace culprit::ai
