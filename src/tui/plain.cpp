#include "tui/plain.hpp"

#include "core/stack.hpp"
#include "tui/style.hpp"

namespace culprit {
namespace {

const char* level_color(Level level) {
    switch (level) {
        case Level::Trace: return "\x1b[2m";
        case Level::Debug: return "\x1b[36m";
        case Level::Info: return "\x1b[32m";
        case Level::Warn: return "\x1b[33m";
        case Level::Error: return "\x1b[31m";
        case Level::Fatal: return "\x1b[1;37;41m";
        case Level::Unknown: break;
    }
    return "\x1b[2m";
}

}  // namespace

void print_plain(std::FILE* out, const Entry& e, const PlainOptions& o) {
    const char* dim = o.color ? "\x1b[2m" : "";
    const char* reset = o.color ? "\x1b[0m" : "";
    const char* cyan = o.color ? "\x1b[36m" : "";
    const char* bold = o.color ? "\x1b[1m" : "";

    std::string level(level_name(e.level));
    level.resize(5, ' ');
    std::fprintf(out, "%s%-12s%s %s%s%s", dim, e.time_text.c_str(), reset, o.color ? level_color(e.level) : "",
                 level.c_str(), reset);
    if (o.show_source && !e.source.empty()) std::fprintf(out, " %s%s%s", bold, e.source.c_str(), reset);
    if (!e.logger.empty()) std::fprintf(out, " %s%-20s%s", cyan, short_logger(e.logger).c_str(), reset);
    std::fprintf(out, " %s", one_line(e.message).c_str());
    for (const auto& [k, v] : e.fields) {
        if (k == "ecs.version" || k == "process.pid" || k == "pid" || k == "hostname" || k == "@version") continue;
        std::fprintf(out, " %s%s=%s%s", dim, k.c_str(), one_line(v).c_str(), reset);
    }
    std::fputc('\n', out);

    if (o.show_stack && !e.stack.empty()) {
        for (const auto& f : parse_stack(e.stack)) {
            const char* style = f.kind == Frame::Kind::Header     ? (o.color ? "\x1b[31m" : "")
                                : f.kind == Frame::Kind::CausedBy ? (o.color ? "\x1b[33m" : "")
                                : f.own                           ? bold
                                : f.kind == Frame::Kind::Frame    ? dim
                                                                  : dim;
            std::fprintf(out, "    %s%s%s%s\n", f.own ? "> " : "  ", style, f.text.c_str(), reset);
        }
    }
}

}  // namespace culprit
