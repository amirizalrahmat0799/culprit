#include <fstream>
#include <sstream>

#include "check.hpp"
#include "core/parser.hpp"

using namespace culprit;

namespace {
std::vector<Entry> parse_all(const std::string& text) {
    Parser p;
    std::vector<Entry> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) p.feed(line, out);
    p.flush(out);
    return out;
}
std::vector<Entry> parse_file(const std::string& name) {
    std::ifstream f(std::string(CULPRIT_TESTDATA) + "/" + name);
    std::stringstream ss;
    ss << f.rdbuf();
    return parse_all(ss.str());
}
}  // namespace

TEST("Spring Boot default pattern") {
    Entry e = parse_line(
        "2026-10-28T09:41:05.019+08:00  INFO 48211 --- [kedai] [           main] com.example.kedai.KedaiApplication       : Started KedaiApplication in 3.214 seconds");
    CHECK(e.level == Level::Info);
    CHECK_EQ(e.time_text, "01:41:05.019");  // +08:00 shown in UTC, the tests' local time
    CHECK_EQ(e.thread, "main");
    CHECK_EQ(e.logger, "com.example.kedai.KedaiApplication");
    CHECK_EQ(e.message, "Started KedaiApplication in 3.214 seconds");

    Entry tomcat = parse_line(
        "2026-10-28T09:46:51.377+08:00 ERROR 48211 --- [kedai] [nio-8080-exec-7] o.a.c.c.C.[.[.[/].[dispatcherServlet]    : Servlet.service() threw exception");
    CHECK(tomcat.level == Level::Error);
    CHECK_EQ(tomcat.logger, "o.a.c.c.C.[.[.[/].[dispatcherServlet]");
    CHECK_EQ(tomcat.message, "Servlet.service() threw exception");
}

TEST("logback, Python and others") {
    Entry lb = parse_line("12:00:01.234 [main] WARN  c.e.Demo - Cache is cold");
    CHECK(lb.level == Level::Warn);
    CHECK_EQ(lb.thread, "main");
    CHECK_EQ(lb.logger, "c.e.Demo");
    CHECK_EQ(lb.message, "Cache is cold");

    Entry py = parse_line("2026-10-28 09:46:51,377 - app.api - ERROR - Creating todo failed");
    CHECK(py.level == Level::Error);
    CHECK_EQ(py.logger, "app.api");
    CHECK_EQ(py.message, "Creating todo failed");

    Entry plain_py = parse_line("WARNING:root:retrying in 2 seconds");
    CHECK(plain_py.level == Level::Warn);
    CHECK_EQ(plain_py.logger, "root");
    CHECK_EQ(plain_py.message, "retrying in 2 seconds");

    Entry uv = parse_line("INFO:     127.0.0.1:52144 - \"GET /api/todos HTTP/1.1\" 200 OK");
    CHECK(uv.level == Level::Info);
    CHECK_EQ(uv.message, "127.0.0.1:52144 - \"GET /api/todos HTTP/1.1\" 200 OK");

    Entry nginx = parse_line("2026/10/28 09:46:51 [error] 29#29: *1 connect() failed (111: Connection refused)");
    CHECK(nginx.level == Level::Error);
    CHECK_EQ(nginx.time_text, "09:46:51.000");
    CHECK_EQ(nginx.message, "*1 connect() failed (111: Connection refused)");

    Entry pg = parse_line("2026-10-28 01:41:00.912 UTC [1] LOG:  database system is ready to accept connections");
    CHECK(pg.level == Level::Info);
    CHECK_EQ(pg.message, "database system is ready to accept connections");

    Entry prose = parse_line("Error budgets are fine");  // a capitalised word without a colon isn't a level
    CHECK(prose.level == Level::Unknown);
    CHECK_EQ(prose.message, "Error budgets are fine");
}

TEST("JSON formats") {
    Entry ecs = parse_line(
        R"j({"@timestamp":"2026-10-28T01:46:51.377Z","log":{"level":"ERROR","logger":"c.e.OrderService"},"process":{"thread":{"name":"exec-7"}},"service":{"name":"kedai"},"message":"Order failed","error":{"stack_trace":"java.lang.NullPointerException: x\n\tat c.e.OrderService.place(OrderService.java:52)\n"},"orderId":"8812"})j");
    CHECK_EQ(ecs.format, "json");
    CHECK(ecs.level == Level::Error);
    CHECK_EQ(ecs.logger, "c.e.OrderService");
    CHECK_EQ(ecs.thread, "exec-7");
    CHECK_EQ(ecs.source, "kedai");
    CHECK_EQ(ecs.message, "Order failed");
    CHECK_EQ(ecs.stack, "java.lang.NullPointerException: x\n\tat c.e.OrderService.place(OrderService.java:52)");
    CHECK(ecs.field("orderId") && *ecs.field("orderId") == "8812");
    CHECK(ecs.field("log.level") == nullptr);  // used, so not repeated as a field

    Entry pino = parse_line(R"j({"level":50,"time":1761615811377,"err":{"type":"TypeError","message":"boom","stack":"TypeError: boom\n    at f (/app/a.js:1:1)"},"msg":"request failed"})j");
    CHECK(pino.level == Level::Error);
    CHECK_EQ(pino.message, "request failed");
    CHECK(pino.stack.rfind("TypeError: boom", 0) == 0);
    CHECK(!pino.time_text.empty());

    Entry gelf = parse_line(R"j({"version":"1.1","short_message":"hi","timestamp":1761615662.118,"level":4})j");
    CHECK(gelf.level == Level::Warn);
    CHECK_EQ(gelf.message, "hi");

    Entry docker = parse_line(R"j({"log":"2026-10-28T09:41:05.019+08:00  INFO 1 --- [main] c.e.App : Started\n","stream":"stdout","time":"2026-10-28T01:41:05.02Z"})j");
    CHECK(docker.level == Level::Info);
    CHECK_EQ(docker.message, "Started");
}

