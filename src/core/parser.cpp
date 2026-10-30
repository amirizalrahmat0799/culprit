#include "core/parser.hpp"

#include <algorithm>
#include <array>
#include <cctype>

#include "core/json.hpp"
#include "core/time.hpp"

namespace culprit {
namespace {

bool is_space(char c) { return c == ' ' || c == '\t'; }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_upper(char c) { return c >= 'A' && c <= 'Z'; }

std::string_view trim(std::string_view s) {
    while (!s.empty() && (is_space(s.front()) || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (is_space(s.back()) || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

bool starts_with(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool all_upper_letters(std::string_view s) {
    if (s.empty()) return false;
    return std::all_of(s.begin(), s.end(), [](char c) { return is_upper(c); });
}

// ---- prefixes added by docker compose / kubectl -------------------------------------------------------

// "backend-1  | text" -> ("backend-1", "text"); "[pod/api-7d9/api] text" -> ("pod/api-7d9/api", "text")
std::string_view strip_prefix(std::string_view line, std::string& source) {
    size_t i = 0;
    while (i < line.size() && (std::isalnum(static_cast<unsigned char>(line[i])) || line[i] == '-' ||
                               line[i] == '_' || line[i] == '.')) {
        ++i;
    }
    if (i > 0 && i < line.size()) {
        size_t j = i;
        while (j < line.size() && line[j] == ' ') ++j;
        if (j > i && j < line.size() && line[j] == '|' && (j + 1 == line.size() || line[j + 1] == ' ')) {
            source.assign(line.substr(0, i));
            return line.substr(std::min(line.size(), j + 2));
        }
    }
    if (starts_with(line, "[pod/")) {
        size_t close = line.find("] ");
        if (close != std::string_view::npos) {
            source.assign(line.substr(1, close - 1));
            return line.substr(close + 2);
        }
    }
    return line;
}

// ---- JSON ---------------------------------------------------------------------------------------------

const std::string* find_field(const json::Fields& f, std::initializer_list<std::string_view> keys, std::string_view* used = nullptr) {
    for (auto key : keys) {
        for (const auto& [k, v] : f) {
            if (k == key) {
                if (used) *used = key;
                return &v;
            }
        }
    }
    return nullptr;
}

Level json_level(const std::string& value) {
    if (!value.empty() && is_digit(value[0])) {
        int n = std::atoi(value.c_str());
        if (n >= 10) {  // pino / bunyan: 10 trace .. 60 fatal
            if (n >= 60) return Level::Fatal;
            if (n >= 50) return Level::Error;
            if (n >= 40) return Level::Warn;
            if (n >= 30) return Level::Info;
            if (n >= 20) return Level::Debug;
            return Level::Trace;
        }
        // syslog severity (GELF): 0 emergency .. 7 debug
        if (n <= 2) return Level::Fatal;
        if (n == 3) return Level::Error;
        if (n == 4) return Level::Warn;
        if (n <= 6) return Level::Info;
        return Level::Debug;
    }
    return parse_level(value).value_or(Level::Unknown);
}

bool parse_json(std::string_view text, Entry& e) {
    if (text.empty() || text[0] != '{') return false;
    auto fields = json::parse_object(text);
    if (!fields) return false;

    // Docker's json-file driver wraps the real line: {"log": "...\n", "stream": "stdout", "time": "..."}
    if (fields->size() <= 3) {
        const std::string* log = find_field(*fields, {"log"});
        if (log && find_field(*fields, {"stream"})) {
            std::string inner = *log;
            while (!inner.empty() && (inner.back() == '\n' || inner.back() == '\r')) inner.pop_back();
            Entry wrapped = parse_line(inner);
            if (wrapped.time_text.empty()) {
                if (const std::string* t = find_field(*fields, {"time"})) {
                    if (auto ts = parse_time_value(*t)) {
                        wrapped.time_text = ts->clock;
                        wrapped.epoch_ms = ts->epoch_ms;
                        wrapped.time_zoned = ts->zoned;
                    }
                }
            }
            e = std::move(wrapped);
            return true;
        }
    }

    e.format = "json";
    std::vector<std::string_view> used;
    auto take = [&](std::initializer_list<std::string_view> keys) -> const std::string* {
        std::string_view key;
        const std::string* v = find_field(*fields, keys, &key);
        if (v) used.push_back(key);
        return v;
    };

    if (const std::string* t = take({"@timestamp", "timestamp", "time", "ts", "t", "date", "datetime", "asctime"})) {
        if (auto ts = parse_time_value(*t)) {
            e.time_text = ts->clock;
            e.epoch_ms = ts->epoch_ms;
            e.time_zoned = ts->zoned;
        } else if (auto ts2 = parse_timestamp(*t)) {
            e.time_text = ts2->clock;
            e.epoch_ms = ts2->epoch_ms;
            e.time_zoned = ts2->zoned;
        }
    }
    if (const std::string* l = take({"level", "log.level", "severity", "lvl", "levelname", "level_name",
                                     "_level_name", "loglevel", "log_level"})) {
        e.level = json_level(*l);
    }
    if (const std::string* m = take({"message", "msg", "short_message", "text", "event"})) e.message = *m;
    if (const std::string* lg = take({"logger", "logger_name", "log.logger", "loggerName", "name", "caller"})) {
        e.logger = *lg;
    }
    if (const std::string* th = take({"thread", "thread_name", "process.thread.name", "threadName"})) e.thread = *th;
    if (const std::string* st = take({"stack_trace", "error.stack_trace", "exception", "stack", "err.stack",
                                      "error.stack", "exc_info", "full_message", "stacktrace"})) {
        e.stack = *st;
    }
    if (e.message.empty()) {
        if (const std::string* em = take({"err.message", "error.message"})) e.message = *em;
    }
    if (const std::string* svc = take({"service.name", "service", "app", "application"})) {
        if (e.source.empty()) e.source = *svc;
    }
    // A GELF full_message repeats the message; ECS repeats the type and message inside error.*
    while (!e.stack.empty() && (e.stack.back() == '\n' || e.stack.back() == '\r')) e.stack.pop_back();

    for (auto& [k, v] : *fields) {
        if (std::find(used.begin(), used.end(), k) != used.end()) continue;
        e.fields.emplace_back(k, v);
    }
    return true;
}

// ---- logfmt: time=... level=info msg="..." key=value ---------------------------------------------------

bool parse_logfmt(std::string_view text, Entry& e) {
    // must start with key=, and mention a level or message key, to avoid treating prose as logfmt
    size_t eq = text.find('=');
    if (eq == std::string_view::npos || eq == 0) return false;
    for (size_t i = 0; i < eq; ++i) {
        char c = text[i];
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '-')) return false;
    }
    if (text.find("level=") == std::string_view::npos && text.find("lvl=") == std::string_view::npos &&
        text.find("msg=") == std::string_view::npos) {
        return false;
    }

    json::Fields kv;
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && is_space(text[i])) ++i;
        size_t ks = i;
        while (i < text.size() && text[i] != '=' && !is_space(text[i])) ++i;
        std::string key(text.substr(ks, i - ks));
        std::string value;
        if (i < text.size() && text[i] == '=') {
            ++i;
            if (i < text.size() && text[i] == '"') {
                ++i;
                while (i < text.size() && text[i] != '"') {
                    if (text[i] == '\\' && i + 1 < text.size()) {
                        char n = text[++i];
                        value += n == 'n' ? '\n' : n == 't' ? '\t' : n;
                    } else {
                        value += text[i];
                    }
                    ++i;
                }
                if (i < text.size()) ++i;
            } else {
                size_t vs = i;
                while (i < text.size() && !is_space(text[i])) ++i;
                value.assign(text.substr(vs, i - vs));
            }
        }
        if (!key.empty()) kv.emplace_back(std::move(key), std::move(value));
    }

    e.format = "logfmt";
    for (auto& [k, v] : kv) {
        if ((k == "time" || k == "ts" || k == "t") && e.time_text.empty()) {
            if (auto ts = parse_time_value(v)) {
                e.time_text = ts->clock;
                e.epoch_ms = ts->epoch_ms;
                e.time_zoned = ts->zoned;
                continue;
            }
        }
        if ((k == "level" || k == "lvl") && e.level == Level::Unknown) {
            e.level = parse_level(v).value_or(Level::Unknown);
            continue;
        }
        if ((k == "msg" || k == "message") && e.message.empty()) {
            e.message = v;
            continue;
        }
        if ((k == "logger" || k == "component") && e.logger.empty()) {
            e.logger = v;
            continue;
        }
        if ((k == "stack" || k == "stacktrace" || k == "error.stack") && e.stack.empty()) {
            e.stack = v;
            continue;
        }
        e.fields.emplace_back(k, v);
    }
    return true;
}

// ---- plain text ----------------------------------------------------------------------------------------

// A timestamp at the start: "2026-10-28 12:00:01,234", "[2026-10-28T12:00:01Z]", "2026/10/28 12:00:01"
size_t read_text_timestamp(std::string_view s, Entry& e) {
    bool bracket = !s.empty() && s[0] == '[';
    std::string_view body = bracket ? s.substr(1) : s;
    std::string normalized;
    // Go's log package and nginx use 2026/10/28
    if (body.size() >= 19 && body[4] == '/' && body[7] == '/') {
        normalized.assign(body.substr(0, 19));
        normalized[4] = '-';
        normalized[7] = '-';
        body = normalized;
    }
    auto ts = parse_timestamp(body);
    if (!ts) return 0;
    size_t n = ts->length;
    if (bracket) {
        if (n + 1 >= s.size() || s[n + 1] != ']') return 0;
        n += 2;
    }
    e.time_text = ts->clock;
    e.epoch_ms = ts->epoch_ms;
    e.time_zoned = ts->zoned;
    return n;
}

bool looks_like_logger(std::string_view t) {
    if (t.empty() || t.size() > 120) return false;
    bool dot = false;
    for (char c : t) {
        if (c == '.') dot = true;
        else if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$' || c == '-' || c == '/' ||
                   c == '[' || c == ']')) {
            return false;
        }
    }
    return dot || std::isalpha(static_cast<unsigned char>(t[0]));
}

void parse_text(std::string_view text, Entry& e) {
    e.format = "text";
    size_t pos = read_text_timestamp(text, e);

    std::string pending_logger;  // a name seen before the level ("time - app.orders - ERROR - msg")
    std::vector<std::string> brackets;
    bool level_found = false;
    size_t message_start = std::string_view::npos;

    for (int token_no = 0; token_no < 10 && pos < text.size(); ++token_no) {
        while (pos < text.size() && is_space(text[pos])) ++pos;
        if (pos >= text.size()) break;

        if (text[pos] == '[') {
            size_t close = text.find(']', pos);
            if (close == std::string_view::npos) break;
            std::string_view inner = trim(text.substr(pos + 1, close - pos - 1));
            if (!level_found) {
                if (auto lv = parse_level(inner)) {
                    e.level = *lv;
                    level_found = true;
                    pos = close + 1;
                    continue;
                }
            }
            brackets.emplace_back(inner);
            pos = close + 1;
            continue;
        }

        size_t end = pos;
        while (end < text.size() && !is_space(text[end])) ++end;
        std::string_view tok = text.substr(pos, end - pos);

        if (!level_found) {
            // "ERROR", "WARN:", "ERROR:root:message" (Python's default), "<ERROR>"
            std::string_view word = tok;
            if (word.size() > 2 && word.front() == '<' && word.back() == '>') word = word.substr(1, word.size() - 2);
            size_t colon = word.find(':');
            std::string_view head = colon == std::string_view::npos ? word : word.substr(0, colon);
            auto lv = head == "LOG" ? std::optional<Level>(Level::Info) : parse_level(head);
            if (lv && (all_upper_letters(head) || (head.size() > 1 && is_upper(head[0]) && colon != std::string_view::npos))) {
                e.level = *lv;
                level_found = true;
                if (colon != std::string_view::npos && colon + 1 < word.size()) {
                    // ERROR:root:message -> logger "root", message "message"
                    std::string_view rest = text.substr(pos + colon + 1);
                    size_t c2 = rest.find(':');
                    if (c2 != std::string_view::npos && looks_like_logger(rest.substr(0, c2))) {
                        e.logger.assign(rest.substr(0, c2));
                        message_start = pos + colon + 1 + c2 + 1;
                    } else {
                        message_start = pos + colon + 1;
                    }
                    break;
                }
                pos = end;
                continue;
            }
        }

        // things to step over: pid, "---", nginx's "29#29:", a time zone, lone separators before the level
        if (tok == "---" || (!level_found && token_no == 0 && (tok == "UTC" || tok == "GMT" || tok == "Z")) || (!level_found && tok == "-") ||
            std::all_of(tok.begin(), tok.end(), [](char c) { return is_digit(c); }) ||
            (tok.find('#') != std::string_view::npos && is_digit(tok[0]) && tok.back() == ':')) {
            pos = end;
            continue;
        }

        if (level_found && (tok == "-" || tok == ":")) {
            message_start = end;
            break;
        }

        if (looks_like_logger(tok)) {
            // a logger is followed by " : " (Spring Boot) or " - " (logback, Python)
            size_t after = end;
            while (after < text.size() && is_space(text[after])) ++after;
            bool separator = after < text.size() && (text[after] == ':' || text[after] == '-') &&
                             (after + 1 == text.size() || is_space(text[after + 1]));
            if (separator) {
                if (level_found) {
                    e.logger.assign(tok);
                    message_start = after + 1;
                    break;
                }
                pending_logger.assign(tok);
                pos = after + 1;
                continue;
            }
        }
        message_start = pos;
        break;
    }

    if (e.logger.empty() && !pending_logger.empty()) e.logger = pending_logger;
    if (!brackets.empty()) e.thread = brackets.back();
    if (message_start == std::string_view::npos) message_start = pos;
    if (message_start > text.size()) message_start = text.size();
    e.message.assign(trim(text.substr(message_start)));
    if (e.level != Level::Unknown && !e.logger.empty() && !e.thread.empty()) e.format = "spring";
}

// ---- web server access logs ------------------------------------------------------------------------
// 172.18.0.1 - - [28/Oct/2026:01:44:17 +0000] "GET / HTTP/1.1" 200 612 "-" "Mozilla/5.0"

bool parse_access_log(std::string_view text, Entry& e) {
    size_t open = text.find(" [");
    size_t quote = text.find(" \"");
    if (open == std::string_view::npos || quote == std::string_view::npos || quote < open || open > 64) return false;
    size_t close = text.find("] ", open);
    if (close == std::string_view::npos || close > quote) return false;
    std::string_view when = text.substr(open + 2, close - open - 2);  // 28/Oct/2026:01:44:17 +0000
    if (when.size() < 20 || when[2] != '/' || when[6] != '/' || when[11] != ':') return false;
    size_t req_end = text.find('"', quote + 2);
    if (req_end == std::string_view::npos) return false;
    std::string_view request = text.substr(quote + 2, req_end - quote - 2);
    std::string_view rest = trim(text.substr(req_end + 1));
    size_t sp = rest.find(' ');
    std::string_view status = rest.substr(0, sp);
    if (status.size() != 3 || !std::all_of(status.begin(), status.end(), [](char c) { return is_digit(c); })) return false;

    static const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int month = 0;
    for (int m = 0; m < 12; ++m) {
        if (when.substr(3, 3) == kMonths[m]) month = m + 1;
    }
    if (month == 0) return false;
    // rebuild as ISO 8601 so the usual parser handles the offset
    std::string iso;
    iso += when.substr(7, 4);
    iso += '-';
    iso += static_cast<char>('0' + month / 10);
    iso += static_cast<char>('0' + month % 10);
    iso += '-';
    iso += when.substr(0, 2);
    iso += 'T';
    iso += when.substr(12, 8);
    if (when.size() >= 26) {
        iso += when.substr(21, 3);
        iso += ':';
        iso += when.substr(24, 2);
    }
    if (auto ts = parse_timestamp(iso)) {
        e.time_text = ts->clock;
        e.epoch_ms = ts->epoch_ms;
        e.time_zoned = ts->zoned;
    }

    int code = (status[0] - '0') * 100 + (status[1] - '0') * 10 + (status[2] - '0');
    e.level = code >= 500 ? Level::Error : code >= 400 ? Level::Warn : Level::Info;
    e.format = "access";
    // "GET /api/orders HTTP/1.1" -> "GET /api/orders 500"
    std::string_view req = request;
    if (size_t proto = req.rfind(" HTTP/"); proto != std::string_view::npos) req = req.substr(0, proto);
    e.message.assign(req);
    e.message += ' ';
    e.message.append(status);
    e.fields.emplace_back("client", std::string(text.substr(0, text.find(' '))));
    e.fields.emplace_back("status", std::string(status));
    if (sp != std::string_view::npos) {
        std::string_view after = trim(rest.substr(sp));
        e.fields.emplace_back("bytes", std::string(after.substr(0, after.find(' '))));
        size_t ua_end = after.rfind('"');
        size_t ua_start = ua_end == std::string_view::npos ? ua_end : after.rfind('"', ua_end - 1);
        if (ua_start != std::string_view::npos && ua_end > ua_start + 1) {
            e.fields.emplace_back("user_agent", std::string(after.substr(ua_start + 1, ua_end - ua_start - 1)));
        }
    }
    return true;
}

// ---- stack traces --------------------------------------------------------------------------------------

// "java.lang.IllegalStateException: ..." or "com.example.OrderException" on its own
bool looks_like_exception_header(std::string_view s) {
    size_t i = 0;
    int dots = 0;
    while (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_' || s[i] == '$' || s[i] == '.')) {
        dots += s[i] == '.';
        ++i;
    }
    if (i == 0) return false;
    std::string_view name = s.substr(0, i);
    bool named = name.ends_with("Exception") || name.ends_with("Error") || name.ends_with("Throwable") ||
                 name.ends_with("Panic");
    if (!named) return false;
    if (i == s.size() || s[i] == ':') return dots > 0 || i == s.size() || is_upper(name[0]);
    return false;
}

}  // namespace

