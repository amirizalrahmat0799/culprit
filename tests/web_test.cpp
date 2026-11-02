#include <httplib.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include "check.hpp"
#include "core/rules.hpp"
#include "web/server.hpp"

namespace fs = std::filesystem;
using namespace culprit;

namespace {

fs::path write_log() {
    fs::path p = fs::temp_directory_path() / ("culprit-web-test-" + std::to_string(std::rand()) + ".log");
    std::ofstream(p, std::ios::binary)
        << "2026-10-28 09:41:00.000  INFO 1 --- [main] com.example.App : Starting\n"
           "2026-10-28 09:41:01.000 ERROR 1 --- [main] com.example.OrderService : Order 42 failed \x1b]0;pwned\x07 now\n"
           "java.lang.IllegalStateException: boom\n"
           "\tat com.example.OrderService.place(OrderService.java:87)\n"
           "\tat org.springframework.web.Servlet.service(Servlet.java:1)\n"
           "2026-10-28 09:41:02.000  WARN 1 --- [main] com.example.App : slow <script>alert(1)</script>\n";
    return p;
}

struct Running {
    fs::path log = write_log();
    web::Server server;
    httplib::Client client;
    httplib::Headers auth;

    static web::Options options(const fs::path& log) {
        web::Options o;
        o.inputs.push_back({log.string(), "", -1});
        o.follow = false;
        o.title = "test.log";
        o.version = "test";
        o.rules = RuleSet::builtin();
        o.no_ai = true;
        o.port = 0;
        o.project_start = fs::temp_directory_path().string();
        return o;
    }
    Running() : server(options(log)), client("127.0.0.1", start_port()) {
        auth = {{"X-Culprit-Token", server.token()}};
        // wait until the file has been read
        for (int i = 0; i < 200; ++i) {
            auto r = client.Get("/api/status", auth);
            if (r && r->body.find("\"following\":false") != std::string::npos) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    int start_port() {
        std::string error;
        CHECK(server.start(&error));
        return server.port();
    }
    ~Running() {
        server.stop();
        std::error_code ec;
        fs::remove(log, ec);
    }
};

bool has(const httplib::Result& r, std::string_view text) { return r && r->body.find(text) != std::string::npos; }

}  // namespace

TEST("web: hosts and tokens") {
    CHECK(web::host_allowed("127.0.0.1:4790", 4790));
    CHECK(web::host_allowed("localhost:4790", 4790));
    CHECK(web::host_allowed("[::1]:4790", 4790));
    CHECK(!web::host_allowed("127.0.0.1:4791", 4790));
    CHECK(!web::host_allowed("evil.example:4790", 4790));
    CHECK(!web::host_allowed("127.0.0.1", 4790));
    CHECK(!web::host_allowed("", 4790));
    const std::string t = web::make_token();
    CHECK_EQ(t.size(), 32u);
    CHECK(t != web::make_token());
    CHECK(web::token_matches(t, t));
    CHECK(!web::token_matches(t.substr(0, 31) + "x", t));
    CHECK(!web::token_matches("", ""));
    CHECK(!web::token_matches(t.substr(1), t));
}

TEST("web: nothing without the token or from another host") {
    Running w;
    auto page = w.client.Get("/");
    CHECK(page && page->status == 403);
    CHECK(!has(page, "<script"));
    page = w.client.Get("/?t=" + w.server.token());
    CHECK(page && page->status == 200);
    CHECK(has(page, "<title>culprit</title>"));
    CHECK(page->get_header_value("Content-Security-Policy").find("default-src 'none'") != std::string::npos);
    CHECK_EQ(page->get_header_value("Referrer-Policy"), std::string("no-referrer"));

    auto api = w.client.Get("/api/status");
    CHECK(api && api->status == 403);
    api = w.client.Get("/api/status", httplib::Headers{{"X-Culprit-Token", "0123456789abcdef0123456789abcdef"}});
    CHECK(api && api->status == 403);
    // a DNS-rebinding page reaches 127.0.0.1 under its own name
    api = w.client.Get("/api/status",
                       httplib::Headers{{"X-Culprit-Token", w.server.token()}, {"Host", "evil.example:" + std::to_string(w.server.port())}});
    CHECK(api && api->status == 403);

    // changing state needs the header (a cross-site form can only put the token in the URL) and our own origin
    auto post = w.client.Post("/api/investigate?t=" + w.server.token(), "{}", "application/json");
    CHECK(post && post->status == 403);
    httplib::Headers foreign = {{"X-Culprit-Token", w.server.token()}, {"Origin", "https://evil.example"}};
    post = w.client.Post("/api/investigate", foreign, "{}", "application/json");
    CHECK(post && post->status == 403);
}

TEST("web: status, matches, rows, entries and the report") {
    Running w;
    auto st = w.client.Get("/api/status", w.auth);
    CHECK(st && st->status == 200);
    CHECK(has(st, "\"total\":3"));
    CHECK(has(st, "\"following\":false"));
    CHECK(has(st, "\"error\":1"));
    CHECK(has(st, "\"available\":false"));  // --no-ai

    auto all = w.client.Get("/api/match?filter=&from=0", w.auth);
    CHECK(has(all, "\"all\":true") && has(all, "\"to\":3"));
    auto errors = w.client.Get("/api/match?filter=level%3E%3Derror&from=0", w.auth);
    CHECK(has(errors, "\"ids\":[1]"));
    auto later = w.client.Get("/api/match?filter=level%3E%3Dwarn&from=2", w.auth);
    CHECK(has(later, "\"ids\":[2]"));
    auto bad = w.client.Get("/api/match?filter=%2F(%2F", w.auth);
    CHECK(has(bad, "\"error\""));

    auto rows = w.client.Get("/api/rows?ids=1,2,99,x", w.auth);
    CHECK(rows && rows->status == 200);
    CHECK(has(rows, "\"i\":1") && has(rows, "\"i\":2") && !has(rows, "\"i\":99"));
    CHECK(has(rows, "\"k\":1"));
    // terminal codes are gone, and markup is just text inside a JSON string
    CHECK(!has(rows, "\x1b") && !has(rows, "pwned"));
    CHECK(has(rows, "<script>alert(1)</script>"));

    auto entry = w.client.Get("/api/entry?i=1", w.auth);
    CHECK(has(entry, "\"level\":\"ERROR\""));
    CHECK(has(entry, "OrderService.java"));
    CHECK(has(entry, "\"own\":true"));
    auto missing = w.client.Get("/api/entry?i=7", w.auth);
    CHECK(missing && missing->status == 404);

    auto report = w.client.Get("/api/report", w.auth);
    CHECK(has(report, "\"root_cause\":{"));
    CHECK(has(report, "IllegalStateException"));

    auto inv = w.client.Post("/api/investigate", w.auth, "{}", "application/json");
    CHECK(inv && (inv->status == 409 || inv->status == 501));  // AI is off in this run (or not built)
}

TEST("web: a long poll returns when something changes, or after it waits") {
    Running w;
    auto st = w.client.Get("/api/status", w.auth);
    CHECK(st && st->status == 200);
    // an old sequence number answers at once
    auto t0 = std::chrono::steady_clock::now();
    auto r = w.client.Get("/api/status?since=0", w.auth);
    CHECK(r && r->status == 200);
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2));
}
