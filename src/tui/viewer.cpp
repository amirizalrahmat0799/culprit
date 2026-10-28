#include "tui/viewer.hpp"

#include <algorithm>
#include <atomic>
#include <ftxui/component/app.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

#include "core/analyzer.hpp"
#include "core/model.hpp"
#include "core/rules.hpp"
#include "core/stack.hpp"
#include "tui/style.hpp"

namespace culprit {
namespace {

using namespace ftxui;

Color level_color(Level level) {
    switch (level) {
        case Level::Trace: return Color::GrayDark;
        case Level::Debug: return Color::CyanLight;
        case Level::Info: return Color::Green;
        case Level::Warn: return Color::Yellow;
        case Level::Error: return Color::Red;
        case Level::Fatal: return Color::RedLight;
        case Level::Unknown: break;
    }
    return Color::GrayDark;
}

Color severity_color(Severity s) {
    switch (s) {
        case Severity::Critical: return Color::RedLight;
        case Severity::Error: return Color::Red;
        case Severity::Warning: return Color::Yellow;
        case Severity::Info: return Color::Cyan;
    }
    return Color::Red;
}

std::string upper(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return out;
}

std::string clip(const std::string& s, size_t n) {
    if (s.size() <= n) return s;
    size_t cut = n > 1 ? n - 1 : 0;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    return s.substr(0, cut) + "…";
}

Element level_badge(Level level) {
    std::string name(level_name(level));
    name.resize(5, ' ');
    auto e = text(name) | color(level_color(level));
    if (level >= Level::Error) e = e | bold;
    if (level == Level::Fatal) e = e | inverted;
    return e;
}

// Splits `s` around case-insensitive matches of the highlight words, marking the matches.
Element highlighted(const std::string& s, const std::vector<std::string>& words) {
    if (words.empty()) return text(s);
    Elements parts;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t best = std::string::npos, best_len = 0;
        for (const auto& w : words) {
            if (w.empty()) continue;
            for (size_t i = pos; i + w.size() <= s.size(); ++i) {
                bool eq = true;
                for (size_t j = 0; j < w.size() && eq; ++j) {
                    eq = std::tolower(static_cast<unsigned char>(s[i + j])) == static_cast<unsigned char>(w[j]);
                }
                if (eq) {
                    if (i < best) best = i, best_len = w.size();
                    break;
                }
            }
        }
        if (best == std::string::npos) {
            parts.push_back(text(s.substr(pos)));
            break;
        }
        if (best > pos) parts.push_back(text(s.substr(pos, best - pos)));
        parts.push_back(text(s.substr(best, best_len)) | bgcolor(Color::Yellow) | color(Color::Black));
        pos = best + best_len;
    }
    return hbox(std::move(parts));
}

constexpr const char* kHelp[] = {
    "  Moving                         Finding",
    "  ↑ ↓ / j k    one line          /         filter (Enter keeps it, Esc cancels)",
    "  PgUp PgDn    one page          n  N      next / previous error",
    "  g  G         first / last      w  W      next / previous warning",
    "  mouse wheel  scroll            e         only warnings and errors (again: all)",
    "                                 x         clear the filter",
    "  Details                        ",
    "  Enter        open / close      Following",
    "  Tab          scroll details    f         follow new lines on / off",
    "  Esc          close             G / End   jump to the end and follow",
    "",
    "  Problems",
    "  a            what culprit found: the likely root cause, known problems and",
    "               repeated errors. Enter shows that problem's entries.",
    "  p  P         next / previous entry with a problem    ◆ marks them",
    "",
    "  Filter:  payment  \"order failed\"  -health  level>=warn  logger=OrderService",
    "           orderId=8812  /time(d)? out/  has:stack  has:problem  problem:hikari",
    "",
    "  q  quit                         ?  close this help",
};

class Viewer {
public:
    Viewer(Reader& reader, const ViewerOptions& options)
        : reader_(reader), options_(options), model_(options.rules) {
        model_.set_filter(options.initial_filter);
        filter_text_ = options.initial_filter;
        message_ = options.message;
    }