std::string_view level_name(Level level) {
    switch (level) {
        case Level::Trace: return "TRACE";
        case Level::Debug: return "DEBUG";
        case Level::Info: return "INFO";
        case Level::Warn: return "WARN";
        case Level::Error: return "ERROR";
        case Level::Fatal: return "FATAL";
        case Level::Unknown: break;
    }
    return "-";
}

std::optional<Level> parse_level(std::string_view text) {
    std::string t = lower(trim(text));
    static const std::array<std::pair<std::string_view, Level>, 22> kWords = {{
        {"trace", Level::Trace},   {"finest", Level::Trace},  {"finer", Level::Trace},
        {"debug", Level::Debug},   {"fine", Level::Debug},    {"config", Level::Debug},
        {"info", Level::Info},     {"information", Level::Info}, {"notice", Level::Info},
        {"warn", Level::Warn},     {"warning", Level::Warn},
        {"error", Level::Error},   {"err", Level::Error},     {"severe", Level::Error},
        {"fatal", Level::Fatal},   {"critical", Level::Fatal}, {"crit", Level::Fatal},
        {"panic", Level::Fatal},   {"alert", Level::Fatal},   {"emerg", Level::Fatal},
        {"emergency", Level::Fatal}, {"dpanic", Level::Fatal},
    }};
    for (const auto& [word, level] : kWords) {
        if (t == word) return level;
    }
    return std::nullopt;
}

