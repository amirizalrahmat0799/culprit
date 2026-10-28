#include "core/time.hpp"

#include <cmath>
#include <cstdio>
#include <ctime>

namespace culprit {
namespace {

bool digits(std::string_view s, size_t pos, size_t n) {
    if (pos + n > s.size()) return false;
    for (size_t i = pos; i < pos + n; ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
    }
    return true;
}

int number(std::string_view s, size_t pos, size_t n) {
    int v = 0;
    for (size_t i = pos; i < pos + n; ++i) v = v * 10 + (s[i] - '0');
    return v;
}

// days since 1970-01-01 for a civil date (Howard Hinnant's algorithm)
int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

// offset of local time from UTC at the given moment, in seconds
int64_t local_offset_seconds(int64_t epoch_seconds) {
    std::time_t t = static_cast<std::time_t>(epoch_seconds);
    std::tm local{}, utc{};
#ifdef _WIN32
    localtime_s(&local, &t);
    gmtime_s(&utc, &t);
#else
    localtime_r(&t, &local);
    gmtime_r(&t, &utc);
#endif
    int64_t l = days_from_civil(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday) * 86400 +
                local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec;
    int64_t u = days_from_civil(utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday) * 86400 + utc.tm_hour * 3600 +
                utc.tm_min * 60 + utc.tm_sec;
    return l - u;
}

// HH:MM:SS with optional .fff or ,fff; returns characters read (0 when not a clock)
size_t read_clock(std::string_view s, size_t pos, int& h, int& m, int& sec, int& ms, std::string& clock) {
    if (!digits(s, pos, 2) || pos + 8 > s.size() || s[pos + 2] != ':' || !digits(s, pos + 3, 2) ||
        s[pos + 5] != ':' || !digits(s, pos + 6, 2)) {
        return 0;
    }
    h = number(s, pos, 2);
    m = number(s, pos + 3, 2);
    sec = number(s, pos + 6, 2);
    if (h > 23 || m > 59 || sec > 60) return 0;
    size_t end = pos + 8;
    ms = 0;
    if (end < s.size() && (s[end] == '.' || s[end] == ',') && digits(s, end + 1, 1)) {
        size_t f = end + 1;
        size_t n = 0;
        int scale = 100;
        while (f + n < s.size() && s[f + n] >= '0' && s[f + n] <= '9') {
            if (n < 3) {
                ms += (s[f + n] - '0') * scale;
                scale /= 10;
            }
            ++n;
        }
        end = f + n;
    }
    char buf[16];
    std::snprintf(buf, sizeof buf, "%02d:%02d:%02d.%03d", h, m, sec, ms);
    clock = buf;
    return end - pos;
}

}  // namespace

std::string clock_from_epoch_ms(int64_t ms) {
    std::time_t t = static_cast<std::time_t>(ms / 1000);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    char buf[16];
    std::snprintf(buf, sizeof buf, "%02d:%02d:%02d.%03d", local.tm_hour, local.tm_min, local.tm_sec,
                  static_cast<int>(((ms % 1000) + 1000) % 1000));
    return buf;
}

std::optional<Timestamp> parse_timestamp(std::string_view s) {
    Timestamp t;
    int h, m, sec, ms;

    // a clock on its own: 12:00:01.234
    if (size_t n = read_clock(s, 0, h, m, sec, ms, t.clock)) {
        t.length = n;
        return t;
    }

    // YYYY-MM-DD, then 'T' or ' ', then a clock
    if (!digits(s, 0, 4) || s.size() < 19 || s[4] != '-' || !digits(s, 5, 2) || s[7] != '-' || !digits(s, 8, 2) ||
        (s[10] != 'T' && s[10] != ' ')) {
        return std::nullopt;
    }
    int year = number(s, 0, 4), month = number(s, 5, 2), day = number(s, 8, 2);
    if (month < 1 || month > 12 || day < 1 || day > 31) return std::nullopt;
    size_t n = read_clock(s, 11, h, m, sec, ms, t.clock);
    if (!n) return std::nullopt;
    size_t end = 11 + n;

    int64_t seconds = days_from_civil(year, month, day) * 86400 + h * 3600 + m * 60 + sec;
    // offset: Z, +08:00, +0800, -05:00; none means local time
    if (end < s.size() && s[end] == 'Z') {
        t.epoch_ms = seconds * 1000 + ms;
        t.zoned = true;
        ++end;
    } else if (s.substr(end, 4) == " UTC" || s.substr(end, 4) == " GMT") {  // PostgreSQL, some Java formats
        t.epoch_ms = seconds * 1000 + ms;
        t.zoned = true;
        end += 4;
    } else if (end < s.size() && (s[end] == '+' || s[end] == '-') && digits(s, end + 1, 2)) {
        int sign = s[end] == '-' ? -1 : 1;
        int oh = number(s, end + 1, 2), om = 0;
        size_t e = end + 3;
        if (e < s.size() && s[e] == ':' && digits(s, e + 1, 2)) {
            om = number(s, e + 1, 2);
            e += 3;
        } else if (digits(s, e, 2)) {
            om = number(s, e, 2);
            e += 2;
        }
        t.epoch_ms = (seconds - sign * (oh * 3600 + om * 60)) * 1000 + ms;
        t.zoned = true;
        end = e;
    } else {
        t.epoch_ms = (seconds - local_offset_seconds(seconds)) * 1000 + ms;
    }
    t.length = end;
    if (t.zoned) t.clock = clock_from_epoch_ms(*t.epoch_ms);
    return t;
}

std::optional<Timestamp> parse_time_value(std::string_view v) {
    if (v.empty()) return std::nullopt;
    if (v[0] < '0' || v[0] > '9') return std::nullopt;
    if (auto t = parse_timestamp(v); t && t->length == v.size()) return t;

    // an epoch number: seconds (1730000000.123) or milliseconds (1730000000123)
    double value = 0;
    size_t i = 0;
    while (i < v.size() && v[i] >= '0' && v[i] <= '9') value = value * 10 + (v[i++] - '0');
    if (i < v.size() && v[i] == '.') {
        double scale = 0.1;
        for (++i; i < v.size() && v[i] >= '0' && v[i] <= '9'; ++i, scale /= 10) value += (v[i] - '0') * scale;
    }
    if (i != v.size()) return std::nullopt;
    int64_t ms;
    if (value > 1e14) ms = static_cast<int64_t>(value / 1000);  // microseconds
    else if (value > 1e11) ms = static_cast<int64_t>(value);    // milliseconds
    else if (value > 1e8) ms = std::llround(value * 1000);  // seconds
    else return std::nullopt;
    Timestamp t;
    t.epoch_ms = ms;
    t.zoned = true;
    t.clock = clock_from_epoch_ms(ms);
    t.length = v.size();
    return t;
}

}  // namespace culprit
