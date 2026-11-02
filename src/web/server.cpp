#include "web/server.hpp"

#include <httplib.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <random>
#include <thread>

#include "core/analyzer.hpp"
#include "core/filter.hpp"
#include "core/stack.hpp"
#include "tui/report.hpp"
#ifdef CULPRIT_WITH_AI
#include "ai/config.hpp"
#include "ai/http.hpp"
#include "ai/investigator.hpp"
#include "ai/json.hpp"
#include "ai/subject.hpp"
#include "ai/workspace.hpp"
#include "tui/finding.hpp"
#endif

namespace culprit {
extern const unsigned char kWebPage[];
extern const size_t kWebPage_size;
}  // namespace culprit

namespace culprit::web {

bool host_allowed(std::string_view host, int port) {
    const std::string p = ":" + std::to_string(port);
    return host == "127.0.0.1" + p || host == "localhost" + p || host == "[::1]" + p;
}

bool token_matches(std::string_view given, std::string_view expected) {
    if (given.size() != expected.size() || expected.empty()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < expected.size(); ++i) diff |= static_cast<unsigned char>(given[i] ^ expected[i]);
    return diff == 0;
}

std::string make_token() {
    std::random_device rd;  // the OS's random source (getrandom, /dev/urandom, BCryptGenRandom)
    static const char* hex = "0123456789abcdef";
    std::string t;
    for (int i = 0; i < 4; ++i) {
        uint32_t v = rd();
        for (int j = 0; j < 8; ++j) {
            t += hex[v & 0xf];
            v >>= 4;
        }
    }
    return t;
}

namespace {

using httplib::Request;
using httplib::Response;

std::string q(std::string_view s) { return json_string(s); }

std::string first_line(std::string_view s, size_t max) {
    size_t nl = s.find('\n');
    std::string_view line = s.substr(0, nl);
    if (line.size() > max) {
        // cut at a UTF-8 character boundary
        size_t cut = max;
        while (cut > 0 && (static_cast<unsigned char>(line[cut]) & 0xC0) == 0x80) --cut;
        return std::string(line.substr(0, cut)) + "…";
    }
    return std::string(line);
}

const char* frame_kind(Frame::Kind k) {
    switch (k) {
        case Frame::Kind::Header: return "header";
        case Frame::Kind::Frame: return "frame";
        case Frame::Kind::CausedBy: return "caused";
        case Frame::Kind::Other: return "other";
    }
    return "other";
}

void json_reply(Response& res, std::string body, int status = 200) {
    res.status = status;
    res.set_content(std::move(body), "application/json; charset=utf-8");
}

void error_reply(Response& res, int status, const std::string& message) {
    json_reply(res, "{\"error\":" + q(message) + "}", status);
}

std::optional<long long> number_param(const Request& req, const char* name) {
    if (!req.has_param(name)) return std::nullopt;
    const std::string v = req.get_param_value(name);
    if (v.empty() || v.size() > 18 || !std::all_of(v.begin(), v.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        return std::nullopt;
    }
    return std::stoll(v);
}

}  // namespace

struct Server::Impl {
    Options opt;
    std::string token = make_token();
    int port = 0;
    httplib::Server http;
    std::thread http_thread;
    std::atomic<bool> http_failed{false};

    std::unique_ptr<Reader> reader;
    std::thread pump;
    std::mutex wake_mutex;
    std::condition_variable wake;
    bool woken = false;

    // everything below is guarded by `mutex`
    std::mutex mutex;
    std::condition_variable changed;
    bool stopping = false;
    uint64_t seq = 1;
    std::vector<Entry> entries;
    Analyzer analyzer;
    std::array<size_t, 7> counts{};
    std::string first_source;
    bool multiple_sources = false;
    std::vector<std::string> read_errors;
    bool reading_done = false;
    uint64_t report_seq = 0;
    std::string report_cache;

#ifdef CULPRIT_WITH_AI
    struct Investigation {
        std::string state = "running";  // running, done, failed, cancelled
        uint32_t index = 0;
        std::string title, where, provider, model;
        bool local = true;
        std::vector<ai::Step> steps;
        std::string finding;  // JSON, when finished
        std::atomic<bool> cancel{false};
        std::thread worker;
        std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point ended;
    };
    std::shared_ptr<Investigation> inv;
#endif

