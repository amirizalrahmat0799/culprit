#include "core/stack.hpp"

#include <array>
#include <cctype>

namespace culprit {
namespace {

bool starts_with(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

int to_int(std::string_view s) {
    int v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') break;
        v = v * 10 + (c - '0');
    }
    return v;
}

// Java packages that are never "your code"
constexpr std::array<std::string_view, 33> kJavaLibraries = {
    "java.",          "javax.",          "jakarta.",        "jdk.",           "sun.",
    "com.sun.",       "kotlin.",         "kotlinx.",        "scala.",         "groovy.",
    "org.springframework.", "org.apache.", "org.hibernate.", "org.eclipse.",  "org.junit.",
    "org.mockito.",   "org.slf4j.",      "ch.qos.logback.", "com.zaxxer.",    "io.netty.",
    "reactor.",       "io.micrometer.",  "org.postgresql.", "com.mysql.",     "org.h2.",
    "com.fasterxml.", "tools.jackson.",  "io.undertow.",    "org.flywaydb.",  "org.aspectj.",
    "net.bytebuddy.", "feign.",          "io.grpc.",
};

bool java_library(std::string_view function) {
    for (auto p : kJavaLibraries) {
        if (starts_with(function, p)) return true;
    }
    // generated proxies: com.example.Foo$$SpringCGLIB$$0, jdk.proxy2.$Proxy
    return function.find("$$") != std::string_view::npos || function.find("$Proxy") != std::string_view::npos;
}

bool path_is_library(std::string_view path) {
    for (std::string_view marker : {"node_modules", "site-packages", "dist-packages", "/lib/python", "\\lib\\python",
                                    "/usr/local/go/", "/usr/lib/go", "/go/pkg/mod/", "<frozen ", "node:internal",
                                    "/rustc/", ".cargo/registry"}) {
        if (path.find(marker) != std::string_view::npos) return true;
    }
    return starts_with(path, "node:") || starts_with(path, "internal/") || starts_with(path, "runtime/");
}

Frame parse_frame(std::string_view raw) {
    Frame f;
    std::string_view s = trim(raw);
    f.text.assign(s);

    // Java: at com.example.Foo.bar(Foo.java:42), possibly "at app//com.example..." or "at java.base/java.lang..."
    if (starts_with(s, "at ") && s.find('(') != std::string_view::npos && s.back() == ')') {
        f.kind = Frame::Kind::Frame;
        std::string_view inside = s.substr(3);
        size_t open = inside.find('(');
        std::string_view fn = inside.substr(0, open);
        if (size_t slash = fn.find('/'); slash != std::string_view::npos) fn = fn.substr(fn.rfind('/') + 1);
        std::string_view loc = inside.substr(open + 1, inside.size() - open - 2);
        f.function.assign(fn);

        // Node: "at name (/path/file.js:12:5)" has a space in the function part, and a path location
        bool node = loc.find('/') != std::string_view::npos || loc.find('\\') != std::string_view::npos ||
                    starts_with(loc, "node:");
        size_t c1 = loc.find(':');
        if (node) {
            // /app/src/app.ts:12:5 (Windows paths have a drive colon first)
            size_t last = loc.rfind(':');
            size_t mid = last == std::string_view::npos ? last : loc.rfind(':', last - 1);
            if (mid != std::string_view::npos && mid > 1) {
                f.file.assign(loc.substr(0, mid));
                f.line = to_int(loc.substr(mid + 1));
            } else {
                f.file.assign(loc);
            }
            f.own = !path_is_library(f.file);
        } else {
            if (c1 != std::string_view::npos) {
                f.file.assign(loc.substr(0, c1));
                f.line = to_int(loc.substr(c1 + 1));
            } else {
                f.file.assign(loc);  // "Native Method", "Unknown Source"
            }
            f.own = !java_library(fn) && c1 != std::string_view::npos;
        }
        return f;
    }

    // Node frames without a function: "at /app/src/server.ts:5:3"
    if (starts_with(s, "at /") || starts_with(s, "at file://")) {
        f.kind = Frame::Kind::Frame;
        std::string_view loc = s.substr(3);
        size_t last = loc.rfind(':');
        size_t mid = last == std::string_view::npos ? last : loc.rfind(':', last - 1);
        if (mid != std::string_view::npos) {
            f.file.assign(loc.substr(0, mid));
            f.line = to_int(loc.substr(mid + 1));
        }
        f.own = !path_is_library(f.file);
        return f;
    }

    // Python: File "/app/app/api.py", line 42, in create_todo
    if (starts_with(s, "File \"")) {
        f.kind = Frame::Kind::Frame;
        size_t close = s.find('"', 6);
        if (close != std::string_view::npos) {
            f.file.assign(s.substr(6, close - 6));
            size_t ln = s.find("line ", close);
            if (ln != std::string_view::npos) f.line = to_int(s.substr(ln + 5));
            size_t in = s.find(", in ", close);
            if (in != std::string_view::npos) f.function.assign(s.substr(in + 5));
        }
        f.own = !path_is_library(f.file);
        return f;
    }

    // Go: /app/handler.go:42 +0x1d  (the function is on the line before)
    if ((s.find(".go:") != std::string_view::npos) && (starts_with(s, "/") || (s.size() > 2 && s[1] == ':'))) {
        f.kind = Frame::Kind::Frame;
        size_t c = s.find(".go:") + 3;
        f.file.assign(s.substr(0, c));
        f.line = to_int(s.substr(c + 1));
        f.own = !path_is_library(f.file);
        return f;
    }

    if (starts_with(s, "Caused by:") || starts_with(s, "During handling of") || starts_with(s, "The above exception")) {
        f.kind = Frame::Kind::CausedBy;
        return f;
    }
    if (starts_with(s, "... ") || starts_with(s, "Traceback (") || starts_with(s, "goroutine ") || s.empty()) {
        f.kind = Frame::Kind::Other;
        return f;
    }
    // "java.lang.IllegalStateException: message", "ValueError: bad", "panic: ..."
    f.kind = Frame::Kind::Header;
    return f;
}

}  // namespace

std::vector<Frame> parse_stack(std::string_view stack) {
    std::vector<Frame> frames;
    size_t start = 0;
    while (start <= stack.size()) {
        size_t nl = stack.find('\n', start);
        std::string_view line = stack.substr(start, nl == std::string_view::npos ? std::string_view::npos : nl - start);
        Frame f = parse_frame(line);
        // Go: "main.handler(...)" followed by its file line; give the file frame the function's name
        if (f.kind == Frame::Kind::Frame && f.function.empty() && !frames.empty() &&
            frames.back().kind == Frame::Kind::Header && frames.back().text.find('(') != std::string::npos &&
            f.file.ends_with(".go")) {
            // the arguments start at the last '(' ("main.(*Store).SetDone(0x0)")
            std::string fn = frames.back().text.substr(0, frames.back().text.rfind('('));
            f.function = fn;
            frames.back().kind = Frame::Kind::Other;
        }
        if (!(f.kind == Frame::Kind::Other && f.text.empty())) frames.push_back(std::move(f));
        if (nl == std::string_view::npos) break;
        start = nl + 1;
    }
    return frames;
}

std::optional<Frame> first_own_frame(const std::vector<Frame>& frames) {
    for (const auto& f : frames) {
        if (f.kind == Frame::Kind::Frame && f.own) return f;
    }
    return std::nullopt;
}

}  // namespace culprit