    int run() {
        auto app = App::Fullscreen();
        app.HandlePipedInput(false);  // main() already gave the keyboard back to stdin
        app_ = &app;
        reader_.start([this] {
            // wake the UI at most once per batch of entries
            if (!wake_pending_.exchange(true)) app_->PostEvent(Event::Custom);
        });

        auto component = Renderer([this] { return render(); });
        component = CatchEvent(component, [this](Event e) {
            // Escape and the next key can arrive together and read as Alt+key ("\x1b" "q"); nothing here
            // uses Alt, so treat it as the two keys the user pressed
            const std::string& in = e.input();
            if (!e.is_mouse() && in.size() == 2 && in[0] == '\x1b' && in[1] >= 0x20 && in[1] < 0x7f && in[1] != '[' &&
                in[1] != 'O') {
                on_event(Event::Escape);
                return on_event(Event::Character(in[1]));
            }
            return on_event(e);
        });
        app.Loop(component);
        reader_.stop();
        return 0;
    }

private:
    // ---- data ----------------------------------------------------------------------------------------

    void drain() {
        wake_pending_ = false;
        auto batch = reader_.take();
        for (auto& err : reader_.take_errors()) message_ = err;
        if (batch.empty()) return;
        model_.append(std::move(batch));
        if (follow_ && !model_.visible().empty()) cursor_ = model_.visible().size() - 1;
    }

    size_t rows() const { return model_.visible().size(); }

    void move_to(long long row) {
        if (rows() == 0) {
            cursor_ = 0;
            return;
        }
        row = std::clamp<long long>(row, 0, static_cast<long long>(rows()) - 1);
        cursor_ = static_cast<size_t>(row);
        follow_ = cursor_ + 1 == rows();
        detail_scroll_ = 0;
    }

    void set_filter(const std::string& q) {
        // keep the selected entry selected when it still passes
        std::optional<uint32_t> selected;
        if (cursor_ < rows()) selected = model_.visible()[cursor_];
        model_.set_filter(q);
        const auto& vis = model_.visible();
        if (selected) {
            auto it = std::lower_bound(vis.begin(), vis.end(), *selected);
            cursor_ = it == vis.end() ? (vis.empty() ? 0 : vis.size() - 1) : static_cast<size_t>(it - vis.begin());
        }
        if (follow_ && !vis.empty()) cursor_ = vis.size() - 1;
    }

    // ---- input ---------------------------------------------------------------------------------------

    bool on_event(Event e) {
        if (e == Event::Custom) {
            drain();
            return true;
        }
        if (editing_) return on_filter_key(e);
        if (panel_) return on_panel_key(e);
        if (help_) {
            if (e == Event::Character('?') || e == Event::Escape || e == Event::Character('q')) help_ = false;
            return true;
        }
        if (e.is_mouse()) return on_mouse(e);

        const long long page = std::max(1, list_height_ - 1);
        if (focus_detail_ && detail_) {
            if (e == Event::ArrowDown || e == Event::Character('j')) return ++detail_scroll_, true;
            if (e == Event::ArrowUp || e == Event::Character('k')) return detail_scroll_ = std::max(0, detail_scroll_ - 1), true;
            if (e == Event::PageDown) return detail_scroll_ += detail_height_, true;
            if (e == Event::PageUp) return detail_scroll_ = std::max(0, detail_scroll_ - detail_height_), true;
        }
        if (e == Event::ArrowDown || e == Event::Character('j')) return move_to(static_cast<long long>(cursor_) + 1), true;
        if (e == Event::ArrowUp || e == Event::Character('k')) return move_to(static_cast<long long>(cursor_) - 1), true;
        if (e == Event::PageDown || e == Event::Character(' ')) return move_to(static_cast<long long>(cursor_) + page), true;
        if (e == Event::PageUp) return move_to(static_cast<long long>(cursor_) - page), true;
        if (e == Event::Home || e == Event::Character('g')) return move_to(0), true;
        if (e == Event::End || e == Event::Character('G')) {
            move_to(static_cast<long long>(rows()) - 1);
            follow_ = true;
            return true;
        }
        if (e == Event::Character('f')) {
            follow_ = !follow_;
            if (follow_) move_to(static_cast<long long>(rows()) - 1), follow_ = true;
            return true;
        }
        if (e == Event::Return) {
            detail_ = !detail_;
            focus_detail_ = false;
            detail_scroll_ = 0;
            return true;
        }
        if (e == Event::Tab) {
            if (detail_) focus_detail_ = !focus_detail_;
            return true;
        }
        if (e == Event::Escape) {
            if (detail_) detail_ = false, focus_detail_ = false;
            else message_.clear();
            return true;
        }
        if (e == Event::Character('/')) {
            editing_ = true;
            filter_before_ = model_.query();
            filter_text_ = model_.query();
            return true;
        }
        if (e == Event::Character('x')) {
            filter_text_.clear();
            set_filter("");
            return true;
        }
        if (e == Event::Character('e')) {
            const std::string term = "level>=warn";
            std::string q = model_.query();
            size_t at = q.find(term);
            if (at != std::string::npos) q.erase(at, term.size());
            else q = term + (q.empty() ? "" : " ") + q;
            while (!q.empty() && q.front() == ' ') q.erase(0, 1);
            while (!q.empty() && q.back() == ' ') q.pop_back();
            filter_text_ = q;
            set_filter(q);
            return true;
        }
        auto jump = [&](int dir, Level min) {
            if (auto r = model_.find(cursor_, dir, min)) move_to(static_cast<long long>(*r));
            else message_ = dir > 0 ? "no more below" : "no more above";
            return true;
        };
        if (e == Event::Character('n')) return jump(1, Level::Error);
        if (e == Event::Character('N')) return jump(-1, Level::Error);
        if (e == Event::Character('w')) return jump(1, Level::Warn);
        if (e == Event::Character('W')) return jump(-1, Level::Warn);
        if (e == Event::Character('?')) return help_ = true, true;
        if (e == Event::Character('a')) {
            panel_ = true;
            panel_selected_ = 0;
            panel_scroll_ = 0;
            return true;
        }
        auto jump_problem = [&](int dir) {
            const auto& vis = model_.visible();
            for (long long i = static_cast<long long>(cursor_) + dir; i >= 0 && i < static_cast<long long>(vis.size()); i += dir) {
                if (model_.entry(vis[static_cast<size_t>(i)]).rule) return move_to(i), true;
            }
            message_ = dir > 0 ? "no more problems below" : "no more problems above";
            return true;
        };
        if (e == Event::Character('p')) return jump_problem(1);
        if (e == Event::Character('P')) return jump_problem(-1);
        if (e == Event::Character('q') || e == Event::CtrlC) {
            app_->Exit();
            return true;
        }
        return false;
    }