    explicit Impl(Options o) : opt(std::move(o)), analyzer(opt.rules) {}

    ~Impl() { shutdown(); }

    void bump() {
        ++seq;
        changed.notify_all();
    }

    void shutdown() {
        {
            std::lock_guard lock(mutex);
            if (stopping) return;
            stopping = true;
        }
        changed.notify_all();
        {
            std::lock_guard lock(wake_mutex);
            woken = true;
        }
        wake.notify_all();
#ifdef CULPRIT_WITH_AI
        std::shared_ptr<Investigation> running;
        {
            std::lock_guard lock(mutex);
            running = inv;
        }
        if (running) {
            running->cancel = true;
            if (running->worker.joinable()) running->worker.join();
        }
#endif
        http.stop();
        if (http_thread.joinable()) http_thread.join();
        if (pump.joinable()) pump.join();
        if (reader) {
            // the reading threads check for stop between reads; let files finish (stdin may block, and is left)
            reader->stop();
            for (int i = 0; i < 40 && !reader->finished(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
    }

    // ---- reading ----------------------------------------------------------------------------------------

    void append(std::vector<Entry>& batch) {
        for (auto& e : batch) {
            ++counts[static_cast<size_t>(e.level)];
            if (!e.source.empty()) {
                if (first_source.empty()) first_source = e.source;
                else if (e.source != first_source) multiple_sources = true;
            }
            analyzer.add(e, static_cast<uint32_t>(entries.size()));
            entries.push_back(std::move(e));
        }
    }

    void run_pump() {
        reader->start([this] {
            {
                std::lock_guard lock(wake_mutex);
                woken = true;
            }
            wake.notify_one();
        });
        while (true) {
            {
                std::unique_lock lock(wake_mutex);
                wake.wait_for(lock, std::chrono::milliseconds(250), [this] { return woken; });
                woken = false;
            }
            const bool finished = reader->finished();  // before take(): everything read by then is in it
            std::vector<Entry> batch = reader->take();
            std::vector<std::string> errors = reader->take_errors();
            std::lock_guard lock(mutex);
            if (stopping) return;
            bool any = !batch.empty() || !errors.empty();
            append(batch);
            for (auto& e : errors) read_errors.push_back(std::move(e));
            if (finished && !reading_done) {
                reading_done = true;
                any = true;
            }
            if (any) bump();
        }
    }

    // ---- JSON ----------------------------------------------------------------------------------------------

    std::string row_json(uint32_t i) const {
        const Entry& e = entries[i];
        std::string s = "{\"i\":" + std::to_string(i) + ",\"t\":" + q(e.time_text) + ",\"l\":" + q(level_name(e.level)) +
                        ",\"s\":" + q(e.source) + ",\"g\":" + q(e.logger) + ",\"m\":" + q(first_line(e.message, 600)) +
                        ",\"k\":" + (e.stack.empty() ? "0" : "1");
        if (e.rule) s += ",\"r\":" + q(e.rule->id) + ",\"rs\":" + q(severity_name(e.rule->severity));
        return s + "}";
    }

    std::string entry_json(uint32_t i) const {
        const Entry& e = entries[i];
        std::string s = "{\"i\":" + std::to_string(i) + ",\"line\":" + std::to_string(e.line) + ",\"time\":" + q(e.time_text) +
                        ",\"level\":" + q(level_name(e.level)) + ",\"source\":" + q(e.source) + ",\"logger\":" + q(e.logger) +
                        ",\"thread\":" + q(e.thread) + ",\"message\":" + q(e.message) + ",\"stack\":" + q(e.stack) +
                        ",\"format\":" + q(e.format) + ",\"raw\":" + q(e.raw);
        if (e.epoch_ms) s += ",\"epoch_ms\":" + std::to_string(*e.epoch_ms);
        s += ",\"fields\":[";
        for (size_t f = 0; f < e.fields.size(); ++f) s += (f ? ",[" : "[") + q(e.fields[f].first) + "," + q(e.fields[f].second) + "]";
        s += "],\"frames\":[";
        if (!e.stack.empty()) {
            auto frames = parse_stack(e.stack);
            for (size_t f = 0; f < frames.size(); ++f) {
                const Frame& fr = frames[f];
                s += (f ? "," : "") + std::string("{\"kind\":\"") + frame_kind(fr.kind) + "\",\"text\":" + q(fr.text) +
                     ",\"file\":" + q(fr.file) + ",\"line\":" + std::to_string(fr.line) + ",\"function\":" + q(fr.function) +
                     ",\"own\":" + (fr.own ? "true" : "false") + "}";
            }
        }
        s += "],\"problem\":";
        std::optional<RuleMatch> m = opt.rules && e.rule ? opt.rules->match(e) : std::nullopt;
        if (m && m->rule) {
            const Rule& r = *m->rule;
            s += "{\"rule\":" + q(r.id) + ",\"title\":" + q(render(r.title, m->captures)) + ",\"severity\":" + q(severity_name(r.severity)) +
                 ",\"symptom\":" + (r.symptom ? "true" : "false") + ",\"explain\":" + q(render(r.explain, m->captures)) +
                 ",\"docs\":" + q(r.docs) + ",\"evidence\":" + q(m->evidence) + ",\"fix\":[";
            for (size_t f = 0; f < r.fix.size(); ++f) s += (f ? "," : "") + q(render(r.fix[f], m->captures));
            s += "]}";
        } else {
            s += "null";
        }
        s += ",\"group\":" + (e.group == UINT32_MAX ? std::string("null") : std::to_string(e.group));
        return s + "}";
    }

    std::string ai_json() const {
#ifdef CULPRIT_WITH_AI
        auto avail = ai::availability(opt.no_ai, opt.project_start);
        if (!avail.allowed) return "{\"built\":true,\"available\":false,\"reason\":" + q(avail.reason) + "}";
        const ai::Settings& st = avail.settings;
        return std::string("{\"built\":true,\"available\":true,\"name\":") + q(st.name) + ",\"model\":" + q(st.model) +
               ",\"host\":" + q(st.host()) + ",\"local\":" + (st.local() ? "true" : "false") +
               ",\"confirm\":" + (st.confirm ? "true" : "false") + "}";
#else
        return "{\"built\":false,\"available\":false,\"reason\":\"This culprit was built without AI.\"}";
#endif
    }

    // with `mutex` held
    std::string status_json(const std::string& ai) const {
        static const std::array<const char*, 7> names = {"unknown", "trace", "debug", "info", "warn", "error", "fatal"};
        std::string s = "{\"seq\":" + std::to_string(seq) + ",\"title\":" + q(opt.title) + ",\"version\":" + q(opt.version) +
                        ",\"total\":" + std::to_string(entries.size()) + ",\"following\":" + (reading_done ? "false" : "true") +
                        ",\"multiple_sources\":" + (multiple_sources ? "true" : "false") +
                        ",\"problems\":" + std::to_string(analyzer.problem_count()) + ",\"filter\":" + q(opt.filter) + ",\"levels\":{";
        for (size_t i = 0; i < names.size(); ++i) s += (i ? "," : "") + q(names[i]) + ":" + std::to_string(counts[i]);
        s += "},\"errors\":[";
        size_t n = 0;
        for (const auto& e : opt.rule_errors) s += (n++ ? "," : "") + q(e);
        for (const auto& e : read_errors) s += (n++ ? "," : "") + q(e);
        s += "],\"ai\":" + ai + ",\"investigation\":";
#ifdef CULPRIT_WITH_AI
        s += inv ? "{\"state\":" + q(inv->state) + ",\"steps\":" + std::to_string(inv->steps.size()) + "}" : "null";
#else
        s += "null";
#endif
        return s + "}";
    }

    // ---- handlers -------------------------------------------------------------------------------------------

    void routes() {
        http.set_payload_max_length(64 * 1024);

        http.set_pre_routing_handler([this](const Request& req, Response& res) {
            res.set_header("Cache-Control", "no-store");
            res.set_header("X-Content-Type-Options", "nosniff");
            res.set_header("Referrer-Policy", "no-referrer");
            res.set_header("X-Frame-Options", "DENY");
            if (!host_allowed(req.get_header_value("Host"), port)) {
                res.status = 403;
                res.set_content("culprit web only answers on 127.0.0.1 and localhost.\n", "text/plain");
                return httplib::Server::HandlerResponse::Handled;
            }
            const bool from_header = req.has_header("X-Culprit-Token");
            const std::string given = from_header ? req.get_header_value("X-Culprit-Token") : req.get_param_value("t");
            if (!token_matches(given, token)) {
                res.status = 403;
                res.set_content("Open the address culprit printed in the terminal; it includes the key to this page.\n",
                                "text/plain");
                return httplib::Server::HandlerResponse::Handled;
            }
            if (req.method != "GET") {
                // a cross-site form can't set headers, and a browser names the page that sent a request
                const std::string origin = req.get_header_value("Origin");
                const std::string self = "http://127.0.0.1:" + std::to_string(port), alias = "http://localhost:" + std::to_string(port);
                if (!from_header || (!origin.empty() && origin != self && origin != alias)) {
                    res.status = 403;
                    res.set_content("Refused.\n", "text/plain");
                    return httplib::Server::HandlerResponse::Handled;
                }
            }
            return httplib::Server::HandlerResponse::Unhandled;
        });

        http.Get("/", [](const Request&, Response& res) {
            res.set_header("Content-Security-Policy",
                           "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; "
                           "img-src data:; base-uri 'none'; form-action 'none'; frame-ancestors 'none'");
            res.set_content(reinterpret_cast<const char*>(kWebPage), kWebPage_size, "text/html; charset=utf-8");
        });

        http.Get("/api/status", [this](const Request& req, Response& res) {
            const std::string ai = ai_json();  // reads ai.toml: outside the lock
            const auto since = number_param(req, "since");
            std::unique_lock lock(mutex);
            if (since) {
                changed.wait_for(lock, std::chrono::seconds(20),
                                 [&] { return stopping || seq != static_cast<uint64_t>(*since); });
            }
            json_reply(res, status_json(ai));
        });

        http.Get("/api/match", [this](const Request& req, Response& res) {
            Filter f = Filter::parse(req.get_param_value("filter"));
            if (!f.error().empty()) return error_reply(res, 200, f.error());
            const size_t from = static_cast<size_t>(number_param(req, "from").value_or(0));
            std::lock_guard lock(mutex);
            const size_t to = entries.size();
            std::string s = "{\"from\":" + std::to_string(std::min(from, to)) + ",\"to\":" + std::to_string(to);
            if (f.empty()) {
                s += ",\"all\":true";
            } else {
                s += ",\"ids\":[";
                bool first = true;
                for (size_t i = from; i < to; ++i) {
                    if (!f.matches(entries[i])) continue;
                    if (!first) s += ',';
                    s += std::to_string(i);
                    first = false;
                }
                s += "]";
            }
            s += ",\"highlights\":[";
            for (size_t i = 0; i < f.highlights().size(); ++i) s += (i ? "," : "") + q(f.highlights()[i]);
            json_reply(res, s + "]}");
        });

        http.Get("/api/rows", [this](const Request& req, Response& res) {
            const std::string ids = req.get_param_value("ids");
            std::lock_guard lock(mutex);
            std::string s = "[";
            size_t n = 0, start = 0;
            while (start < ids.size() && n < 1000) {
                size_t comma = ids.find(',', start);
                std::string part = ids.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                start = comma == std::string::npos ? ids.size() : comma + 1;
                if (part.empty() || part.size() > 10 || !std::all_of(part.begin(), part.end(), [](char c) { return c >= '0' && c <= '9'; })) {
                    continue;
                }
                const unsigned long long i = std::stoull(part);
                if (i >= entries.size()) continue;
                s += (n++ ? "," : "") + row_json(static_cast<uint32_t>(i));
            }
            json_reply(res, s + "]");
        });

        http.Get("/api/entry", [this](const Request& req, Response& res) {
            const auto i = number_param(req, "i");
            std::lock_guard lock(mutex);
            if (!i || static_cast<size_t>(*i) >= entries.size()) return error_reply(res, 404, "No such entry");
            json_reply(res, entry_json(static_cast<uint32_t>(*i)));
        });

        http.Get("/api/report", [this](const Request&, Response& res) {
            std::lock_guard lock(mutex);
            if (report_seq != seq) {
                report_cache = report_json(analyzer.report(), opt.title);
                report_seq = seq;
            }
            json_reply(res, report_cache);
        });

#ifdef CULPRIT_WITH_AI
        http.Get("/api/investigation", [this](const Request&, Response& res) {
            std::lock_guard lock(mutex);
            if (!inv) return json_reply(res, "{\"state\":\"none\"}");
            const auto end = inv->state == "running" ? std::chrono::steady_clock::now() : inv->ended;
            const auto secs = std::chrono::duration_cast<std::chrono::seconds>(end - inv->started).count();
            std::string s = "{\"state\":" + q(inv->state) + ",\"index\":" + std::to_string(inv->index) + ",\"title\":" + q(inv->title) +
                            ",\"where\":" + q(inv->where) + ",\"provider\":" + q(inv->provider) + ",\"model\":" + q(inv->model) +
                            ",\"local\":" + (inv->local ? "true" : "false") + ",\"seconds\":" + std::to_string(secs) +
                            ",\"cancelling\":" + (inv->cancel && inv->state == "running" ? "true" : "false") + ",\"steps\":[";
            static const char* kinds[] = {"thinking", "tool", "note", "warning"};
            for (size_t i = 0; i < inv->steps.size(); ++i) {
                const ai::Step& st = inv->steps[i];
                s += (i ? "," : "") + std::string("{\"kind\":\"") + kinds[static_cast<int>(st.kind)] + "\",\"text\":" +
                     q(first_line(st.text, 2000)) + "}";
            }
            s += "],\"finding\":" + (inv->finding.empty() ? std::string("null") : inv->finding);
            json_reply(res, s + "}");
        });

        http.Post("/api/investigate", [this](const Request& req, Response& res) { investigate(req, res); });

        http.Post("/api/investigation/cancel", [this](const Request&, Response& res) {
            std::lock_guard lock(mutex);
            if (inv && inv->state == "running") {
                inv->cancel = true;
                bump();
            }
            json_reply(res, "{\"ok\":true}");
        });
#else
        http.Post("/api/investigate", [](const Request&, Response& res) {
            error_reply(res, 501, "This culprit was built without AI.");
        });
#endif
    }

#ifdef CULPRIT_WITH_AI
    void investigate(const Request& req, Response& res) {
        auto body = ai::Json::parse(req.body.empty() ? "{}" : req.body);
        if (!body || !body->is_object()) return error_reply(res, 400, "Expected a JSON object");
        const bool confirmed = (*body)["confirm"].is_bool() && (*body)["confirm"].boolean();

        auto avail = ai::availability(opt.no_ai, opt.project_start);
        if (!avail.allowed) return error_reply(res, 409, avail.reason);
        const ai::Settings settings = avail.settings;
        const std::string root = ai::find_project_root(opt.project_start);

        std::unique_lock lock(mutex);
        if (inv && inv->state == "running") return error_reply(res, 409, "An investigation is already running.");
        if (entries.empty()) return error_reply(res, 404, "There are no log entries yet.");

        Report report = analyzer.report();
        std::optional<uint32_t> target;
        const ai::Json& index = (*body)["index"];
        if (index.is_number()) {
            const int64_t i = index.integer();
            if (i < 0 || static_cast<size_t>(i) >= entries.size()) return error_reply(res, 404, "No such entry.");
            target = static_cast<uint32_t>(i);
        } else {
            target = ai::default_target(report);
            if (!target) return error_reply(res, 404, "There is no error to investigate.");
        }
        const uint32_t at = ai::richest_occurrence(*target, entries.size(), [this](uint32_t i) -> const Entry& { return entries[i]; });
        std::vector<const Entry*> before;
        for (uint32_t i = at >= 20 ? at - 20 : 0; i < at; ++i) before.push_back(&entries[i]);
        ai::Subject subject = ai::make_subject(entries[at], before, &report, opt.rules.get());

        if (!settings.local() && settings.confirm && !confirmed) {
            const size_t chars = subject.entry.size() + subject.context.size() + subject.analysis.size() + subject.known.size();
            return json_reply(res,
                              "{\"needs_confirm\":true,\"name\":" + q(settings.name) + ",\"host\":" + q(settings.host()) +
                                  ",\"model\":" + q(settings.model) + ",\"lines_before\":" + std::to_string(before.size()) +
                                  ",\"chars\":" + std::to_string(chars) + ",\"title\":" + q(subject.title) + "}",
                              428);
        }

        // the worker searches the log through this; entries may grow meanwhile, so it locks
        subject.search_log = [this](const std::string& text) {
            std::lock_guard l(mutex);
            return ai::search_entries(entries, text);
        };

        if (inv && inv->worker.joinable()) inv->worker.join();  // a finished one
        auto next = std::make_shared<Investigation>();
        next->index = at;
        next->title = subject.title;
        next->where = subject.where;
        next->provider = settings.name;
        next->model = settings.model;
        next->local = settings.local();
        inv = next;
        bump();
        next->worker = std::thread([this, next, subject = std::move(subject), settings, root] {
            auto transport = ai::system_transport();
            auto model = ai::make_model(settings, *transport);
            ai::Workspace workspace(root);
            ai::Investigator investigator(*model, workspace, settings);
            ai::Finding f = investigator.run(
                subject,
                [&](const ai::Step& step) {
                    std::lock_guard l(mutex);
                    next->steps.push_back(step);
                    bump();
                },
                &next->cancel);
            std::lock_guard l(mutex);
            next->finding = finding_json(f, subject).dump();
            next->state = next->cancel ? "cancelled" : f.ok ? "done" : "failed";
            next->ended = std::chrono::steady_clock::now();
            bump();
        });
        json_reply(res, "{\"started\":true,\"index\":" + std::to_string(at) + "}");
    }
#endif
};

Server::Server(Options options) : impl_(std::make_unique<Impl>(std::move(options))) {}
Server::~Server() = default;

int Server::port() const { return impl_->port; }
const std::string& Server::token() const { return impl_->token; }
std::string Server::url() const { return "http://127.0.0.1:" + std::to_string(impl_->port) + "/?t=" + impl_->token; }

bool Server::start(std::string* error) {
    Impl& s = *impl_;
    s.routes();
    if (s.opt.port > 0) {
        // the usual port first, so the address stays the same between runs; then a few after it
        for (int p = s.opt.port; p < s.opt.port + 10 && s.port == 0; ++p) {
            if (s.http.bind_to_port("127.0.0.1", p)) s.port = p;
        }
    }
    if (s.port == 0) {
        int p = s.http.bind_to_any_port("127.0.0.1");
        if (p <= 0) {
            if (error) *error = "can't listen on 127.0.0.1";
            return false;
        }
        s.port = p;
    }
    s.reader = std::make_unique<Reader>(s.opt.inputs, s.opt.follow);
    s.pump = std::thread([&s] { s.run_pump(); });
    s.http_thread = std::thread([&s] {
        if (!s.http.listen_after_bind()) s.http_failed = true;
        {
            std::lock_guard lock(s.mutex);
        }
        s.changed.notify_all();
    });
    s.http.wait_until_ready();
    return true;
}

void Server::wait() {
    Impl& s = *impl_;
    std::unique_lock lock(s.mutex);
    s.changed.wait(lock, [&s] { return s.stopping || s.http_failed.load(); });
}

void Server::stop() { impl_->shutdown(); }

}  // namespace culprit::web