Entry parse_line(std::string_view line) {
    Entry e;
    std::string source;
    std::string_view text = strip_prefix(line, source);
    e.source = source;
    std::string_view body = trim(text);
    if (!parse_json(body, e) && !parse_logfmt(body, e) && !parse_access_log(body, e)) parse_text(text, e);
    if (e.level == Level::Unknown && starts_with(body, "panic: ")) e.level = Level::Fatal;  // Go
    if (e.source.empty()) e.source = source;
    e.raw.assign(line);
    return e;
}

bool Parser::is_continuation(std::string_view text, const std::string& prefix_source) const {
    if (!pending_) return false;
    if (prefix_source != current_.source && !(prefix_source.empty() && current_.source == source_)) {
        // a line from another Compose service can't continue this one
        if (!prefix_source.empty()) return false;
    }
    // Compose prints its own lines ("app-1 exited with code 1") without a prefix; they never continue a
    // service's entry
    if (prefixed_ && prefix_source.empty()) return false;
    if (text.empty()) return in_stack_;
    if (is_space(text[0])) return true;  // "\tat com.example...", "  File \"app.py\", line 3"
    if (starts_with(text, "Caused by:") || starts_with(text, "Suppressed:") || starts_with(text, "... ") ||
        starts_with(text, "Traceback (most recent call last)") || starts_with(text, "During handling of") ||
        starts_with(text, "The above exception") || starts_with(text, "goroutine ")) {
        return true;
    }
    if (looks_like_exception_header(text)) return true;
    if (in_stack_) {
        // inside a stack trace, anything that doesn't start a new entry belongs to it
        // (Go frames like "main.main()", Python's final "ValueError: bad input")
        Entry probe;
        if (read_text_timestamp(text, probe) > 0) return false;
        if (!text.empty() && text[0] == '{') return false;
        // "ERROR ...", "WARN: ...", "WARNING:root:..." start a new entry
        size_t end = text.find_first_of(" :");
        std::string_view first = text.substr(0, end);
        if ((parse_level(first) || first == "LOG") && all_upper_letters(first)) return false;
        return true;
    }
    return false;
}