TEST("logfmt and access logs") {
    Entry slog = parse_line(R"j(time=2026-10-28T09:45:30.004+08:00 level=WARN msg="slow query" query="SELECT 1" duration=2.8s)j");
    CHECK_EQ(slog.format, "logfmt");
    CHECK(slog.level == Level::Warn);
    CHECK_EQ(slog.message, "slow query");
    CHECK(slog.field("query") && *slog.field("query") == "SELECT 1");
    CHECK(slog.field("duration") && *slog.field("duration") == "2.8s");

    Entry access = parse_line(R"j(172.18.0.1 - - [28/Oct/2026:01:46:51 +0000] "POST /api/orders HTTP/1.1" 500 49 "-" "Mozilla/5.0")j");
    CHECK_EQ(access.format, "access");
    CHECK(access.level == Level::Error);
    CHECK_EQ(access.message, "POST /api/orders 500");
    CHECK(access.field("user_agent") && *access.field("user_agent") == "Mozilla/5.0");
    CHECK(access.epoch_ms.has_value());
}

TEST("Compose and kubectl prefixes") {
    Entry c = parse_line("backend-1   | 2026-10-28T09:46:51.377+08:00 ERROR 1 --- [kedai] [exec-7] c.e.OrderService : Order failed");
    CHECK_EQ(c.source, "backend-1");
    CHECK(c.level == Level::Error);
    CHECK_EQ(c.message, "Order failed");
    Entry k = parse_line("[pod/api-7d9/api] level=error msg=boom");
    CHECK_EQ(k.source, "pod/api-7d9/api");
    CHECK(k.level == Level::Error);
}

TEST("Java stack traces join the entry before them") {
    auto entries = parse_file("spring-boot.log");
    CHECK_EQ(entries.size(), 18u);
    int errors = 0;
    for (const auto& e : entries) {
        if (e.level == Level::Error) {
            ++errors;
            CHECK(e.has_stack());
        }
        CHECK(!e.message.empty());
    }
    CHECK_EQ(errors, 2);
    const Entry& npe = entries[13];
    CHECK(npe.stack.rfind("java.lang.NullPointerException", 0) == 0);
    CHECK(npe.stack.find("Thread.run") != std::string::npos);
    CHECK(npe.stack.back() == ')');  // trailing blank line dropped
    const Entry& hikari = entries[16];
    CHECK(hikari.stack.find("Caused by: java.sql.SQLTransientConnectionException") != std::string::npos);
    CHECK(hikari.stack.find("... 14 common frames omitted") != std::string::npos);
}

TEST("Python tracebacks and Go panics") {
    auto py = parse_file("python.log");
    CHECK_EQ(py.size(), 4u);
    if (py.size() == 4) {
        CHECK(py[2].level == Level::Error);
        CHECK(py[2].stack.rfind("Traceback", 0) == 0);
        CHECK(py[2].stack.find("sqlalchemy.exc.OperationalError") != std::string::npos);
        CHECK(py[3].level == Level::Warn);
    }
    auto go = parse_file("go.log");
    CHECK_EQ(go.size(), 4u);
    if (go.size() == 4) {
        CHECK(go[3].level == Level::Fatal);
        CHECK(go[3].stack.find("/app/handler.go:77") != std::string::npos);
    }
}

TEST("Compose logs keep stack traces per service") {
    auto entries = parse_file("compose.log");
    CHECK_EQ(entries.size(), 5u);
    if (entries.size() == 5) {
        CHECK_EQ(entries[3].source, "backend-1");
        CHECK(entries[3].stack.find("IllegalStateException") != std::string::npos);
        CHECK_EQ(entries[4].source, "frontend-1");
        CHECK(entries[4].level == Level::Error);  // HTTP 500
    }
}

TEST("every sample parses") {
    for (const char* name : {"spring-boot-ecs.json", "express-pino.json"}) {
        auto entries = parse_file(name);
        CHECK(!entries.empty());
        for (const auto& e : entries) CHECK_EQ(e.format, "json");
    }
}