    // ---- problems panel ------------------------------------------------------------------------------

    struct PanelItem {
        bool problem;  // else a group
        size_t index;  // into report_.problems / report_.groups
    };

    const Report& report() {
        // recomputed only when entries arrived since
        if (report_total_ != model_.total() || !report_ready_) {
            report_ = model_.analyzer().report();
            report_total_ = model_.total();
            report_ready_ = true;
            items_.clear();
            for (size_t i = 0; i < report_.problems.size(); ++i) items_.push_back({true, i});
            for (size_t i = 0; i < report_.groups.size(); ++i) items_.push_back({false, i});
            // the root cause first
            if (report_.root) {
                bool problem = report_.root->kind == RootCause::Kind::Problem;
                auto it = std::find_if(items_.begin(), items_.end(),
                                       [&](const PanelItem& p) { return p.problem == problem && p.index == report_.root->index; });
                if (it != items_.end()) std::rotate(items_.begin(), it, it + 1);
            }
        }
        return report_;
    }

    bool is_root(const PanelItem& item) const {
        return report_.root && (report_.root->kind == RootCause::Kind::Problem) == item.problem && report_.root->index == item.index;
    }

    bool on_panel_key(Event e) {
        report();
        const long long n = static_cast<long long>(items_.size());
        auto select = [&](long long i) {
            panel_selected_ = static_cast<size_t>(std::clamp<long long>(i, 0, std::max<long long>(0, n - 1)));
            panel_detail_scroll_ = 0;
            return true;
        };
        if (e == Event::Escape || e == Event::Character('a') || e == Event::Character('q')) return panel_ = false, true;
        if (e == Event::ArrowDown || e == Event::Character('j')) return select(static_cast<long long>(panel_selected_) + 1);
        if (e == Event::ArrowUp || e == Event::Character('k')) return select(static_cast<long long>(panel_selected_) - 1);
        if (e == Event::PageDown) return select(static_cast<long long>(panel_selected_) + 8);
        if (e == Event::PageUp) return select(static_cast<long long>(panel_selected_) - 8);
        if (e == Event::Home || e == Event::Character('g')) return select(0);
        if (e == Event::End || e == Event::Character('G')) return select(n - 1);
        if (e == Event::Tab) return panel_detail_scroll_ += 3, true;
        if (e == Event::TabReverse) return panel_detail_scroll_ = std::max(0, panel_detail_scroll_ - 3), true;
        if (e == Event::Return && n > 0) {
            const PanelItem& item = items_[panel_selected_];
            std::string q = item.problem ? "problem:" + report_.problems[item.index].rule->id
                                         : "group:" + std::to_string(report_.groups[item.index].id);
            uint32_t first = item.problem ? report_.problems[item.index].first.index : report_.groups[item.index].first.index;
            panel_ = false;
            follow_ = false;
            filter_text_ = q;
            set_filter(q);
            const auto& vis = model_.visible();
            auto it = std::lower_bound(vis.begin(), vis.end(), first);
            move_to(it == vis.end() ? 0 : static_cast<long long>(it - vis.begin()));
            follow_ = false;
            detail_ = true;
            message_.clear();
            return true;
        }
        if (e == Event::Character('?')) return help_ = true, panel_ = false, true;
        return true;
    }