namespace {

bool has_control(std::string_view s) {
    for (size_t i = 0; i < s.size(); ++i) {
        const auto c = static_cast<unsigned char>(s[i]);
        if ((c < 0x20 && c != '\t') || c == 0x7f) return true;
        if (c == 0xc2 && i + 1 < s.size() && static_cast<unsigned char>(s[i + 1]) >= 0x80 &&
            static_cast<unsigned char>(s[i + 1]) <= 0x9f) {
            return true;
        }
    }
    return false;
}

void clean_in_place(std::string& s, bool keep_newlines) {
    if (has_control(s)) s = strip_terminal_codes(s, keep_newlines);
}

}  // namespace

std::string strip_terminal_codes(std::string_view s, bool keep_newlines) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        // C1 controls written as UTF-8 (U+0080-U+009F); U+009B is a CSI on some terminals
        if (c == 0xc2 && i + 1 < s.size() && static_cast<unsigned char>(s[i + 1]) >= 0x80 &&
            static_cast<unsigned char>(s[i + 1]) <= 0x9f) {
            i += 2;
            continue;
        }
        if (c != 0x1b) {
            if ((c < 0x20 && c != '\t' && !(keep_newlines && c == '\n')) || c == 0x7f) {
                ++i;
                continue;
            }
            out += static_cast<char>(c);
            ++i;
            continue;
        }
        // ESC: work out how long the sequence is and skip it
        ++i;
        if (i >= s.size()) break;
        const char kind = s[i++];
        if (kind == '[') {
            // CSI: parameters and intermediates, then one final byte in @..~
            while (i < s.size() && !(s[i] >= '@' && s[i] <= '~')) ++i;
            if (i < s.size()) ++i;
        } else if (kind == ']' || kind == 'P' || kind == '_' || kind == '^' || kind == 'X') {
            // OSC, DCS, APC, PM, SOS: a string ended by BEL or ESC backslash
            while (i < s.size()) {
                if (s[i] == '\a') {
                    ++i;
                    break;
                }
                if (s[i] == '\x1b' && i + 1 < s.size() && s[i + 1] == '\\') {
                    i += 2;
                    break;
                }
                ++i;
            }
        } else {
            // two- or three-character sequences: ESC ( B, ESC =, ESC 7, ...
            while (i < s.size() && s[i - 1] >= ' ' && s[i - 1] <= '/') ++i;
        }
    }
    return out;
}

