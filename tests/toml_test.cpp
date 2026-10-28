#include "check.hpp"
#include "core/toml.hpp"

using namespace culprit;

TEST("toml: strings, numbers, booleans and lists") {
    toml::Error err;
    auto doc = toml::parse(R"t(
# a comment
[[rule]]
id = "a.b"            # trailing comment
basic = "tab\there \"quoted\" \u00e9"
literal = 'C:\path\(\d+)'
multi = """
first line \
continued
second"""
n = -42
yes = true
list = [
  "one",   # comments inside lists
  'two',
]
[[rule]]
id = "c"
)t", err);
    CHECK(doc.has_value());
    if (!doc) return;
    const auto& rules = doc->arrays.at("rule");
    CHECK_EQ(rules.size(), 2u);
    CHECK_EQ(rules[0].get("id")->str, std::string("a.b"));
    CHECK_EQ(rules[0].get("basic")->str, std::string("tab\there \"quoted\" \xc3\xa9"));
    CHECK_EQ(rules[0].get("literal")->str, std::string("C:\\path\\(\\d+)"));
    CHECK_EQ(rules[0].get("multi")->str, std::string("first line continued\nsecond"));
    CHECK_EQ(rules[0].get("n")->integer, -42);
    CHECK(rules[0].get("yes")->boolean);
    CHECK_EQ(rules[0].get("list")->items.size(), 2u);
    CHECK_EQ(rules[0].get("list")->items[1].str, std::string("two"));
    CHECK_EQ(rules[0].get("multi")->line, 7);
    CHECK_EQ(rules[1].line, 17);
}

TEST("toml: mistakes are reported with their line") {
    toml::Error err;
    CHECK(!toml::parse("[[rule]]\nid = \"x\"\nregex = \"\\d+\"\n", err));
    CHECK_EQ(err.line, 3);
    CHECK(err.message.find("escape") != std::string::npos);

    CHECK(!toml::parse("a = 1\na = 2\n", err));
    CHECK_EQ(err.line, 2);
    CHECK(!toml::parse("a = \"open\n", err));
    CHECK(!toml::parse("a = { b = 1 }\n", err));
    CHECK(!toml::parse("a = 1.5\n", err));
    CHECK(!toml::parse("a.b = 1\n", err));
    CHECK(!toml::parse("a = [1, 2\n", err));
    CHECK(!toml::parse("a = 1 b\n", err));
}