    Element panel_row(const PanelItem& item, bool selected, int width) {
        Elements cells;
        std::string count, when, source, title;
        Element badge;
        if (item.problem) {
            const Problem& p = report_.problems[item.index];
            std::string sev = upper(severity_name(p.rule->severity));
            sev.resize(8, ' ');
            badge = text(sev) | color(severity_color(p.rule->severity)) | bold;
            title = p.title();
            if (p.variants.size() > 1) title += " (+" + std::to_string(p.variants.size() - 1) + " more)";
            count = "×" + thousands(p.count);
            when = p.first.time;
            source = p.first.source;
        } else {
            const ErrorGroup& g = report_.groups[item.index];
            std::string lv(level_name(g.level));
            lv.resize(8, ' ');
            badge = text(lv) | color(level_color(g.level)) | dim;
            title = g.title;
            count = "×" + thousands(g.count);
            when = g.first.time;
            source = g.first.source;
        }
        cells.push_back(text(selected ? " ▶ " : "   ") | color(Color::Cyan));
        cells.push_back(badge);
        if (is_root(item)) cells.push_back(text(" ROOT CAUSE ") | bold | color(Color::Black) | bgcolor(Color::RedLight));
        cells.push_back(text(" " + one_line(title)) | (selected ? bold : nothing) | flex);
        cells.push_back(text(" " + count + " ") | bold | size(WIDTH, EQUAL, 9));
        if (width > 90) {
            cells.push_back(text(when.empty() ? "" : when) | dim | size(WIDTH, EQUAL, 13));
            cells.push_back(text(" " + clip(source, 14)) | dim | size(WIDTH, EQUAL, 16));
        }
        auto line = hbox(std::move(cells));
        if (selected) line = line | bgcolor(Color::RGB(40, 52, 66));
        return line;
    }

    std::string where_text(const Occurrence& o) {
        std::string s;
        if (!o.source.empty()) s += o.source + "  ";
        if (!o.time.empty()) s += o.time + "  ";
        if (o.line) s += "line " + std::to_string(o.line);
        return s;
    }

