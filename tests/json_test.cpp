#include "check.hpp"
#include "core/json.hpp"

using namespace culprit;

TEST("flat objects") {
    auto f = json::parse_object(R"({"level":"INFO","msg":"hi","n":42,"ok":true,"none":null})");
    CHECK(f.has_value());
    if (!f) return;
    CHECK_EQ(f->size(), 5u);
    CHECK_EQ((*f)[0].first, "level");
    CHECK_EQ((*f)[1].second, "hi");
    CHECK_EQ((*f)[2].second, "42");
    CHECK_EQ((*f)[3].second, "true");
    CHECK_EQ((*f)[4].second, "null");
}

TEST("nested objects become dotted keys, arrays stay as text") {
    auto f = json::parse_object(R"({"log":{"level":"ERROR","origin":{"file":"A.java"}},"tags":["a", "b"],"x":{}})");
    CHECK(f.has_value());
    if (!f) return;
    CHECK_EQ((*f)[0].first, "log.level");
    CHECK_EQ((*f)[1].first, "log.origin.file");
    CHECK_EQ((*f)[2].first, "tags");
    CHECK_EQ((*f)[2].second, R"(["a", "b"])");
}

TEST("escapes and unicode") {
    auto f = json::parse_object(R"({"m":"line1\nline2\t\"q\" é 😀 \/"})");
    CHECK(f.has_value());
    if (!f) return;
    CHECK_EQ((*f)[0].second, "line1\nline2\t\"q\" \xC3\xA9 \xF0\x9F\x98\x80 /");
}

TEST("not JSON") {
    CHECK(!json::parse_object("hello").has_value());
    CHECK(!json::parse_object(R"({"a":1)").has_value());
    CHECK(!json::parse_object(R"({"a":1} trailing)").has_value());
    CHECK(!json::parse_object(R"([1,2])").has_value());
    CHECK(!json::parse_object(R"({"a":"unterminated})").has_value());
    CHECK(json::parse_object(" { } ").has_value());
}
