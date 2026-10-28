#include "tui/finding.hpp"

#include "ai/json.hpp"
#include "tui/style.hpp"

namespace culprit {
namespace {

struct C {
    const char *reset = "", *bold = "", *dim = "", *red = "", *yellow = "", *green = "", *cyan = "";
    explicit C(bool on) {
        if (!on) return;
        reset = "\x1b[0m";
        bold = "\x1b[1m";
        dim = "\x1b[2m";
        red = "\x1b[31m";
        yellow = "\x1b[33m";
        green = "\x1b[32m";
        cyan = "\x1b[36m";
    }
};

size_t width_of(std::string_view s) {
    size_t n = 0;
    for (unsigned char c : s) n += (c & 0xC0) != 0x80;
    return n;
}

std::string wrap(std::string_view text, size_t width, const std::string& indent) {
    std::string out;
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find('\n', start);
        std::string_view para = text.substr(start, nl == std::string_view::npos ? std::string_view::npos : nl - start);
        std::string line;
        size_t pos = 0;
        while (pos < para.size()) {
            size_t sp = para.find(' ', pos);
            std::string_view word = para.substr(pos, sp == std::string_view::npos ? std::string_view::npos : sp - pos);
            if (!line.empty() && width_of(line) + 1 + width_of(word) > width) {
                out += indent + line + "\n";
                line.clear();
            }
            if (!line.empty()) line += ' ';
            line.append(word);
            if (sp == std::string_view::npos) break;
            pos = sp + 1;
        }
        out += indent + line + "\n";
        if (nl == std::string_view::npos) break;
        start = nl + 1;
    }
    return out;
}

std::string stats(const ai::Finding& f) {
    std::string s = std::to_string(f.steps) + (f.steps == 1 ? " step" : " steps") + " · " + std::to_string(f.files_read.size()) +
                    (f.files_read.size() == 1 ? " file read" : " files read");
    if (f.input_tokens || f.output_tokens) {
        s += " · " + thousands(static_cast<size_t>(f.input_tokens)) + " tokens in, " + thousands(static_cast<size_t>(f.output_tokens)) + " out";
    }
    s += " · " + f.provider + " " + f.model;
    return s;
}

void text(std::FILE* out, const ai::Finding& f, bool color, int width_i) {
    C c(color);
    const size_t width = static_cast<size_t>(std::max(60, width_i));
    if (!f.ok) {
        std::fprintf(out, "\n%sThe investigation stopped:%s %s\n", c.red, c.reset, f.error.c_str());
        return;
    }
    const char* conf_color = f.confidence == "high" ? c.green : f.confidence == "medium" ? c.yellow : c.red;
    std::fprintf(out, "\n%sROOT CAUSE%s %s(%s confidence)%s\n", c.bold, c.reset, conf_color, f.confidence.c_str(), c.reset);
    std::fprintf(out, "%s", c.bold);
    std::fputs(wrap(f.summary, width - 2, "  ").c_str(), out);
    std::fprintf(out, "%s", c.reset);
    if (!f.explanation.empty() && f.explanation != f.summary) {
        std::fputs("\n", out);
        std::fputs(wrap(f.explanation, width - 2, "  ").c_str(), out);
    }
    if (!f.evidence.empty()) {
        std::fprintf(out, "\n  %sEvidence%s\n", c.bold, c.reset);
        for (const auto& e : f.evidence) {
            std::fprintf(out, "    %s%s%s %s%s:%d%s", e.verified ? c.green : c.red, e.verified ? "✓" : "✗", c.reset, c.cyan,
                         e.file.c_str(), e.line, c.reset);
            if (!e.note.empty()) std::fprintf(out, "  %s(%s)%s", e.verified ? c.dim : c.red, e.note.c_str(), c.reset);
            std::fputs("\n", out);
            std::string q = e.quote;
            while (!q.empty() && (q.back() == '\n' || q.back() == ' ')) q.pop_back();
            std::fprintf(out, "%s", c.dim);
            std::fputs(wrap(q, width - 8, "        ").c_str(), out);
            std::fprintf(out, "%s", c.reset);
        }
    }
    if (!f.fix.empty()) {
        std::fprintf(out, "\n  %sWhat to do%s\n", c.bold, c.reset);
        for (size_t i = 0; i < f.fix.size(); ++i) {
            std::string item = wrap(f.fix[i], width - 7, "       ");
            item.replace(0, 7, "    " + std::to_string(i + 1) + ". ");
            std::fputs(item.c_str(), out);
        }
    }
    if (!f.confidence_note.empty()) std::fprintf(out, "\n  %sNote: %s.%s\n", c.yellow, f.confidence_note.c_str(), c.reset);
    std::fprintf(out, "\n  %s%s%s\n", c.dim, stats(f).c_str(), c.reset);
    std::fprintf(out, "  %sAI can be wrong. Evidence marked ✓ was checked against your files.%s\n", c.dim, c.reset);
}

void markdown(std::FILE* out, const ai::Finding& f, const ai::Subject& s) {
    std::fprintf(out, "### Investigation: %s\n\n", s.title.c_str());
    if (!f.ok) {
        std::fprintf(out, "The investigation stopped: %s\n", f.error.c_str());
        return;
    }
    std::fprintf(out, "**Root cause** (%s confidence): %s\n\n", f.confidence.c_str(), f.summary.c_str());
    if (!f.explanation.empty() && f.explanation != f.summary) std::fprintf(out, "%s\n\n", f.explanation.c_str());
    if (!f.evidence.empty()) {
        std::fputs("**Evidence**\n\n", out);
        for (const auto& e : f.evidence) {
            std::fprintf(out, "- %s `%s:%d`%s\n\n  ```\n  %s\n  ```\n", e.verified ? "✓" : "✗ (not found in the file)", e.file.c_str(), e.line,
                         e.verified && !e.note.empty() ? (" — " + e.note).c_str() : "", e.quote.c_str());
        }
        std::fputs("\n", out);
    }
    if (!f.fix.empty()) {
        std::fputs("**What to do**\n\n", out);
        for (size_t i = 0; i < f.fix.size(); ++i) std::fprintf(out, "%zu. %s\n", i + 1, f.fix[i].c_str());
        std::fputs("\n", out);
    }
    if (!f.confidence_note.empty()) std::fprintf(out, "_Note: %s._\n\n", f.confidence_note.c_str());
    std::fprintf(out, "_%s. AI can be wrong; evidence marked ✓ was checked against the files._\n", stats(f).c_str());
}

void json(std::FILE* out, const ai::Finding& f, const ai::Subject& s) {
    using ai::Json;
    Json ev = Json::array();
    for (const auto& e : f.evidence) {
        ev.push(Json::object({{"file", e.file}, {"line", e.line}, {"quote", e.quote}, {"verified", e.verified}, {"note", e.note}}));
    }
    Json fix = Json::array();
    for (const auto& x : f.fix) fix.push(x);
    Json files = Json::array();
    for (const auto& x : f.files_read) files.push(x);
    Json j = Json::object({{"subject", s.title},
                           {"where", s.where},
                           {"ok", f.ok},
                           {"error", f.error},
                           {"summary", f.summary},
                           {"explanation", f.explanation},
                           {"fix", fix},
                           {"evidence", ev},
                           {"confidence", f.confidence},
                           {"confidence_note", f.confidence_note},
                           {"structured", f.structured},
                           {"steps", f.steps},
                           {"files_read", files},
                           {"input_tokens", static_cast<int64_t>(f.input_tokens)},
                           {"output_tokens", static_cast<int64_t>(f.output_tokens)},
                           {"provider", f.provider},
                           {"model", f.model},
                           {"local", f.local}});
    std::fprintf(out, "%s\n", j.dump().c_str());
}

}  // namespace

void print_step(std::FILE* out, const ai::Step& step, bool color) {
    C c(color);
    switch (step.kind) {
        case ai::Step::Kind::Thinking: std::fprintf(out, "  %s… %s%s\n", c.dim, step.text.c_str(), c.reset); break;
        case ai::Step::Kind::Tool: std::fprintf(out, "  %s▸%s %s\n", c.cyan, c.reset, step.text.c_str()); break;
        case ai::Step::Kind::Note: {
            std::string t = one_line(step.text);
            if (t.size() > 160) t = t.substr(0, 157) + "...";
            std::fprintf(out, "    %s%s%s\n", c.dim, t.c_str(), c.reset);
            break;
        }
        case ai::Step::Kind::Warning: std::fprintf(out, "  %s! %s%s\n", c.yellow, step.text.c_str(), c.reset); break;
    }
    std::fflush(out);
}

void print_finding(std::FILE* out, const ai::Finding& f, const ai::Subject& s, ReportFormat format, bool color, int width) {
    switch (format) {
        case ReportFormat::Text: text(out, f, color, width); break;
        case ReportFormat::Markdown: markdown(out, f, s); break;
        case ReportFormat::Json: json(out, f, s); break;
    }
}

}  // namespace culprit
