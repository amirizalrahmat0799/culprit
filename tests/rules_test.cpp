#include "check.hpp"
#include "core/parser.hpp"
#include "core/rules.hpp"

using namespace culprit;

namespace {
Entry entry(const std::string& text) {
    Parser p;
    std::vector<Entry> out;
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find('\n', start);
        p.feed(std::string_view(text).substr(start, nl == std::string::npos ? std::string::npos : nl - start), out);
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    p.flush(out);
    return out.empty() ? Entry{} : out.front();
}
}  // namespace

TEST("rules: every built-in rule loads, has an example, and recognises it") {
    auto set = RuleSet::builtin();
    CHECK(set->rules().size() > 100);
    std::vector<std::string> errors;
    for (const auto& r : set->rules()) {
        if (r->examples.empty()) errors.push_back(r->id + " has no examples");
        if (r->explain.empty()) errors.push_back(r->id + " has no explanation");
        if (r->fix.empty()) errors.push_back(r->id + " has no fix");
    }
    size_t checked = set->check_examples(errors);
    CHECK(checked >= set->rules().size());
    for (const auto& e : errors) std::cerr << "  " << e << "\n";
    CHECK(errors.empty());
}

TEST("rules: captures fill the title, explanation and fixes") {
    auto set = RuleSet::builtin();
    auto m = set->match(entry("2026-10-28 09:41:05.233 ERROR 1 --- [main] o.s.b.d.LoggingFailureAnalysisReporter : Web server failed to start. Port 9090 was already in use."));
    CHECK(m.has_value());
    if (!m) return;
    CHECK_EQ(m->rule->id, std::string("spring.port-in-use"));
    CHECK_EQ(render(m->rule->title, m->captures), std::string("Port 9090 is already in use"));
    CHECK(render(m->rule->fix[0], m->captures).find("lsof -i :9090") != std::string::npos);
}

TEST("rules: info lines don't match rules that expect a problem") {
    auto set = RuleSet::builtin();
    CHECK(!set->match(entry("2026-10-28 09:41:05.233  INFO 1 --- [main] c.e.Demo : retrying after connection refused")));
    CHECK(set->match(entry("2026-10-28 09:41:05.233 ERROR 1 --- [main] c.e.Demo : giving up after connection refused")));
}

TEST("rules: the most specific rule wins") {
    auto set = RuleSet::builtin();
    auto m = set->match(entry("Error: listen EADDRINUSE: address already in use :::3000"));
    CHECK(m && m->rule->id == "node.address-in-use");
    m = set->match(entry("org.postgresql.util.PSQLException: Connection to db:5432 refused. Check that the hostname and port are correct and that the postmaster is accepting TCP/IP connections."));
    CHECK(m && m->rule->id == "postgres.connection-refused");
}

TEST("rules: your own rules add to, replace and turn off built-in ones") {
    auto set = RuleSet::builtin();
    std::vector<std::string> errors;
    set->add(R"t(
[[rule]]
id = "kedai.payment-declined"
title = "Payment declined: {reason}"
severity = "warning"
contains = ["payment declined"]
regex = 'payment declined: (\w+)'
captures = ["reason"]
explain = "The bank said no ({reason})."
fix = ["Nothing to fix in code."]
examples = ["2026-10-28 10:00:00.000  WARN 1 --- [main] c.e.Pay : payment declined: insufficient_funds"]

[[rule]]
id = "java.npe"
title = "Null again"
contains = ["nullpointerexception"]

[[rule]]
id = "net.timeout"
disabled = true
)t", "team.toml", errors);
    CHECK(errors.empty());
    for (const auto& e : errors) std::cerr << "  " << e << "\n";
    auto m = set->match(entry("2026-10-28 10:00:00.000  WARN 1 --- [main] c.e.Pay : payment declined: insufficient_funds"));
    CHECK(m && m->rule->id == "kedai.payment-declined" && render(m->rule->title, m->captures) == "Payment declined: insufficient_funds");
    CHECK_EQ(set->find("java.npe")->title, std::string("Null again"));
    CHECK_EQ(set->find("java.npe")->origin, std::string("team.toml"));
    CHECK(set->find("net.timeout") == nullptr);
    CHECK(!set->match(entry("java.net.SocketTimeoutException: Read timed out")));
    set->check_examples(errors);
    CHECK(errors.empty());
}

TEST("rules: mistakes in rule files are explained") {
    RuleSet set;
    std::vector<std::string> errors;
    set.add(R"t(
[[rule]]
id = "Bad Id"
title = "x"
contains = ["x"]

[[rule]]
id = "no-contains"
title = "x"

[[rule]]
id = "bad-regex"
title = "x"
contains = ["x"]
regex = '(unclosed'

[[rule]]
id = "unknown-placeholder"
title = "Port {port}"
contains = ["x"]

[[rule]]
id = "typo"
title = "x"
contians = ["x"]

[[rule]]
id = "good"
title = "fine"
contains = ["fine"]
examples = ["nothing to see here"]
)t", "mine.toml", errors);
    CHECK_EQ(errors.size(), 5u);
    for (const auto& e : errors) CHECK(e.rfind("mine.toml:", 0) == 0);
    CHECK_EQ(set.rules().size(), 1u);
    CHECK(errors.size() > 4 && errors[4].find("contians") != std::string::npos);
    set.check_examples(errors);
    CHECK_EQ(errors.size(), 6u);
    CHECK(errors.back().find("isn't recognised") != std::string::npos);
}

TEST("rules: field rules compare a field and expose its value") {
    auto set = RuleSet::builtin();
    auto m = set->match(entry("10.0.0.1 - - [28/Oct/2026:09:41:12 +0800] \"GET / HTTP/1.1\" 504 0 \"-\" \"curl/8.5.0\""));
    CHECK(m && m->rule->id == "http.bad-gateway");
    if (m) CHECK_EQ(render(m->rule->title, m->captures), std::string("HTTP 504 responses"));
    CHECK(!set->match(entry("10.0.0.1 - - [28/Oct/2026:09:41:12 +0800] \"GET / HTTP/1.1\" 200 12 \"-\" \"curl/8.5.0\"")));
}
