#pragma once
// Stack traces broken into frames, so the viewer can show your own code and dim the framework's, and the
// analyser can find where in your project an error happened.
//
//   Java    "\tat com.example.shop.OrderService.place(OrderService.java:42)"
//   Python  "  File \"/app/app/api.py\", line 42, in create_todo"
//   Node    "    at createApp (/app/src/app.ts:12:5)"
//   Go      "\t/app/handler.go:42 +0x1d"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace culprit {

struct Frame {
    enum class Kind { Header, Frame, CausedBy, Other };
    Kind kind = Kind::Other;
    std::string text;      // the line as written, without leading whitespace
    std::string function;  // "com.example.shop.OrderService.place", "create_todo"
    std::string file;      // "OrderService.java", "/app/app/api.py"
    int line = 0;
    bool own = false;      // in the project's code rather than a library, framework or runtime
};

std::vector<Frame> parse_stack(std::string_view stack);

/// The first frame in the project's own code, i.e. where to look first.
std::optional<Frame> first_own_frame(const std::vector<Frame>& frames);

}  // namespace culprit