    Element panel_detail(const PanelItem& item) {
        Elements out;
        auto heading = [](const std::string& t) { return text(t) | bold | color(Color::Cyan); };
        std::optional<Frame> own;
        if (item.problem) {
            const Problem& p = report_.problems[item.index];
            out.push_back(hbox({text(p.title()) | bold | color(severity_color(p.rule->severity)),
                                text("  " + p.rule->id) | dim}));
            out.push_back(text("first: " + where_text(p.first) + "   last: " + where_text(p.last)) | dim);
            if (!p.message.empty()) out.push_back(paragraph(p.message) | dim);
            out.push_back(text(""));
            out.push_back(paragraph(p.explain()));
            auto fixes = p.fix();
            if (!fixes.empty()) {
                out.push_back(text(""));
                out.push_back(heading("What to do"));
                for (size_t i = 0; i < fixes.size(); ++i) {
                    out.push_back(hbox({text(" " + std::to_string(i + 1) + ". "), paragraph(fixes[i]) | flex}));
                }
            }
            if (p.variants.size() > 1) {
                out.push_back(text(""));
                out.push_back(heading("Seen as"));
                for (const auto& v : p.variants) out.push_back(text(" " + v));
            }
            own = p.where;
        } else {
            const ErrorGroup& g = report_.groups[item.index];
            out.push_back(paragraph(g.title) | bold | color(level_color(g.level)));
            out.push_back(text("first: " + where_text(g.first) + "   last: " + where_text(g.last)) | dim);
            if (!g.exception.empty() && g.exception != g.title) {
                out.push_back(hbox({text("root exception: ") | color(Color::Yellow), paragraph(g.exception) | flex}));
            }
            out.push_back(text(""));
            out.push_back(paragraph("No rule recognises this. The same error (apart from numbers and ids) was seen " +
                                    thousands(g.count) + (g.count == 1 ? " time." : " times.") +
                                    " Press Enter to see it in the log with its stack trace.") | dim);
            own = g.where;
        }
        if (own) {
            out.push_back(text(""));
            out.push_back(hbox({text("▶ in your code: ") | color(Color::Green) | bold,
                                text(own->file + (own->line ? ":" + std::to_string(own->line) : "")) | bold,
                                text(own->function.empty() ? "" : "  " + own->function) | dim}));
        }
        if (is_root(item) && report_.root->followers > 0) {
            const RootCause& root = *report_.root;
            out.push_back(text(""));
            std::string f = "Then " + thousands(root.followers) + (root.followers == 1 ? " error" : " errors") + " followed " +
                            root.window + ", most likely because of this: ";
            for (size_t i = 0; i < root.follower_top.size(); ++i) {
                f += (i ? ", " : "") + root.follower_top[i].first + " ×" + thousands(root.follower_top[i].second);
            }
            out.push_back(paragraph(f) | color(Color::Yellow));
        }
        if (is_root(item)) {
            out.push_back(text(""));
            out.push_back(paragraph("Why this one: " + report_.root->why + ".") | dim);
        }
        int max_scroll = std::max(0, static_cast<int>(out.size()) - 3);
        panel_detail_scroll_ = std::min(panel_detail_scroll_, max_scroll);
        Elements visible(out.begin() + panel_detail_scroll_, out.end());
        return vbox(std::move(visible));
    }

    Element panel(int height) {
        report();
        Elements out;
        const Report& r = report_;
        if (items_.empty()) {
            std::string why = model_.total() == 0 ? "Nothing read yet." : "No problems found: no rule matched, and there are no errors or warnings.";
            if (!options_.rules) why = "No rules are loaded.";
            return vbox({filler(), text(why) | dim | center, filler()});
        }
        // a summary line
        size_t problems = r.problems.size(), groups = r.groups.size();
        std::string summary = " " + thousands(problems) + (problems == 1 ? " known problem" : " known problems") + ", " +
                              thousands(groups) + (groups == 1 ? " other error or warning" : " other errors and warnings");
        if (r.root) {
            const char* conf = r.root->confidence == RootCause::Confidence::High     ? "high"
                               : r.root->confidence == RootCause::Confidence::Medium ? "medium"
                                                                                     : "low";
            summary += " · root cause confidence: " + std::string(conf);
        }
        out.push_back(text(summary) | dim);

        // the list gets up to half the height; the selected item's explanation the rest
        int list_h = std::clamp(static_cast<int>(items_.size()), 3, std::max(3, height / 2 - 1));
        if (panel_selected_ >= items_.size()) panel_selected_ = items_.size() - 1;
        if (panel_selected_ < panel_scroll_) panel_scroll_ = panel_selected_;
        if (panel_selected_ >= panel_scroll_ + static_cast<size_t>(list_h)) panel_scroll_ = panel_selected_ - list_h + 1;
        Elements rows;
        for (size_t i = panel_scroll_; i < items_.size() && i < panel_scroll_ + static_cast<size_t>(list_h); ++i) {
            // a divider before the first unrecognised group
            if (i > 0 && !items_[i].problem && items_[i - 1].problem && !is_root(items_[i - 1])) {
                rows.push_back(text("   errors and warnings no rule knows") | dim | italic);
            }
            rows.push_back(panel_row(items_[i], i == panel_selected_, width_));
        }
        rows.push_back(filler());
        out.push_back(vbox(std::move(rows)) | size(HEIGHT, EQUAL, list_h + 1));
        out.push_back(separatorLight());
        out.push_back(panel_detail(items_[panel_selected_]) | flex);
        return vbox(std::move(out));
    }

