#pragma once
// culprit web: the logs, the analysis and the AI investigator in a browser tab.
//
// A small HTTP server on 127.0.0.1 serves one page (compiled into culprit) and a JSON API. Logs often hold
// secrets, and any website the browser visits can send requests to localhost, so every request needs:
//   - the token in the URL culprit prints (128 random bits), sent by the page in the X-Culprit-Token header
//   - a Host header naming 127.0.0.1 or localhost, so a DNS-rebinding site can't pose as this server
//   - for anything that changes state (starting an investigation), the header rather than the URL, which a
//     cross-site form can't set, and an Origin of this server when the browser sends one
// Nothing is listened for on other network interfaces.
//
// The API (all GET unless marked, all JSON):
//   /api/status?since=S     waits up to 20 s for something to change after S (a long poll), then reports
//                           counts, whether the input is still being read, and the investigation's state
//   /api/match?filter=Q&from=K   indices of entries from K on that pass the filter (or "all")
//   /api/rows?ids=1,2,3     one-line summaries for the list
//   /api/entry?i=N          one entry in full: fields, stack frames, the problem a rule recognised
//   /api/report             the analysis, as `culprit analyze --format json` prints it
//   /api/investigation      the AI investigation: its steps so far and its finding
//   POST /api/investigate   {"index": N (optional), "confirm": true when the user agreed to send}
//   POST /api/investigation/cancel

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/reader.hpp"
#include "core/rules.hpp"

namespace culprit::web {

struct Options {
    std::vector<Reader::Input> inputs;
    bool follow = true;
    std::string title;    // what is shown: file names, or "stdin"
    std::string version;
    std::shared_ptr<const RuleSet> rules;
    std::vector<std::string> rule_errors;  // problems loading rule files, shown on the page
    std::string filter;   // the filter the page starts with
    bool no_ai = false;
    std::string project_start;  // where the project is looked for (the current folder)
    int port = 4790;      // tried first; another free port is used when it's taken. 0: any free port
};

class Server {
public:
    explicit Server(Options options);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    /// Binds to 127.0.0.1, starts reading the inputs and serving. False, with `error`, when it can't.
    bool start(std::string* error);
    int port() const;
    const std::string& token() const;
    /// The address to open: http://127.0.0.1:PORT/?t=TOKEN
    std::string url() const;
    /// Blocks until stop() (from another thread) or the server fails.
    void wait();
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Whether a Host header names this server: 127.0.0.1, localhost or [::1], with the port.
bool host_allowed(std::string_view host, int port);
/// Compares in constant time.
bool token_matches(std::string_view given, std::string_view expected);
/// 32 hex digits from the system's random source.
std::string make_token();
/// Opens our own http://127.0.0.1 address in the default browser. False when there's none to open.
bool open_in_browser(const std::string& url);

}  // namespace culprit::web
