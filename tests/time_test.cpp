#include "check.hpp"
#include "core/time.hpp"

using namespace culprit;

TEST("ISO 8601 with offsets") {
    auto t = parse_timestamp("2026-10-28T09:41:02.118+08:00  INFO");
    CHECK(t.has_value());
    if (!t) return;
    CHECK_EQ(t->clock, "01:41:02.118");  // shown in local time (UTC in the tests)
    CHECK(t->zoned);
    CHECK_EQ(t->length, 29u);
    CHECK(t->epoch_ms.has_value());
    auto z = parse_timestamp("2026-10-28T01:41:02.118Z");
    CHECK(z && z->epoch_ms && *z->epoch_ms == *t->epoch_ms);  // the same moment
    auto compact = parse_timestamp("2026-10-28T09:41:02+0800 x");
    CHECK(compact && compact->epoch_ms && *compact->epoch_ms == *t->epoch_ms - 118);
}

TEST("other timestamp shapes") {
    auto py = parse_timestamp("2026-10-28 09:41:02,118 - app");
    CHECK(py && py->clock == "09:41:02.118" && py->length == 23);
    auto clock = parse_timestamp("12:00:01.5 [main]");
    CHECK(clock && clock->clock == "12:00:01.500" && !clock->epoch_ms);
    auto micro = parse_timestamp("2026-10-28T09:41:02.118734Z");
    CHECK(micro && micro->clock == "09:41:02.118");
    auto pg = parse_timestamp("2026-10-28 01:41:00.912 UTC [1] LOG");
    CHECK(pg && pg->zoned && pg->length == 27);
    auto local = parse_timestamp("2026-10-28 09:41:02,118");
    CHECK(local && !local->zoned && local->clock == "09:41:02.118");
    CHECK(!parse_timestamp("hello").has_value());
    CHECK(!parse_timestamp("2026-13-01 00:00:00").has_value());
    CHECK(!parse_timestamp("25:00:00").has_value());
}

TEST("epoch numbers") {
    auto ms = parse_time_value("1761615662118");
    auto s = parse_time_value("1761615662.118");
    CHECK(ms && s && ms->epoch_ms && s->epoch_ms);
    if (ms && s) CHECK_EQ(*ms->epoch_ms, *s->epoch_ms);
    CHECK(!parse_time_value("42").has_value());
    CHECK(!parse_time_value("abc").has_value());
    auto iso = parse_time_value("2026-10-28T01:41:05.019Z");
    CHECK(iso && iso->clock.size() == 12);
}