    bool on_filter_key(Event e) {
        if (e == Event::Return) {
            editing_ = false;
            return true;
        }
        if (e == Event::Escape) {
            editing_ = false;
            filter_text_ = filter_before_;
            set_filter(filter_before_);
            return true;
        }
        if (e == Event::Backspace) {
            if (!filter_text_.empty()) {
                // remove a whole UTF-8 character
                size_t n = filter_text_.size() - 1;
                while (n > 0 && (static_cast<unsigned char>(filter_text_[n]) & 0xC0) == 0x80) --n;
                filter_text_.erase(n);
            }
        } else if (e == Event::CtrlU) {
            filter_text_.clear();
        } else if (e.is_character()) {
            filter_text_ += e.character();
        } else {
            return true;
        }
        set_filter(filter_text_);
        return true;
    }

    bool on_mouse(Event e) {
        auto& m = e.mouse();
        if (m.button == Mouse::WheelDown) return move_to(static_cast<long long>(cursor_) + 3), true;
        if (m.button == Mouse::WheelUp) return move_to(static_cast<long long>(cursor_) - 3), true;
        if (m.button == Mouse::Left && m.motion == Mouse::Pressed) {
            int row = m.y - 1;  // below the header
            if (row >= 0 && row < list_height_) {
                size_t target = top_ + static_cast<size_t>(row);
                if (target < rows()) {
                    if (target == cursor_) detail_ = !detail_;
                    move_to(static_cast<long long>(target));
                }
            }
            return true;
        }
        return false;
    }

    // ---- drawing -------------------------------------------------------------------------------------

    Element render() {
        drain();
        auto terminal = Terminal::Size();
        int height = std::max(6, terminal.dimy);
        width_ = terminal.dimx;
        detail_height_ = detail_ ? std::max(6, height * 9 / 20) : 0;
        list_height_ = height - 3 - (detail_ ? detail_height_ + 1 : 0);

        // keep the cursor on screen
        if (cursor_ >= rows() && rows() > 0) cursor_ = rows() - 1;
        if (cursor_ < top_) top_ = cursor_;
        if (cursor_ >= top_ + static_cast<size_t>(list_height_)) top_ = cursor_ - list_height_ + 1;
        if (rows() <= static_cast<size_t>(list_height_)) top_ = 0;

        Elements screen;
        screen.push_back(header());
        if (panel_) {
            screen.push_back(panel(height - 3) | size(HEIGHT, EQUAL, height - 3));
            screen.push_back(text(""));
            screen.push_back(panel_footer());
            return vbox(std::move(screen));
        }
        screen.push_back(list() | size(HEIGHT, EQUAL, list_height_));
        if (detail_) {
            screen.push_back(separatorLight());
            screen.push_back(details() | size(HEIGHT, EQUAL, detail_height_));
        }
        screen.push_back(filter_bar());
        screen.push_back(footer());
        Element main = vbox(std::move(screen));
        if (help_) {
            Elements lines;
            for (const char* l : kHelp) lines.push_back(text(l));
            main = dbox({main, vbox(std::move(lines)) | borderRounded | bgcolor(Color::Black) | clear_under | center});
        }
        return main;
    }

    Element header() {
        Elements parts{text(" culprit ") | bold | color(Color::Black) | bgcolor(Color::RedLight),
                       text(" " + options_.title + " ") | bold};
        if (panel_) parts.push_back(text(" PROBLEMS ") | bold | color(Color::Black) | bgcolor(Color::MagentaLight));
        parts.push_back(filler());
        parts.push_back(text(thousands(model_.total()) + " entries  ") | color(Color::White));
        size_t errors = model_.count(Level::Error) + model_.count(Level::Fatal);
        size_t warnings = model_.count(Level::Warn);
        parts.push_back(text("● " + thousands(errors) + " errors  ") | color(errors ? Color::RedLight : Color::GrayLight));
        parts.push_back(text("● " + thousands(warnings) + " warnings  ") |
                        color(warnings ? Color::YellowLight : Color::GrayLight));
        if (options_.rules) {
            size_t problems = model_.analyzer().problem_count();
            parts.push_back(text("◆ " + thousands(problems) + (problems == 1 ? " problem  " : " problems  ")) |
                            color(problems ? Color::MagentaLight : Color::GrayLight) | (problems ? bold : nothing));
        }
        bool live = !reader_.finished();
        if (follow_ && live) parts.push_back(text(" FOLLOWING ") | color(Color::Black) | bgcolor(Color::Green));
        else if (live) parts.push_back(text(" PAUSED ") | color(Color::Black) | bgcolor(Color::Yellow));
        else parts.push_back(text(" END ") | color(Color::GrayLight));
        return hbox(std::move(parts)) | bgcolor(Color::RGB(36, 41, 51));
    }