void Parser::feed(std::string_view raw_line, std::vector<Entry>& out) {
    std::string cleaned;
    if (has_control(raw_line)) {
        cleaned = strip_terminal_codes(raw_line);
        raw_line = cleaned;
    }
    std::string_view line = raw_line;
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    ++line_;

    std::string prefix_source;
    std::string_view text = strip_prefix(line, prefix_source);

    // blank lines never make an entry: inside a stack trace they're kept, otherwise skipped (Spring Boot
    // prints one between "threw exception" and the trace)
    if (trim(text).empty()) {
        if (pending_ && in_stack_) {
            current_.raw += '\n';
            current_.raw.append(line);
            current_.stack += '\n';
        }
        return;
    }

    if (is_continuation(text, prefix_source)) {
        current_.raw += '\n';
        current_.raw.append(line);
        if (!current_.stack.empty()) current_.stack += '\n';
        current_.stack.append(text);
        in_stack_ = true;
        return;
    }

    flush(out);
    current_ = parse_line(line);
    current_.line = line_;
    prefixed_ = !prefix_source.empty();
    if (current_.source.empty()) current_.source = source_;
    pending_ = true;
    in_stack_ = !current_.stack.empty() || starts_with(trim(text), "panic:");
}

void Parser::flush(std::vector<Entry>& out) {
    if (!pending_) return;
    // a stack trace printed under a line with no level means something went wrong
    if (current_.level == Level::Unknown && !current_.stack.empty()) current_.level = Level::Error;
    // JSON logs can carry escape codes inside strings ("\u001b[31m"), decoded only now
    clean_in_place(current_.message, true);
    clean_in_place(current_.stack, true);
    clean_in_place(current_.logger, false);
    clean_in_place(current_.thread, false);
    clean_in_place(current_.source, false);
    clean_in_place(current_.time_text, false);
    for (auto& [k, v] : current_.fields) {
        clean_in_place(k, false);
        clean_in_place(v, true);
    }
    while (!current_.stack.empty() && (current_.stack.back() == '\n' || current_.stack.back() == ' ')) current_.stack.pop_back();
    out.push_back(std::move(current_));
    current_ = Entry{};
    pending_ = false;
    in_stack_ = false;
}

}  // namespace culprit
