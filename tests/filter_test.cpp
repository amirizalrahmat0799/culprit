#include "check.hpp"
#include "core/filter.hpp"
#include "core/parser.hpp"

using namespace culprit;

namespace {
Entry make(Level level, std::string msg, std::string logger = "c.e.OrderService") {
    Entry e;
    e.level = level;
    e.message = std::move(msg);
    e.logger = std::move(logger);
    e.fields.emplace_back("orderId", "8812");
    return e;
}
bool m(std::string_view q, const Entry& e) { return Filter::parse(q).matches(e); }
}  // namespace

TEST("words, phrases and negation") {
    Entry e = make(Level::Error, "Payment failed for order 8812");
    CHECK(m("", e));
    CHECK(m("payment", e));
    CHECK(m("PAYMENT FAILED", e));
    CHECK(m("\"failed for\"", e));
    CHECK(!m("\"failed order\"", e));
    CHECK(!m("-payment", e));
    CHECK(m("-health", e));
    CHECK(m("orderservice", e));   // logger
    CHECK(m("8812", e));           // field value
}

TEST("levels") {
    Entry warn = make(Level::Warn, "slow");
    Entry unknown = make(Level::Unknown, "plain");
    CHECK(m("level>=warn", warn));
    CHECK(m("level=warn", warn));
    CHECK(m("level:warning", warn));
    CHECK(!m("level>=error", warn));
    CHECK(m("level<=info", make(Level::Debug, "x")));
    CHECK(!m("level>=warn", unknown));
    auto bad = Filter::parse("level>=loud");
    CHECK(!bad.error().empty());
    CHECK(bad.matches(warn));  // a mistake doesn't hide everything
}

TEST("fields, regex and stack") {
    Entry e = make(Level::Error, "timed out after 30005ms");
    CHECK(m("orderId=8812", e));
    CHECK(m("orderid=881", e));
    CHECK(!m("orderId!=8812", e));
    CHECK(m("logger=OrderService", e));
    CHECK(!m("logger=Payment", e));
    CHECK(m("/time(d)? out/", e));
    CHECK(m("/TIMED\\s+OUT/", e));
    CHECK(!m("/^out/", e));
    CHECK(!Filter::parse("/(unclosed/").error().empty());
    CHECK(!m("has:stack", e));
    e.stack = "java.lang.X";
    CHECK(m("has:stack", e));
    CHECK(m("payment -health level>=error", make(Level::Error, "payment ok")));
}

TEST("highlights") {
    auto f = Filter::parse("Payment -noise \"order failed\" orderId=88");
    CHECK_EQ(f.highlights().size(), 3u);
}

TEST("a regex on a very long line doesn't overflow the stack") {
    // std::regex recurses per character; one 200 KB line used to crash culprit
    Entry e = make(Level::Error, std::string(200000, 'x') + " read timeout");
    CHECK(m("/.*timeout/", e));
    CHECK(!m("/.*nothing/", e));
    e.stack = std::string(5000, 'y') + "\n  at Foo.bar(Foo.java:1)";
    CHECK(m("/foo\\.bar/", e));
}

TEST("a regex that throws on one window is tried again on smaller pieces") {
    // MSVC throws regex_error past its recursion limit; simulate it for windows longer than 100 characters
    std::string line = std::string(3000, 'x') + " read timeout " + std::string(3000, 'y');
    int calls = 0;
    bool hit = regex_windows(line, [&](std::string_view w) {
        ++calls;
        if (w.size() > 100) throw std::regex_error(std::regex_constants::error_stack);
        return w.find("timeout") != std::string_view::npos;
    });
    CHECK(hit);
    CHECK(calls > 1);
    CHECK(!regex_windows(std::string(2000, 'z'), [](std::string_view) -> bool {
        throw std::regex_error(std::regex_constants::error_complexity);
    }));
}