    Element row(size_t visible_index) {
        const Entry& e = model_.entry(model_.visible()[visible_index]);
        const bool selected = visible_index == cursor_;
        const auto& words = model_.filter().highlights();

        Elements cells;
        cells.push_back(text(e.time_text.empty() ? std::string(12, ' ') : e.time_text) | dim);
        cells.push_back(text(" "));
        cells.push_back(level_badge(e.level));
        if (model_.multiple_sources()) {
            std::string src = e.source.empty() ? "-" : e.source;
            if (src.size() > 14) src = src.substr(0, 13) + "…";
            cells.push_back(text(" " + src) | bold | size(WIDTH, EQUAL, 16));
        }
        if (logger_column_) {
            std::string lg = short_logger(e.logger);
            if (lg.size() > 22) lg = lg.substr(0, 21) + "…";
            cells.push_back(text(" " + lg) | color(Color::Cyan) | size(WIDTH, EQUAL, 24));
        } else {
            cells.push_back(text(" "));
        }
        if (e.rule) cells.push_back(text("◆ ") | color(severity_color(e.rule->severity)) | bold);
        else cells.push_back(text("  "));
        cells.push_back(highlighted(one_line(e.message), words) | flex);
        if (e.has_stack()) {
            cells.push_back(text(" +" + std::to_string(line_count(e.stack)) + " lines ") | color(Color::Red) | dim);
        }
        auto line = hbox(std::move(cells));
        if (selected) line = line | bgcolor(Color::RGB(40, 52, 66)) | bold;
        return line;
    }

    Element list() {
        if (rows() == 0) {
            std::string why = model_.total() == 0 ? (reader_.finished() ? "No log lines." : "Waiting for log lines…")
                                                   : "Nothing matches the filter. Press x to clear it.";
            return vbox({filler(), text(why) | dim | center, filler()});
        }
        Elements lines;
        size_t end = std::min(rows(), top_ + static_cast<size_t>(list_height_));
        // a logger column when any line on screen has a logger, so messages start in the same place
        logger_column_ = false;
        for (size_t i = top_; i < end && !logger_column_; ++i) {
            logger_column_ = !model_.entry(model_.visible()[i]).logger.empty();
        }
        for (size_t i = top_; i < end; ++i) lines.push_back(row(i));
        lines.push_back(filler());
        return vbox(std::move(lines));
    }

    Element details() {
        if (rows() == 0) return text("");
        const Entry& e = model_.entry(model_.visible()[cursor_]);
        Elements out;
        Elements title{level_badge(e.level), text(" " + e.time_text) | dim};
        if (!e.source.empty()) title.push_back(text("  " + e.source) | bold);
        if (!e.logger.empty()) title.push_back(text("  " + e.logger) | color(Color::Cyan));
        if (!e.thread.empty()) title.push_back(text("  [" + e.thread + "]") | dim);
        out.push_back(hbox(std::move(title)));

        // the message, wrapped
        size_t start = 0;
        while (start <= e.message.size()) {
            size_t nl = e.message.find('\n', start);
            out.push_back(paragraph(e.message.substr(start, nl == std::string::npos ? std::string::npos : nl - start)) | bold);
            if (nl == std::string::npos) break;
            start = nl + 1;
        }

        // what culprit knows about it
        if (e.rule && options_.rules) {
            if (auto m = options_.rules->match(e); m && m->rule == e.rule) {
                const Rule& r = *m->rule;
                out.push_back(text(""));
                out.push_back(hbox({text("◆ ") | color(severity_color(r.severity)) | bold,
                                    text(culprit::render(r.title, m->captures)) | bold | color(severity_color(r.severity)),
                                    text("  " + r.id) | dim}));
                out.push_back(hbox({text("  "), paragraph(culprit::render(r.explain, m->captures)) | flex}));
                for (size_t i = 0; i < r.fix.size(); ++i) {
                    out.push_back(hbox({text("  " + std::to_string(i + 1) + ". ") | color(Color::Cyan),
                                        paragraph(culprit::render(r.fix[i], m->captures)) | flex}));
                }
                out.push_back(text(""));
            }
        } else if (e.group != UINT32_MAX) {
            if (const ErrorGroup* g = model_.analyzer().group(e.group); g && g->count > 1) {
                out.push_back(text("  seen " + thousands(g->count) + " times, first at " +
                                   (g->first.time.empty() ? "line " + std::to_string(g->first.line) : g->first.time) +
                                   "  (filter: group:" + std::to_string(g->id) + ")") | dim);
            }
        }

        for (const auto& [k, v] : e.fields) {
            out.push_back(hbox({text("  " + k) | color(Color::Cyan) | size(WIDTH, GREATER_THAN, 18), text("  "),
                                text(one_line(v))}));
        }

        if (e.has_stack()) {
            auto frames = parse_stack(e.stack);
            auto own = first_own_frame(frames);
            if (own) {
                std::string where = own->file + (own->line ? ":" + std::to_string(own->line) : "");
                out.push_back(hbox({text("  ▶ in your code: ") | color(Color::Green) | bold, text(where) | bold,
                                    text(own->function.empty() ? "" : "  " + own->function) | dim}));
            }
            for (const auto& f : frames) {
                Element line = text("    " + f.text);
                switch (f.kind) {
                    case Frame::Kind::Header: line = line | color(Color::Red) | bold; break;
                    case Frame::Kind::CausedBy: line = line | color(Color::Yellow) | bold; break;
                    case Frame::Kind::Frame:
                        line = f.own ? hbox({text("  ▶ ") | color(Color::Green), text(f.text) | bold})
                                     : line | dim;
                        break;
                    case Frame::Kind::Other: line = line | dim; break;
                }
                out.push_back(line);
            }
        } else if (e.format == "text" && e.raw != e.message) {
            out.push_back(text("  " + one_line(e.raw)) | dim);
        }

        // scroll
        int max_scroll = std::max(0, static_cast<int>(out.size()) - detail_height_);
        detail_scroll_ = std::min(detail_scroll_, max_scroll);
        Elements visible(out.begin() + detail_scroll_, out.end());
        auto body = vbox(std::move(visible));
        if (focus_detail_) body = hbox({text("┃") | color(Color::Cyan), body | flex});
        return body;
    }

