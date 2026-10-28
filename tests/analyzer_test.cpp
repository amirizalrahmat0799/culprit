#include <fstream>

#include "check.hpp"
#include "core/analyzer.hpp"
#include "core/parser.hpp"

using namespace culprit;

namespace {
Report analyse_file(const std::string& name, std::vector<Entry>* keep = nullptr) {
    std::ifstream in(std::string(CULPRIT_TESTDATA) + "/" + name);
    Parser p;
    std::vector<Entry> entries;
    std::string line;
    while (std::getline(in, line)) p.feed(line, entries);
    p.flush(entries);
    Analyzer a(RuleSet::builtin());
    for (size_t i = 0; i < entries.size(); ++i) a.add(entries[i], static_cast<uint32_t>(i));
    if (keep) *keep = entries;
    return a.report();
}
const Problem* find(const Report& r, const std::string& id) {
    for (const auto& p : r.problems) {
        if (p.rule->id == id) return &p;
    }
    return nullptr;
}
}  // namespace

TEST("analyzer: numbers and ids don't split a group") {
    CHECK_EQ(normalize_message("Order 8812 not found"), std::string("Order # not found"));
    CHECK_EQ(normalize_message("Order 8812 not found"), normalize_message("Order 17 not found"));
    CHECK_EQ(normalize_message("user 3f2a9c1e-77b0-4c2e-9d7e-0b1d2c3e4f5a gone"), std::string("user # gone"));
    CHECK_EQ(normalize_message("pod api-7d9f8c6b5 crashed"), std::string("pod api-#d#f#c#b# crashed"));
    CHECK_EQ(normalize_message("cache  miss\tfor key"), std::string("cache miss for key"));
    CHECK_EQ(normalize_message("bad request"), std::string("bad request"));
}

TEST("analyzer: the root exception is the last Caused by, or the traceback's last line") {
    auto java = parse_stack("java.lang.IllegalStateException: Failed\n\tat a.B.c(B.java:1)\nCaused by: java.net.ConnectException: Connection refused\n\tat x.Y.z(Y.java:2)");
    CHECK_EQ(root_exception(java), std::string("java.net.ConnectException: Connection refused"));
    auto python = parse_stack("Traceback (most recent call last):\n  File \"/app/api.py\", line 12, in create\n    return int(x)\nValueError: invalid literal for int() with base 10: 'abc'");
    CHECK_EQ(root_exception(python), std::string("ValueError: invalid literal for int() with base 10: 'abc'"));
}

TEST("analyzer: a failed migration is the root cause of the 502s that follow") {
    std::vector<Entry> entries;
    Report r = analyse_file("incident-migration.log", &entries);
    CHECK(r.root.has_value());
    if (!r.root) return;
    CHECK(r.root->kind == RootCause::Kind::Problem);
    CHECK(r.root->confidence == RootCause::Confidence::High);
    const Problem& cause = r.problems[r.root->index];
    CHECK_EQ(cause.rule->id, std::string("flyway.migration-failed"));
    CHECK_EQ(cause.title(), std::string("Database migration V7__add_refunds.sql failed"));
    CHECK_EQ(cause.count, 2u);  // the context warning and "Application run failed"
    CHECK_EQ(cause.first.line, 11u);
    CHECK_EQ(cause.first.source, std::string("app-1"));
    // what followed: the container stopping, nginx failing to connect, 502s
    CHECK(r.root->followers >= 9);
    const Problem* refused = find(r, "nginx.upstream-refused");
    CHECK(refused && refused->count == 4);
    const Problem* bad_gateway = find(r, "http.bad-gateway");
    CHECK(bad_gateway && bad_gateway->count == 4 && bad_gateway->title() == "HTTP 502 responses");
    CHECK(find(r, "container.exited") != nullptr);
    // the database's own error came first, and nothing recognises it: it's a group, and the first error
    CHECK(r.first_error.has_value());
    if (r.first_error) CHECK_EQ(r.first_error->source, std::string("db-1"));
    CHECK(!r.groups.empty());
    // entries are marked for the viewer's problem: filter
    size_t marked = 0;
    for (const auto& e : entries) marked += e.rule != nullptr;
    CHECK_EQ(marked, 11u);
}

TEST("analyzer: a pool that ran out is found inside a stack trace") {
    Report r = analyse_file("spring-boot.log");
    const Problem* pool = find(r, "hikari.pool-exhausted");
    CHECK(pool != nullptr);
    if (pool) {
        CHECK_EQ(pool->title(), std::string("Connection pool HikariPool-1 ran out of connections"));
        CHECK(pool->explain().find("30005 ms") != std::string::npos);
    }
    CHECK(r.root.has_value());
}

TEST("analyzer: the same unrecognised error is grouped") {
    Analyzer a(RuleSet::builtin());
    Parser p;
    std::vector<Entry> es;
    for (const char* l : {"2026-10-28 10:00:00.000 ERROR 1 --- [main] c.e.Shop : Order 8812 could not be priced",
                          "2026-10-28 10:00:01.000 ERROR 1 --- [main] c.e.Shop : Order 9001 could not be priced",
                          "2026-10-28 10:00:02.000 ERROR 1 --- [main] c.e.Shop : Basket 12 is empty",
                          "2026-10-28 10:00:03.000  WARN 1 --- [main] c.e.Shop : Slow price lookup 2300 ms"}) {
        p.feed(l, es);
    }
    p.flush(es);
    for (size_t i = 0; i < es.size(); ++i) a.add(es[i], static_cast<uint32_t>(i));
    Report r = a.report();
    CHECK_EQ(r.groups.size(), 3u);
    CHECK_EQ(r.groups[0].count, 2u);
    CHECK_EQ(r.groups[0].title, std::string("Order 8812 could not be priced"));
    CHECK(r.groups.back().level == Level::Warn);
    CHECK(es[0].group == es[1].group && es[0].group != es[2].group);
    CHECK(r.root && r.root->kind == RootCause::Kind::Group && r.root->confidence == RootCause::Confidence::Medium);
    CHECK_EQ(a.problem_count(), 2u);
}
