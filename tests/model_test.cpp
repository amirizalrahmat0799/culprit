#include "check.hpp"
#include "core/model.hpp"

using namespace culprit;

namespace {
Entry make(Level level, std::string msg, std::string source = "") {
    Entry e;
    e.level = level;
    e.message = std::move(msg);
    e.source = std::move(source);
    return e;
}
}  // namespace

TEST("appending applies the filter to new entries") {
    LogModel m;
    m.set_filter("level>=warn");
    m.append({make(Level::Info, "a"), make(Level::Error, "b"), make(Level::Warn, "c")});
    CHECK_EQ(m.total(), 3u);
    CHECK_EQ(m.visible().size(), 2u);
    m.append({make(Level::Fatal, "d")});
    CHECK_EQ(m.visible().size(), 3u);
    CHECK_EQ(m.count(Level::Error), 1u);
    m.set_filter("");
    CHECK_EQ(m.visible().size(), 4u);
}

TEST("finding errors") {
    LogModel m;
    m.append({make(Level::Info, "a"), make(Level::Error, "b"), make(Level::Info, "c"), make(Level::Warn, "d"),
              make(Level::Error, "e")});
    auto next = m.find(0, 1, Level::Error);
    CHECK(next && *next == 1);
    next = m.find(1, 1, Level::Error);
    CHECK(next && *next == 4);
    CHECK(!m.find(4, 1, Level::Error).has_value());
    auto prev = m.find(4, -1, Level::Warn);
    CHECK(prev && *prev == 3);
}

TEST("sources") {
    LogModel m;
    m.append({make(Level::Info, "a", "api"), make(Level::Info, "b", "api")});
    CHECK(!m.multiple_sources());
    m.append({make(Level::Info, "c", "db")});
    CHECK(m.multiple_sources());
}