    Element filter_bar() {
        if (editing_) {
            return hbox({text(" / ") | bold | color(Color::Cyan), text(filter_text_), text(" ") | inverted, filler(),
                         text(model_.filter().error().empty() ? thousands(rows()) + " match " : model_.filter().error() + " ") |
                             (model_.filter().error().empty() ? dim : color(Color::Red))});
        }
        if (!model_.query().empty()) {
            return hbox({text(" filter: ") | dim, text(model_.query()) | color(Color::Cyan), filler(),
                         text(thousands(rows()) + " of " + thousands(model_.total()) + " ") | dim});
        }
        if (!message_.empty()) return text(" " + message_) | color(Color::Yellow);
        return text("");
    }

    Element footer() {
        auto key = [](const char* k, const char* what) {
            return hbox({text(k) | bold | color(Color::Cyan), text(" " + std::string(what) + "  ") | dim});
        };
        return hbox({text(" "), key("/", "filter"), key("Enter", "details"), key("a", "problems"), key("n/N", "errors"),
                     key("e", "warn+err"), key("f", "follow"), key("?", "help"), key("q", "quit"), filler()});
    }

    Element panel_footer() {
        auto key = [](const char* k, const char* what) {
            return hbox({text(k) | bold | color(Color::Cyan), text(" " + std::string(what) + "  ") | dim});
        };
        return hbox({text(" "), key("↑↓", "choose"), key("Enter", "show its entries"), key("Tab", "scroll text"),
                     key("Esc", "back to the log"), filler()});
    }

    Reader& reader_;
    ViewerOptions options_;
    App* app_ = nullptr;
    LogModel model_;
    std::atomic<bool> wake_pending_{false};

    size_t cursor_ = 0;
    size_t top_ = 0;
    bool follow_ = true;
    bool detail_ = false;
    bool focus_detail_ = false;
    int detail_scroll_ = 0;
    bool editing_ = false;
    bool help_ = false;
    std::string filter_text_;
    std::string filter_before_;
    std::string message_;
    int list_height_ = 10;
    int detail_height_ = 0;
    int width_ = 80;
    bool logger_column_ = true;

    bool panel_ = false;
    size_t panel_selected_ = 0;
    size_t panel_scroll_ = 0;
    int panel_detail_scroll_ = 0;
    Report report_;
    size_t report_total_ = 0;
    bool report_ready_ = false;
    std::vector<PanelItem> items_;
};

}  // namespace

int run_viewer(Reader& reader, const ViewerOptions& options) {
    Viewer viewer(reader, options);
    return viewer.run();
}

}  // namespace culprit
