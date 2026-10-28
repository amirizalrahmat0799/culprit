#include "tui/viewer.hpp"

#include <algorithm>
#include <atomic>
#include <ftxui/component/app.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

#include "core/model.hpp"
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
    "  Filter:  payment  \"order failed\"  -health  level>=warn  logger=OrderService",
    "           orderId=8812  /time(d)? out/  has:stack",
    "",
    "  q  quit                         ?  close this help",
};

class Viewer {
public:
    Viewer(Reader& reader, const ViewerOptions& options) : reader_(reader), options_(options) {
        model_.set_filter(options.initial_filter);
        filter_text_ = options.initial_filter;
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
        if (e == Event::Character('q') || e == Event::CtrlC) {
            app_->Exit();
            return true;
        }
        return false;
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
        parts.push_back(filler());
        parts.push_back(text(thousands(model_.total()) + " entries  ") | color(Color::White));
        size_t errors = model_.count(Level::Error) + model_.count(Level::Fatal);
        size_t warnings = model_.count(Level::Warn);
        parts.push_back(text("● " + thousands(errors) + " errors  ") | color(errors ? Color::RedLight : Color::GrayLight));
        parts.push_back(text("● " + thousands(warnings) + " warnings  ") |
                        color(warnings ? Color::YellowLight : Color::GrayLight));
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
        return hbox({text(" "), key("/", "filter"), key("Enter", "details"), key("n/N", "errors"),
                     key("e", "warn+err"), key("f", "follow"), key("?", "help"), key("q", "quit"), filler()});
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
};

}  // namespace

int run_viewer(Reader& reader, const ViewerOptions& options) {
    Viewer viewer(reader, options);
    return viewer.run();
}

}  // namespace culprit
