#include "ai/json.hpp"
#include "check.hpp"

using culprit::ai::Json;

TEST("ai json: parse and write back") {
    auto j = Json::parse(R"({"a":1,"b":[true,false,null,"x\n\"y\"",2.5],"c":{"d":"\u00e9\ud83d\ude00"}})");
    CHECK(j.has_value());
    if (!j) return;
    CHECK_EQ((*j)["a"].integer(), 1);
    CHECK_EQ((*j)["b"].size(), 5u);
    CHECK((*j)["b"][0].boolean());
    CHECK((*j)["b"][2].is_null());
    CHECK_EQ((*j)["b"][3].str(), std::string("x\n\"y\""));
    CHECK_EQ((*j)["c"]["d"].str(), std::string("\xc3\xa9\xf0\x9f\x98\x80"));
    CHECK((*j)["missing"]["deeper"].is_null());
    CHECK_EQ(j->dump(), std::string(R"({"a":1,"b":[true,false,null,"x\n\"y\"",2.5],"c":{"d":"é😀"}})"));
}

TEST("ai json: building values keeps member order and replaces keys") {
    Json o = Json::object({{"model", "qwen3:8b"}, {"stream", false}});
    o.set("model", "other").set("n", 3);
    CHECK_EQ(o.dump(), std::string(R"({"model":"other","stream":false,"n":3})"));
    Json a;
    a.push(1).push("two");
    CHECK_EQ(a.dump(), std::string(R"([1,"two"])"));
}

TEST("ai json: bad input is rejected, bad UTF-8 is replaced on output") {
    std::string err;
    CHECK(!Json::parse("{\"a\":}", &err));
    CHECK(!err.empty());
    CHECK(!Json::parse("[1,2", &err));
    CHECK(!Json::parse("{} x", &err));
    CHECK(!Json::parse("\"\\q\"", &err));
    CHECK_EQ(Json(std::string("ok \xff\xfe end")).dump(), std::string("\"ok \\ufffd\\ufffd end\""));
    CHECK_EQ(Json(std::string("tab\tnul\x01")).dump(), std::string("\"tab\\tnul\\u0001\""));
}

TEST("ai json: huge and odd numbers become clamped integers") {
    CHECK_EQ(Json::parse("1e30")->integer(), INT64_MAX);
    CHECK_EQ(Json::parse("-1e30")->integer(), INT64_MIN);
    CHECK_EQ(Json::parse("42.9")->integer(), 42);
}
